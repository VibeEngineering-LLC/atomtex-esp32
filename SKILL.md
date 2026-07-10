---
name: atomtex-esp32
description: Use when developing or flashing the ATOMTEX БДКГ-05 dosimeter to ESP32-S3 WiFi bridge firmware — Modbus-RTU-over-USB-CDC protocol, register map, ESP-IDF build, LittleFS CSV logging, Web UI, factory-only partitions (no OTA). Fork of the AtomSpectra waterfall analyzer with dosimeter support layered on.
---

# atomtex-esp32 — ATOMTEX БДКГ-05 ↔ ESP32-S3 WiFi-мост

Прошивка ESP32-S3, подключающая дозиметр **ATOMTEX БДКГ-05** по USB (адаптер USB→RS485) и отдающая
показания (МЭД, ЦПС, температура, накопленная доза) в Web UI + CSV-архив на флеше.

**Родословная (важно):** проект — форк/надстройка прошивки анализатора **AtomSpectra (гамма-водопад)**
для ESP32-S3. Поверх waterfall-стека (spectrum/shproto/FTDI) добавлена поддержка БДКГ-05
(Modbus-over-CDC). В дереве сосуществуют ДВА USB-стека; в режиме дозиметра legacy-транспорт
анализатора **не стартует** (`main/main.c:72-75`). Не путать команды/протоколы двух приборов.

## Когда применять

- Правка/сборка/прошивка прошивки БДКГ-05-моста (компоненты `bdkg05`/`bdkg_usb`/`bdkg_log`/`modbus_rtu`).
- Работа с протоколом БДКГ-05 (Modbus RTU: init-цепочка, poll-регистры, CRC hi-lo).
- Web UI `/bdkg`, API `/api/bdkg*`, CSV-логи на LittleFS.
- Публикация в публичный репо `VibeEngineering-LLC/atomtex-esp32` (MIT).

НЕ для: BLE-приборов (AtomFast/Radex/RadonEye — свои скиллы), чистого waterfall-анализа AtomSpectra.

## Архитектура

Точка входа `app_main()` (`main/main.c:33`): `wifi_manager_init` → `bdkg_usb_init` (headless до
captive-portal, `main.c:42`) → captive-portal gate → `spectrum_init` + LittleFS mount → повторный
идемпотентный `bdkg_usb_init` (`main.c:77`) → `web_server_init` → `init_sntp` → `bdkg_log_init`
(`main.c:78-86`). Двойной `bdkg_usb_init` намеренный, защищён guard `s_inited` (`bdkg_usb.c:18,191`).

Компоненты (`main/CMakeLists.txt:4` REQUIRES `modbus_rtu bdkg05 bdkg_usb bdkg_log shproto`):

| Компонент | Роль | Публичное API (`include/*.h`) |
|---|---|---|
| `modbus_rtu` | Чистый C Modbus-RTU мастер, host-тестируемый, без ESP-зависимостей | `mb_crc16`, `mb_build_read_input/holding/write_coil/write_register/read_exception`, `mb_parse_response` |
| `bdkg05` | Транспорт-агностичный драйвер дозиметра поверх callback `transact()` (USB/UART/mock) | `bdkg05_bind`, `bdkg05_wait_ready`, `bdkg05_init`, `bdkg05_read_input`, `bdkg05_read` |
| `bdkg_usb` | Modbus-over-USB-CDC: авто-детект USB→RS485, приведение линии, poll 1 Гц, snapshot для web | `bdkg_usb_init`, `bdkg_usb_is_connected`, `bdkg_usb_get_latest` |
| `bdkg_log` | Логгер в CSV по дате на LittleFS + HTTP-скачивание | `bdkg_log_init`, `bdkg_log_register` |
| `shproto` | Legacy AtomSpectra-протокол (START 0xFE/ESC 0xFD/FINISH 0xA5). Для БДКГ-05 НЕ используется | — |

## Ключевые файлы

