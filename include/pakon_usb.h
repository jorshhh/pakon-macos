/*
 * pakon_usb.h — the transport layer.
 *
 * Owns the libusb context, device enumeration/classification, firmware
 * download, interface claiming, and raw bulk send/recv. It knows nothing about
 * Pakon packet framing (no pakon_proto types here). The only shared vocabulary
 * with the protocol layer is pakon_result, which lives in pakon_log.h.
 *
 * Everything below is declared for the full design but is STUBBED in Phase 0:
 * enumeration, firmware load, and bulk I/O require hardware (Phases 1-2) and
 * return PAKON_ERR_UNIMPLEMENTED until then.
 */
#ifndef PAKON_USB_H
#define PAKON_USB_H

#include <stdint.h>
#include <stddef.h>

#include "pakon_log.h"   /* for pakon_result */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pakon identities, CONFIRMED on the F-135 hardware via USB capture:
 *   - COLD  = 0F05:F235 = EEPROM bootstrap (no strings); needs a stage-2
 *             firmware download. Implements no application protocol.
 *   - WARM  = 0F05:F135 = operational ("Pakon F135-USB Film Scanner") after
 *             the firmware download; this is the device we drive.
 * The PID is set by the booted firmware, not a clean model code, so other
 * models (F-235/F-335) may differ — those are theoretical until tested.
 */
#define PAKON_VID        0x0F05
#define PAKON_WARM_VID   PAKON_VID
#define PAKON_WARM_PID   0xF135   /* operational (this F-135 unit) */
#define PAKON_COLD_VID   PAKON_VID
#define PAKON_COLD_PID   0xF235   /* EEPROM bootstrap (this F-135 unit) */

/* True if vid:pid is the operational (warm) Pakon. */
int pakon_is_warm_id(uint16_t vid, uint16_t pid);

/*
 * Operational (f135) endpoints, CONFIRMED from a scan capture: interface 0,
 * single setting, 3 bulk endpoints.
 */
#define PAKON_EP_CMD_OUT   0x01   /* command frames host -> device */
#define PAKON_EP_CMD_IN    0x81   /* command reply / status device -> host */
#define PAKON_EP_IMAGE_IN  0x86   /* bulk image stream device -> host */

typedef enum {
    PAKON_DEV_UNKNOWN = 0,
    PAKON_DEV_COLD,    /* FX2 bootloader, needs firmware download */
    PAKON_DEV_WARM     /* 0F05:Fx35, ready for the protocol layer */
} pakon_dev_class;

/* Summary of one device on the bus, for the diagnostic listing that helps
 * confirm the cold VID/PID at STOP POINT A. */
typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint8_t  bus;
    uint8_t  address;
    uint8_t  dev_class;     /* bDeviceClass */
} pakon_usb_devinfo;

/* Opaque transport context (wraps libusb_context). */
typedef struct pakon_ctx pakon_ctx;

/* Opaque open-device handle (wraps libusb_device_handle + endpoint map). */
typedef struct pakon_dev pakon_dev;

/* Describes one of the warm device's endpoints, for the Phase 1 dump. The
 * interface/altsetting context matters: FX2 devices often expose their bulk
 * endpoints only in a non-default alternate setting, so Phase 2 must know which
 * interface+alt to select before the endpoints are usable. */
typedef struct {
    uint8_t address;        /* bEndpointAddress (dir bit included) */
    uint8_t attributes;     /* bmAttributes (transfer type) */
    uint16_t max_packet;    /* wMaxPacketSize */
    uint8_t interface;      /* bInterfaceNumber it lives on */
    uint8_t altsetting;     /* bAlternateSetting it lives in */
} pakon_endpoint;

/* ---- context lifecycle ---- */

pakon_result pakon_usb_init(pakon_ctx **out_ctx);
void         pakon_usb_exit(pakon_ctx *ctx);

/* ---- enumeration / classification (Phase 1) ---- */

/*
 * Scan the bus and report whether a cold or warm Pakon is present.
 * `out_class` is set to PAKON_DEV_UNKNOWN when nothing matches; the call then
 * returns PAKON_ERR_NO_DEVICE. A warm match uses the documented 0F05:F135
 * identity; a cold match is only possible once PAKON_COLD_VID/PID is filled in
 * after STOP POINT A (it is 0 until then, so cold never matches yet).
 */
