/*
 * pakon_fw.h — FX2 firmware-load sequence built from Intel HEX (pure).
 *
 * The cold scanner (0f05:f235) is an EZ-USB FX2 that needs two downloads
 * (pakon-reference usb-identity-and-firmware.md):
 *   1. hold the 8051 in reset: CPUCS = 1 (written at 0x7F92 and 0xE600)
 *   2. stage-1 loader to internal RAM, vendor 0xA0 (firmware/PknLdr.hex,
 *      extracted from the OEM F235Ldr.sys by tools/extract_fx2_loader.py)
 *   3. run it: CPUCS = 0
 *   4. personality read: OUT 0xA4 wValue 0x00A1, then IN 0xA9 (8 bytes)
 *   5. main image (firmware/Pakon7.hex): records at >= 0x2000 to external
 *      RAM with 0xA3 (served by the stage-1 loader), then CPUCS = 1 and the
 *      records below 0x2000 to internal RAM with 0xA0
 *   6. CPUCS = 1, CPUCS = 0: run it; the device re-enumerates as 0f05:f135
 * One transfer per HEX record, in file order. test_fw checks the result
 * against the capture resources/f135.pakfw byte for byte when the two OEM
 * files are present locally. No libusb here; pakon_usb sends it.
 */
#ifndef PAKON_FW_H
#define PAKON_FW_H

#include <stdint.h>
#include <stddef.h>

#include "pakon_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  request_type;   /* 0x40 vendor OUT, 0xC0 vendor IN */
    uint8_t  request;        /* 0xA0, 0xA3, 0xA4, 0xA9 */
    uint16_t value;          /* address, or the 0xA4 select */
    uint16_t index;
    uint16_t length;
    uint8_t  data[16];       /* OUT payload */
} pakon_fw_xfer;

typedef struct {
    pakon_fw_xfer *x;
    size_t n, cap;
    size_t personality_at;   /* index of the 0xA9 personality read */
} pakon_fw_seq;

/* Build the load sequence from the two HEX texts. Stage 1 must lie below
 * 0x2000. Returns PAKON_ERR_FIRMWARE on a malformed HEX or a record over 16
 * bytes. Free with pakon_fw_free. */
pakon_result pakon_fw_build(const char *stage1, size_t stage1_len,
                            const char *main_image, size_t main_len,
                            pakon_fw_seq *out);
void pakon_fw_free(pakon_fw_seq *seq);

/* The F-135/F-135+ personality: C0 boot record for 0f05:f235 revision
 * 0xAA07 (c0 05 0f 35 f2 07 aa xx), the key Pakon7.hex is for. */
int pakon_fw_personality_is_f135(const uint8_t rec[8]);

#ifdef __cplusplus
}
#endif

#endif /* PAKON_FW_H */
