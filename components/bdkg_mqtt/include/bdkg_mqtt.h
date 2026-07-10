#pragma once

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
//  E7 (#BDKG-7): MQTT-паблишер показаний БДКГ-05 + Home Assistant discovery.
//
//  Плата публикует снимок дозиметра (bdkg_usb_get_latest) раз в 10 с в topic
//  bdkg05/<id>/state и при каждом коннекте — retain-конфиги HA discovery
//  (prefix homeassistant). <id> = 6 hex-символов из последних 3 байт STA MAC.
//  Конфиг брокера — ТОЛЬКО из NVS (namespace "mqtt"), дефолтного адреса нет.
//
//  БАН Народмон (CLAUDE.md, HARD HOLD): uri с подстрокой "narodmon"
//  отвергается (bdkg_mqtt_set_cfg вернёт BDKG_MQTT_ERR_BLOCKED).
// ============================================================================

typedef struct {
    bool enabled;       // публикация включена
    char uri[96];       // адрес брокера, напр. "mqtt://<host>:1883"
    char user[48];      // опц. логин брокера ("" = без авторизации)
    char pass[48];      // опц. пароль брокера
} bdkg_mqtt_cfg_t;

typedef struct {
    bool     connected;     // MQTT-сессия установлена
    uint32_t published;     // счётчик успешных publish state с момента boot
    int64_t  last_pub_at;   // time(NULL) последнего publish, 0 если не было
} bdkg_mqtt_stat_t;

// Коды возврата bdkg_mqtt_set_cfg.
#define BDKG_MQTT_OK             0
#define BDKG_MQTT_ERR_NVS       (-1)   // ошибка записи NVS
#define BDKG_MQTT_ERR_INVALID   (-2)   // невалидный uri (не mqtt:// и не mqtts://)
#define BDKG_MQTT_ERR_BLOCKED   (-3)   // адрес запрещён (Народмон-бан)

// Загрузить конфиг из NVS, стартовать клиента (если enabled) и поднять
// publish-задачу. Идемпотентно: повторный вызов — no-op.
void bdkg_mqtt_init(void);

// Снимок текущего конфига. ВНИМАНИЕ: out->pass заполняется реальным паролем —
// веб-слой НЕ должен отдавать его наружу (вернуть только has_pass).
void bdkg_mqtt_get_cfg(bdkg_mqtt_cfg_t *out);

// Сохранить конфиг в NVS (write-on-change — #NVS-1) и перезапустить клиента.
// Возвращает BDKG_MQTT_OK или отрицательный BDKG_MQTT_ERR_*.
int  bdkg_mqtt_set_cfg(const bdkg_mqtt_cfg_t *c);

// Снимок рантайм-статистики для веб-статуса.
void bdkg_mqtt_get_stat(bdkg_mqtt_stat_t *out);
