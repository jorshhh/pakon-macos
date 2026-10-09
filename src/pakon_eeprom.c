/*
 * pakon_eeprom.c — per-unit EEPROM decoder. See pakon_eeprom.h.
 *
 * Mirrors decode_sections() in tools/pakon_eeprom.py; offsets are into a
 * section copy, header included.
 */
#include "pakon_eeprom.h"

#include <math.h>
#include <string.h>

const uint16_t pakon_eeprom_copy_offset[PAKON_EEPROM_NCOPIES] = {
    0x000, 0x400, 0x800, 0xA00
};
const uint16_t pakon_eeprom_copy_len[PAKON_EEPROM_NCOPIES] = {
    PAKON_EEPROM_A_LEN, PAKON_EEPROM_A_LEN, PAKON_EEPROM_B_LEN, PAKON_EEPROM_B_LEN
};

/* Section A per-base {Offset, MotorSpeed, MotorSpeed_Ir} and matrices. */
static const size_t a_base_at[PAKON_EEPROM_NBASES] = { 0x14, 0x1A, 0x20 };
#define A_NEG_MATRIX_AT  0x26
#define A_POS_MATRIX_AT  0x9E
/* Section B per-base {normal, drag, ir, drag_ir}. */
static const size_t b_adjust_at[PAKON_EEPROM_NBASES] = { 8, 16, 24 };
#define B_UNKNOWN_AT     32

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float lef32(const uint8_t *p)
{
    uint32_t bits = le32(p);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

uint32_t pakon_eeprom_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

int pakon_eeprom_copy_valid(pakon_eeprom_copy which,
                            const uint8_t *data, size_t len)
{
    if ((unsigned)which >= PAKON_EEPROM_NCOPIES || !data)
        return 0;
    size_t expected = pakon_eeprom_copy_len[which];
    /* Never trust the stored length beyond checking it equals the fixed one. */
    if (len != expected || le32(data) != expected)
        return 0;
    return le32(data + 4) == pakon_eeprom_crc32(data + 8, len - 8);
}

static void read_matrix(const uint8_t *p, float m[3][10], int *finite)
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 10; c++) {
            m[r][c] = lef32(p + 4 * (10 * r + c));
            if (!isfinite(m[r][c]))
                *finite = 0;
        }
}

pakon_result pakon_eeprom_decode(const uint8_t *const data[PAKON_EEPROM_NCOPIES],
                                 const size_t len[PAKON_EEPROM_NCOPIES],
                                 pakon_eeprom *out)
{
    if (!data || !len || !out)
        return PAKON_ERR_PARAM;
    memset(out, 0, sizeof *out);

    for (int i = 0; i < PAKON_EEPROM_NCOPIES; i++)
        out->valid[i] = pakon_eeprom_copy_valid((pakon_eeprom_copy)i,
                                                data[i], len[i]);

    for (int s = 0; s < 2; s++) {
        int pri = 2 * s, bak = 2 * s + 1;
        size_t n = pakon_eeprom_copy_len[pri];
        if (data[pri] && data[bak] && len[pri] == n && len[bak] == n)
            out->copies_equal[s] = memcmp(data[pri], data[bak], n) == 0;
        else
            out->copies_equal[s] = -1;   /* not a complete comparison */
        out->selected[s] = out->valid[pri] ? pri : out->valid[bak] ? bak : -1;
        if (out->selected[s] < 0)
            pakon_logf(PAKON_LOG_WARN, "eeprom: section %c: neither copy validates",
                       'A' + s);
        else if (!out->valid[pri] || !out->valid[bak])
            pakon_logf(PAKON_LOG_WARN, "eeprom: section %c: %s invalid, %s selected",
                       'A' + s, out->valid[pri] ? "backup" : "primary",
                       out->valid[pri] ? "primary" : "backup");
        else if (!out->copies_equal[s])
            pakon_logf(PAKON_LOG_WARN,
                       "eeprom: section %c: valid copies differ, primary selected",
                       'A' + s);
    }

    int ok = out->selected[0] >= 0 && out->selected[1] >= 0;

    if (out->selected[0] >= 0) {
        const uint8_t *a = data[out->selected[0]];
        out->hardware_version = le32(a + 0x08);
        out->scanner_type     = le32(a + 0x0C);
        out->serial           = le32(a + 0x10);
        if (out->scanner_type != PAKON_EEPROM_TYPE_F135 &&
            out->scanner_type != PAKON_EEPROM_TYPE_F135_PLUS) {
            pakon_logf(PAKON_LOG_WARN, "eeprom: unknown scanner type %u",
                       out->scanner_type);
            ok = 0;
        }
        for (int b = 0; b < PAKON_EEPROM_NBASES; b++) {
            const uint8_t *p = a + a_base_at[b];
            out->base[b].offset         = le16(p);
            out->base[b].motor_speed    = le16(p + 2);
            out->base[b].motor_speed_ir = le16(p + 4);
        }
        int finite = 1;
        read_matrix(a + A_NEG_MATRIX_AT, out->neg_matrix, &finite);
        read_matrix(a + A_POS_MATRIX_AT, out->pos_matrix, &finite);
        if (!finite) {
            pakon_logf(PAKON_LOG_WARN, "eeprom: nonfinite matrix coefficients");
            ok = 0;
        }
    }

    if (out->selected[1] >= 0) {
        const uint8_t *b = data[out->selected[1]];
        for (int i = 0; i < PAKON_EEPROM_NBASES; i++) {
            const uint8_t *p = b + b_adjust_at[i];
            out->adjust[i].normal  = le16(p);
            out->adjust[i].drag    = le16(p + 2);
            out->adjust[i].ir      = le16(p + 4);
            out->adjust[i].drag_ir = le16(p + 6);
        }
        out->section_b_unknown = le32(b + B_UNKNOWN_AT);
    }

    return ok ? PAKON_OK : PAKON_ERR_PROTO;
}

const char *pakon_eeprom_model(uint32_t scanner_type)
{
    switch (scanner_type) {
    case PAKON_EEPROM_TYPE_F135:      return "F-135";
    case PAKON_EEPROM_TYPE_F135_PLUS: return "F-135+";
    default:                          return "unknown";
    }
}
