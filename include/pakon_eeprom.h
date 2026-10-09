/*
 * pakon_eeprom.h — per-unit EEPROM decoder (hardware-free).
 *
 * The per-unit EEPROM (I2C 0x52) holds two sections, each stored twice and
 * each framed as {u32 length; u32 crc32 (zlib, over the payload)}:
 *
 *   A primary 0x000, A backup 0x400 — 398 bytes: hardware version, scanner
 *     type, serial, per-base Offset / MotorSpeed / MotorSpeed_Ir, and the
 *     NegMatrix / PosMatrix (3x10 floats each).
 *   B primary 0x800, B backup 0xA00 — 36 bytes: per-base motor-adjust words.
 *
 * Layout from pakon-reference (docs/calibration.md); tools/pakon_eeprom.py is
 * the reference implementation and the two must agree. Reading the chip is
 * not done here: this module only parses bytes already read (or loaded from
 * an archive). Nothing here writes to a scanner.
 */
#ifndef PAKON_EEPROM_H
#define PAKON_EEPROM_H

#include <stdint.h>
#include <stddef.h>

#include "pakon_log.h"   /* for pakon_result */

#ifdef __cplusplus
extern "C" {
#endif

/* The four section copies, in read order. */
typedef enum {
    PAKON_EEPROM_A_PRIMARY = 0,
    PAKON_EEPROM_A_BACKUP,
    PAKON_EEPROM_B_PRIMARY,
    PAKON_EEPROM_B_BACKUP,
    PAKON_EEPROM_NCOPIES
} pakon_eeprom_copy;

#define PAKON_EEPROM_A_LEN  398u
#define PAKON_EEPROM_B_LEN  36u

/* Chip offset and fixed length of each copy, indexed by pakon_eeprom_copy. */
extern const uint16_t pakon_eeprom_copy_offset[PAKON_EEPROM_NCOPIES];
extern const uint16_t pakon_eeprom_copy_len[PAKON_EEPROM_NCOPIES];

/* Scanner type at section A 0x0C. */
#define PAKON_EEPROM_TYPE_F135       1350u
#define PAKON_EEPROM_TYPE_F135_PLUS  1351u

/* Resolution bases, in EEPROM order. */
enum { PAKON_EEPROM_BASE4 = 0, PAKON_EEPROM_BASE8, PAKON_EEPROM_BASE16,
       PAKON_EEPROM_NBASES };

typedef struct {
    uint16_t offset;          /* the OEM's "Offset" (likely CCD window start) */
    uint16_t motor_speed;
    uint16_t motor_speed_ir;
} pakon_eeprom_base;

typedef struct {
    uint16_t normal, drag, ir, drag_ir;
} pakon_eeprom_motor_adjust;

/* Decoded EEPROM plus per-copy validity and the copy each section came from. */
typedef struct {
    int valid[PAKON_EEPROM_NCOPIES];   /* length + CRC check per copy */
    int copies_equal[2];               /* A, B: 1 equal, 0 differ, -1 n/a */
    int selected[2];                   /* A, B: a pakon_eeprom_copy or -1 */

    /* Section A (meaningful only when selected[0] >= 0). */
    uint32_t hardware_version;
    uint32_t scanner_type;
    uint32_t serial;
    pakon_eeprom_base base[PAKON_EEPROM_NBASES];
    float neg_matrix[3][10];
    float pos_matrix[3][10];

    /* Section B (meaningful only when selected[1] >= 0). */
    pakon_eeprom_motor_adjust adjust[PAKON_EEPROM_NBASES];
    uint32_t section_b_unknown;
} pakon_eeprom;

/* CRC-32 as computed by zlib's crc32(0, data, len). */
uint32_t pakon_eeprom_crc32(const uint8_t *data, size_t len);

/*
 * Nonzero if `data` is a complete copy of `which`: exact fixed length, a
 * matching length field, and a matching CRC over bytes 8..end.
 */
int pakon_eeprom_copy_valid(pakon_eeprom_copy which,
                            const uint8_t *data, size_t len);

/*
 * Decode up to four copies. `data[i]` may be NULL (copy not read). For each
 * section the primary is used if valid, else the backup; a section with no
 * valid copy is left zeroed with selected[] = -1. Returns PAKON_OK only if
 * both sections decoded, the scanner type is known and both matrices are
 * finite (the Python tool's "decodable"); otherwise PAKON_ERR_PROTO with
 * whatever did decode still filled in.
 */
pakon_result pakon_eeprom_decode(const uint8_t *const data[PAKON_EEPROM_NCOPIES],
                                 const size_t len[PAKON_EEPROM_NCOPIES],
                                 pakon_eeprom *out);

/* "F-135", "F-135+" or "unknown" (never NULL). */
const char *pakon_eeprom_model(uint32_t scanner_type);

#ifdef __cplusplus
}
#endif

#endif /* PAKON_EEPROM_H */
