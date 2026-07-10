idf_component_register(
    SRCS "main.c" "wifi_manager.c" "web_server.c" "boot_config.c" "tz_config.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_server nvs_flash esp_event esp_timer littlefs json mdns esp_app_format modbus_rtu bdkg05 bdkg_usb bdkg_log bdkg_mqtt
    EMBED_FILES "${PROJECT_DIR}/web/setup.html" "${PROJECT_DIR}/web/bdkg.html" "${PROJECT_DIR}/web/bdkg_system.html"
)
