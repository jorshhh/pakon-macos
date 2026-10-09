/*
 * pakon_setup.h — controller init and per-mode configure, built in code.
 *
 * Builds the frames the OEM sends after model detection (from the SCN 0x97
 * controller init) up to, not including, the first acquire, and the
 * teardown after the last image read. Pure: frames go
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
    uint16_t delay_ms[PAKON_SEQ_MAX];   /* wait before sending pkt[i] */
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

/*
 * Teardown after a scan (OEM FN_bAfterScan): bank 0x82 reg 0 back to the
 * configured value (acquire bit clear; clearing it is what lets 0xA2 stop
 * the motor), LEDs off, ResetFifos, LOW 0x92 (end acquisition), 20 ms,
 * SCN 0xA2 (release the drive). Same in all seven captures.
 */
pakon_result pakon_setup_teardown(pakon_seq *seq, const pakon_setup_state *state);

/* ---- Event service (OEM Thread_PpbInterrupt, docs/PROTOCOL.md) -----------
 *
 * Poll the host `03 01 10`; if its flags carry PAKON_HOST_FLAG_EVENT, read
 * each controller's event status (LOW, then SCN) and, for each one pending,
 * send the follow-up built by pakon_event_followup. pakon_cmd_service_events
 * runs the whole loop over USB.
 */
#define PAKON_HOST_FLAG_EVENT     0x80u
#define PAKON_HOST_FLAG_OVERFLOW  0x02u   /* image FIFO overflow */

/* `01 03 <a> 01 02`: read the 1-byte event status. */
void pakon_event_read_frame(pakon_packet *pkt, uint8_t addr);

/* Reply `01 03 <a> <flags> <status>` to that read: returns 1 and sets
 * `*status` if an event is pending (flags bit 0x80), else 0. */
int pakon_event_parse(const pakon_packet *reply, uint8_t addr, uint8_t *status);

/*
 * Follow-up for a pending event: the ack `02 05 <a> 02 06 00 <status>` and
 * its busy poll; for LOW, then the sensor/DX block `01 03 LOW 1e 90` and,
 * if status & 0x5B (lamp), the temperatures `0x84` and `0x88`.
 *
 * From the 95 acks in our seven captures (all from LOW): 0x90 followed 93
 * of them, whatever the status, so it is read every time rather than only on
 * status & 0xA4 as TLB.dll's rule reads; 0x84/0x88 followed every 0x02 and
 * 0x40 ack. The lamp-flags read 0x83 (2 of 95) depends on reply data we do
 * not have, and is left out. test_setup reproduces 91 of the 95 exactly.
 */
pakon_result pakon_event_followup(pakon_seq *seq, uint8_t addr, uint8_t status,
                                  int is_low);

#ifdef __cplusplus
}
#endif

#endif /* PAKON_SETUP_H */
