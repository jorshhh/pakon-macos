/*
 * pakon_cmd.h — command primitive (glue over transport + protocol).
 *
 * This is the one place allowed to use BOTH layers: it serializes a protocol
 * frame (pakon_proto) and moves it over the transport (pakon_usb) on the
 * confirmed EP1 command channel. The pure layers stay independent; this is the
 * coordination point. The caller must have claimed interface 0 first
 * (pakon_usb_claim(dev, 0, 0)).
 */
#ifndef PAKON_CMD_H
#define PAKON_CMD_H

#include "pakon_usb.h"
#include "pakon_proto.h"
#include "pakon_setup.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Send command frame `cmd` on EP1 OUT and read the reply on EP1 IN into
 * `reply`. Returns PAKON_OK on a well-formed reply (the caller inspects
 * pakon_packet_status(reply)); PAKON_ERR_TIMEOUT / _USB / _PROTO otherwise.
 * `reply` may be NULL if no reply is expected.
 */
pakon_result pakon_cmd(pakon_dev *dev, const pakon_packet *cmd,
                       pakon_packet *reply, unsigned timeout_ms);

/* Convenience: build a frame from (type, data, dlen) and run pakon_cmd. */
pakon_result pakon_cmd_raw(pakon_dev *dev, uint8_t type,
                           const uint8_t *data, size_t dlen,
                           pakon_packet *reply, unsigned timeout_ms);

/*
 * Best-effort bridge open: HostReset (cmd 0x85) + HostSetMode (write 0x8f 00)
 * to AD_HOST. The bridge only replies to these on the FIRST open after
 * power-on or firmware load; on later opens the replies time out while the
 * bridge keeps working, and the OEM fires HostReset without reading replies —
 * so reply timeouts here are tolerated, not errors. Required before any PIC
 * traffic flows in a fresh power session.
 */
void pakon_bridge_open(pakon_dev *dev, unsigned timeout_ms);

/*
 * PIC presence probe — the OEM's model-detection primitive. Sends the probe
 * frame `04 03 <addr> 00 00` and classifies the reply. Confirmed on both a
 * real F-135 (via captures) and a real F-135+ (live): the F-135 PICs answer
 * at 0x20/0x24, the Plus PICs at 0x40/0x44, each set absent on the other
 * model. See docs/SCANNER_FAMILY.md.
 */
typedef enum {
    PAKON_PIC_PRESENT,   /* well-formed reply, status PS_SUCCESS */
    PAKON_PIC_ABSENT,    /* well-formed reply, status PS_NOT_ACKED */
    PAKON_PIC_ERROR      /* transport failure, malformed reply, or any other
                            status (bus error, busy, ...) — NOT the same as
                            absent; callers must not conclude a model from it */
} pakon_pic_state;

pakon_pic_state pakon_probe_pic(pakon_dev *dev, uint8_t pic_address,
                                uint8_t *status_out, unsigned timeout_ms);

/*
 * Send a generated sequence (pakon_setup) with the OEM reply rules
 * (PPB_CheckReply, docs/PROTOCOL.md):
 *   - WRITE/COMMAND: reply `07 02 <a> <status>`; 0/8 OK, 3/6/9 retried (3
 *     tries), anything else PAKON_ERR_STATUS.
 *   - STATUS poll `03 01 <a>`: repeated until flags bit 0 (busy) clears, up to
 *     44 tries with a growing delay; else PAKON_ERR_TIMEOUT.
 *   - READ/STATUS replies must echo the type and address; flags 0x20, or 0x04
 *     from a controller, are PAKON_ERR_STATUS.
 * Refuses type 0 frames and bootloader addresses before sending anything.
 */
pakon_result pakon_cmd_run_seq(pakon_dev *dev, const pakon_seq *seq,
                               unsigned timeout_ms);

/*
 * One pass of the event service: poll the host `03 01 10`; if an event is
 * pending, read LOW then SCN event status and send pakon_event_followup for
 * each pending one. `*host_flags` gets the host flags (0x02 = FIFO overflow);
 * `*serviced` is incremented per controller event acknowledged. Either may
 * be NULL. Call every image read while scanning (the OEM polls every 1 ms).
 */
pakon_result pakon_cmd_service_events(pakon_dev *dev, uint8_t low, uint8_t scn,
                                      unsigned timeout_ms, uint8_t *host_flags,
                                      unsigned *serviced);

#ifdef __cplusplus
}
#endif

#endif /* PAKON_CMD_H */
