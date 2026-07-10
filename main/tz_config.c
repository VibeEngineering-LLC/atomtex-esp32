#include "tz_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "tzcfg";
#define TZ_NS       "tz"
#define TZ_KEY      "tzstr"
#define TZ_DEFAULT  "MSK-3"
#define TZ_MAX      15

void tz_config_load(char *out, size_t cap)
{
    if (!out || cap == 0) return;
    // дефолт на случай отсутствия ключа / недоступности NVS
    strncpy(out, TZ_DEFAULT, cap - 1);
    out[cap - 1] = '\0';

    nvs_handle_t h;
    if (nvs_open(TZ_NS, NVS_READONLY, &h) != ESP_OK) return;   // namespace ещё нет → дефолт
    size_t len = cap;
    if (nvs_get_str(h, TZ_KEY, out, &len) != ESP_OK) {
        strncpy(out, TZ_DEFAULT, cap - 1);   // ключа нет → вернуть дефолт
        out[cap - 1] = '\0';
    }
    nvs_close(h);
}
int tz_config_save(const char *tz)
{
    if (!tz || tz[0] == '\0') return -1;
    if (strlen(tz) > TZ_MAX) return -1;   // POSIX TZ-строки короткие

    nvs_handle_t h;
    if (nvs_open(TZ_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_str(h, TZ_KEY, tz);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) { ESP_LOGW(TAG, "save failed (0x%x)", (int)e); return -1; }
    return 0;
}
