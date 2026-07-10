#include "boot_config.h"
#include "bdkg_usb.h"   // E4: live snapshot getter for /api/bdkg
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <stddef.h>
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_app_desc.h"   // #FW-28: esp_app_get_description() -> версия прошивки
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_littlefs.h"
#include "esp_random.h"
#include "bdkg_log.h"   // E5 (#BDKG-5): регистрация хендлеров логов БДКГ-05
#include "bdkg_mqtt.h"   // E7 (#BDKG-7): конфиг/статус MQTT-паблишера
#include "tz_config.h"   // #BDKG-21: get/set часового пояса

static const char *TAG = "web";
static char s_reg_report[220] = "ok";  // E4 diag: URI-registration failures, echoed by /healthcheck

// CSRF-токен: генерируется при старте, выдаётся по GET /api/csrf-token,
// требуется в заголовке X-CSRF-Token на всех мутирующих POST. Защищает
// открытый-в-LAN Web UI от drive-by cross-origin POST: сторонняя страница
// не может прочитать токен (same-origin policy) → не может подделать запрос.
static char s_csrf[33];

static void csrf_generate(void)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++)
        s_csrf[i] = hx[esp_random() & 0x0F];
    s_csrf[32] = '\0';
}

// P3-9: сравнение токена за постоянное время — не утекает позиция
// первого несовпавшего байта по времени ответа.
static bool csrf_ct_eq(const char *a, const char *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)a[i] ^ (uint8_t)b[i];
    return d == 0;
}

// Проверяет X-CSRF-Token. При несовпадении сам шлёт 403 и возвращает false.
static bool csrf_check(httpd_req_t *req)
{
    char hdr[40] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-CSRF-Token", hdr, sizeof(hdr)) == ESP_OK
        && s_csrf[0] && strlen(hdr) == 32 && csrf_ct_eq(hdr, s_csrf, 32))
        return true;
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Bad or missing CSRF token");
    return false;
}

// Экспорт CSRF-проверки для bdkg_log.c (csrf_check здесь static).
bool web_csrf_check(httpd_req_t *req)
{
    return csrf_check(req);
}

static esp_err_t handle_csrf_token(httpd_req_t *req)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "{\"token\":\"%s\"}", s_csrf);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t handle_boot_config_get(httpd_req_t *req) {
    boot_config_t bc;
    boot_config_load(&bc);

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"autostart_bdkg_log\":%s,\"clear_bdkg_storage\":%s}",
             bc.autostart_bdkg_log ? "true" : "false",
             bc.clear_bdkg_storage ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t handle_boot_config_set(httpd_req_t *req) {
    if (!csrf_check(req)) return ESP_FAIL;

    char body[256];
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[ret] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }

    boot_config_t bc;
    boot_config_load(&bc);

    cJSON *item = cJSON_GetObjectItem(root, "autostart_bdkg_log");
    if (item) bc.autostart_bdkg_log = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "clear_bdkg_storage");
    if (item) bc.clear_bdkg_storage = cJSON_IsTrue(item);

    cJSON_Delete(root);

    int save_result = boot_config_save(&bc);
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":%s}", save_result == 0 ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

#define EMBED_HTML_HANDLER(fn,sym) static esp_err_t fn(httpd_req_t *req){ \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_end"); \
    httpd_resp_set_type(req,"text/html"); \
    httpd_resp_send(req,(const char *)sym##_start, sym##_end - sym##_start); \
    return ESP_OK; }
EMBED_HTML_HANDLER(handle_bdkg_page,    bdkg_html)
EMBED_HTML_HANDLER(handle_bdkg_system_page, bdkg_system_html)


