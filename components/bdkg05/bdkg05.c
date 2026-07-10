#include "bdkg05.h"
#include "modbus_rtu.h"
#include <string.h>

void bdkg05_bind(bdkg05_t *h, bdkg05_transact_fn transact, void *ctx, uint8_t addr) {
    if (!h) return;
    h->transact = transact;
    h->ctx = ctx;
    h->addr = addr ? addr : BDKG05_DEFAULT_ADDR;
}

static bdkg05_status_t bdkg_txn(bdkg05_t *h, const uint8_t *req, size_t req_len,
                                uint8_t expected_fc, uint8_t *payload, size_t pcap, size_t *plen) {
    uint8_t resp[64];
    size_t rlen = 0;
    int io = h->transact(h->ctx, req, req_len, resp, sizeof(resp), &rlen);
    if (io != 0) return BDKG05_ERR_IO;
    uint8_t exc = 0;
    mb_status_t st = mb_parse_response(resp, rlen, expected_fc, payload, pcap, plen, &exc);
    return (st == MB_OK) ? BDKG05_OK : BDKG05_ERR_PROTO;
}

bdkg05_status_t bdkg05_read_input(bdkg05_t *h, uint16_t reg, uint16_t count,
                                  uint8_t *out_be, size_t cap, size_t *out_n) {
    if (!h || !h->transact) return BDKG05_ERR_ARG;
    uint8_t req[8];
    size_t rl = mb_build_read_input(req, h->addr, reg, count);
    uint8_t pl[64];
    size_t pn = 0;
    bdkg05_status_t s = bdkg_txn(h, req, rl, MB_FC_READ_INPUT, pl, sizeof(pl), &pn);
    if (s != BDKG05_OK) return s;
    size_t nbytes = pl[0];
    if (nbytes + 1 > pn) return BDKG05_ERR_PROTO;
    if (nbytes > cap) return BDKG05_ERR_PROTO;
    memcpy(out_be, pl + 1, nbytes);
    if (out_n) *out_n = nbytes;
    return BDKG05_OK;
}

static uint16_t be_u16(const uint8_t *b) {
    return (uint16_t)((b[0] << 8) | b[1]);
}

static uint32_t be_u32(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static float be_f32(const uint8_t *b) {
    union { uint32_t u; float f; } c;
    c.u = be_u32(b);
    return c.f;
}

static bdkg05_status_t rd_u16(bdkg05_t *h, uint16_t reg, uint16_t *v) {
    uint8_t b[4];
    size_t n = 0;
    bdkg05_status_t s = bdkg05_read_input(h, reg, 1, b, sizeof(b), &n);
    if (s) return s;
    if (n < 2) return BDKG05_ERR_PROTO;
    *v = be_u16(b);
    return BDKG05_OK;
}

static bdkg05_status_t rd_u32(bdkg05_t *h, uint16_t reg, uint32_t *v) {
    uint8_t b[4];
    size_t n = 0;
    bdkg05_status_t s = bdkg05_read_input(h, reg, 2, b, sizeof(b), &n);
    if (s) return s;
    if (n < 4) return BDKG05_ERR_PROTO;
    *v = be_u32(b);
    return BDKG05_OK;
}

static bdkg05_status_t rd_f32(bdkg05_t *h, uint16_t reg, float *v) {
    uint8_t b[4];
    size_t n = 0;
    bdkg05_status_t s = bdkg05_read_input(h, reg, 2, b, sizeof(b), &n);
    if (s) return s;
    if (n < 4) return BDKG05_ERR_PROTO;
    *v = be_f32(b);
    return BDKG05_OK;
}

bdkg05_status_t bdkg05_wait_ready(bdkg05_t *h, uint32_t timeout_ms, int (*sleep_ms)(uint32_t)) {
    if (!h || !h->transact) return BDKG05_ERR_ARG;
    uint32_t elapsed = 0;
    while (1) {
        uint8_t req[4];
        size_t rl = mb_build_read_exception(req, h->addr);
        uint8_t pl[64];
        size_t pn = 0;
        if (bdkg_txn(h, req, rl, MB_FC_READ_EXCEPTION, pl, sizeof(pl), &pn) == BDKG05_OK)
            return BDKG05_OK;
        if (sleep_ms) {
            if (sleep_ms(50)) return BDKG05_ERR_TIMEOUT;
        }
        elapsed += 50;
        if (elapsed >= timeout_ms) return BDKG05_ERR_TIMEOUT;
    }
}

bdkg05_status_t bdkg05_init(bdkg05_t *h) {
    if (!h || !h->transact) return BDKG05_ERR_ARG;

    uint8_t req[8];       // write_register/write_coil emit 8 bytes (frame+CRC)
    size_t rl;
    uint8_t pl[8];
    size_t pn = 0;

    // Write MODE register
    rl = mb_build_write_register(req, h->addr, BDKG05_REG_MODE, 0xFFFF);
    if (bdkg_txn(h, req, rl, MB_FC_WRITE_REGISTER, pl, sizeof(pl), &pn) != BDKG05_OK)
        return BDKG05_ERR_PROTO;

    // Write START coil
    rl = mb_build_write_coil(req, h->addr, BDKG05_COIL_START, true);
    if (bdkg_txn(h, req, rl, MB_FC_WRITE_COIL, pl, sizeof(pl), &pn) != BDKG05_OK)
        return BDKG05_ERR_PROTO;

    // Write LATCH coil
    rl = mb_build_write_coil(req, h->addr, BDKG05_COIL_LATCH, true);
    if (bdkg_txn(h, req, rl, MB_FC_WRITE_COIL, pl, sizeof(pl), &pn) != BDKG05_OK)
        return BDKG05_ERR_PROTO;

    return BDKG05_OK;
}

bdkg05_status_t bdkg05_read(bdkg05_t *h, bdkg05_reading_t *r) {
    if (!h || !h->transact || !r) return BDKG05_ERR_ARG;
    memset(r, 0, sizeof(*r));
    r->valid = false;

    uint16_t t;
    bdkg05_status_t s = rd_u16(h, BDKG05_REG_TEMP, &t);
    if (s) return s;
    r->temp_c = t / 256.0f;

    s = rd_u32(h, BDKG05_REG_CPS_INST, &r->cps_inst);
    if (s) return s;

    s = rd_f32(h, BDKG05_REG_CPS_AVG, &r->cps_avg);
    if (s) return s;

    s = rd_f32(h, BDKG05_REG_CPS_ERR, &r->cps_err_pct);
    if (s) return s;

    s = rd_f32(h, BDKG05_REG_MED, &r->med_sv_h);
    if (s) return s;

    s = rd_f32(h, BDKG05_REG_MED_ERR, &r->med_err_pct);
    if (s) return s;

    s = rd_f32(h, BDKG05_REG_DOSE, &r->dose_sv);
    if (s) return s;

    r->valid = true;
    return BDKG05_OK;
}
