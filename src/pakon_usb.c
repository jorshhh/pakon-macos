/*
 * pakon_usb.c — transport layer (Phase 1, up to STOP POINT A).
 *
 * Implemented and exercisable without the cold VID/PID:
 *   - libusb context lifecycle
 *   - real bus enumeration + warm (0F05:F135) classification
 *   - a full-bus diagnostic listing (pakon_usb_list) to identify the cold
 *     device at STOP POINT A
 *   - opening a warm device and dumping its endpoint map
 *   - the FX2 RAM-download mechanism (standard EZ-USB protocol) and Intel HEX
 *     plumbing, wired together but GATED: it refuses to run until PAKON_COLD_*
 *     is filled in, so no cold VID/PID is ever guessed.
 *
 * Bulk send/recv remain Phase 2.
 */
#include "pakon_usb.h"
#include "pakon_fw.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>

/* Per-control-transfer timeout when replaying the firmware script. */
#define FX2_TIMEOUT_MS  2000

#define MAX_ENDPOINTS   32

struct pakon_ctx {
    libusb_context *usb;
};

struct pakon_dev {
    libusb_device_handle *handle;
    uint16_t vid;
    uint16_t pid;
    pakon_endpoint endpoints[MAX_ENDPOINTS];
    size_t n_endpoints;
    int     claimed;        /* nonzero once an interface is claimed */
    uint8_t cur_ifc;        /* currently claimed interface */
    uint8_t cur_alt;        /* currently selected alternate setting */
};

pakon_result pakon_usb_init(pakon_ctx **out_ctx)
{
    if (!out_ctx)
        return PAKON_ERR_PARAM;

    pakon_ctx *ctx = (pakon_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return PAKON_ERR_USB;

    int rc = libusb_init(&ctx->usb);
    if (rc != 0) {
        pakon_logf(PAKON_LOG_ERROR, "libusb_init failed: %s",
                   libusb_strerror((enum libusb_error)rc));
        free(ctx);
        return PAKON_ERR_USB;
    }

    pakon_logf(PAKON_LOG_DEBUG, "libusb context initialized");
    *out_ctx = ctx;
    return PAKON_OK;
}

void pakon_usb_exit(pakon_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->usb)
        libusb_exit(ctx->usb);
    free(ctx);
}

int pakon_is_warm_id(uint16_t vid, uint16_t pid)
{
    return vid == PAKON_WARM_VID && pid == PAKON_WARM_PID;
}

static int is_warm(const struct libusb_device_descriptor *d)
{
    return pakon_is_warm_id(d->idVendor, d->idProduct);
}

static int is_cold(const struct libusb_device_descriptor *d)
{
    return d->idVendor == PAKON_COLD_VID && d->idProduct == PAKON_COLD_PID;
}

pakon_result pakon_usb_find(pakon_ctx *ctx, pakon_dev_class *out_class)
{
    if (!ctx || !out_class)
        return PAKON_ERR_PARAM;

    *out_class = PAKON_DEV_UNKNOWN;

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx->usb, &list);
    if (n < 0) {
        pakon_logf(PAKON_LOG_ERROR, "get_device_list: %s",
                   libusb_strerror((enum libusb_error)n));
        return PAKON_ERR_USB;
    }

    pakon_dev_class found = PAKON_DEV_UNKNOWN;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0)
            continue;
        if (is_warm(&d)) {
            found = PAKON_DEV_WARM;
            break;                  /* warm wins outright */
        }
        if (is_cold(&d))
            found = PAKON_DEV_COLD;
    }
    libusb_free_device_list(list, 1);

    if (found == PAKON_DEV_UNKNOWN) {
        pakon_logf(PAKON_LOG_DEBUG, "no Pakon device found");
        return PAKON_ERR_NO_DEVICE;
    }

    pakon_logf(PAKON_LOG_INFO, "found %s Pakon device",
               found == PAKON_DEV_WARM ? "warm" : "cold");
    *out_class = found;
    return PAKON_OK;
}