// E4: live БДКГ-05 reading as JSON, polled by /bdkg page at 1 Hz. NEVER emits
// the serial number. connected = USB adapter present; valid = fresh reading.
static esp_err_t handle_bdkg_json(httpd_req_t *req)
{
    bdkg_snapshot_t s;
    bdkg_usb_get_latest(&s);
    char buf[288];
    int n = snprintf(buf, sizeof(buf),
        "{\"connected\":%s,\"valid\":%s,\"recording\":%s,\"med_usv_h\":%.4f,\"med_err\":%.2f,"
        "\"cps\":%.2f,\"cps_err\":%.2f,\"cps_inst\":%.0f,\"temp_c\":%.2f,\"dose_usv\":%.4f}",
        bdkg_usb_is_connected() ? "true" : "false",
        s.valid ? "true" : "false",
        bdkg_log_is_enabled() ? "true" : "false",
        s.med_sv_h * 1e6, s.med_err_pct, s.cps_avg, s.cps_err_pct, s.cps_inst, s.temp_c, s.dose_sv * 1e6);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

// #BDKG-17: история последних 300 отсчётов с платы (JSON). Буфер в куче, НЕ на стеке.
static esp_err_t handle_bdkg_history_json(httpd_req_t *req)
{
    bdkg_hist_point_t *pts = malloc(sizeof(bdkg_hist_point_t) * 300);
    if (!pts) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    size_t np = bdkg_usb_get_history(pts, 300);
    size_t cap = 16384;
    char *out = malloc(cap);
    if (!out) { free(pts); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    size_t p = 0;
    p += snprintf(out + p, cap - p, "{\"points\":[");
    for (size_t i = 0; i < np && p < cap - 96; i++) {
        p += snprintf(out + p, cap - p,
            "%s{\"t\":%lld,\"med\":%.4f,\"cps\":%.2f,\"temp\":%.2f}",
            i ? "," : "", (long long)pts[i].t_unix,
            pts[i].med_sv_h * 1e6, pts[i].cps_inst, pts[i].temp_c);  // #BDKG-28 график = сырое ЦПС
    }
    p += snprintf(out + p, cap - p, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, p);
    free(out); free(pts);
    return ESP_OK;
}
// #BDKG-16 «Сброс замера»: асинхронно триггерит реинициализацию прибора.
static esp_err_t handle_bdkg_reset(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    bdkg_usb_request_reset();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
// #BDKG-32: GET конфига MQTT-паблишера. НИКОГДА не отдаёт пароль — только has_pass.
static esp_err_t handle_bdkg_mqtt_get(httpd_req_t *req)
{
    bdkg_mqtt_cfg_t c;
    bdkg_mqtt_get_cfg(&c);
    bdkg_mqtt_stat_t st;
    bdkg_mqtt_get_stat(&st);
    char resp[320];
    snprintf(resp, sizeof(resp),
        "{\"enabled\":%s,\"uri\":\"%s\",\"user\":\"%s\",\"has_pass\":%s,"
        "\"connected\":%s,\"published\":%" PRIu32 ",\"last_pub\":%lld}",
        c.enabled ? "true" : "false",
        c.uri, c.user,
        c.pass[0] ? "true" : "false",
        st.connected ? "true" : "false",
        st.published, (long long)st.last_pub_at);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}
// #BDKG-32: POST конфига MQTT. Пустой pass = сохранить прежний (preserve из NVS).
static esp_err_t handle_bdkg_mqtt_set(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[256] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    bdkg_mqtt_cfg_t c;
    bdkg_mqtt_get_cfg(&c);   // стартуем от текущего, перекрываем переданными ключами
    cJSON *it;
    if ((it = cJSON_GetObjectItem(root, "enabled"))) c.enabled = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "uri")) && cJSON_IsString(it))  snprintf(c.uri,  sizeof(c.uri),  "%s", it->valuestring);
    if ((it = cJSON_GetObjectItem(root, "user")) && cJSON_IsString(it)) snprintf(c.user, sizeof(c.user), "%s", it->valuestring);
    // пароль: перезаписываем ТОЛЬКО если пришёл непустой (пустой = оставить прежний из NVS)
    if ((it = cJSON_GetObjectItem(root, "pass")) && cJSON_IsString(it) && it->valuestring[0]) snprintf(c.pass, sizeof(c.pass), "%s", it->valuestring);
    cJSON_Delete(root);
    int rc = bdkg_mqtt_set_cfg(&c);
    httpd_resp_set_type(req, "application/json");
    char resp[48];
    if (rc == BDKG_MQTT_OK) snprintf(resp, sizeof(resp), "{\"ok\":true}");
    else                    snprintf(resp, sizeof(resp), "{\"ok\":false,\"err\":%d}", rc);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}
