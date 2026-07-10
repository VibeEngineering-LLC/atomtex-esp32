idf_component_register(
    SRCS "bdkg_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt nvs_flash json esp_event esp_netif bdkg_usb
)