Публичный репо (SSOT): `VibeEngineering-LLC/atomtex-esp32` — firmware в КОРНЕ репо (не в подпапке).
Локальное dev-дерево (приватные пути/COM/IP) — см. `_PRIVATE-OVERLAY.md`.

| Файл | Назначение |
|---|---|
| `main/main.c` | app_main, порядок инициализации, guard двойного bdkg_usb_init |
| `main/web_server.c` | HTTP-роуты, `handle_bdkg_json` (`:411-425`), регистрация bdkg_log (`:1446`) |
| `components/bdkg05/include/bdkg05.h` | Карта регистров, slave 0x01, init-контракт, safety-инварианты |
| `components/bdkg_usb/bdkg_usb.c` | FTDI SIO control-transfers, poll_task, phase-1 эмуляция |
| `components/bdkg_log/bdkg_log.c` | CSV-формат, ротация KEEP_DAYS/MAX_BYTES, роуты /api/bdkg/logs* |
| `components/modbus_rtu/include/modbus_rtu.h` | FC-опкоды, CRC hi-lo quirk |
| `docs/dosimeter-ui-commands.ref.md` | Эталонная карта команд БДКГ-05 (спека + pcap ATexch) |
| `partitions.csv` | factory-only layout (см. «Партиции») |

## Протокол БДКГ-05 (Modbus RTU over USB-CDC)

Провенанс: `docs/dosimeter-ui-commands.ref.md`, `bdkg05.h`, `bdkg_usb.c`, `modbus_rtu.h`.

**Транспорт.** Modbus RTU поверх USB-CDC host. Адаптер USB→RS485 (FTDI/CP210x/CH340) авто-детект по
VID/PID (`bdkg_usb.c:46-51`). Линия **19200 8N1**, slave addr **0x01** (`bdkg05.h:18`).
FTDI как vendor-class: SIO control-transfers — baud divisor `wValue=0x809C`, 8N1 `0x0008`,
DTR+RTS `0x0303`, **latency timer 1 ms** (дефолт 16 ms рвёт хвост кадра → ошибка -3) (`bdkg_usb.c:102-114`).
FTDI 2 статус-байта в начале каждого ≤64B чанка стрипаются (`bdkg_usb.c:25-36`).

**CRC.** CRC-16/Modbus poly 0xA001 init 0xFFFF, но на проводе **hi-lo** (bUseCorrectCRC, `modbus_rtu.h:35-37`).

**FC-опкоды** (`modbus_rtu.h:16-22`): `READ_HOLDING 0x03`, `READ_INPUT 0x04`, `WRITE_COIL 0x05`,
`WRITE_REGISTER 0x06`, `READ_EXCEPTION 0x07`.

**Init-цепочка** (`bdkg05.h:76`, `docs/...:17-25`):
1. FC 0x07 Read Exception — `wait_ready` (холодный прибор ~3.65 c игнорирует WRITE до готовности).
2. **Phase-1 эмуляция:** ~3 c pre-init FC04-чтений (read-before-write, результат отбрасывается) —
   иначе dose-engine/HV не стартует (`bdkg_usb.c:129-138`).
3. FC 0x06 Write Register `0x0000 = 0xFFFF` (dosimeter mode).
4. FC 0x05 Write Coil `0x0023 = ON` (start).
5. FC 0x05 Write Coil `0x0022 = ON` (latch).

**Poll 1 Гц — 7× FC 0x04 Read Input Registers** (`bdkg05.h:20-27`):

| Поле | Рег | Тип | Обработка |
|---|---|---|---|
| Температура | 0x0001 | u16 fixed 8.8 | raw/256 (`bdkg05.c:136`) |
| ЦПС инстант | 0x0002 | u32 BE | как есть |
| ЦПС avg | 0x0004 | f32 BE | как есть |
| ЦПС погр. % | 0x0006 | f32 BE | — |
| МЭД (Зв/ч) | 0x0008 | f32 BE | ×1e6 → мкЗв/ч в UI |
| МЭД погр. % | 0x000A | f32 BE | — |
| Доза накопл. (Зв) | 0x000C | f32 BE | ×1e6 → мкЗв |

