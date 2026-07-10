// bdkg_log.c — ESP-IDF компонент записи БДКГ-05 в КОЛЬЦЕВОЙ бинарный буфер на LittleFS.
// #BDKG-41: посуточный CSV заменён единым кольцом на флеше (перезапись старейших).
//   Формат записи 16 байт (packed): uint32 t_unix, float med_sv_h, float cps_inst, float temp_c.
//   Ёмкость BDKG_RING_CAP отсчётов @1 Гц (~6 суток). Пишется батчами (износ флеша).
//   Скачивание — ОДИН файл: вся история хронологически в CSV (GET /api/bdkg/log).
//   Boot-preload: последние BDKG_HIST_MAX точек восстанавливаются в RAM-кольцо графика.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "bdkg_log.h"
#include "bdkg_usb.h"

#define BDKG_LOG_DIR        "/storage/bdkg"
#define BDKG_RING_PATH      BDKG_LOG_DIR "/ring.bin"
#define BDKG_HDR_PATH       BDKG_LOG_DIR "/ring.hdr"
#define BDKG_LOG_PERIOD_S   1                       // #BDKG-41: сырьё 1 Гц, без прореживания
#define BDKG_RING_CAP       518400u                 // 6 сут × 86400 с (@1 Гц) = 8.29 МБ на флеше
#define BDKG_RING_BATCH     30                       // флеш раз в ~30 с (RAM-стейджинг до сброса)
#define BDKG_HDR_MAGIC      0x42524E47u              // 'BRNG'
#define BDKG_HDR_VER        1u

// Одна запись кольца (16 байт). Единицы — СИ (как в bdkg_hist_point_t), CSV домножает на 1e6.
typedef struct __attribute__((packed)) {
    uint32_t t;         // unix seconds
    float    med_sv_h;  // Sv/h
    float    cps_inst;  // counts/s (сырое)
    float    temp_c;    // C
} bdkg_rec_t;

// Заголовок состояния кольца (persist в BDKG_HDR_PATH).
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t ver;
    uint32_t count;     // сколько записей реально в кольце (0..CAP)
    uint32_t widx;      // индекс СЛЕДУЮЩЕЙ записи (0..CAP-1)
} ring_hdr_t;

static const char *TAG = "bdkg_log";
static bool started = false;
// #BDKG-16: запись включается кнопкой Старт (main.c ставит из boot_config).
static volatile bool s_enabled = false;

// Состояние кольца в RAM + сериализация доступа (writer=logger_task, readers=download/preload).
static SemaphoreHandle_t s_ring_mtx = NULL;
static uint32_t s_count = 0, s_widx = 0;
static bdkg_rec_t s_stage[BDKG_RING_BATCH];
static size_t s_nstage = 0;
static int64_t s_last_sample_us = -1;   // дедуп: не писать повтор того же отсчёта

// CSRF-проверка экспортируется из web_server.c.
extern bool web_csrf_check(httpd_req_t *req);

static void ensure_dir(void) { mkdir(BDKG_LOG_DIR, 0777); }

static bool time_is_valid(struct tm *out_lt) {
    time_t now = time(NULL);
    localtime_r(&now, out_lt);
    return (out_lt->tm_year + 1900 >= 2024);
}

// ---- Заголовок ----
static void hdr_save_locked(void) {
    ring_hdr_t h = { BDKG_HDR_MAGIC, BDKG_HDR_VER, s_count, s_widx };
    FILE *f = fopen(BDKG_HDR_PATH, "wb");
    if (!f) { ESP_LOGW(TAG, "hdr open fail"); return; }
    fwrite(&h, sizeof(h), 1, f);
    fclose(f);
}

static void hdr_load(void) {
    ring_hdr_t h;
    FILE *f = fopen(BDKG_HDR_PATH, "rb");
    if (f && fread(&h, sizeof(h), 1, f) == 1 &&
        h.magic == BDKG_HDR_MAGIC && h.ver == BDKG_HDR_VER &&
        h.count <= BDKG_RING_CAP && h.widx < BDKG_RING_CAP) {
        s_count = h.count;
        s_widx  = h.widx;
        ESP_LOGI(TAG, "ring loaded: count=%u widx=%u", (unsigned)s_count, (unsigned)s_widx);
    } else {
        s_count = 0; s_widx = 0;
        ESP_LOGW(TAG, "ring hdr missing/invalid -> empty");
    }
    if (f) fclose(f);
}

