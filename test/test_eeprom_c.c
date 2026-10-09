/*
 * test_eeprom_c — hardware-free tests for the C EEPROM decoder.
 *
 * Decodes the in-repo fixtures (test/fixtures/eeprom/) and checks the values
 * test/test_eeprom.py asserts for the Python decoder, plus CRC fallback and
 * corruption cases. Usage: test_eeprom_c FIXTURE_DIR
 */
#include "pakon_eeprom.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
        if (cond) { printf("ok   - %s\n", (msg)); }             \
        else { printf("FAIL - %s\n", (msg)); failures++; }      \
    } while (0)

static const char *copy_name[PAKON_EEPROM_NCOPIES] = {
    "sectionA_primary", "sectionA_backup", "sectionB_primary", "sectionB_backup"
};

typedef struct {
    uint8_t buf[PAKON_EEPROM_NCOPIES][PAKON_EEPROM_A_LEN + 1];
    const uint8_t *data[PAKON_EEPROM_NCOPIES];
    size_t len[PAKON_EEPROM_NCOPIES];
} unit;

static int load_unit(const char *root, const char *name, unit *u)
{
    for (int i = 0; i < PAKON_EEPROM_NCOPIES; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s/eeprom_0x52_%s.bin",
                 root, name, copy_name[i]);
        FILE *f = fopen(path, "rb");
        if (!f) {
            printf("cannot open %s\n", path);
            return 0;
        }
        u->len[i] = fread(u->buf[i], 1, sizeof u->buf[i], f);
        u->data[i] = u->buf[i];
        fclose(f);
    }
    return 1;
}

