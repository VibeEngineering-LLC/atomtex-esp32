#include "boot_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "bootcfg";
#define BOOT_NS "boot"

static bool get_flag(nvs_handle_t h, const char *key)
{
    uint8_t v = 0;
    return (nvs_get_u8(h, key, &v) == ESP_OK) && (v != 0);
}

void boot_config_load(boot_config_t *out)
{
    if (!out) return;
    out->autostart_bdkg_log = false;
    out->clear_bdkg_storage = false;

    nvs_handle_t h;
    if (nvs_open(BOOT_NS, NVS_READONLY, &h) != ESP_OK) return;   // namespace ещё нет → все false
    out->autostart_bdkg_log = get_flag(h, "as_bdkg");
    out->clear_bdkg_storage = get_flag(h, "clr_bdkg");
    nvs_close(h);
}

int boot_config_save(const boot_config_t *in)
{
    if (!in) return -1;
    nvs_handle_t h;
    if (nvs_open(BOOT_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = ESP_OK;
    e |= nvs_set_u8(h, "as_bdkg",  in->autostart_bdkg_log  ? 1 : 0);
    e |= nvs_set_u8(h, "clr_bdkg", in->clear_bdkg_storage  ? 1 : 0);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) { ESP_LOGW(TAG, "save failed (0x%x)", (int)e); return -1; }
    return 0;
}