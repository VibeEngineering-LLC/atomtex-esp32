// bdkg_log.h — логирование отсчётов БДКГ-05 в CSV по дате на LittleFS + HTTP-скачивание.
#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Запуск логгер-таска БДКГ-05 (пишет /storage/bdkg/YYYY-MM-DD.csv раз в BDKG_LOG_PERIOD_S).
// Звать ПОСЛЕ монтирования storage (spectrum_init) и bdkg_usb_init. Идемпотентно.
void bdkg_log_init(void);

// #BDKG-41: восстановить RAM-кольцо графика последними точками с флеша (после ребута).
// Звать ПОСЛЕ bdkg_log_init и bdkg_usb_init (сидит bdkg_usb-кольцо).
void bdkg_log_preload(void);

// Регистрация HTTP-хендлеров: GET /api/bdkg/logs (список) + GET /api/bdkg/log?date=... (скачать)
// + POST /api/bdkg/log/start, POST /api/bdkg/log/stop (управление записью, CSRF).
// Звать в web_server_init после создания httpd server.
void bdkg_log_register(httpd_handle_t server);

// #BDKG-16: управление записью CSV. logger-таск живёт всегда, но пишет строки
// только когда enabled=true. Дефолт после boot — false (см. main.c bdkg_log_set_enabled).
void bdkg_log_set_enabled(bool on);
bool bdkg_log_is_enabled(void);

// #BDKG-19: удалить все /storage/bdkg/*.csv (очистка архива). Звать до bdkg_log_init.
void bdkg_log_clear_storage(void);

#ifdef __cplusplus
}
#endif
