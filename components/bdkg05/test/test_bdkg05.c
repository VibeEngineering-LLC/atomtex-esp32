// Host bench for bdkg05 — mock transact simulates БДКГ-05 Modbus replies.
#include "bdkg05.h"
#include "modbus_rtu.h"
#include <stdio.h>
#include <string.h>
static int fails = 0;
static int fc07_calls = 0;
static void put_f32(uint8_t *b, float f) {
    union { uint32_t u; float f; } c; c.f = f;
    b[0]=(c.u>>24)&0xFF; b[1]=(c.u>>16)&0xFF; b[2]=(c.u>>8)&0xFF; b[3]=c.u&0xFF;
}
static void check(const char *name, int ok) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) fails++;
}
static int approx(float a, float b) {
    float d = a - b; if (d < 0) d = -d;
    return d < 1e-6f * (b < 0 ? -b : b) + 1e-9f;
}
// Build a canned FC04 response for one register group into resp; return length.
static size_t fc04_resp(uint8_t *resp, uint8_t addr, uint16_t reg, uint16_t count) {
    resp[0] = addr; resp[1] = 0x04; resp[2] = (uint8_t)(count * 2);
    uint8_t *d = resp + 3;
    memset(d, 0, count * 2);
    switch (reg) {
        case 0x0001: d[0]=0x19; d[1]=0x00; break;                 // raw 6400 -> 25.0 C
        case 0x0002: d[0]=0; d[1]=0; d[2]=0; d[3]=0x2A; break;     // cps_inst 42
        case 0x0004: put_f32(d, 40.5f); break;                    // cps_avg
        case 0x0006: put_f32(d, 5.0f); break;                     // cps_err %
        case 0x0008: put_f32(d, 1.5e-7f); break;                  // МЭД Sv/h
        case 0x000A: put_f32(d, 10.0f); break;                    // МЭД err %
        case 0x000C: put_f32(d, 3.0e-6f); break;                  // dose Sv
    }
    size_t n = 3 + (size_t)count * 2;
    uint8_t hi, lo; mb_crc16(resp, n, &hi, &lo);
    resp[n] = hi; resp[n + 1] = lo;
    return n + 2;
}
// Mock transport: FC07 -> ready, FC04 -> canned data, FC05/06 -> echo request.
static int mock_txn(void *ctx, const uint8_t *req, size_t req_len,
                    uint8_t *resp, size_t cap, size_t *rlen) {
    (void)ctx; (void)cap;
    uint8_t addr = req[0], fc = req[1];
    size_t n;
    if (fc == 0x07) {
        fc07_calls++;
        resp[0]=addr; resp[1]=0x07; resp[2]=0x00;
        uint8_t hi, lo; mb_crc16(resp, 3, &hi, &lo); resp[3]=hi; resp[4]=lo;
        n = 5;
    } else if (fc == 0x04) {
        uint16_t reg = (uint16_t)((req[2]<<8) | req[3]);
        uint16_t cnt = (uint16_t)((req[4]<<8) | req[5]);
        n = fc04_resp(resp, addr, reg, cnt);
    } else if (fc == 0x05 || fc == 0x06) {
        memcpy(resp, req, req_len); n = req_len;      // write echo
    } else {
        return -1;
    }
    *rlen = n;
    return 0;
}
static int io_fail_txn(void *c, const uint8_t *q, size_t l,
                       uint8_t *r, size_t p, size_t *n) {
    (void)c;(void)q;(void)l;(void)r;(void)p;(void)n; return -1;
}
static int sleep_noop(uint32_t ms) { (void)ms; return 0; }
int main(void) {
    bdkg05_t h;
    bdkg05_bind(&h, mock_txn, NULL, BDKG05_DEFAULT_ADDR);

    check("wait_ready", bdkg05_wait_ready(&h, 1000, sleep_noop) == BDKG05_OK);
    check("wait_ready polled FC07", fc07_calls >= 1);
    check("init", bdkg05_init(&h) == BDKG05_OK);

    bdkg05_reading_t r;
    check("read OK", bdkg05_read(&h, &r) == BDKG05_OK);
    check("valid flag", r.valid);
    check("temp 25.0", approx(r.temp_c, 25.0f));
    check("cps_inst 42", r.cps_inst == 42);
    check("cps_avg 40.5", approx(r.cps_avg, 40.5f));
    check("cps_err 5.0", approx(r.cps_err_pct, 5.0f));
    check("med 1.5e-7", approx(r.med_sv_h, 1.5e-7f));
    check("med_err 10.0", approx(r.med_err_pct, 10.0f));
    check("dose 3.0e-6", approx(r.dose_sv, 3.0e-6f));
    printf("       МЭД=%g Sv/h (%g uSv/h) T=%.2f C cps=%u\n",
           r.med_sv_h, r.med_sv_h * 1e6, r.temp_c, r.cps_inst);
    bdkg05_t h2;
    bdkg05_bind(&h2, io_fail_txn, NULL, BDKG05_DEFAULT_ADDR);
    check("wait_ready timeout", bdkg05_wait_ready(&h2, 200, sleep_noop) == BDKG05_ERR_TIMEOUT);

    printf(fails ? "\n== %d FAIL ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
