#include <stdlib.h>
#include <time.h>
#include "wifi_manager.h"
#include "web_server.h"
#include "boot_config.h"
#include "tz_config.h"
#include "bdkg_usb.h"
#include "bdkg_log.h"
#include "bdkg_mqtt.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static void storage_mount(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS");
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info("storage", &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted: total=%zu bytes, used=%zu bytes", total, used);
    } else {
        ESP_LOGE(TAG, "Failed to get LittleFS info");
    }
}

static void init_sntp(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting up...");

    char tzbuf[16];
    tz_config_load(tzbuf, sizeof(tzbuf));
    setenv("TZ", tzbuf, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", tzbuf);

    storage_mount();

    wifi_manager_init();

    bdkg_usb_init();

    if (wifi_manager_is_ap_mode()) {
        ESP_LOGI(TAG, "Captive portal active, waiting for WiFi config...");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    boot_config_t boot_config;
    boot_config_load(&boot_config);
    ESP_LOGI(TAG, "Boot config: autostart_bdkg_log=%s, clear_bdkg_storage=%s",
             boot_config.autostart_bdkg_log ? "true" : "false",
             boot_config.clear_bdkg_storage ? "true" : "false");

    web_server_init();

    init_sntp();

    if (boot_config.clear_bdkg_storage) {
        bdkg_log_clear_storage();
        ESP_LOGW(TAG, "BDKG storage cleared as requested");
    }

    bdkg_log_init();
    bdkg_log_set_enabled(boot_config.autostart_bdkg_log);

    bdkg_mqtt_init();

    ESP_LOGI(TAG, "All subsystems initialized");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "USB: %s, WiFi: %s",
                 bdkg_usb_is_connected() ? "OK" : "--",
                 wifi_is_connected() ? "OK" : "--");
    }
}