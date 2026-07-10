# БДКГ-05 дозиметр-режим — карта команд (reference, зафиксировано 2026-07-09)

**Провенанс источника:** внутренняя спека программиста `atomtex-usb-collector`
(`_spec/dosimeter-ui-commands.md`, дата документа 2026-07-09). Application-уровень
«UI → Modbus-команда» для ATOMTEX БДКГ-05, провенанс самой спеки — pcap ATexch кадры 35-42 +
код `atc/modbus/device.py`.

Зафиксировано в проект `bdkg05-esp32-bridge` как эталон для сверки прошивки. Wire-уровень
(байты FC/CRC/карта регистров) — в исходниках `components/modbus_rtu/` и `components/bdkg05/`
(отдельного протокол-документа для БДКГ-05 в проекте пока нет).

**Транспорт:** Modbus RTU, `addr=1`, `baud=19200`, 8N1, кадр `[addr][FC][data][CRC-lo][CRC-hi]`.

---

## Эталонная последовательность (из спеки)

### Старт — `enter_dosimeter_mode()`, 4 команды
Критично: **без FC 0x07 wait_ready на холодном приборе (~3.65 с boot) init игнорится → МЭД=0**.

| # | FC | data (hex) | Смысл |
|---|---|---|---|
| 0 | 0x07 Read Exception Status | — | health-poll до готовности |
| 1 | 0x06 Write Register | `00 00 FF FF` | рег 0x0000 = 0xFFFF, старт измерения |
| 2 | 0x05 Write Coil | `00 23 FF 00` | coil 0x0023 ON |
| 3 | 0x05 Write Coil | `00 22 FF 00` | coil 0x0022 ON |

### Опрос 1 Гц — `read_dosimeter()`, 7× FC 0x04 Read Input Registers

| Вывод | Рег | Тип | Формула |
|---|---|---|---|
| Температура | 0x0001 (1) | u16 fixed 8.8 | `u16/256` |
| Мгн. скорость | 0x0002 (2) | u32 BE | как есть |
| CPS | 0x0004 (2) | f32 BE | как есть |
| погр. CPS | 0x0006 (2) | f32 BE % | — |
| МЭД | 0x0008 (2) | f32 BE Зв/ч | ×1e6 → мкЗв/ч в UI |
| погр. МЭД | 0x000A (2) | f32 BE % | — |
| Доза | 0x000C (2) | f32 BE Зв | накопленная |

### Фон — 1× при connect
FC 0x03 Read Holding, рег 0x000A, 2 рег → f32 BE. НЕ в poll-цикле.

Кнопки «Обновить», «↺ Сброс макс.» — чисто клиентские, Modbus не шлют.

---

## Сверка с прошивкой bdkg05-esp32-bridge (2026-07-09) — ✅ СОВПАДАЕТ

Провенанс кода: `components/bdkg05/bdkg05.c`, `components/bdkg05/include/bdkg05.h`,
`components/modbus_rtu/`, `components/bdkg_usb/bdkg_usb.c`.

| Элемент | Спека | Прошивка (файл:символ) | ✓ |
|---|---|---|---|
| addr | 1 | `BDKG05_DEFAULT_ADDR 0x01` (bdkg05.h:18) | ✅ |
| baud | 19200 8N1 | FTDI `0x809C` (bdkg_usb.c:106,113) | ✅ |
| wait_ready | FC 0x07 | `bdkg05_wait_ready` → `MB_FC_READ_EXCEPTION` (bdkg05.c:84-100) | ✅ |
| старт | FC 0x06 `0x0000=0xFFFF` | `BDKG05_REG_MODE 0x0000` write `0xFFFF` (bdkg05.c:111, .h:30) | ✅ |
| coil 0x23 | FC 0x05 ON | `BDKG05_COIL_START 0x0023` (bdkg05.c:116, .h:31) | ✅ |
| coil 0x22 | FC 0x05 ON | `BDKG05_COIL_LATCH 0x0022` (bdkg05.c:121, .h:32) | ✅ |
| temp 0x0001 | u16/256 | `BDKG05_REG_TEMP` `t/256.0f` (bdkg05.c:134-136, .h:21) | ✅ |
| мгн 0x0002 | u32 BE | `BDKG05_REG_CPS_INST` `rd_u32` (bdkg05.c:138, .h:22) | ✅ |
| CPS 0x0004 | f32 BE | `BDKG05_REG_CPS_AVG` `rd_f32` (bdkg05.c:141, .h:23) | ✅ |
| погр.CPS 0x0006 | f32 % | `BDKG05_REG_CPS_ERR` (bdkg05.c:144, .h:24) | ✅ |
| МЭД 0x0008 | f32 Зв/ч | `BDKG05_REG_MED` (bdkg05.c:147, .h:25) | ✅ |
| погр.МЭД 0x000A | f32 % | `BDKG05_REG_MED_ERR` (bdkg05.c:150, .h:26) | ✅ |
| доза 0x000C | f32 Зв | `BDKG05_REG_DOSE` (bdkg05.c:153, .h:27) | ✅ |
| FC-константы | 03/04/05/06/07 | modbus_rtu.h:16-20 идентичны | ✅ |

**Порядок caller'а** (`bdkg_usb.c:118-172`): open_dev → cfg_line (19200) → `wait_ready(6000ms)`
→ 6× `bdkg05_read` read-before-write (~3с phase-1, эмуляция ATexch-паузы до t≈3.65с) → `bdkg05_init`
→ poll 1 Гц. Согласуется с провенансом спеки (init на t≈3.65с) и объясняет анти-«МЭД=0» тайминг.

### Расхождения — только диагностические, не функциональные

1. **Спека: фон = FC 0x03 holding 0x000A.** Прошивка после init читает holding **0x0000 (1 рег)**
   ДОПОЛНИТЕЛЬНО к 0x000A — это DIAG-блок (`bdkg_usb.c:144-168`) для локализации бага «МЭД=0»
   (H2: bg!=0 → dosimeter-space жив; bg=0 → режим не переключился). Не противоречит спеке, это
   расширенная инструментация поверх эталона.
2. Прошивка держит fallback-путь `s_rx_ring==NULL` и прочие ESP-специфичные вещи — вне области спеки.

**Вывод:** карта команд прошивки идентична эталону программиста. Init-последовательность
(FC07→FC06→FC05→FC05), регистры опроса, типы и формулы совпадают побайтово. Расхождения — только
доп. DIAG-чтения, функциональной дельты нет.