// ---- Сброс стейджинга на флеш (вызывать под s_ring_mtx) ----
static void flush_locked(void) {
    if (s_nstage == 0) return;
    FILE *f = fopen(BDKG_RING_PATH, "r+b");
    if (!f) f = fopen(BDKG_RING_PATH, "w+b");   // первый раз — создать
    if (!f) { ESP_LOGW(TAG, "ring open fail"); s_nstage = 0; return; }

    for (size_t i = 0; i < s_nstage; i++) {
        // Запись всегда в позицию s_widx (монотонно растёт, оборачивается на CAP).
        // fseek за EOF при росте кольца — LittleFS расширяет файл нулями, это ок.
        if (fseek(f, (long)s_widx * (long)sizeof(bdkg_rec_t), SEEK_SET) != 0) break;
        if (fwrite(&s_stage[i], sizeof(bdkg_rec_t), 1, f) != 1) break;
        s_widx = (s_widx + 1) % BDKG_RING_CAP;
        if (s_count < BDKG_RING_CAP) s_count++;
    }
    fclose(f);
    s_nstage = 0;
    hdr_save_locked();
}

static void ring_append(const bdkg_rec_t *rec) {
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    s_stage[s_nstage++] = *rec;
    if (s_nstage >= BDKG_RING_BATCH) flush_locked();
    xSemaphoreGive(s_ring_mtx);
}

// ---- Logger task: 1 Гц, пишет только когда enabled И время валидно И отсчёт новый ----
static void logger_task(void *arg) {
    ensure_dir();
    while (1) {
        struct tm lt;
        if (s_enabled && time_is_valid(&lt)) {
            bdkg_snapshot_t s;
            if (bdkg_usb_get_latest(&s) && s.valid && s.sample_us != s_last_sample_us) {
                s_last_sample_us = s.sample_us;
                bdkg_rec_t rec = {
                    .t = (uint32_t)time(NULL),
                    .med_sv_h = s.med_sv_h,
                    .cps_inst = s.cps_inst,
                    .temp_c   = s.temp_c,
                };
                ring_append(&rec);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BDKG_LOG_PERIOD_S * 1000));
    }
}

// ---- Чтение N последних записей (хронологически) в out. Возврат = сколько прочитано. ----
// Учитывает не-сброшенный стейджинг (последние s_nstage — самые свежие).
static size_t ring_read_tail(bdkg_rec_t *out, size_t want) {
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    size_t total = s_count + s_nstage;
    size_t n = (want < total) ? want : total;
    size_t skip = total - n;             // сколько старейших пропустить
    // Логическая последовательность: [ flash 0..s_count ) затем [ stage 0..s_nstage )
    // flash-запись логического индекса k лежит в слоте (oldest + k) % CAP.
    uint32_t oldest = (s_count < BDKG_RING_CAP) ? 0 : s_widx;
    FILE *f = NULL;
    size_t got = 0;
    for (size_t k = skip; k < total && got < n; k++) {
        if (k < s_count) {
            if (!f) f = fopen(BDKG_RING_PATH, "rb");
            if (!f) break;
            uint32_t slot = (oldest + k) % BDKG_RING_CAP;
            if (fseek(f, (long)slot * (long)sizeof(bdkg_rec_t), SEEK_SET) != 0) break;
            if (fread(&out[got], sizeof(bdkg_rec_t), 1, f) != 1) break;
        } else {
            out[got] = s_stage[k - s_count];
        }
        got++;
    }
    if (f) fclose(f);
    xSemaphoreGive(s_ring_mtx);
    return got;
}

void bdkg_log_init(void) {
    if (started) return;
    started = true;
    if (!s_ring_mtx) s_ring_mtx = xSemaphoreCreateMutex();
    ensure_dir();
    hdr_load();
    xTaskCreatePinnedToCore(logger_task, "bdkg_log", 4096, NULL, 3, NULL, 0);
}

// #BDKG-41: восстановить RAM-кольцо графика последними точками с флеша (после ребута).
void bdkg_log_preload(void) {
    if (!s_ring_mtx) return;
    size_t cap = bdkg_usb_hist_capacity();
    bdkg_rec_t *tail = malloc(sizeof(bdkg_rec_t) * cap);
    if (!tail) { ESP_LOGW(TAG, "preload OOM"); return; }
    size_t n = ring_read_tail(tail, cap);
    if (n == 0) { free(tail); return; }
    bdkg_hist_point_t *pts = malloc(sizeof(bdkg_hist_point_t) * n);
    if (!pts) { free(tail); ESP_LOGW(TAG, "preload OOM2"); return; }
    for (size_t i = 0; i < n; i++) {
        pts[i].t_unix   = (int64_t)tail[i].t;
        pts[i].med_sv_h = tail[i].med_sv_h;
        pts[i].cps_avg  = tail[i].cps_inst;   // усреднённого в кольце нет — дублируем сырое
        pts[i].cps_inst = tail[i].cps_inst;
        pts[i].temp_c   = tail[i].temp_c;
    }
    bdkg_usb_seed_history(pts, n);
    ESP_LOGI(TAG, "preload: seeded %u points", (unsigned)n);
    free(pts); free(tail);
}