pakon_result pakon_usb_list(pakon_ctx *ctx, pakon_usb_devinfo *arr,
                            size_t max, size_t *out_count)
{
    if (!ctx || !out_count)
        return PAKON_ERR_PARAM;
    *out_count = 0;

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx->usb, &list);
    if (n < 0) {
        pakon_logf(PAKON_LOG_ERROR, "get_device_list: %s",
                   libusb_strerror((enum libusb_error)n));
        return PAKON_ERR_USB;
    }

    size_t count = 0;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0)
            continue;
        if (arr && count < max) {
            arr[count].vid       = d.idVendor;
            arr[count].pid       = d.idProduct;
            arr[count].bus       = libusb_get_bus_number(list[i]);
            arr[count].address   = libusb_get_device_address(list[i]);
            arr[count].dev_class = d.bDeviceClass;
        }
        count++;
    }
    libusb_free_device_list(list, 1);

    *out_count = count;
    return PAKON_OK;
}

/* Collect every endpoint across all interfaces/altsettings of the active
 * config. FX2 devices commonly leave altsetting 0 empty and expose the bulk
 * endpoints in a higher alternate setting, so we must scan all of them. */
static pakon_result cache_endpoints(pakon_dev *dev, libusb_device *udev)
{
    struct libusb_config_descriptor *cfg = NULL;
    int rc = libusb_get_active_config_descriptor(udev, &cfg);
    if (rc != 0) {
        pakon_logf(PAKON_LOG_ERROR, "get_active_config: %s",
                   libusb_strerror((enum libusb_error)rc));
        return PAKON_ERR_USB;
    }

    dev->n_endpoints = 0;
    pakon_logf(PAKON_LOG_DEBUG, "active config has %u interface(s)",
               cfg->bNumInterfaces);

    for (uint8_t ifc = 0; ifc < cfg->bNumInterfaces; ifc++) {
        const struct libusb_interface *iface = &cfg->interface[ifc];
        for (int alt = 0; alt < iface->num_altsetting; alt++) {
            const struct libusb_interface_descriptor *id =
                &iface->altsetting[alt];
            pakon_logf(PAKON_LOG_DEBUG,
                       "  interface %u altsetting %u: %u endpoint(s)",
                       id->bInterfaceNumber, id->bAlternateSetting,
                       id->bNumEndpoints);
            for (uint8_t e = 0; e < id->bNumEndpoints &&
                 dev->n_endpoints < MAX_ENDPOINTS; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                pakon_endpoint *out = &dev->endpoints[dev->n_endpoints++];
                out->address    = ep->bEndpointAddress;
                out->attributes = ep->bmAttributes;
                out->max_packet = ep->wMaxPacketSize;
                out->interface  = id->bInterfaceNumber;
                out->altsetting = id->bAlternateSetting;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return PAKON_OK;
}

pakon_result pakon_usb_open(pakon_ctx *ctx, pakon_dev **out_dev)
{
    if (!ctx || !out_dev)
        return PAKON_ERR_PARAM;
    *out_dev = NULL;

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx->usb, &list);
    if (n < 0)
        return PAKON_ERR_USB;

    libusb_device *match = NULL;
    struct libusb_device_descriptor md = {0};
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0)
            continue;
        if (is_warm(&d)) {
            match = list[i];
            md = d;
            break;
        }
    }

    if (!match) {
        libusb_free_device_list(list, 1);
        pakon_logf(PAKON_LOG_DEBUG, "no warm device to open");
        return PAKON_ERR_NO_DEVICE;
    }

    pakon_dev *dev = (pakon_dev *)calloc(1, sizeof(*dev));
    if (!dev) {
        libusb_free_device_list(list, 1);
        return PAKON_ERR_USB;
    }
    dev->vid = md.idVendor;
    dev->pid = md.idProduct;

    int rc = libusb_open(match, &dev->handle);
    if (rc != 0) {
        pakon_logf(PAKON_LOG_ERROR, "libusb_open: %s",
                   libusb_strerror((enum libusb_error)rc));
        free(dev);
        libusb_free_device_list(list, 1);
        return PAKON_ERR_USB;
    }

    pakon_result r = cache_endpoints(dev, match);
    libusb_free_device_list(list, 1);
    if (r != PAKON_OK) {
        libusb_close(dev->handle);
        free(dev);
        return r;
    }

    /* Interface claiming + macOS class-driver detach is Phase 2. */
    *out_dev = dev;
    return PAKON_OK;
}

