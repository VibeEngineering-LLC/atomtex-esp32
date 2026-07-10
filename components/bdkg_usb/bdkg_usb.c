#include "bdkg_usb.h"
#include "bdkg05.h"
#include "modbus_rtu.h"
#include "esp_log.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>
#include <time.h>

static const char *TAG = "bdkg_usb";
static cdc_acm_dev_hdl_t s_dev = NULL;
static StreamBufferHandle_t s_rx = NULL;
static uint16_t s_vid = 0, s_pid = 0;
static bool s_inited = false;   // idempotency guard: main.c зовёт bdkg_usb_init() дважды

// E4: latest reading snapshot published by poll_task, read by web /api/bdkg.
static bdkg_snapshot_t s_snap = {0};
static SemaphoreHandle_t s_snap_mtx = NULL;
static void snap_publish(const bdkg05_reading_t *r);

// #BDKG-17: on-board кольцо истории (300 точек, ~5.9 КБ .bss). Пишется в poll_task
// под s_snap_mtx, читается /api/bdkg/history. s_hist_n растёт до 300 и стопорится.
#define BDKG_HIST_MAX 300
static bdkg_hist_point_t s_hist[BDKG_HIST_MAX];
static size_t s_hist_idx = 0, s_hist_n = 0;

// #BDKG-16 «Сброс замера»: poll_task на след. итерации переинициализирует прибор.
static volatile bool s_reset_req = false;
void bdkg_usb_request_reset(void) { s_reset_req = true; }

// FTDI packetizes into <=64B chunks, each prefixed by 2 status bytes
// (modem, line-status). Strip both before feeding Modbus RX stream.
static bool rx_cb(const uint8_t *data, size_t len, void *arg) {
    (void)arg;
    if (!s_rx) return true;
    for (size_t off = 0; off < len; ) {
        size_t ch = len - off; if (ch > 64) ch = 64;
        if (ch > 2) xStreamBufferSend(s_rx, data + off + 2, ch - 2, 0);
        off += ch;
    }
    return true;
}

static void ev_cb(const cdc_acm_host_dev_event_data_t *e, void *ctx) {
    (void)ctx;
    if (e->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGW(TAG, "device disconnected");
        if (s_dev) { cdc_acm_host_close(s_dev); s_dev = NULL; }
    }
}

static bool enum_cb(const usb_device_desc_t *d, uint8_t *cfg) {
    s_vid = d->idVendor; s_pid = d->idProduct;
    ESP_LOGI(TAG, "enum VID=%04x PID=%04x class=%d", d->idVendor, d->idProduct, d->bDeviceClass);
    *cfg = 1;
    return true;
}

static void lib_task(void *arg) {
    (void)arg;
    while (1) {
        uint32_t fl;
        usb_host_lib_handle_events(portMAX_DELAY, &fl);
        if (fl & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static int sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); return 0; }

// txn: Modbus request/response over CDC. No framed length from device -> read
// first byte with a wide deadline, then accumulate until a short idle gap.
static int txn(void *ctx, const uint8_t *req, size_t rl, uint8_t *resp, size_t cap, size_t *rn) {
    (void)ctx;
    if (!s_dev || !s_rx) return -1;
    xStreamBufferReset(s_rx);
    if (cdc_acm_host_data_tx_blocking(s_dev, req, rl, 500) != ESP_OK) return -1;
    size_t got = xStreamBufferReceive(s_rx, resp, cap, pdMS_TO_TICKS(500));
    if (got == 0) return -1;
    while (got < cap) {
        size_t r = xStreamBufferReceive(s_rx, resp + got, cap - got, pdMS_TO_TICKS(20));
        if (r == 0) break;
        got += r;
    }
    *rn = got;
    return 0;
}

// open_dev: try CDC-ACM first, fall back to vendor-specific (CP210x/CH340/FTDI
// enumerate as vendor class). Set line coding 19200 8N1 + assert DTR/RTS.
static bool open_dev(void) {
    const cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 256,
        .in_buffer_size = 256,
        .event_cb = ev_cb,
        .data_cb = rx_cb,
        .user_arg = NULL,
    };
    esp_err_t e = cdc_acm_host_open(s_vid, s_pid, 0, &cfg, &s_dev);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "CDC-ACM open failed (%s), trying vendor", esp_err_to_name(e));
        e = cdc_acm_host_open_vendor_specific(s_vid, s_pid, 0, &cfg, &s_dev);
    }
    if (e != ESP_OK) { ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(e)); return false; }
    return true;
}