void bdkg_log_set_enabled(bool on) {
    s_enabled = on;
    ESP_LOGI(TAG, "recording %s", on ? "ON" : "OFF");
}

bool bdkg_log_is_enabled(void) { return s_enabled; }

// #BDKG-41: полная очистка кольца (удалить оба файла, обнулить состояние). Звать до init.
void bdkg_log_clear_storage(void) {
    unlink(BDKG_RING_PATH);
    unlink(BDKG_HDR_PATH);
    // Легаси: старые посуточные CSV, если остались от прежней прошивки.
    DIR *d = opendir(BDKG_LOG_DIR);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t l = strlen(de->d_name);
            if (l >= 4 && strcmp(&de->d_name[l - 4], ".csv") == 0) {
                char full[300];
                snprintf(full, sizeof(full), "%s/%s", BDKG_LOG_DIR, de->d_name);
                unlink(full);
            }
        }
        closedir(d);
    }
    s_count = 0; s_widx = 0; s_nstage = 0;
    ESP_LOGW(TAG, "ring cleared");
}

// ---- HTTP: инфо о кольце (JSON) ----
static esp_err_t h_logs(httpd_req_t *req) {
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    uint32_t count = s_count + s_nstage;
    xSemaphoreGive(s_ring_mtx);
    uint32_t newest_t = 0;
    bdkg_rec_t edge;
    if (count && ring_read_tail(&edge, 1) == 1) newest_t = edge.t;
    char buf[192];
    int n = snprintf(buf, sizeof(buf),
        "{\"count\":%u,\"cap\":%u,\"bytes\":%u,\"newest\":%u}",
        (unsigned)count, (unsigned)BDKG_RING_CAP,
        (unsigned)(count * (uint32_t)sizeof(bdkg_rec_t)),
        (unsigned)newest_t);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

// ---- HTTP: скачать ВСЮ историю одним CSV-файлом ----
static esp_err_t h_download(httpd_req_t *req) {
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    if (s_nstage) flush_locked();                 // сбросить хвост, чтобы попал в файл
    uint32_t count = s_count;
    uint32_t oldest = (s_count < BDKG_RING_CAP) ? 0 : s_widx;
    xSemaphoreGive(s_ring_mtx);

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"bdkg_history.csv\"");
    httpd_resp_sendstr_chunk(req, "iso_time,med_uSv_h,cps_inst,temp_c\n");

    if (count == 0) { httpd_resp_send_chunk(req, NULL, 0); return ESP_OK; }

    FILE *f = fopen(BDKG_RING_PATH, "rb");
    if (!f) { httpd_resp_send_chunk(req, NULL, 0); return ESP_OK; }

    char chunk[2048];
    size_t p = 0;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t slot = (oldest + k) % BDKG_RING_CAP;
        bdkg_rec_t r;
        if (fseek(f, (long)slot * (long)sizeof(bdkg_rec_t), SEEK_SET) != 0) break;
        if (fread(&r, sizeof(r), 1, f) != 1) break;
        struct tm lt;
        time_t tt = (time_t)r.t;
        localtime_r(&tt, &lt);
        char iso[24];
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &lt);
        p += snprintf(chunk + p, sizeof(chunk) - p, "%s,%.4f,%.2f,%.2f\n",
                      iso, r.med_sv_h * 1e6, r.cps_inst, r.temp_c);
        if (p > sizeof(chunk) - 96) {
            if (httpd_resp_send_chunk(req, chunk, p) != ESP_OK) { fclose(f); return ESP_FAIL; }
            p = 0;
        }
    }
    if (p) httpd_resp_send_chunk(req, chunk, p);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// #BDKG-16: Старт/Стоп записи (CSRF-защищённые POST).
static esp_err_t h_log_start(httpd_req_t *req) {
    if (!web_csrf_check(req)) return ESP_FAIL;
    bdkg_log_set_enabled(true);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_log_stop(httpd_req_t *req) {
    if (!web_csrf_check(req)) return ESP_FAIL;
    bdkg_log_set_enabled(false);
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    flush_locked();                               // сбросить незаписанный хвост
    xSemaphoreGive(s_ring_mtx);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

void bdkg_log_register(httpd_handle_t server) {
    httpd_uri_t u_logs = { .uri="/api/bdkg/logs", .method=HTTP_GET, .handler=h_logs, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u_logs);
    httpd_uri_t u_dl = { .uri="/api/bdkg/log", .method=HTTP_GET, .handler=h_download, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u_dl);
    httpd_uri_t u_start = { .uri="/api/bdkg/log/start", .method=HTTP_POST, .handler=h_log_start, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u_start);
    httpd_uri_t u_stop = { .uri="/api/bdkg/log/stop", .method=HTTP_POST, .handler=h_log_stop, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u_stop);
}