void pakon_usb_close(pakon_dev *dev)
{
    if (!dev)
        return;
    if (dev->claimed && dev->handle)
        libusb_release_interface(dev->handle, dev->cur_ifc);
    if (dev->handle)
        libusb_close(dev->handle);
    free(dev);
}

void pakon_usb_dev_ids(const pakon_dev *dev, uint16_t *vid, uint16_t *pid)
{
    if (!dev)
        return;
    if (vid) *vid = dev->vid;
    if (pid) *pid = dev->pid;
}

pakon_result pakon_usb_endpoints(pakon_dev *dev, pakon_endpoint *eps,
                                 size_t max, size_t *out_count)
{
    if (!dev || !out_count)
        return PAKON_ERR_PARAM;
    *out_count = dev->n_endpoints;
    if (eps) {
        size_t copy = dev->n_endpoints < max ? dev->n_endpoints : max;
        memcpy(eps, dev->endpoints, copy * sizeof(*eps));
    }
    return PAKON_OK;
}

/* ------------------------------------------------------------------ */
/* Firmware download (f235 bootstrap -> f135 operational).             */
/*                                                                     */
/* We replay the exact FX2 control-transfer sequence captured from the */
/* working driver (a .pakfw script produced by                         */
/* tools/analyze_capture.py --extract-firmware). This is the standard  */
/* EZ-USB load (0xA0 internal + 0xA3 external RAM + CPUCS reset),       */
/* sourced from our own capture so no .hex blob is needed.             */
/* ------------------------------------------------------------------ */

/* Replay one control transfer line: "bmReqType bReq wValue wIndex wLen [data]"
 * (all hex). Returns 0 on success, -1 on a malformed line, +1 on a USB error
 * (tolerated near the end where the device renumerates out from under us). */
static int replay_fw_line(libusb_device_handle *h, const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '#' || *line == '\0' || *line == '\n')
        return 0;   /* comment / blank */

    unsigned brt, breq, wval, widx, wlen;
    int consumed = 0;
    if (sscanf(line, "%x %x %x %x %x%n",
               &brt, &breq, &wval, &widx, &wlen, &consumed) != 5)
        return -1;

    uint8_t data[64] = {0};
    size_t dlen = 0;
    const char *p = line + consumed;
    while (dlen < sizeof(data)) {
        while (*p == ' ' || *p == '\t') p++;
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]))
            break;
        unsigned byte;
        sscanf(p, "%2x", &byte);
        data[dlen++] = (uint8_t)byte;
        p += 2;
    }
    if (wlen > sizeof(data))
        return -1;

    int rc = libusb_control_transfer(h, (uint8_t)brt, (uint8_t)breq,
                                     (uint16_t)wval, (uint16_t)widx,
                                     data, (uint16_t)wlen, FX2_TIMEOUT_MS);
    if (rc < 0) {
        pakon_logf(PAKON_LOG_DEBUG, "fw xfer req=0x%02x val=0x%04x: %s",
                   breq, wval, libusb_strerror((enum libusb_error)rc));
        return 1;
    }
    pakon_hexdump(PAKON_LOG_TRACE, "fw xfer", data, wlen);
    return 0;
}

/* Poll for the warm (f135) device to appear, up to `timeout_ms`. */
static pakon_result wait_for_warm(pakon_ctx *ctx, unsigned timeout_ms)
{
    for (unsigned waited = 0; waited <= timeout_ms; waited += 200) {
        pakon_dev_class cls = PAKON_DEV_UNKNOWN;
        if (pakon_usb_find(ctx, &cls) == PAKON_OK && cls == PAKON_DEV_WARM)
            return PAKON_OK;
        usleep(200000);
    }
    return PAKON_ERR_TIMEOUT;
}