// FTDI FT232 is vendor-class, not CDC — line coding via SIO control transfers.
// 19200 -> FTDI divisor wValue=0x809C wIndex=0; 8N1 -> SET_DATA 0x0008.
#define FTDI_OUT 0x40
static void cfg_line(void) {
    cdc_acm_host_send_custom_request(s_dev, FTDI_OUT, 0, 0x0000, 0, 0, NULL); // RESET
    cdc_acm_host_send_custom_request(s_dev, FTDI_OUT, 3, 0x809C, 0, 0, NULL); // baud 19200
    cdc_acm_host_send_custom_request(s_dev, FTDI_OUT, 4, 0x0008, 0, 0, NULL); // 8N1
    cdc_acm_host_send_custom_request(s_dev, FTDI_OUT, 2, 0x0000, 0, 0, NULL); // no flow ctrl
    cdc_acm_host_send_custom_request(s_dev, FTDI_OUT, 1, 0x0303, 0, 0, NULL); // DTR+RTS
    cdc_acm_host_send_custom_request(s_dev, FTDI_OUT, 9, 0x0001, 0, 0, NULL); // latency timer 1ms (иначе 16ms дефолт рвёт хвост кадра → -3)
    cdc_acm_host_send_custom_request(s_dev, FTDI_OUT, 0, 0x0001, 0, 0, NULL); // purge RX
    cdc_acm_host_send_custom_request(s_dev, FTDI_OUT, 0, 0x0002, 0, 0, NULL); // purge TX
    ESP_LOGI(TAG, "FTDI configured 19200 8N1 VID=%04x PID=%04x", s_vid, s_pid);
}

