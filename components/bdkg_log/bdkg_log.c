// bdkg_log.c — ESP-IDF компонент логирования БДКГ-05 в CSV на LittleFS
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "bdkg_log.h"
#include "bdkg_usb.h"

#define BDKG_LOG_DIR        "/storage/bdkg"
#define BDKG_LOG_PERIOD_S   10
#define BDKG_LOG_KEEP_DAYS  7
#define BDKG_LOG_MAX_BYTES  (4*1024*1024)
#define BDKG_LOG_CSV_HEADER "iso_time,med_uSv_h,cps_avg,cps_inst,temp_c,dose_uSv\n"

static const char *TAG = "bdkg_log";
static bool started = false;
// #BDKG-16: запись включается кнопкой Старт (main.c ставит из boot_config при
// старте; дефолт false — из коробки лог не растёт до явного Старта).
static volatile bool s_enabled = false;

// CSRF-проверка экспортируется из web_server.c (там csrf_check static).
extern bool web_csrf_check(httpd_req_t *req);

static void rotate(void);  // forward decl: используется в append_row до определения

static bool time_is_valid(struct tm *out_lt) {
    time_t now = time(NULL);
    localtime_r(&now, out_lt);
    return (out_lt->tm_year + 1900 >= 2024);
}

static void ensure_dir(void) {
    mkdir(BDKG_LOG_DIR, 0777);
}

static bool valid_date(const char *s) {
    if (strlen(s) != 10) return false;
    if (s[4] != '-' || s[7] != '-') return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

static void append_row(const struct tm *lt, const bdkg_snapshot_t *s) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%04d-%02d-%02d.csv", BDKG_LOG_DIR,
             lt->tm_year+1900, lt->tm_mon+1, lt->tm_mday);
    
    struct stat st;
    bool is_new = (stat(path,&st) != 0);
    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGW(TAG,"open fail %s", path);
        return;
    }
    
    if (is_new) {
        fputs(BDKG_LOG_CSV_HEADER, f);
    }
    
    char iso[24];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", lt);
    fprintf(f, "%s,%.4f,%.2f,%.2f,%.2f,%.4f\n", iso, s->med_sv_h*1e6, s->cps_avg, s->cps_inst,
            s->temp_c, s->dose_sv*1e6);
    fclose(f);
    
    if (is_new) {
        rotate();
    }
}

static void rotate(void) {
    DIR *d = opendir(BDKG_LOG_DIR);
    if (!d) return;
    
    char names[64][16];
    int count = 0;
    struct dirent *de;
    
    while ((de = readdir(d)) != NULL && count < 64) {
        if (strlen(de->d_name) == 14 && strcmp(&de->d_name[strlen(de->d_name)-4], ".csv") == 0) {
            strncpy(names[count], de->d_name, 15);
            names[count][15] = '\0';
            count++;
        }
    }
    closedir(d);
    
    if (count <= BDKG_LOG_KEEP_DAYS) return;
    
    // Сортировка по имени
    for (int i = 1; i < count; i++) {
        char *key = names[i];
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            strcpy(names[j+1], names[j]);
            j--;
        }
        strcpy(names[j+1], key);
    }
    
    // Удаление старых файлов по количеству дней
    while (count > BDKG_LOG_KEEP_DAYS) {
        char fullpath[64];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", BDKG_LOG_DIR, names[0]);
        unlink(fullpath);
        ESP_LOGI(TAG, "rotate: unlink %s", names[0]);
        count--;
        for (int i = 0; i < count; i++) {
            strcpy(names[i], names[i+1]);
        }
    }
    
    // Удаление по размеру
    int64_t total_size = 0;
    for (int i = 0; i < count; i++) {
        char fullpath[64];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", BDKG_LOG_DIR, names[i]);
        struct stat st;
        if (stat(fullpath, &st) == 0) {
            total_size += st.st_size;
        }
    }
    
    while (total_size > BDKG_LOG_MAX_BYTES && count >= 2) {
        char fullpath[64];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", BDKG_LOG_DIR, names[0]);
        struct stat st;
        if (stat(fullpath, &st) == 0) {
            total_size -= st.st_size;
        }
        unlink(fullpath);
        ESP_LOGI(TAG, "rotate: unlink %s", names[0]);
        count--;
        for (int i = 0; i < count; i++) {
            strcpy(names[i], names[i+1]);
        }
    }
}