pakon_result pakon_usb_load_firmware(pakon_ctx *ctx, const char *script_path)
{
    if (!ctx || !script_path)
        return PAKON_ERR_PARAM;

    FILE *fp = fopen(script_path, "r");
    if (!fp) {
        pakon_logf(PAKON_LOG_ERROR, "cannot open firmware script '%s'",
                   script_path);
        return PAKON_ERR_FIRMWARE;
    }

    /* Open the cold bootstrap device (0F05:F235). */
    libusb_device_handle *h =
        libusb_open_device_with_vid_pid(ctx->usb, PAKON_COLD_VID, PAKON_COLD_PID);
    if (!h) {
        pakon_logf(PAKON_LOG_ERROR,
                   "cannot open cold device %04x:%04x (is it in bootstrap "
                   "state and free, i.e. not held by a VM?)",
                   PAKON_COLD_VID, PAKON_COLD_PID);
        fclose(fp);
        return PAKON_ERR_NO_DEVICE;
    }
    (void)libusb_set_auto_detach_kernel_driver(h, 1);
    (void)libusb_claim_interface(h, 0);

    char line[8192];
    unsigned n = 0, errs = 0;
    pakon_result r = PAKON_OK;
    while (fgets(line, sizeof(line), fp)) {
        int rc = replay_fw_line(h, line);
        if (rc < 0) {
            pakon_logf(PAKON_LOG_ERROR, "malformed firmware line %u", n + 1);
            r = PAKON_ERR_FIRMWARE;
            break;
        }
        if (rc > 0)
            errs++;     /* tolerated (renumeration tail) */
        n++;
    }
    fclose(fp);
    (void)libusb_release_interface(h, 0);
    libusb_close(h);

    if (r != PAKON_OK)
        return r;

    pakon_logf(PAKON_LOG_INFO,
               "replayed %u firmware transfers (%u late USB errors); "
               "waiting for re-enumeration to %04x:%04x",
               n, errs, PAKON_WARM_VID, PAKON_WARM_PID);

    return wait_for_warm(ctx, 5000);
}

