# ATOMTEX BDKG-05 → ESP32-S3 → Web UI / MQTT

[🇷🇺 Русская версия](README.md) · **🇬🇧 English**

A WiFi gateway for the **ATOMTEX BDKG-05** dosimeter, built on the ESP32-S3 with USB OTG Host.

It connects to the instrument over **USB** (Modbus RTU over emulated USB-serial), polls dose
rate, count rate, accumulated dose and temperature once a second, and shows a live graph in
your browser — no app, no Home Assistant (optional, via MQTT), no cloud.

![ESP32-S3](https://img.shields.io/badge/ESP32--S3-N16R8-blue) ![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.4-green) ![USB](https://img.shields.io/badge/USB-OTG%20Host-orange) ![Protocol](https://img.shields.io/badge/Protocol-Modbus%20RTU-yellowgreen) ![MQTT](https://img.shields.io/badge/MQTT-HA%20discovery-informational)

```
ATOMTEX BDKG-05 ─USB (Modbus RTU)─► ESP32-S3 ─Web UI /bdkg─► browser
                                        │
                                        ├─► MQTT + HA discovery (optional, off by default)
                                        │
                                        └─► CSV log on flash (LittleFS), one-click download
```

## ⚠ Before you start

On every USB connect, the firmware sends the vendor-documented "enter dosimeter mode"
sequence to the instrument — a health-poll readiness check followed by a mode register write
and two measurement-start coils. This is not an out-of-spec command; it is required to get
any reading at all.

- The **"Сброс" (Reset)** button on the `/bdkg` page repeats this initialization on the
  instrument — **accumulated dose and the current averaging on the instrument are reset.**
  Use it deliberately.
- Firmware/OTA only affects the ESP32 board, not the instrument itself. An interrupted flash
  may require reflashing the board over USB (UART port, not OTG).
- MQTT/HA publishing and CSV logging to flash are **off by default**, enabled manually on the
  System tab.

## Disclaimer

The firmware is provided "as is", without any warranty. The author is not liable for any
direct or indirect damage: resetting the instrument's accumulated dose/readings, data loss,
the instrument or board becoming inoperable, or any other consequence of use. By using this
firmware you accept these terms and act at your own risk.

---

## What it solves

Collecting BDKG-05 readings normally requires a direct USB connection to a PC with dedicated
collector software. This firmware turns a cheap ESP32-S3 into a **persistent WiFi bridge**:

- the instrument connects to the ESP over USB — a small board sits right next to it, no PC needed;
- readings are available **in any browser** over WiFi: dose rate (µSv/h), count rate, temperature,
  accumulated dose;
- **300-point history lives on the board** (ring buffer) — after F5 the graph is not empty and
  does not jump, data does not depend on the browser;
- **CSV log on flash** (LittleFS) — Start/Stop recording, "Download today" in one click;
- **MQTT + Home Assistant discovery** — optional, broker config set in the Web UI, retained
  auto-discovery;
- **independent zoom on time (X) and value (Y)** for each of the three graphs (wheel /
  Shift+wheel / drag / double-click), toggleable SMA smoothing with a configurable window;
- local time on the board (timezone picked on the System tab) — CSV and timestamps are not in UTC.

No cloud. No accounts. Everything runs on your local network (MQTT is opt-in, to your own broker).

## What you see in the Web UI

**`/bdkg` — Monitoring**
- Three live graphs: dose rate (µSv/h), count rate, temperature — updated once a second
- Time axis (X) with real `HH:MM:SS` labels, independent X/Y zoom on each graph
- Toggleable SMA smoothing with a configurable window (2–120 s)
- Recording (REC) indicator and free flash space badge — both live
- **Start / Stop** buttons for CSV log recording, **Reset** measurement (with confirmation)
- "Download today" button — today's CSV in one click

**`/bdkg/system` — System**
- ESP32-S3: chip/flash/PSRAM/firmware version/uptime/heap
- Flash (LittleFS): used/free
- WiFi: SSID/RSSI/IP, "Reset WiFi" button
- BDKG-05 instrument: USB connection status, log recording status
- MQTT: broker address, username/password, publishing toggle
- Boot autostart checkboxes: "Start recording on boot", "Clear CSV archive on boot"
- Timezone (list of Russian timezones, POSIX format)
- Reboot board button

Both pages share one color palette (GitHub-dark, blue accent).

## MQTT / Home Assistant

The board publishes a snapshot of the readings every 10 s to topic `bdkg05/<id>/state`
(`<id>` — 6 hex characters from the board's STA MAC address) and retained Home Assistant
discovery configs (`homeassistant/...`) on every broker connect. Broker config lives only in
the board's NVS, set from the Web UI System tab — no address is baked in, publishing is off
by default.

## Protocol

BDKG-05 speaks **Modbus RTU** over USB-serial (emulated USB-CDC, 19200 baud, 8N1, instrument
address `1`). Initialization: readiness health-poll (FC 0x07) → mode register write (FC 0x06)
→ two measurement-start coils (FC 0x05). Polling — 7 read registers once a second (FC 0x04):
temperature, instantaneous count rate, count rate with error, dose rate with error, accumulated
dose.

## What you need

**Hardware:**
- **ESP32-S3-DevKitC-1 N16R8** (16 MB Flash, 8 MB PSRAM) — needs an S3 with USB OTG
- **USB-C OTG cable** — from the ESP32-S3 (host) to the BDKG-05 instrument (device)
- **ATOMTEX BDKG-05** dosimeter (USB port)
- A USB cable to flash the ESP (through the UART port, not OTG)

**Software:**
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) **v5.4**
  **or** Docker (`espressif/idf:v5.4`)

## Quick start (5 minutes)

```bash
# 1. Clone
git clone https://github.com/VibeEngineering-LLC/atomtex-esp32.git
cd atomtex-esp32

# 2. Build (option A: local ESP-IDF)
idf.py set-target esp32s3
idf.py build

# 2. Build (option B: Docker, no ESP-IDF install needed)
docker run --rm -v "$(pwd):/project" -w /project espressif/idf:v5.4 \
  bash -c ". /opt/esp/idf/export.sh && idf.py build"

# 3. Flash (use your own COM port)
idf.py -p COM14 flash

# 4. Connect to WiFi
#    The board raises an AP captive portal → enter SSID and password

# 5. Open in a browser
#    http://<board-IP>/bdkg

# 6. Connect the BDKG-05 with a USB-C OTG cable to the ESP32-S3 USB port
#    Readings appear automatically
```

## Web API

| Endpoint | Method | What it does |
|---|---|---|
| `/` | GET | 302 redirect to `/bdkg` |
| `/healthcheck` | GET | Health probe (for monitoring/CI) |
| `/bdkg` | GET | Web UI — monitoring |
| `/bdkg/system` | GET | Web UI — system |
| `/api/csrf-token` | GET | Issue a CSRF token (required in `X-CSRF-Token` on all POSTs) |
| `/api/bdkg` | GET | Current reading snapshot + recording status (JSON) |
| `/api/bdkg/history` | GET | Last 300 points from the board (JSON) |
| `/api/bdkg/log/start` / `/stop` | POST | Start/stop CSV log recording |
| `/api/bdkg/reset` | POST | Reset measurement (re-initialize instrument) |
| `/api/bdkg/logs` | GET | List CSV log files |
| `/api/bdkg/log?date=` | GET | Download CSV log for a date |
| `/api/bdkg/mqtt` | GET/POST | MQTT publishing config |
| `/api/tz` | GET/POST | Board timezone |
| `/api/boot-config` | GET/POST | Autostart recording / clear flash on boot |
| `/api/system` | GET | ESP32 health (heap, uptime, flash, WiFi) |
| `/api/wifi/reset` | POST | Reset WiFi, reboot into setup mode |
| `/api/reboot-esp` | POST | Reboot the ESP32 |

> **All POST endpoints require the `X-CSRF-Token` header**, obtained from `GET /api/csrf-token`.
> The Web UI does this automatically.

## Security and trust model

The gateway is designed for a **trusted local network** (home WiFi) and **has no user
authentication** — anyone on the same network can open the Web UI and control
recording/reset. This is a deliberate choice for a cloud-free, account-free home instrument;
do not expose the board directly to the internet.

What is protected: a **CSRF token** on all mutating POSTs — a third-party browser tab cannot
"blindly" trigger a measurement reset or WiFi reset (same-origin policy).

**Outbound calls:** SNTP requests to `pool.ntp.org` (time sync) and, if manually enabled,
publishing to the broker configured by the operator. No other outbound internet connections.

## Project structure

```
atomtex-esp32/
├── components/
│   ├── bdkg05/            BDKG-05 protocol over Modbus (registers, coils, init sequence)
│   ├── bdkg_usb/          USB-CDC transport + 1 Hz poll task + 300-point history ring
│   ├── bdkg_log/          CSV log on LittleFS (start/stop, rotation, HTTP download)
│   ├── bdkg_mqtt/         MQTT publisher + Home Assistant discovery
│   └── modbus_rtu/        Modbus RTU frames (FC 03/04/05/06/07, CRC-16)
├── main/
│   ├── main.c              entry point, SNTP, timezone
│   ├── boot_config.c       board boot behavior (NVS flags)
│   ├── tz_config.c         timezone (NVS, POSIX string)
│   ├── wifi_manager.c      STA + AP captive portal
│   └── web_server.c        HTTP API + CSRF
├── web/
│   ├── bdkg.html            Web UI — monitoring
│   ├── bdkg_system.html     Web UI — system
│   └── setup.html           captive portal (WiFi setup)
├── partitions.csv
├── sdkconfig.defaults       ESP32-S3 USB OTG config
├── CMakeLists.txt
├── LICENSE                  MIT
└── README.md                Russian version of this file
```

> This project branched off from
> [atomspectra-waterfall-esp32](https://github.com/VibeEngineering-LLC/atomspectra-waterfall-esp32)
> (KB Radar Atom Spectra gamma spectrometer), keeping only the ESP32-S3/USB-OTG-host/WiFi
> plumbing. The spectrometer/waterfall code (USB-CDC bridge to the FTDI adapter, shproto, TCP
> bridge, spectrogram, and their Web UI) has been **physically removed** from this repository —
> only the BDKG-05 code remains.

## License

MIT — see [`LICENSE`](LICENSE).

## Credits

- **ATOMTEX** ([atomtex.com](https://atomtex.com/)) — manufacturer of the BDKG-05 dosimeter.
- **Espressif** — ESP-IDF and the USB Host stack.
