// Host bench for modbus_rtu — validates frame build + parse against Python oracle.
// Build: gcc -I../include ../modbus_rtu.c test_modbus.c -o test_modbus && ./test_modbus
#include "modbus_rtu.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void hex(const char *tag, const uint8_t *b, size_t n) {
    printf("%s:", tag);
    for (size_t i = 0; i < n; i++) printf(" %02X", b[i]);
    printf("\n");
}

// Compare built frame vs expected oracle bytes.
static void expect(const char *name, const uint8_t *got, size_t glen,
                   const uint8_t *exp, size_t elen) {
    int ok = (glen == elen) && (memcmp(got, exp, glen) == 0);
    printf("[%s] %s (len %zu/%zu)\n", ok ? "PASS" : "FAIL", name, glen, elen);
    if (!ok) { hex("  got", got, glen); hex("  exp", exp, elen); fails++; }
}

int main(void) {
    uint8_t out[32];
    size_t n;

    // --- CRC/frame-build oracle vectors (from Python crc.py reference) ---
    n = mb_build_read_input(out, 0x01, 0x0001, 0x0001);
    expect("read_input(1,1,1)", out, n,
           (uint8_t[]){0x01,0x04,0x00,0x01,0x00,0x01,0x0A,0x60}, 8);

    n = mb_build_read_exception(out, 0x01);
    expect("read_exception(1)", out, n,
           (uint8_t[]){0x01,0x07,0xE2,0x41}, 4);

    n = mb_build_read_input(out, 0x01, 0x0008, 0x0001);
    expect("read_input(1,0x08,1)", out, n,
           (uint8_t[]){0x01,0x04,0x00,0x08,0x00,0x01,0x08,0xB0}, 8);

    // --- parse FC04 response: simulate device МЭД = 1.5e-7 Sv/h (0.15 µSv/h) ---
    // f32 big-endian for 1.5e-7 = 0x34210643 (IEEE-754).
    uint8_t resp[16] = {0x01, 0x04, 0x04, 0x34, 0x21, 0x06, 0x43};
    uint8_t chi, clo;
    mb_crc16(resp, 7, &chi, &clo);
    resp[7] = chi; resp[8] = clo;
    uint8_t pl[16]; size_t pl_len; uint8_t exc = 0;
    mb_status_t st = mb_parse_response(resp, 9, MB_FC_READ_INPUT,
                                       pl, sizeof(pl), &pl_len, &exc);
    printf("[%s] parse FC04 status=%d pl_len=%zu\n",
           (st == MB_OK && pl_len == 5) ? "PASS" : "FAIL", (int)st, pl_len);
    if (st != MB_OK || pl_len != 5) fails++;
    union { uint32_t u; float f; } cv;
    cv.u = ((uint32_t)pl[1] << 24) | ((uint32_t)pl[2] << 16) |
           ((uint32_t)pl[3] << 8) | (uint32_t)pl[4];
    printf("       decoded MED = %g Sv/h (%g uSv/h)\n", cv.f, cv.f * 1e6);

    // --- CRC error must be caught ---
    resp[8] ^= 0xFF;
    st = mb_parse_response(resp, 9, MB_FC_READ_INPUT, pl, sizeof(pl), &pl_len, &exc);
    printf("[%s] parse CRC-error detect status=%d\n",
           (st == MB_ERR_CRC) ? "PASS" : "FAIL", (int)st);
    if (st != MB_ERR_CRC) fails++;

    printf(fails ? "\n== %d FAIL ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