**Фон** — 1× при connect: FC 0x03 Read Holding рег 0x000A (вне poll-цикла).

**`bdkg_snapshot_t`** (`bdkg_usb.h:15-25`): `med_sv_h, med_err_pct, cps_avg, cps_err_pct, cps_inst,
temp_c, dose_sv, valid, sample_us`.

### ⚠ Safety-инварианты (HARD, из кода)

- **НИКОГДА** не читать спектр (FC 0x0B) в режиме дозиметра, не выводить дозу из спектра (`bdkg05.h:5-6`).
- **НИКОГДА** не логировать/публиковать серийник прибора (Report Slave ID FC 0x11) — web не отдаёт
  серийник (`web_server.c:409-410`). Соответствует политике проекта «серийник БДКГ не логировать».
- Холодный прибор без `wait_ready`+phase-1 → МЭД=0 (частая ошибка).

## Сборка (ESP-IDF v5.4, target esp32s3)

ESP-IDF **v5.4** (CI `.github/workflows/build.yml:16`), target **esp32s3**. Managed components
(`main/idf_component.yml`, версии `dependencies.lock`): `espressif/mdns` 1.11.2, `espressif/usb_host_cdc_acm`
2.4.0, `joltwallet/littlefs` 1.22.1.

**Сборка в Docker** (кириллица в пути → нужен MSYS_NO_PATHCONV, см. esp32-dev правило #10):
```bash
cd "<project>" && MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/project" -w /project espressif/idf:v5.4 idf.py build
```
`-Werror=format-truncation` активен: `snprintf` с `dirent.d_name` (до 255 B) падает — копировать в
фикс-буфер `char n[16]; strncpy(n,de->d_name,15); n[15]=0;` перед snprintf (паттерн в `bdkg_log.c`).

CI: `build.yml` (ESP-IDF build) + `host-tests.yml` (`tests/host && make test`, затем ASan/UBSan `make asan`).

## Партиции и хранилище (factory-only — OTA НЕвозможен)

`partitions.csv`: `factory,app,factory,0x10000,0x300000` + `storage,data,spiffs,0x310000,0xCF0000`.
**Нет `ota_0/ota_1/ota_data`** → обновление по воздуху НЕвозможно, только USB-перезаливка. Смена на
OTA-layout = разовая USB-перезаливка (backlog-задача web-OTA).

`storage` монтируется как **LittleFS** по метке `partition_label="storage"` (подтип spiffs — лишь
резерв области), `esp_vfs_littlefs_register` в `main/spectrum.c:110-115`, `format_if_mount_failed=true`.

**CSV-логи БДКГ-05:** `/storage/bdkg/YYYY-MM-DD.csv`, период записи 10 c (`BDKG_LOG_PERIOD_S`), пишет
только при валидном времени (year≥2024). Формат: `iso_time,med_uSv_h,cps_avg,cps_inst,temp_c,dose_uSv`
(МЭД/доза ×1e6). Ротация: `KEEP_DAYS 7` (по числу файлов) + `MAX_BYTES 4МБ` (по сумме размеров),
запускается при создании нового файла (`bdkg_log.c:16-20,70-72,92,128`).

## Web UI + API

`web/` (8 файлов, вшиты через `EMBED_FILES` в `main/CMakeLists.txt:5`): `index/setup/waterfall/saved/
system/service/monitor/bdkg.html`. Страница дозиметра — `/bdkg`.

**БДКГ-05 API** (`web_server.c`):
- `GET /api/bdkg` → `handle_bdkg_json` (`:411-425`), опрос страницей 1 Гц. JSON: `connected, valid,
  med_usv_h, med_err, cps, cps_err, cps_inst, temp_c, dose_usv`. Серийник НЕ отдаётся (`:409-410`).
- `GET /api/bdkg/logs` — список CSV; `GET /api/bdkg/log?date=YYYY-MM-DD` — скачать CSV;
  `POST /api/bdkg/logs/clear` — стереть весь флеш-архив (`bdkg_log.c:292-298`).

Прочие роуты (waterfall/spectrum/system) — родительского AtomSpectra-стека.

## MQTT / Home Assistant — статус

**В дереве `bdkg05-esp32-bridge` НЕ найдено** (grep `mqtt|esp_mqtt|homeassistant` по main/+components/
= 0 совпадений, проверено 2026-07-10). Значится как незакрытый эпик E7 в `_BDKG_UI_PLAN.md`.
Задачи трекера #BDKG-7/32/39 помечены closed — расхождение (закрыты, вероятно, в другой ветке/клоне).
**Перед работой с MQTT — проверить фактическое наличие паблишера в конкретном клоне**, не полагаться
на статус задач. Anti-hallucination: не утверждать, что MQTT есть, без grep.

## Flasher / release

`version.txt` = `v0.1.0`. В firmware-репо CI только build+host-tests, **release-workflow нет** —
`factory.bin` собирается вне репо (ESP-IDF `esptool merge_bin` @ offset 0x0) и публикуется в GitHub
Release; flasher тянет его pull-моделью. Полная процедура (5 шагов автора, esptool-команда, реестр
4 проектов) — приватный проектный reference `references/flasher-pull-architecture.md` в ESP32-AT.
Приборный serial БДКГ-05 в public-сборку/логи не попадает (safety-инвариант выше).

## Тест-планы

**План А — первый прогон на новом железе:** собрать (Docker) → USB-заливка esptool (проверить MAC
платы коротким UART-логом, НЕ шить COM5=SoundBlaster) → captive-portal WiFi → подключить БДКГ-05 через
USB→RS485 → `/bdkg` показывает ненулевой МЭД (если 0 — не прошла init-цепочка wait_ready/phase-1).

**План B — правка протокола:** менять только `bdkg05.c`/`bdkg_usb.c` → host-тесты `tests/host` (`make
test`+`make asan`) → Docker build → USB-заливка (OTA невозможен) → 5+ мин прогон, сверить регистры со
`docs/dosimeter-ui-commands.ref.md`.

**План C — новая версия для flasher:** зелёный build → anti-creds (`strings firmware.bin` пусто) →
bump `version.txt` → `esptool merge_bin` factory.bin @ 0x0 → GitHub Release с asset + SHA256.

## Known issues / quirks

- **МЭД=0** — не пройдена init: `wait_ready` (FC07) + phase-1 (~3 c FC04 read-before-write) обязательны
  до WRITE (`bdkg05.h:70-72`, `bdkg_usb.c:129-138`).
- **FTDI latency 16 ms** рвёт хвост кадра → ошибка -3; выставлять 1 ms (`bdkg_usb.c:111`).
- **DIAG-блок** в `poll_task` (FC03 holding + сырые FC04-дампы) — отладочный, вычищать перед public-релизом.
- **Двойной `bdkg_usb_init`** намеренный (headless + post-portal), guard `s_inited`.
- **Legacy USB анализатора** несовместим с Modbus, не стартует в режиме дозиметра (`main.c:72-75`).
- MQTT/HA не реализован (см. раздел выше).

## Contributing / публикация

Overlay-модель: SSOT — публичный `VibeEngineering-LLC/atomtex-esp32` (MIT, `main`). Правки прямо в
checkout, push с `git add` конкретных путей (НЕ `-A`), commit author verter73, anti-creds scan
(MAC/IP/SSID/токены/серийник). Приватная операционная часть — `_PRIVATE-OVERLAY.md` (git-игнор).
НЕ копировать в `Verter73/claude-skills` (там только redirect-README).