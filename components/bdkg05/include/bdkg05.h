// БДКГ-05 (ATOMTEX) dosimeter driver — Modbus RTU over a transport callback.
// Transport-agnostic: caller supplies transact() (USB CDC-ACM, UART, or host mock).
// Port of atomtex-usb-collector/atc/modbus/{client,device}.py (HEAD 0fa307c6).
//
// SAFETY (prompt §5): NEVER read spectrum (FC 0x0B) in dosimeter mode; NEVER derive
// dose from spectrum; NEVER log/push serial number (Report Slave ID 0x11).
#ifndef BDKG05_H
#define BDKG05_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BDKG05_DEFAULT_ADDR 0x01

// FC04 input-register map (big-endian on the wire). Widths in 16-bit registers.
#define BDKG05_REG_TEMP       0x0001  // u16, fixed-8.8: T[°C] = raw / 256.0
#define BDKG05_REG_CPS_INST   0x0002  // u32, instantaneous counts-per-second
#define BDKG05_REG_CPS_AVG    0x0004  // f32, averaged CPS
#define BDKG05_REG_CPS_ERR    0x0006  // f32, CPS statistical error, %
#define BDKG05_REG_MED        0x0008  // f32, МЭД (ambient dose-equiv rate), Sv/h
#define BDKG05_REG_MED_ERR    0x000A  // f32, МЭД error, %
#define BDKG05_REG_DOSE       0x000C  // f32, accumulated dose, Sv

// Init registers/coils (client.py init sequence).
#define BDKG05_REG_MODE       0x0000  // write 0xFFFF to select dosimeter mode
#define BDKG05_COIL_START     0x0023  // write TRUE to start measurement
#define BDKG05_COIL_LATCH     0x0022  // write TRUE to latch/apply

// One snapshot of the dosimeter. SI units stored internally (Sv, Sv/h).
typedef struct {
    float    temp_c;       // °C
    uint32_t cps_inst;     // counts/s (instantaneous)
    float    cps_avg;      // counts/s (averaged)
    float    cps_err_pct;  // %
    float    med_sv_h;     // Sv/h (energy-compensated ambient dose rate)
    float    med_err_pct;  // %
    float    dose_sv;      // Sv (accumulated)
    bool     valid;        // true only if every register read succeeded
} bdkg05_reading_t;

// Transport callback: send req (req_len bytes), receive a full Modbus RTU reply
// into resp (up to resp_cap). Write reply length to *resp_len. Return 0 on
// success, negative on transport error/timeout. Implemented by USB/UART/mock.
typedef int (*bdkg05_transact_fn)(void *ctx,
                                  const uint8_t *req, size_t req_len,
                                  uint8_t *resp, size_t resp_cap, size_t *resp_len);

typedef struct {
    bdkg05_transact_fn transact;  // required
    void   *ctx;                  // opaque, passed back to transact
    uint8_t addr;                 // Modbus slave address (BDKG05_DEFAULT_ADDR)
} bdkg05_t;

typedef enum {
    BDKG05_OK          = 0,
    BDKG05_ERR_ARG     = -1,  // null handle/callback
    BDKG05_ERR_IO      = -2,  // transport returned error/timeout
    BDKG05_ERR_PROTO   = -3,  // bad frame / CRC / fc mismatch / exception
    BDKG05_ERR_TIMEOUT = -4,  // wait_ready exceeded budget
} bdkg05_status_t;

// Init handle with a transport. addr = BDKG05_DEFAULT_ADDR if 0.
void bdkg05_bind(bdkg05_t *h, bdkg05_transact_fn transact, void *ctx, uint8_t addr);

// Poll FC 0x07 (Read Exception) until device answers or timeout. Cold device
// ignores WRITEs (~3.65 s) until ready — MUST pass before bdkg05_init().
// sleep_ms may be NULL (busy poll); return non-zero from it to abort.
bdkg05_status_t bdkg05_wait_ready(bdkg05_t *h, uint32_t timeout_ms,
                                  int (*sleep_ms)(uint32_t));

// Init sequence: write_register(MODE,0xFFFF) → write_coil(START,1) → write_coil(LATCH,1).
bdkg05_status_t bdkg05_init(bdkg05_t *h);

// Read one register group (count 16-bit registers) via FC 0x04 into out_be
// (raw big-endian payload bytes, without byte_count). *out_n = bytes copied.
bdkg05_status_t bdkg05_read_input(bdkg05_t *h, uint16_t reg, uint16_t count,
                                  uint8_t *out_be, size_t cap, size_t *out_n);

// Full dosimeter snapshot: 7 FC04 transactions, big-endian decode into *r.
// r->valid is set only when every register read succeeded.
bdkg05_status_t bdkg05_read(bdkg05_t *h, bdkg05_reading_t *r);

#ifdef __cplusplus
}
#endif
#endif // BDKG05_H
