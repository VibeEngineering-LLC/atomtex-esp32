idf_component_register(
    SRCS "bdkg_usb.c"
    INCLUDE_DIRS "include"
    REQUIRES usb_host_cdc_acm bdkg05 modbus_rtu esp_timer
)