static char *read_text(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = n > 0 ? malloc((size_t)n + 1) : NULL;
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

pakon_result pakon_usb_load_firmware_hex(pakon_ctx *ctx, const char *stage1_path,
                                         const char *main_path)
{
    if (!ctx || !stage1_path || !main_path)
        return PAKON_ERR_PARAM;
    size_t l1 = 0, l2 = 0;
    char *s1 = read_text(stage1_path, &l1), *s2 = read_text(main_path, &l2);
    pakon_fw_seq seq;
    pakon_result r;
    if (!s1 || !s2) {
        pakon_logf(PAKON_LOG_ERROR, "cannot read '%s' / '%s'", stage1_path, main_path);
        free(s1);
        free(s2);
        return PAKON_ERR_FIRMWARE;
    }
    r = pakon_fw_build(s1, l1, s2, l2, &seq);
    free(s1);
    free(s2);
    if (r != PAKON_OK)
        return r;

    libusb_device_handle *h =
        libusb_open_device_with_vid_pid(ctx->usb, PAKON_COLD_VID, PAKON_COLD_PID);
    if (!h) {
        pakon_logf(PAKON_LOG_ERROR, "cannot open cold device %04x:%04x",
                   PAKON_COLD_VID, PAKON_COLD_PID);
        pakon_fw_free(&seq);
        return PAKON_ERR_NO_DEVICE;
    }
    (void)libusb_set_auto_detach_kernel_driver(h, 1);
    (void)libusb_claim_interface(h, 0);

    unsigned errs = 0;
    for (size_t i = 0; i < seq.n && r == PAKON_OK; i++) {
        pakon_fw_xfer *x = &seq.x[i];
        uint8_t buf[16];
        memcpy(buf, x->data, sizeof buf);
        int rc = libusb_control_transfer(h, x->request_type, x->request, x->value,
                                         x->index, buf, x->length, FX2_TIMEOUT_MS);
        if (rc < 0) {
            /* Only the final run (CPUCS = 0) may fail as the device renumerates. */
            if (i + 2 >= seq.n) {
                errs++;
                continue;
            }
            pakon_logf(PAKON_LOG_ERROR, "fw xfer %zu req=0x%02x val=0x%04x: %s", i,
                       x->request, x->value, libusb_strerror((enum libusb_error)rc));
            r = PAKON_ERR_USB;
            break;
        }
        if (i == seq.personality_at) {
            pakon_hexdump(PAKON_LOG_INFO, "personality", buf, (size_t)rc);
            if (rc != 8 || !pakon_fw_personality_is_f135(buf)) {
                pakon_logf(PAKON_LOG_ERROR, "personality is not F235_AA07 "
                           "(F-135/F-135+); not loading Pakon7.hex. Power-cycle.");
                r = PAKON_ERR_FIRMWARE;
            }
        }
    }
    size_t sent = seq.n;
    pakon_fw_free(&seq);
    (void)libusb_release_interface(h, 0);
    libusb_close(h);
    if (r != PAKON_OK)
        return r;
    pakon_logf(PAKON_LOG_INFO, "sent %zu firmware transfers from HEX (%u late "
               "USB errors); waiting for %04x:%04x", sent, errs,
               PAKON_WARM_VID, PAKON_WARM_PID);
    return wait_for_warm(ctx, 5000);
}

pakon_result pakon_usb_claim(pakon_dev *dev, uint8_t ifc, uint8_t alt)
{
    if (!dev || !dev->handle)
        return PAKON_ERR_PARAM;

    /* Linux: let libusb detach any kernel driver holding the interface. */
    (void)libusb_set_auto_detach_kernel_driver(dev->handle, 1);

    int rc = libusb_claim_interface(dev->handle, ifc);
    if (rc != 0) {
        pakon_logf(PAKON_LOG_ERROR, "claim interface %u: %s", ifc,
                   libusb_strerror((enum libusb_error)rc));
        return PAKON_ERR_USB;
    }

    rc = libusb_set_interface_alt_setting(dev->handle, ifc, alt);
    if (rc != 0) {
        pakon_logf(PAKON_LOG_ERROR, "set interface %u alt %u: %s", ifc, alt,
                   libusb_strerror((enum libusb_error)rc));
        libusb_release_interface(dev->handle, ifc);
        return PAKON_ERR_USB;
    }

    dev->claimed = 1;
    dev->cur_ifc = ifc;
    dev->cur_alt = alt;
    pakon_logf(PAKON_LOG_INFO, "claimed interface %u, alt setting %u", ifc, alt);
    return PAKON_OK;
}

pakon_result pakon_usb_release(pakon_dev *dev)
{
    if (!dev || !dev->handle)
        return PAKON_ERR_PARAM;
    if (!dev->claimed)
        return PAKON_OK;
    libusb_release_interface(dev->handle, dev->cur_ifc);
    dev->claimed = 0;
    return PAKON_OK;
}

/* Look up the transfer type (LIBUSB_TRANSFER_TYPE_*) of endpoint `ep` in the
 * currently-selected alt setting. Returns -1 if not found in this alt. */
static int endpoint_type(const pakon_dev *dev, uint8_t ep)
{
    for (size_t i = 0; i < dev->n_endpoints; i++) {
        if (dev->endpoints[i].altsetting == dev->cur_alt &&
            dev->endpoints[i].address == ep)
            return dev->endpoints[i].attributes & 0x03;
    }
    return -1;
}

/* Shared bulk/interrupt transfer with stall recovery + tracing. */
static pakon_result do_transfer(pakon_dev *dev, uint8_t ep, uint8_t *buf,
                                int len, int *transferred, unsigned timeout_ms)
{
    int type = endpoint_type(dev, ep);
    if (type < 0) {
        pakon_logf(PAKON_LOG_ERROR,
                   "endpoint 0x%02x not present in alt setting %u",
                   ep, dev->cur_alt);
        return PAKON_ERR_PARAM;
    }

    int rc;
    if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT)
        rc = libusb_interrupt_transfer(dev->handle, ep, buf, len,
                                       transferred, timeout_ms);
    else
        rc = libusb_bulk_transfer(dev->handle, ep, buf, len,
                                  transferred, timeout_ms);

    if (rc == LIBUSB_ERROR_TIMEOUT) {
        pakon_logf(PAKON_LOG_WARN, "endpoint 0x%02x: timeout (%d bytes moved)",
                   ep, *transferred);
        return PAKON_ERR_TIMEOUT;
    }
    if (rc == LIBUSB_ERROR_PIPE) {
        /* Endpoint stalled; clear the halt so the next attempt can proceed. */
        pakon_logf(PAKON_LOG_WARN, "endpoint 0x%02x stalled; clearing halt", ep);
        libusb_clear_halt(dev->handle, ep);
        return PAKON_ERR_USB;
    }
    if (rc != 0) {
        pakon_logf(PAKON_LOG_ERROR, "endpoint 0x%02x transfer: %s", ep,
                   libusb_strerror((enum libusb_error)rc));
        return PAKON_ERR_USB;
    }
    return PAKON_OK;
}