static void poll_task(void *arg) {
    (void)arg;
    while (s_vid == 0) vTaskDelay(pdMS_TO_TICKS(200));
    while (!open_dev()) vTaskDelay(pdMS_TO_TICKS(2000));
    cfg_line();

    bdkg05_t h;
    bdkg05_bind(&h, txn, NULL, BDKG05_DEFAULT_ADDR);
    ESP_LOGI(TAG, "wait_ready...");
    bdkg05_status_t s = bdkg05_wait_ready(&h, 6000, sleep_ms);
    ESP_LOGI(TAG, "wait_ready -> %d", s);

    // §10.2/§10.3: ATexch шлёт init на t≈3.65с — после ~2с boot-паузы + phase-1
    // read-before-write. На холодном приборе WRITE до готовности игнорируется
    // (echo есть, но coil turnOnHV/resetAveraging не срабатывает → МЭД=0).
    // Эмулируем phase-1: ~3с pre-init FC04-чтений, ПОТОМ init.
    for (int w = 0; w < 6; w++) {
        bdkg05_reading_t pr;
        bdkg05_read(&h, &pr);   // read-before-write, результат отбрасываем
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    s = bdkg05_init(&h);
    ESP_LOGI(TAG, "init -> %d", s);

    // DIAG H2 (systematic-debug, single-var): МЭД(FC04 0x0008)=0 при init->0.
    // Локализация: FC03 holding control-рег 0x0000 (режим?) + фон МЭД 0x000A
    // (референс read_background тут читал 0.091 мкЗв/ч). bg!=0 → dosimeter-space
    // жив, МЭД=0 = dose-engine/HV не стартанул; bg=0 → режим не переключился.
    {
        uint8_t req[8], resp[64], pl[64]; size_t rl, rn = 0, pn = 0; uint8_t exc = 0;
        rl = mb_build_read_holding(req, BDKG05_DEFAULT_ADDR, 0x0000, 1);
        if (txn(NULL, req, rl, resp, sizeof(resp), &rn) == 0 &&
            mb_parse_response(resp, rn, MB_FC_READ_HOLDING, pl, sizeof(pl), &pn, &exc) == MB_OK)
            ESP_LOGI(TAG, "DIAG hold[0x0000]=%02x%02x bc=%u", pl[1], pl[2], (unsigned)pl[0]);
        else ESP_LOGW(TAG, "DIAG hold[0x0000] fail rn=%u exc=%d", (unsigned)rn, exc);
        rl = mb_build_read_holding(req, BDKG05_DEFAULT_ADDR, 0x000A, 2);
        if (txn(NULL, req, rl, resp, sizeof(resp), &rn) == 0 &&
            mb_parse_response(resp, rn, MB_FC_READ_HOLDING, pl, sizeof(pl), &pn, &exc) == MB_OK) {
            union { uint32_t u; float f; } c;
            c.u = ((uint32_t)pl[1]<<24)|((uint32_t)pl[2]<<16)|((uint32_t)pl[3]<<8)|pl[4];
            ESP_LOGI(TAG, "DIAG bg_med[0x000A]=%.4e raw=%02x%02x%02x%02x", c.f, pl[1], pl[2], pl[3], pl[4]);
        } else ESP_LOGW(TAG, "DIAG bg_med fail rn=%u exc=%d", (unsigned)rn, exc);
        // DIAG H4: сырьё FC04 0x0004 (рабочий CPS=96) vs 0x0008 (нулевой МЭД).
        // Тот же FC/длина/транспорт, соседние адреса — сравнить кадры на границе.
        for (uint16_t reg = 0x0004; reg <= 0x0008; reg += 4) {
            rl = mb_build_read_input(req, BDKG05_DEFAULT_ADDR, reg, 2);
            rn = 0;
            int io = txn(NULL, req, rl, resp, sizeof(resp), &rn);
            ESP_LOGI(TAG, "DIAG raw FC04[0x%04x] io=%d rn=%u : %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     reg, io, (unsigned)rn, resp[0], resp[1], resp[2], resp[3],
                     resp[4], resp[5], resp[6], resp[7], resp[8]);
        }
    }

    // E4: непрерывный опрос 1 Гц. Каждый успешный отсчёт публикуется в snapshot
    // (web /api/bdkg). Логи прорежены — INFO раз в 30 с, WARN на каждый сбой.
    unsigned n = 0;
    while (1) {
        // #BDKG-16 «Сброс замера»: переинициализация прибора (MODE+START+LATCH)
        // ДО чтения — сбрасывает усреднение/HV на приборе.
        if (s_reset_req) {
            s_reset_req = false;
            bdkg05_status_t rs = bdkg05_init(&h);
            ESP_LOGI(TAG, "reset: re-init -> %d", rs);
        }
        bdkg05_reading_t r;
        s = bdkg05_read(&h, &r);
        if (s == BDKG05_OK && r.valid) {
            snap_publish(&r);
            // #BDKG-17: записать точку истории под тем же мьютексом.
            if (s_snap_mtx) {
                xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
                s_hist[s_hist_idx].t_unix   = (int64_t)time(NULL);
                s_hist[s_hist_idx].med_sv_h = r.med_sv_h;
                s_hist[s_hist_idx].cps_avg  = r.cps_avg;
                s_hist[s_hist_idx].cps_inst = r.cps_inst;  // #BDKG-28 сырое ЦПС
                s_hist[s_hist_idx].temp_c   = r.temp_c;
                s_hist_idx = (s_hist_idx + 1) % BDKG_HIST_MAX;
                if (s_hist_n < BDKG_HIST_MAX) s_hist_n++;
                xSemaphoreGive(s_snap_mtx);
            }
            if (n % 30 == 0)
                ESP_LOGI(TAG, "MED=%.3f uSv/h CPS=%.1f Ninst=%u T=%.1fC dose=%.3f uSv",
                         r.med_sv_h * 1e6, r.cps_avg, (unsigned)r.cps_inst, r.temp_c, r.dose_sv * 1e6);
        } else {
            ESP_LOGW(TAG, "read failed: %d", s);
        }
        n++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void bdkg_usb_init(void) {
    if (s_inited) return;   // main.c: headless-before-WiFi (стр.42) + после spectrum_init (стр.77)
    s_inited = true;
    if (!s_snap_mtx) s_snap_mtx = xSemaphoreCreateMutex();
    s_rx = xStreamBufferCreate(1024, 1);
    const usb_host_config_t hc = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = enum_cb,
    };
    if (usb_host_install(&hc) != ESP_OK) { ESP_LOGE(TAG, "usb_host_install failed"); return; }
    xTaskCreatePinnedToCore(lib_task, "bdkg_usblib", 4096, NULL, 7, NULL, 0);
    const cdc_acm_host_driver_config_t dc = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 8,
        .xCoreID = 0,
    };
    if (cdc_acm_host_install(&dc) != ESP_OK) { ESP_LOGE(TAG, "cdc_acm_host_install failed"); return; }
    xTaskCreatePinnedToCore(poll_task, "bdkg_poll", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "bdkg_usb init done, waiting for adapter...");
}

bool bdkg_usb_is_connected(void) { return s_dev != NULL; }

// E4: publish latest reading (poll_task side).
static void snap_publish(const bdkg05_reading_t *r) {
    if (!s_snap_mtx) return;
    xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
    s_snap.med_sv_h  = r->med_sv_h;
    s_snap.med_err_pct = r->med_err_pct;
    s_snap.cps_avg   = r->cps_avg;
    s_snap.cps_err_pct = r->cps_err_pct;
    s_snap.cps_inst  = r->cps_inst;
    s_snap.temp_c    = r->temp_c;
    s_snap.dose_sv   = r->dose_sv;
    s_snap.valid     = r->valid;
    s_snap.sample_us = esp_timer_get_time();
    xSemaphoreGive(s_snap_mtx);
}

bool bdkg_usb_get_latest(bdkg_snapshot_t *out) {
    if (!out) return false;
    if (!s_snap_mtx) { out->valid = false; return false; }
    xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_snap_mtx);
    return out->valid;
}

// #BDKG-17: копия кольца истории в хронологическом порядке (старые→новые).
// Если max < числа накопленных точек — отдаём max НОВЕЙШИХ.
size_t bdkg_usb_get_history(bdkg_hist_point_t *out, size_t max) {
    if (!out || max == 0 || !s_snap_mtx) return 0;
    xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
    size_t n = s_hist_n;
    if (n > max) n = max;
    size_t start = (s_hist_n < BDKG_HIST_MAX) ? 0 : s_hist_idx;  // индекс старейшей точки
    size_t skip  = s_hist_n - n;                                 // сколько старейших отбросить
    for (size_t i = 0; i < n; i++)
        out[i] = s_hist[(start + skip + i) % BDKG_HIST_MAX];
    xSemaphoreGive(s_snap_mtx);
    return n;
}

