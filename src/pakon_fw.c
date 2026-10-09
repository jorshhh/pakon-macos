/*
 * pakon_fw.c — FX2 firmware-load sequence. See pakon_fw.h.
 */
#include "pakon_fw.h"
#include "pakon_hex.h"

#include <stdlib.h>
#include <string.h>

#define CPUCS_EZUSB  0x7F92u   /* older EZ-USB */
#define CPUCS_FX2    0xE600u
#define EXT_RAM      0x2000u   /* first external-RAM address */

static int push(pakon_fw_seq *s, uint8_t type, uint8_t req, uint16_t value,
                uint16_t length, const uint8_t *data)
{
    if (s->n == s->cap) {
        size_t cap = s->cap ? 2 * s->cap : 256;
        pakon_fw_xfer *x = realloc(s->x, cap * sizeof *x);
        if (!x)
            return -1;
        s->x = x;
        s->cap = cap;
    }
    pakon_fw_xfer *t = &s->x[s->n++];
    memset(t, 0, sizeof *t);
    t->request_type = type;
    t->request = req;
    t->value = value;
    t->length = length;
    if (data && (type & 0x80) == 0)
        memcpy(t->data, data, length);
    return 0;
}

static int cpucs(pakon_fw_seq *s, uint8_t v)
{
    return push(s, 0x40, 0xA0, CPUCS_EZUSB, 1, &v) ||
           push(s, 0x40, 0xA0, CPUCS_FX2, 1, &v);
}

typedef struct {
    pakon_fw_seq *seq;
    uint8_t req;        /* 0xA0 or 0xA3 */
    int want_ext;       /* -1 any (stage 1, must be internal), 0 int, 1 ext */
    int bad;
} rec_ctx;

static pakon_result on_record(uint32_t addr, const uint8_t *data, uint8_t len,
                              void *user)
{
    rec_ctx *c = user;
    int ext = addr >= EXT_RAM;
    if (len > 16 || addr > 0xFFFF || (c->want_ext < 0 && ext)) {
        c->bad = 1;
        return PAKON_ERR_FIRMWARE;
    }
    if (c->want_ext >= 0 && ext != c->want_ext)
        return PAKON_OK;
    if (push(c->seq, 0x40, c->req, (uint16_t)addr, len, data))
        return PAKON_ERR_FIRMWARE;
    return PAKON_OK;
}

pakon_result pakon_fw_build(const char *stage1, size_t stage1_len,
                            const char *main_image, size_t main_len,
                            pakon_fw_seq *out)
{
    if (!stage1 || !main_image || !out)
        return PAKON_ERR_PARAM;
    memset(out, 0, sizeof *out);
    rec_ctx c = { out, 0xA0, -1, 0 };
    pakon_result r = PAKON_ERR_FIRMWARE;

    if (cpucs(out, 1) || cpucs(out, 1))
        goto fail;
    if (pakon_hex_parse_mem(stage1, stage1_len, on_record, &c) != PAKON_OK)
        goto fail;
    if (cpucs(out, 0) ||
        push(out, 0x40, 0xA4, 0x00A1, 0, NULL))
        goto fail;
    out->personality_at = out->n;
    if (push(out, 0xC0, 0xA9, 0x0000, 8, NULL))
        goto fail;

    c = (rec_ctx){ out, 0xA3, 1, 0 };
    if (pakon_hex_parse_mem(main_image, main_len, on_record, &c) != PAKON_OK)
        goto fail;
    if (cpucs(out, 1))
        goto fail;
    c = (rec_ctx){ out, 0xA0, 0, 0 };
    if (pakon_hex_parse_mem(main_image, main_len, on_record, &c) != PAKON_OK)
        goto fail;
    if (cpucs(out, 1) || cpucs(out, 0))
        goto fail;
    return PAKON_OK;
fail:
    pakon_fw_free(out);
    return r;
}

void pakon_fw_free(pakon_fw_seq *seq)
{
    if (!seq)
        return;
    free(seq->x);
    memset(seq, 0, sizeof *seq);
}

int pakon_fw_personality_is_f135(const uint8_t rec[8])
{
    static const uint8_t want[7] = { 0xC0, 0x05, 0x0F, 0x35, 0xF2, 0x07, 0xAA };
    return rec && memcmp(rec, want, sizeof want) == 0;
}