pakon_result pakon_usb_send(pakon_dev *dev, uint8_t ep,
                            const uint8_t *buf, size_t len,
                            size_t *out_sent, unsigned timeout_ms)
{
    if (!dev || !dev->handle || !buf || (ep & 0x80))
        return PAKON_ERR_PARAM;
    if (!dev->claimed)
        return PAKON_ERR_USB;

    pakon_hexdump(PAKON_LOG_TRACE, "send", buf, len);
    int moved = 0;
    pakon_result r = do_transfer(dev, ep, (uint8_t *)buf, (int)len, &moved,
                                 timeout_ms);
    if (out_sent)
        *out_sent = (size_t)moved;
    return r;
}

pakon_result pakon_usb_recv(pakon_dev *dev, uint8_t ep,
                            uint8_t *buf, size_t len,
                            size_t *out_received, unsigned timeout_ms)
{
    if (out_received)
        *out_received = 0;
    if (!dev || !dev->handle || !buf || !(ep & 0x80))
        return PAKON_ERR_PARAM;
    if (!dev->claimed)
        return PAKON_ERR_USB;

    int moved = 0;
    pakon_result r = do_transfer(dev, ep, buf, (int)len, &moved, timeout_ms);
    if (moved > 0)
        pakon_hexdump(PAKON_LOG_TRACE, "recv", buf, (size_t)moved);
    if (out_received)
        *out_received = (size_t)moved;
    return r;
}

pakon_result pakon_usb_control(pakon_dev *dev, uint8_t bm_request_type,
                               uint8_t b_request, uint16_t w_value,
                               uint16_t w_index, uint8_t *buf, uint16_t len,
                               size_t *out_len, unsigned timeout_ms)
{
    if (out_len)
        *out_len = 0;
    if (!dev || !dev->handle || (len && !buf))
        return PAKON_ERR_PARAM;

    int rc = libusb_control_transfer(dev->handle, bm_request_type, b_request,
                                     w_value, w_index, buf, len, timeout_ms);
    if (rc < 0) {
        pakon_logf(PAKON_LOG_WARN, "control req=0x%02x val=0x%04x: %s",
                   b_request, w_value, libusb_strerror((enum libusb_error)rc));
        return (rc == LIBUSB_ERROR_TIMEOUT) ? PAKON_ERR_TIMEOUT : PAKON_ERR_USB;
    }
    if (out_len)
        *out_len = (size_t)rc;
    return PAKON_OK;
}