static void logger_task(void *arg) {
    ensure_dir();
    while (1) {
        struct tm lt;
        // #BDKG-16: цикл живёт всегда, запись — только когда включено И время валидно.
        if (s_enabled && time_is_valid(&lt)) {
            bdkg_snapshot_t s;
            if (bdkg_usb_get_latest(&s) && s.valid) {
                append_row(&lt, &s);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BDKG_LOG_PERIOD_S * 1000));
    }
}

void bdkg_log_init(void) {
    if (started) return;
    started = true;
    xTaskCreatePinnedToCore(logger_task, "bdkg_log", 4096, NULL, 3, NULL, 0);
}

// #BDKG-16: управление записью CSV.
void bdkg_log_set_enabled(bool on) {
    s_enabled = on;
    ESP_LOGI(TAG, "recording %s", on ? "ON" : "OFF");
}

bool bdkg_log_is_enabled(void) { return s_enabled; }

// #BDKG-19: удалить все CSV-файлы архива БДКГ-05 (полная очистка каталога).
void bdkg_log_clear_storage(void) {
    DIR *d = opendir(BDKG_LOG_DIR);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l >= 4 && strcmp(&de->d_name[l - 4], ".csv") == 0) {
            char full[300];
            snprintf(full, sizeof(full), "%s/%s", BDKG_LOG_DIR, de->d_name);
            unlink(full);
            ESP_LOGI(TAG, "clear: unlink %s", de->d_name);
        }
    }
    closedir(d);
}

static esp_err_t h_logs(httpd_req_t *req) {
    DIR *d = opendir(BDKG_LOG_DIR);
    static char buf[4096];
    char *p = buf;
    int len = snprintf(p, sizeof(buf), "{\"files\":[");
    p += len;
    
    if (!d) {
        strcpy(p, "]}");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, buf);
        return ESP_OK;
    }
    
    char names[64][16];
    int count = 0;
    struct dirent *de;
    
    while ((de = readdir(d)) != NULL && count < 64) {
        if (strlen(de->d_name) == 14 && strcmp(&de->d_name[strlen(de->d_name)-4], ".csv") == 0) {
            strncpy(names[count], de->d_name, 15);
            names[count][15] = '\0';
            count++;
        }
    }
    closedir(d);
    
    // Сортировка по имени
    for (int i = 1; i < count; i++) {
        char *key = names[i];
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            strcpy(names[j+1], names[j]);
            j--;
        }
        strcpy(names[j+1], key);
    }
    
    for (int i = 0; i < count; i++) {
        char fullpath[64];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", BDKG_LOG_DIR, names[i]);
        struct stat st;
        if (stat(fullpath, &st) == 0) {
            char date[16];
            strncpy(date, names[i], 10);
            date[10] = '\0';
            len = snprintf(p, buf + sizeof(buf) - p, "{\"date\":\"%s\",\"name\":\"%s\",\"size\":%ld}%s",
                           date, names[i], st.st_size, (i < count-1) ? "," : "");
            p += len;
        }
    }
    
    strcpy(p, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t h_download(httpd_req_t *req) {
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no query");
        return ESP_FAIL;
    }
    
    char date[16];
    if (httpd_query_key_value(q, "date", date, sizeof(date)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no date");
        return ESP_FAIL;
    }
    
    if (!valid_date(date)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad date");
        return ESP_FAIL;
    }
    
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.csv", BDKG_LOG_DIR, date);
    
    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no log");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "text/csv");
    char cd[64];
    snprintf(cd, sizeof(cd), "attachment; filename=\"bdkg_%s.csv\"", date);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    
    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// #BDKG-16: Старт/Стоп записи CSV (CSRF-защищённые POST).
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
