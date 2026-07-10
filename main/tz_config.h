#pragma once

#include <stddef.h>

// #BDKG-21: часовой пояс платы (persist в NVS, namespace "tz", ключ "tzstr").
// POSIX TZ-строка (напр. "MSK-3" = UTC+3 фиксированно, без летнего перевода).
// Применяется в main.c через setenv("TZ",...)+tzset() до первого localtime_r,
// чтобы strftime в CSV-логе БДКГ-05 и XML/N42-экспортах давал местное время.

#ifdef __cplusplus
extern "C" {
#endif

// Читает TZ-строку из NVS в out (не более cap байт, всегда \0-терминирует).
// Если ключа нет / NVS недоступен → дефолт "MSK-3". Безопасно после nvs_flash_init().
void tz_config_load(char *out, size_t cap);

// Сохраняет TZ-строку (макс 15 символов) в NVS. Возвращает 0 при успехе, -1 при ошибке.
int  tz_config_save(const char *tz);

#ifdef __cplusplus
}
#endif
