#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// E3: Modbus-RTU-over-USB-CDC transport for БДКГ-05. Opens the enumerated
// USB->RS485 adapter (auto-detect VID/PID), 19200 8N1, primes the device
// (dosimeter mode + resetAveraging + turnOnHV) and runs a continuous 1 Hz
// poll. NEVER logs serial number.
void bdkg_usb_init(void);
bool bdkg_usb_is_connected(void);

// E4: latest live reading snapshot for the web UI. Filled by the poll task
// under a mutex; getter copies it out. valid=false until first successful
// post-init read.
typedef struct {
    float    med_sv_h;   // Sv/h (energy-compensated ambient dose rate)
    float    med_err_pct;// % statistical error on МЭД (БДКГ-05 reg 0x000A)
    float    cps_avg;    // counts/s (averaged)
    float    cps_err_pct;// % statistical error on CPS (БДКГ-05 reg 0x0006)
    float    cps_inst;   // counts/s (instantaneous)
    float    temp_c;     // C
    float    dose_sv;    // Sv (accumulated)
    bool     valid;      // false until first good read after init
    int64_t  sample_us;  // esp_timer_get_time() at sample moment
} bdkg_snapshot_t;

// Copy the latest snapshot. Returns out->valid (false if no good reading yet).
bool bdkg_usb_get_latest(bdkg_snapshot_t *out);

// #BDKG-41: ёмкость RAM-кольца истории (было 300). Точек по 1 Гц = секунд окна.
#define BDKG_HIST_MAX 3600

// #BDKG-17/41: on-board history ring. Точка = момент времени + основные величины
// для восстановления живого графика после F5 и после ребута (boot-preload с флеша).
typedef struct {
    int64_t t_unix;    // time(NULL) в момент отсчёта (валиден после SNTP-синка)
    float   med_sv_h;  // Sv/h
    float   cps_avg;    // counts/s (усреднённое прибором)
    float   cps_inst;   // #BDKG-28 counts/s мгновенное (сырое, для графика)
    float   temp_c;     // C
} bdkg_hist_point_t;

// Копирует историю в хронологическом порядке (старые→новые) в out (до max точек).
// Возвращает число реально записанных точек (0..min(300,max)).
size_t bdkg_usb_get_history(bdkg_hist_point_t *out, size_t max);

// #BDKG-41: ёмкость RAM-кольца (BDKG_HIST_MAX). Для аллокаций у вызывающих.
size_t bdkg_usb_hist_capacity(void);

// #BDKG-41: засеять RAM-кольцо истории точками (хронологически, старые→новые).
// Замещает текущее содержимое кольца. Звать один раз на boot из bdkg_log_preload.
void bdkg_usb_seed_history(const bdkg_hist_point_t *pts, size_t n);

// #BDKG-16 «Сброс замера»: попросить poll-таск переинициализировать прибор
// (bdkg05_init заново) на следующей итерации. Асинхронно, без блокировки.
void bdkg_usb_request_reset(void);