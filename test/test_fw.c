/*
 * test_fw — FX2 firmware-load sequence from Intel HEX (no hardware).
 *
 * Always: a synthetic stage 1 and main image give the documented order.
 * When firmware/PknLdr.hex and firmware/Pakon7.hex exist (user-supplied OEM
 * files, see firmware/README.md): the generated sequence must equal the
 * capture resources/f135.pakfw transfer for transfer.
 * Usage: test_fw REPO_DIR
 */
#include "pakon_fw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
        if (cond) { printf("ok   - %s\n", (msg)); }             \
        else { printf("FAIL - %s\n", (msg)); failures++; }      \
    } while (0)

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        b[n] = 0;
        *len = (size_t)n;
    }
    return b;
}

static void synthetic(void)
{
    const char *stage1 = ":02000000AABB99\n:00000001FF\n";
    /* main: one external record at 0x2000 (11 22), one internal at 0x0010 (33). */
    const char *main_image = ":022000001122AB\n:0100100033BC\n:00000001FF\n";
    pakon_fw_seq s;
    if (pakon_fw_build(stage1, strlen(stage1), main_image, strlen(main_image),
                       &s) != PAKON_OK) {
        CHECK(0, "synthetic sequence builds");
        return;
    }
    /* 4 CPUCS, stage1, 2 CPUCS, A4, A9, ext, 2 CPUCS, int, 4 CPUCS = 17 */
    CHECK(s.n == 17, "synthetic: 17 transfers");
    CHECK(s.x[0].request == 0xA0 && s.x[0].value == 0x7F92 && s.x[0].data[0] == 1 &&
          s.x[1].value == 0xE600, "starts with CPUCS = 1 at 0x7F92 and 0xE600");
    CHECK(s.x[4].request == 0xA0 && s.x[4].value == 0 && s.x[4].length == 2 &&
          s.x[4].data[0] == 0xAA, "stage 1 to internal RAM with 0xA0");
    CHECK(s.x[7].request == 0xA4 && s.x[7].value == 0x00A1 &&
          s.personality_at == 8 && s.x[8].request_type == 0xC0 &&
          s.x[8].request == 0xA9 && s.x[8].length == 8, "personality select and read");
    CHECK(s.x[9].request == 0xA3 && s.x[9].value == 0x2000,
          "external records with 0xA3");
    CHECK(s.x[12].request == 0xA0 && s.x[12].value == 0x0010,
          "internal records with 0xA0 after CPUCS = 1");
    CHECK(s.x[16].value == 0xE600 && s.x[16].data[0] == 0, "ends with CPUCS = 0");
    pakon_fw_free(&s);

    const char *bad_stage1 = ":022000001122AB\n:00000001FF\n";
    CHECK(pakon_fw_build(bad_stage1, strlen(bad_stage1), main_image,
                         strlen(main_image), &s) == PAKON_ERR_FIRMWARE,
          "stage 1 above 0x2000 refused");

    const uint8_t f135[8] = { 0xC0, 0x05, 0x0F, 0x35, 0xF2, 0x07, 0xAA, 0x04 };
    const uint8_t f335[8] = { 0xC0, 0x05, 0x0F, 0x35, 0xF2, 0x08, 0xAA, 0x04 };
    CHECK(pakon_fw_personality_is_f135(f135) && !pakon_fw_personality_is_f135(f335),
          "personality: F235_AA07 accepted, AA08 refused");
}

static void against_capture(const char *repo)
{
    char p1[1024], p2[1024], p3[1024];
    snprintf(p1, sizeof p1, "%s/firmware/PknLdr.hex", repo);
    snprintf(p2, sizeof p2, "%s/firmware/Pakon7.hex", repo);
    snprintf(p3, sizeof p3, "%s/resources/f135.pakfw", repo);
    size_t l1 = 0, l2 = 0;
    char *s1 = slurp(p1, &l1), *s2 = slurp(p2, &l2);
    FILE *cap = fopen(p3, "r");
    if (!s1 || !s2 || !cap) {
        printf("skip - OEM firmware not in firmware/ (see firmware/README.md)\n");
        free(s1);
        free(s2);
        if (cap)
            fclose(cap);
        return;
    }
    pakon_fw_seq s;
    if (pakon_fw_build(s1, l1, s2, l2, &s) != PAKON_OK) {
        CHECK(0, "OEM sequence builds");
        fclose(cap);
        free(s1);
        free(s2);
        return;
    }

    char line[512];
    size_t i = 0, mismatch = (size_t)-1;
    while (fgets(line, sizeof line, cap)) {
        if (line[0] == '#')
            continue;
        unsigned bm, rq, wv, wi, wl;
        int used = 0;
        if (sscanf(line, "%x %x %x %x %x%n", &bm, &rq, &wv, &wi, &wl, &used) != 5)
            continue;
        uint8_t d[16] = { 0 };
        size_t dn = 0;
        for (char *q = line + used; dn < sizeof d; q += 2) {
            while (*q == ' ')
                q++;
            unsigned b;
            if (sscanf(q, "%2x", &b) != 1)
                break;
            d[dn++] = (uint8_t)b;
        }
        if (i >= s.n) {
            mismatch = i;
            break;
        }
        const pakon_fw_xfer *x = &s.x[i];
        int same = x->request_type == bm && x->request == rq && x->value == wv &&
                   x->index == wi && x->length == wl &&
                   ((bm & 0x80) || memcmp(x->data, d, wl) == 0);
        if (!same && mismatch == (size_t)-1)
            mismatch = i;
        i++;
    }
    fclose(cap);
    char msg[160];
    snprintf(msg, sizeof msg, "%zu generated transfers equal the %zu in f135.pakfw",
             s.n, i);
    CHECK(mismatch == (size_t)-1 && i == s.n, msg);
    if (mismatch != (size_t)-1)
        printf("     first difference at transfer %zu\n", mismatch);
    pakon_fw_free(&s);
    free(s1);
    free(s2);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s REPO_DIR\n", argv[0]);
        return 2;
    }
    synthetic();
    against_capture(argv[1]);
    if (failures) {
        printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall tests passed\n");
    return 0;
}
