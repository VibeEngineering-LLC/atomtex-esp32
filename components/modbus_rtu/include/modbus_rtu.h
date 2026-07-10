// Modbus RTU master — чистый C, без ESP-зависимостей (host-тестируемый).
// Порт atomtex-usb-collector/atc/modbus/{crc,frame}.py (HEAD 0fa307c6).
// БДКГ-05 quirk: CRC wire-порядок hi-lo (bUseCorrectCRC=TRUE), кадры append_crc.
#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Function codes (frame.py).
#define MB_FC_READ_HOLDING     0x03
#define MB_FC_READ_INPUT       0x04
#define MB_FC_WRITE_COIL       0x05
#define MB_FC_WRITE_REGISTER   0x06
#define MB_FC_READ_EXCEPTION   0x07
#define MB_FC_READ_BLOCK       0x0B
#define MB_FC_REPORT_SLAVE_ID  0x11
#define MB_FC_ERROR_MASK       0x80

// Коды результата парсинга.
typedef enum {
    MB_OK              = 0,
    MB_ERR_TOO_SHORT   = -1,
    MB_ERR_CRC         = -2,
    MB_ERR_EXCEPTION   = -3,
    MB_ERR_FC_MISMATCH = -4,
    MB_ERR_BUF         = -5,
} mb_status_t;

// CRC-16/Modbus (poly 0xA001, init 0xFFFF). Возвращает 2 wire-байта в
// БДКГ-05-порядке hi-lo. Идентично crc16_modbus() из crc.py.
void mb_crc16(const uint8_t *data, size_t len, uint8_t *wire_hi, uint8_t *wire_lo);

// Проверяет CRC кадра (последние 2 байта). len — полная длина с CRC.
bool mb_verify_crc(const uint8_t *frame, size_t len);

// Сборка запросов. Пишут в out, возвращают длину кадра (байт).
size_t mb_build_read_input(uint8_t *out, uint8_t addr, uint16_t reg, uint16_t count);
size_t mb_build_read_holding(uint8_t *out, uint8_t addr, uint16_t reg, uint16_t count);
size_t mb_build_write_coil(uint8_t *out, uint8_t addr, uint16_t coil, bool on);
size_t mb_build_write_register(uint8_t *out, uint8_t addr, uint16_t reg, uint16_t val);
size_t mb_build_read_exception(uint8_t *out, uint8_t addr);
size_t mb_build_report_slave_id(uint8_t *out, uint8_t addr);

// Разбор ответа: длина/CRC/exception/fc, копирует payload (без addr, fc, crc)
// в out_payload. Для FC 0x03/0x04 первый байт payload — byte_count.
// *out_len — длина payload. При MB_ERR_EXCEPTION в *out_exc_code — код.
mb_status_t mb_parse_response(const uint8_t *frame, size_t frame_len,
                              uint8_t expected_fc,
                              uint8_t *out_payload, size_t out_cap, size_t *out_len,
                              uint8_t *out_exc_code);

#ifdef __cplusplus
}
#endif
#endif // MODBUS_RTU_H
