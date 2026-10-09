/*
 * pakon_setup.h — controller init and per-mode configure, built in code.
 *
 * Builds the frames the OEM sends after model detection (from the SCN 0x97
 * controller init) up to, not including, the first acquire. Pure: frames go
 * into a pakon_seq, nothing is sent here (pakon_cmd_run_seq sends one).
 *
 * Transcribed from five captures (resources/scan.pakscan, F-135 serial 3054;
 * resources/f135plus/base16/base8/base4/base4_ir.pakscan, F-135+ serial
 * 16402). With the controller addresses as parameters, init is identical on
 * both models except the LED period word; configure differs only by the
 * per-mode values in pakon_scan_mode. test/test_setup.c checks the generated
 * frames against every one of those captures.
 *
 * Left out, because their timing is not part of this sequence: event-service
 * traffic (host polls, event reads/acks, temperature and 0x90 reads) and the
 * front-panel status-LED word (bank 0x82 reg 9 = 0x0313), which the captures
 * write at different points.
 */
#ifndef PAKON_SETUP_H
#define PAKON_SETUP_H

#include <stdint.h>
#include <stddef.h>

#include "pakon_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAKON_SEQ_MAX  96u

/* A list of frames in send order. A PH_READ_STATUS frame stands for the
 * OEM busy poll: the sender repeats it until bit 0 clears (see
 * pakon_cmd_run_seq). */
typedef struct {
    pakon_packet pkt[PAKON_SEQ_MAX];
    size_t n;
    int overflow;   /* set if a frame did not fit; the sequence is unusable */
} pakon_seq;

/* LOW 0x82 period word sent with all-zero duties during init, as captured.
 * Constant across the four F-135+ modes; how it is derived is not known. */
#define PAKON_SETUP_LED_PERIOD_F135       0x0997u
#define PAKON_SETUP_LED_PERIOD_F135_PLUS  0x03D6u

/* Values the FPGA and LOW controller hold after init; configure writes a
 * register only when its value changes, as the OEM does. */
typedef struct {
    uint8_t  low, scn;          /* controller addresses from the probes */
    uint16_t reg0, reg4, reg5, reg6;   /* bank 0x82 */
    uint8_t  low89;             /* LOW 0x89 */
} pakon_setup_state;

/* One resolution/IR mode. All values come from the caller: the trigger and
 * integration per mode (docs/TLB_FINDINGS.md, captures), the pixel window
 * from the unit's EEPROM Offset (pakon_setup_pixel_window). */
typedef struct {
    uint16_t trigger;       /* LOW 0x91 scan-line trigger value */
    int      ir;            /* bank 0x82 reg 0 bit 0x100 */
    int      dual_tap;      /* bank 0x82 reg 0 bit 0x002 */
    uint16_t integration;   /* bank 0x82 reg 6, <= 0xFFD */
    uint16_t pixel_start;   /* bank 0x82 reg 4 */
    uint16_t pixel_end;     /* bank 0x82 reg 5 */
    uint8_t  low89;         /* LOW 0x89 per-resolution flag (1 at F-135+ Base 8) */
} pakon_scan_mode;

void pakon_seq_init(pakon_seq *seq);

/*
 * Pixel window for calibration: start = optically black pixels (6, or 3
 * dual-tap); end = EEPROM Offset for the base + line height (2000, or 1000
 * dual-tap). Matches all five captures.
 */
void pakon_setup_pixel_window(uint16_t eeprom_offset, int dual_tap,
                              uint16_t *start, uint16_t *end);

/* Controller init after model detection. Fills `state`. */
pakon_result pakon_setup_init(pakon_seq *seq, uint8_t low, uint8_t scn,
                              uint16_t led_period, pakon_setup_state *state);

/* Mode configure after init (or a previous configure). Updates `state`. */
pakon_result pakon_setup_configure(pakon_seq *seq, pakon_setup_state *state,
                                   const pakon_scan_mode *mode);

#ifdef __cplusplus
}
#endif

#endif /* PAKON_SETUP_H */