// #BDKG-21: текущий часовой пояс (POSIX TZ-строка).
static esp_err_t handle_tz_get(httpd_req_t *req)
{
    char tz[16];
    tz_config_load(tz, sizeof(tz));
    char resp[48];
    int n = snprintf(resp, sizeof(resp), "{\"tz\":\"%s\"}", tz);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, n);
    return ESP_OK;
}
// #BDKG-21: сохранить TZ и применить сразу (setenv+tzset, без ребута).
static esp_err_t handle_tz_set(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[64] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    body[recv_len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON"); return ESP_FAIL; }
    cJSON *it = cJSON_GetObjectItem(root, "tz");
    int rc = -1;
    if (it && cJSON_IsString(it) && it->valuestring) {
        rc = tz_config_save(it->valuestring);
        if (rc == 0) { setenv("TZ", it->valuestring, 1); tzset(); }
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_root_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/bdkg");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_system(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "fw_version", app_desc ? app_desc->version : "?");

    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", (double)esp_get_minimum_free_heap_size());

    int64_t uptime_us = esp_timer_get_time();
    cJSON_AddNumberToObject(root, "uptime_sec", (double)(uptime_us / 1000000));

    cJSON_AddBoolToObject(root, "bdkg_connected", bdkg_usb_is_connected());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());

    size_t total, used;
    if (esp_littlefs_info("storage", &total, &used) == ESP_OK) {
        cJSON_AddNumberToObject(root, "flash_total", (double)total);
        cJSON_AddNumberToObject(root, "flash_used", (double)used);
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(root, "rssi", (double)ap.rssi);
        cJSON_AddStringToObject(root, "ssid", (char*)ap.ssid);
    }

    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);

    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_reboot_esp(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_healthcheck(httpd_req_t *req) {
    bool bdkg = bdkg_usb_is_connected();
    bool wifi = wifi_is_connected();
    int64_t uptime_us = esp_timer_get_time();

    const char *status_str = (bdkg && wifi) ? "200 OK" : "503 Service Unavailable";
    httpd_resp_set_status(req, status_str);

    httpd_resp_set_type(req, "application/json");

    char buf[640];
    snprintf(buf, sizeof(buf), "{\"status\":\"%s\",\"bdkg_connected\":%s,\"wifi_connected\":%s,\"uptime_sec\":%.3f,\"reg\":\"%s\"}",
             (bdkg && wifi) ? "ok" : "degraded",
             bdkg ? "true" : "false",
             wifi ? "true" : "false",
             (double)(uptime_us / 1000000.0),
             s_reg_report);

    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t handle_wifi_reset(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

void web_server_init(void)
{
    csrf_generate();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // #WF-1: httpd на core 1 к остальной прикладной сети (#TCP-2). Дефолт
    // tskNO_AFFINITY позволял httpd (prio 5) исполняться на core 0 рядом с
    // USB-приёмом — уводим целиком.
    config.core_id = 1;
    config.max_uri_handlers = 30;        // 17 uris[] + 4 bdkg_log = 21. 30 = запас.
    config.stack_size = 8192;
    config.max_open_sockets = 11;        // из 16 LWIP-сокетов; запас для sntp
    config.lru_purge_enable = true;      // при исчерпании пула закрыть LRU-соединение, не отказывать (errno 23)
    config.uri_match_fn = httpd_uri_match_wildcard;
    // #UI-15 P2: сжимаем default recv/send (~5 c) — освобождаем сокеты быстрее
    // под давлением F5+poll, иначе пул держит «полудохлые» соединения долго.
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    const httpd_uri_t uris[] = {
        {"/api/csrf-token",              HTTP_GET,  handle_csrf_token,       NULL},
        {"/api/boot-config",             HTTP_GET,  handle_boot_config_get,  NULL},
        {"/api/boot-config",             HTTP_POST, handle_boot_config_set,  NULL},
        {"/api/system",                  HTTP_GET,  handle_system,           NULL},
        {"/api/wifi/reset",              HTTP_POST, handle_wifi_reset,       NULL},
        {"/api/reboot-esp",              HTTP_POST, handle_reboot_esp,       NULL},
        {"/healthcheck",                 HTTP_GET,  handle_healthcheck,      NULL},
        {"/api/bdkg",                    HTTP_GET,  handle_bdkg_json,        NULL},
        {"/api/bdkg/history",            HTTP_GET,  handle_bdkg_history_json,NULL},
        {"/api/bdkg/reset",              HTTP_POST, handle_bdkg_reset,       NULL},
        {"/api/bdkg/mqtt",               HTTP_GET,  handle_bdkg_mqtt_get,    NULL},
        {"/api/bdkg/mqtt",               HTTP_POST, handle_bdkg_mqtt_set,    NULL},
        {"/api/tz",                      HTTP_GET,  handle_tz_get,           NULL},
        {"/api/tz",                      HTTP_POST, handle_tz_set,           NULL},
        {"/bdkg",                        HTTP_GET,  handle_bdkg_page,        NULL},
        {"/bdkg/system",                 HTTP_GET,  handle_bdkg_system_page, NULL},
        {"/",                            HTTP_GET,  handle_root_redirect,    NULL},
    };

    size_t rp = 0;
    s_reg_report[0] = '\0';
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t rr = httpd_register_uri_handler(server, &uris[i]);
        if (rr != ESP_OK) {
            ESP_LOGE(TAG, "REG FAIL [%u] %s -> %s", (unsigned)i, uris[i].uri, esp_err_to_name(rr));
            rp += snprintf(s_reg_report + rp, sizeof(s_reg_report) - rp,
                           "[%u]%s=%s ", (unsigned)i, uris[i].uri, esp_err_to_name(rr));
            if (rp >= sizeof(s_reg_report)) rp = sizeof(s_reg_report) - 1;
        }
    }
    if (s_reg_report[0] == '\0') snprintf(s_reg_report, sizeof(s_reg_report), "ok");

    // E5 (#BDKG-5): хендлеры логов БДКГ-05 (/api/bdkg/logs, /api/bdkg/log?date=YYYY-MM-DD)
    bdkg_log_register(server);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
}