/* Rewrite the CRC field of a copy after an edit. */
static void fix_crc(unit *u, int i)
{
    uint32_t c = pakon_eeprom_crc32(u->buf[i] + 8, u->len[i] - 8);
    for (int k = 0; k < 4; k++)
        u->buf[i][4 + k] = (uint8_t)(c >> (8 * k));
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s FIXTURE_DIR\n", argv[0]);
        return 2;
    }
    const char *root = argv[1];
    pakon_eeprom e;
    unit u;

    /* zlib check value. */
    CHECK(pakon_eeprom_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u,
          "crc32 matches zlib check value");

    /* Base F-135, all four copies valid. */
    if (!load_unit(root, "F135-2233", &u))
        return 1;
    CHECK(pakon_eeprom_decode(u.data, u.len, &e) == PAKON_OK, "2233 decodes");
    CHECK(e.valid[0] && e.valid[1] && e.valid[2] && e.valid[3],
          "2233 all copies valid");
    CHECK(e.copies_equal[0] == 1 && e.copies_equal[1] == 1, "2233 copies equal");
    CHECK(e.selected[0] == PAKON_EEPROM_A_PRIMARY &&
          e.selected[1] == PAKON_EEPROM_B_PRIMARY, "2233 primaries selected");
    CHECK(e.serial == 2233 && e.scanner_type == PAKON_EEPROM_TYPE_F135 &&
          !strcmp(pakon_eeprom_model(e.scanner_type), "F-135"),
          "2233 serial and model");
    CHECK(e.base[PAKON_EEPROM_BASE4].offset == 27 &&
          e.base[PAKON_EEPROM_BASE4].motor_speed == 8162 &&
          e.base[PAKON_EEPROM_BASE4].motor_speed_ir == 6119, "2233 base 4");
    CHECK(e.base[PAKON_EEPROM_BASE16].offset == 54 &&
          e.base[PAKON_EEPROM_BASE16].motor_speed == 2325 &&
          e.base[PAKON_EEPROM_BASE16].motor_speed_ir == 1530, "2233 base 16");
    CHECK(e.adjust[PAKON_EEPROM_BASE4].ir == 1008, "2233 base 4 ir adjust");
    CHECK(e.pos_matrix[0][0] == 0.25f, "2233 PosMatrix[0][0] = 0.25");
    CHECK(fabsf(e.neg_matrix[0][0] - 0.27680f) < 1e-4f,
          "2233 NegMatrix[0][0] ~ 0.27680");

    /* F-135+ with a corrupt section A primary: falls back to the backup. */
    if (!load_unit(root, "F135plus-16402", &u))
        return 1;
    CHECK(pakon_eeprom_decode(u.data, u.len, &e) == PAKON_OK, "16402 decodes");
    CHECK(!e.valid[0] && e.valid[1] && e.valid[2] && e.valid[3],
          "16402 only section A primary invalid");
    CHECK(e.selected[0] == PAKON_EEPROM_A_BACKUP, "16402 section A backup selected");
    CHECK(e.copies_equal[0] == 0, "16402 section A copies differ");
    CHECK(e.serial == 16402 && !strcmp(pakon_eeprom_model(e.scanner_type), "F-135+"),
          "16402 serial and model");
    /* Per-mode motor speeds seen in serial 16402's captures (STATUS.md). */
    CHECK(e.base[PAKON_EEPROM_BASE16].motor_speed == 5900 &&
          e.base[PAKON_EEPROM_BASE8].motor_speed == 11434 &&
          e.base[PAKON_EEPROM_BASE4].motor_speed == 25726 &&
          e.base[PAKON_EEPROM_BASE4].motor_speed_ir == 19278,
          "16402 motor speeds match its captures");
    CHECK(e.pos_matrix[0][1] == 0.0f, "16402 PosMatrix[0][1] = 0");

    /* Section B primary zeroed: backup used. */
    load_unit(root, "F135-2233", &u);
    memset(u.buf[PAKON_EEPROM_B_PRIMARY], 0, u.len[PAKON_EEPROM_B_PRIMARY]);
    CHECK(pakon_eeprom_decode(u.data, u.len, &e) == PAKON_OK &&
          e.selected[1] == PAKON_EEPROM_B_BACKUP, "section B backup fallback");

    /* Missing copy: no comparison, primary still used. */
    load_unit(root, "F135-2233", &u);
    u.data[PAKON_EEPROM_B_BACKUP] = NULL;
    CHECK(pakon_eeprom_decode(u.data, u.len, &e) == PAKON_OK &&
          e.copies_equal[1] == -1 && !e.valid[PAKON_EEPROM_B_BACKUP] &&
          e.selected[1] == PAKON_EEPROM_B_PRIMARY, "missing backup tolerated");

    /* Both copies of a section bad: not decodable, section left zeroed. */
    for (int s = 0; s < 2; s++) {
        load_unit(root, "F135-2233", &u);
        memset(u.buf[2 * s], 0xFF, u.len[2 * s]);
        memset(u.buf[2 * s + 1], 0xFF, u.len[2 * s + 1]);
        int bad = pakon_eeprom_decode(u.data, u.len, &e) == PAKON_ERR_PROTO &&
                  e.selected[s] == -1 && e.selected[1 - s] >= 0;
        bad = bad && (s == 0 ? e.serial == 0 : e.adjust[0].ir == 0);
        CHECK(bad, s == 0 ? "section A both bad: not decodable"
                          : "section B both bad: not decodable");
    }

    /* Truncated, oversized, bad length field, flipped payload bit. */
    load_unit(root, "F135-2233", &u);
    {
        const uint8_t *a = u.data[PAKON_EEPROM_A_PRIMARY];
        size_t n = u.len[PAKON_EEPROM_A_PRIMARY];
        CHECK(pakon_eeprom_copy_valid(PAKON_EEPROM_A_PRIMARY, a, n), "intact copy valid");
        CHECK(!pakon_eeprom_copy_valid(PAKON_EEPROM_A_PRIMARY, a, 7), "short header rejected");
        CHECK(!pakon_eeprom_copy_valid(PAKON_EEPROM_A_PRIMARY, a, n - 1), "truncated rejected");
        CHECK(!pakon_eeprom_copy_valid(PAKON_EEPROM_A_PRIMARY, a, n + 1), "oversized rejected");
        CHECK(!pakon_eeprom_copy_valid(PAKON_EEPROM_B_PRIMARY, a, n), "wrong section rejected");
        u.buf[PAKON_EEPROM_A_PRIMARY][n - 1] ^= 1;
        CHECK(!pakon_eeprom_copy_valid(PAKON_EEPROM_A_PRIMARY, a, n), "flipped bit rejected");
        u.buf[PAKON_EEPROM_A_PRIMARY][n - 1] ^= 1;
        u.buf[PAKON_EEPROM_A_PRIMARY][0] = 0xFF;
        CHECK(!pakon_eeprom_copy_valid(PAKON_EEPROM_A_PRIMARY, a, n), "bad length field rejected");
    }

    /* Valid but different copies: primary wins. */
    load_unit(root, "F135-2233", &u);
    u.buf[PAKON_EEPROM_A_BACKUP][0x10] = 0xE7;   /* serial -> 999-ish */
    fix_crc(&u, PAKON_EEPROM_A_BACKUP);
    CHECK(pakon_eeprom_decode(u.data, u.len, &e) == PAKON_OK &&
          e.valid[PAKON_EEPROM_A_BACKUP] && e.copies_equal[0] == 0 &&
          e.selected[0] == PAKON_EEPROM_A_PRIMARY && e.serial == 2233,
          "valid differing copies: primary selected");

    /* Unknown type and a NaN coefficient: decoded but not usable. */
    load_unit(root, "F135-2233", &u);
    u.buf[PAKON_EEPROM_A_PRIMARY][0x0C] = 0x0F;
    u.buf[PAKON_EEPROM_A_PRIMARY][0x0D] = 0x27;  /* 9999 */
    memcpy(u.buf[PAKON_EEPROM_A_PRIMARY] + 0x26, "\x00\x00\xc0\x7f", 4);  /* NaN */
    fix_crc(&u, PAKON_EEPROM_A_PRIMARY);
    CHECK(pakon_eeprom_decode(u.data, u.len, &e) == PAKON_ERR_PROTO &&
          e.scanner_type == 9999 && isnan(e.neg_matrix[0][0]) &&
          !strcmp(pakon_eeprom_model(e.scanner_type), "unknown"),
          "unknown type and NaN matrix not decodable");

    CHECK(pakon_eeprom_decode(NULL, u.len, &e) == PAKON_ERR_PARAM, "NULL args rejected");

    if (failures) {
        printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall tests passed\n");
    return 0;
}
