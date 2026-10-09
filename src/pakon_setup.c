/*
 * pakon_setup.c — controller init and per-mode configure. See pakon_setup.h.
 */
#include "pakon_setup.h"

#include <string.h>

#define BANK_CCD  0x82u   /* FPGA bank 0x82: CCD timing and control */
#define BANK_AFE  0x84u   /* FPGA bank 0x84: CCD analog front end */

void pakon_seq_init(pakon_seq *seq)
{
    seq->n = 0;
    seq->overflow = 0;
    seq->delay_ms[0] = 0;
}

static void push(pakon_seq *seq, uint8_t type, const uint8_t *data, size_t n)
{
    uint16_t delay = seq->n < PAKON_SEQ_MAX ? seq->delay_ms[seq->n] : 0;
    if (seq->n >= PAKON_SEQ_MAX ||
        pakon_packet_build(&seq->pkt[seq->n], type, data, n) != PAKON_OK) {
        seq->overflow = 1;
        return;
    }
    seq->delay_ms[seq->n] = delay;
    seq->n++;
    if (seq->n < PAKON_SEQ_MAX)
        seq->delay_ms[seq->n] = 0;
}

/* Wait `ms` before the next frame pushed. */
static void delay_next(pakon_seq *seq, uint16_t ms)
{
    if (seq->n < PAKON_SEQ_MAX)
        seq->delay_ms[seq->n] = ms;
}

static void busy_poll(pakon_seq *seq, uint8_t addr)
{
    push(seq, PH_READ_STATUS, &addr, 1);
}

/* Controller write `02 <n+3> <a> <n> <reg> <data>`, then the busy poll. */
static void write_reg(pakon_seq *seq, uint8_t addr, uint8_t reg,
                      const uint8_t *val, uint8_t n)
{
    uint8_t d[PAKON_DATA_MAX];
    d[0] = addr;
    d[1] = n;
    d[2] = reg;
    memcpy(d + 3, val, n);
    push(seq, PH_WRITE, d, 3u + n);
    busy_poll(seq, addr);
}

static void write_u8(pakon_seq *seq, uint8_t addr, uint8_t reg, uint8_t v)
{
    write_reg(seq, addr, reg, &v, 1);
}

/* FPGA register write `02 06 SCN 03 <bank> <reg> <v16 LE>`. */
static void write_fpga(pakon_seq *seq, uint8_t scn, uint8_t bank,
                       uint8_t reg, uint16_t v)
{
    uint8_t val[3] = { reg, (uint8_t)v, (uint8_t)(v >> 8) };
    write_reg(seq, scn, bank, val, 3);
}

static void command(pakon_seq *seq, uint8_t addr, uint8_t cmd)
{
    uint8_t d[3] = { addr, 0x00, cmd };
    push(seq, PH_CMD, d, 3);
    busy_poll(seq, addr);
}

/* Controller read `01 03 <a> <n> <reg>` (no busy poll). */
static void read_reg(pakon_seq *seq, uint8_t addr, uint8_t n, uint8_t reg)
{
    uint8_t d[3] = { addr, n, reg };
    push(seq, PH_READ, d, 3);
}

/* ResetFifos: host `02 04 10 01 84 02`, then LOW command 0x8A. */
static void reset_fifos(pakon_seq *seq, uint8_t low)
{
    const uint8_t host[4] = { AD_HOST, 0x01, 0x84, 0x02 };
    push(seq, PH_WRITE, host, sizeof host);
    command(seq, low, 0x8A);
}

void pakon_setup_pixel_window(uint16_t eeprom_offset, int dual_tap,
                              uint16_t *start, uint16_t *end)
{
    *start = dual_tap ? 3 : 6;
    *end = (uint16_t)(eeprom_offset + (dual_tap ? 1000 : 2000));
}

