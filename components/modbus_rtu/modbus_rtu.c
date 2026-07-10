#include "modbus_rtu.h"
#include <string.h>

static uint8_t crc_hi[256];
static uint8_t crc_lo[256];
static bool crc_initialized = false;

static void init_crc(void) {
    if (crc_initialized) return;
    crc_initialized = true;
    for (int i = 0; i < 256; i++) {
        uint16_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
        crc_hi[i] = crc & 0xFF;
        crc_lo[i] = (crc >> 8) & 0xFF;
    }
}

void mb_crc16(const uint8_t *data, size_t len, uint8_t *wire_hi, uint8_t *wire_lo) {
    init_crc();
    uint8_t uchCRChi = 0xFF;
    uint8_t uchCRClo = 0xFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t idx = uchCRChi ^ data[i];
        uchCRChi = uchCRClo ^ crc_hi[idx];
        uchCRClo = crc_lo[idx];
    }
    // crc.py: return uchCRClo, uchCRChi  # (wire_high, wire_low)
    *wire_hi = uchCRClo;
    *wire_lo = uchCRChi;
}

bool mb_verify_crc(const uint8_t *frame, size_t len) {
    if (len < 3) return false;
    uint8_t wire_hi, wire_lo;
    mb_crc16(frame, len - 2, &wire_hi, &wire_lo);
    return frame[len - 2] == wire_hi && frame[len - 1] == wire_lo;
}

size_t mb_build_read_input(uint8_t *out, uint8_t addr, uint16_t reg, uint16_t count) {
    out[0] = addr;
    out[1] = MB_FC_READ_INPUT;
    out[2] = (reg >> 8) & 0xFF;
    out[3] = reg & 0xFF;
    out[4] = (count >> 8) & 0xFF;
    out[5] = count & 0xFF;
    uint8_t hi, lo;
    mb_crc16(out, 6, &hi, &lo);
    out[6] = hi;
    out[7] = lo;
    return 8;
}

size_t mb_build_read_holding(uint8_t *out, uint8_t addr, uint16_t reg, uint16_t count) {
    out[0] = addr;
    out[1] = MB_FC_READ_HOLDING;
    out[2] = (reg >> 8) & 0xFF;
    out[3] = reg & 0xFF;
    out[4] = (count >> 8) & 0xFF;
    out[5] = count & 0xFF;
    uint8_t hi, lo;
    mb_crc16(out, 6, &hi, &lo);
    out[6] = hi;
    out[7] = lo;
    return 8;
}

size_t mb_build_write_coil(uint8_t *out, uint8_t addr, uint16_t coil, bool on) {
    out[0] = addr;
    out[1] = MB_FC_WRITE_COIL;
    out[2] = (coil >> 8) & 0xFF;
    out[3] = coil & 0xFF;
    uint16_t val = on ? 0xFF00 : 0x0000;
    out[4] = (val >> 8) & 0xFF;
    out[5] = val & 0xFF;
    uint8_t hi, lo;
    mb_crc16(out, 6, &hi, &lo);
    out[6] = hi;
    out[7] = lo;
    return 8;
}

size_t mb_build_write_register(uint8_t *out, uint8_t addr, uint16_t reg, uint16_t val) {
    out[0] = addr;
    out[1] = MB_FC_WRITE_REGISTER;
    uint16_t value = val;
    out[2] = (reg >> 8) & 0xFF;
    out[3] = reg & 0xFF;
    out[4] = (value >> 8) & 0xFF;
    out[5] = value & 0xFF;
    uint8_t hi, lo;
    mb_crc16(out, 6, &hi, &lo);
    out[6] = hi;
    out[7] = lo;
    return 8;
}

size_t mb_build_read_exception(uint8_t *out, uint8_t addr) {
    out[0] = addr;
    out[1] = MB_FC_READ_EXCEPTION;
    uint8_t hi, lo;
    mb_crc16(out, 2, &hi, &lo);
    out[2] = hi;
    out[3] = lo;
    return 4;
}

size_t mb_build_report_slave_id(uint8_t *out, uint8_t addr) {
    out[0] = addr;
    out[1] = MB_FC_REPORT_SLAVE_ID;
    uint8_t hi, lo;
    mb_crc16(out, 2, &hi, &lo);
    out[2] = hi;
    out[3] = lo;
    return 4;
}

mb_status_t mb_parse_response(const uint8_t *frame, size_t frame_len, uint8_t expected_fc,
                              uint8_t *out_payload, size_t out_cap, size_t *out_len,
                              uint8_t *out_exc_code) {
    if (frame_len < 5) return MB_ERR_TOO_SHORT;
    if (!mb_verify_crc(frame, frame_len)) return MB_ERR_CRC;
    uint8_t fc = frame[1];
    if (fc & MB_FC_ERROR_MASK) {
        if (out_exc_code) *out_exc_code = frame[2];
        return MB_ERR_EXCEPTION;
    }
    if (fc != expected_fc) return MB_ERR_FC_MISMATCH;
    size_t payload_len = frame_len - 4; // drop addr, fc, crc_hi, crc_lo
    if (payload_len > out_cap) return MB_ERR_BUF;
    memcpy(out_payload, frame + 2, payload_len);
    if (out_len) *out_len = payload_len;
    return MB_OK;
}