pakon_result pakon_usb_find(pakon_ctx *ctx, pakon_dev_class *out_class);

/*
 * Diagnostic: copy up to `max` device summaries for everything on the bus into
 * `arr`, writing the real count to `*out_count`. Used by `pakon_probe --list`
 * to identify the scanner's cold VID/PID before it is hardcoded anywhere.
 */
pakon_result pakon_usb_list(pakon_ctx *ctx, pakon_usb_devinfo *arr,
                            size_t max, size_t *out_count);

/* ---- firmware download (Phase 1) ---- */

/*
 * Replay a captured firmware-load script (.pakfw, e.g. resources/f135.pakfw)
 * to the cold FX2 device and wait for it to re-enumerate as the warm Pakon.
 */
pakon_result pakon_usb_load_firmware(pakon_ctx *ctx, const char *script_path);

/*
 * Load the firmware from Intel HEX, no capture: the stage-1 loader
 * (firmware/PknLdr.hex, from tools/extract_fx2_loader.py) and the main image
 * (firmware/Pakon7.hex), sent as built by pakon_fw_build. Stops before the
 * main image unless the personality the stage-1 loader reports is the
 * F-135/F-135+ one (F235_AA07). Waits for the warm device.
 */
pakon_result pakon_usb_load_firmware_hex(pakon_ctx *ctx, const char *stage1_path,
                                         const char *main_path);

/* ---- open / close (Phase 1-2) ---- */

pakon_result pakon_usb_open(pakon_ctx *ctx, pakon_dev **out_dev);
void         pakon_usb_close(pakon_dev *dev);

/* VID/PID of the opened device (e.g. to report the model). */
void pakon_usb_dev_ids(const pakon_dev *dev, uint16_t *vid, uint16_t *pid);

/*
 * Copy up to `max` endpoint descriptors of the open device into `eps`; the
 * actual count is written to `*out_count`. STUB.
 */
pakon_result pakon_usb_endpoints(pakon_dev *dev, pakon_endpoint *eps,
                                 size_t max, size_t *out_count);

/* ---- interface claim / alt-setting selection (Phase 2) ---- */

/*
 * Claim interface `ifc` and select alternate setting `alt`. On Linux this
 * auto-detaches any kernel driver first. Must be called before send/recv so
 * the right endpoints are active (the FX2 bulk endpoints only exist in a
 * non-default alt setting). Records the current ifc/alt so send/recv can
 * dispatch bulk vs interrupt transfers correctly.
 */
pakon_result pakon_usb_claim(pakon_dev *dev, uint8_t ifc, uint8_t alt);
pakon_result pakon_usb_release(pakon_dev *dev);

/* ---- raw I/O on an explicit endpoint (Phase 2) ---- */

/*
 * Transfer on endpoint `ep` (full bEndpointAddress incl. direction bit). The
 * transfer type (bulk vs interrupt) is looked up from the endpoint map of the
 * currently-selected alt setting, so the caller need only give the address.
 * Exact sizing, a libusb timeout, full hexdump tracing, and stall (PIPE)
 * recovery via clear_halt are part of the contract. The command endpoint is
 * not hardcoded because it has not yet been confirmed on hardware.
 *
 * send: writes exactly `len` bytes; `*out_sent` gets the count actually sent.
 * recv: reads up to `len` bytes; `*out_received` gets the count received.
 */
pakon_result pakon_usb_send(pakon_dev *dev, uint8_t ep,
                            const uint8_t *buf, size_t len,
                            size_t *out_sent, unsigned timeout_ms);
pakon_result pakon_usb_recv(pakon_dev *dev, uint8_t ep,
                            uint8_t *buf, size_t len,
                            size_t *out_received, unsigned timeout_ms);

/*
 * Generic EP0 control transfer on an open device (used for the scanner's
 * 0xA4/0xA9 parameter reads). Direction comes from bit 7 of `bm_request_type`.
 * For IN transfers `buf`/`len` receive up to `len` bytes; `*out_len` (if given)
 * gets the byte count actually transferred.
 */
pakon_result pakon_usb_control(pakon_dev *dev, uint8_t bm_request_type,
                               uint8_t b_request, uint16_t w_value,
                               uint16_t w_index, uint8_t *buf, uint16_t len,
                               size_t *out_len, unsigned timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* PAKON_USB_H */