pakon_result pakon_setup_init(pakon_seq *seq, uint8_t low, uint8_t scn,
                              uint16_t led_period, pakon_setup_state *state)
{
    if (!seq || !state)
        return PAKON_ERR_PARAM;

    write_u8(seq, scn, 0x97, 0x01);              /* SCN controller init */
    write_u8(seq, low, 0x03, 0x01);
    read_reg(seq, low, 0x0C, 0x07);
    write_u8(seq, scn, 0x03, 0x01);
    read_reg(seq, scn, 0x0C, 0x07);
    reset_fifos(seq, low);

    /* Lamp and motherboard temperature warning / fault bands. */
    write_reg(seq, low, 0x8F, (const uint8_t[]){ 0xE8, 0xFF, 0x18, 0x00 }, 4);
    write_reg(seq, low, 0x8C, (const uint8_t[]){ 0xE0, 0xFF, 0x20, 0x00 }, 4);
    write_reg(seq, low, 0x8B, (const uint8_t[]){ 0xF0, 0x00, 0x20, 0x03 }, 4);
    write_reg(seq, low, 0x8D, (const uint8_t[]){ 0xA0, 0x00, 0x70, 0x03 }, 4);
    /* TEC setup: replay as-is, never sweep (safety rule). */
    write_u8(seq, low, 0xD0, 0x00);
    write_u8(seq, low, 0xD1, 0x01);
    write_reg(seq, low, 0x87, (const uint8_t[]){ 0x00, 0x00 }, 2);

    /* LEDs: enable, zero duties with the period word, then all off. */
    write_u8(seq, low, 0x80, 0x01);
    {
        uint8_t duties[12] = { 0 };
        duties[10] = (uint8_t)led_period;
        duties[11] = (uint8_t)(led_period >> 8);
        write_reg(seq, low, 0x82, duties, sizeof duties);
    }
    write_u8(seq, low, 0x80, 0x00);
    write_u8(seq, low, 0x89, 0x00);

    /* CCD FPGA defaults. */
    state->low = low;
    state->scn = scn;
    state->reg6 = 0x0FFD;
    state->reg0 = 0x0060;
    state->reg4 = 0x003E;
    state->reg5 = 0x080E;
    state->low89 = 0x00;
    write_fpga(seq, scn, BANK_CCD, 0x6, state->reg6);
    write_fpga(seq, scn, BANK_CCD, 0x0, state->reg0);
    write_fpga(seq, scn, BANK_CCD, 0xB, 0x0000);
    write_fpga(seq, scn, BANK_CCD, 0x4, state->reg4);
    write_fpga(seq, scn, BANK_CCD, 0x5, state->reg5);
    write_fpga(seq, scn, BANK_CCD, 0x1, 0x0000);   /* exposure R/G/B */
    write_fpga(seq, scn, BANK_CCD, 0x2, 0x0000);
    write_fpga(seq, scn, BANK_CCD, 0x3, 0x0000);
    write_fpga(seq, scn, BANK_CCD, 0xA, 0x0400);
    write_fpga(seq, scn, BANK_AFE, 0x0, 0x0078);
    write_fpga(seq, scn, BANK_AFE, 0x1, 0x0080);

    command(seq, scn, 0xA2);                     /* release the motor drive */
    write_fpga(seq, scn, BANK_CCD, 0x9, 0x0014);   /* status LEDs */
    write_fpga(seq, scn, BANK_CCD, 0x9, 0x0017);

    return seq->overflow ? PAKON_ERR_PARAM : PAKON_OK;
}

pakon_result pakon_setup_configure(pakon_seq *seq, pakon_setup_state *state,
                                   const pakon_scan_mode *mode)
{
    if (!seq || !state || !mode || mode->integration > 0x0FFD)
        return PAKON_ERR_PARAM;
    uint8_t low = state->low, scn = state->scn;

    reset_fifos(seq, low);
    write_reg(seq, low, 0x91, (const uint8_t[]){ (uint8_t)mode->trigger,
                                                 (uint8_t)(mode->trigger >> 8),
                                                 0x01 }, 3);

    /* Order as captured: IR bit, integration, dual-tap bit, pixel window.
     * Unchanged registers are not rewritten. */
    if (mode->ir && !(state->reg0 & 0x100)) {
        state->reg0 |= 0x100;
        write_fpga(seq, scn, BANK_CCD, 0x0, state->reg0);
    }
    if (state->reg6 != mode->integration) {
        state->reg6 = mode->integration;
        write_fpga(seq, scn, BANK_CCD, 0x6, state->reg6);
    }
    if (mode->dual_tap && !(state->reg0 & 0x002)) {
        state->reg0 |= 0x002;
        write_fpga(seq, scn, BANK_CCD, 0x0, state->reg0);
    }
    if (state->reg4 != mode->pixel_start) {
        state->reg4 = mode->pixel_start;
        write_fpga(seq, scn, BANK_CCD, 0x4, state->reg4);
    }
    if (state->reg5 != mode->pixel_end) {
        state->reg5 = mode->pixel_end;
        write_fpga(seq, scn, BANK_CCD, 0x5, state->reg5);
    }
    if (state->low89 != mode->low89) {
        state->low89 = mode->low89;
        write_u8(seq, low, 0x89, state->low89);
    }
    reset_fifos(seq, low);

    return seq->overflow ? PAKON_ERR_PARAM : PAKON_OK;
}

pakon_result pakon_setup_teardown(pakon_seq *seq, const pakon_setup_state *state)
{
    if (!seq || !state)
        return PAKON_ERR_PARAM;
    uint8_t low = state->low, scn = state->scn;

    write_fpga(seq, scn, BANK_CCD, 0x0, (uint16_t)(state->reg0 & ~1u));
    write_u8(seq, low, 0x80, 0x00);              /* LEDs off */
    reset_fifos(seq, low);
    command(seq, low, 0x92);                     /* end acquisition */
    delay_next(seq, 20);
    command(seq, scn, 0xA2);                     /* release the drive */

    return seq->overflow ? PAKON_ERR_PARAM : PAKON_OK;
}
