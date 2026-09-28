/*
 * pakon_probe — standalone libusb test harness.
 *
 * Phase 1 (up to STOP POINT A):
 *   (default)         enumerate; report warm / cold / no device. With a warm
 *                     device present, open it and print its endpoint map.
 *   --list            dump every USB device on the bus (VID:PID, bus/addr,
 *                     class) with hints — use this to identify the scanner's
 *                     COLD VID/PID before any driver loads (STOP POINT A).
 *   --load-firmware   download firmware to a cold device (gated until the cold
 *                     VID/PID is confirmed and filled into pakon_usb.h).
 *   --raw HEX         claim interface 0 at --alt, send HEX on --out, and (if
 *                     --in given) read + dump the reply. Use this to find the
 *                     command channel empirically (Phase 2).
 */
#include "pakon_usb.h"
#include "pakon_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--list] [--load-firmware HEX]\n"
        "       %s --raw HEX --out 0xNN [--in 0xNN] [--alt N] [--timeout MS]\n"
        "  (default)            enumerate; if warm, print the endpoint map\n"
        "  --list               dump all USB devices\n"
        "  --probe-open         sweep all alts/endpoints with the open packet\n"
        "  --load-firmware HEX  download firmware to a cold device  [gated]\n"
        "  --raw HEX            send hex bytes (e.g. 0403100085) on --out\n"
        "  --out 0xNN           OUT endpoint for --raw (required with --raw)\n"
        "  --in 0xNN            IN endpoint to read the reply from (optional)\n"
        "  --alt N              interface-0 alt setting to select (default 1)\n"
        "  --timeout MS         per-transfer timeout (default 1000)\n"
        "\n"
        "Set PAKON_DEBUG=0..4 for increasing trace verbosity.\n",
        argv0, argv0);
}

/* Parse a hex string (optional spaces) into bytes. Returns count, or -1. */
static int parse_hex(const char *s, uint8_t *out, size_t max)
{
    size_t n = 0;
    int hi = -1;
    for (; *s; s++) {
        if (*s == ' ' || *s == ':' || *s == ',') continue;
        int v;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else return -1;
        if (hi < 0) { hi = v; }
        else {
            if (n >= max) return -1;
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) return -1;   /* odd number of nibbles */
    return (int)n;
}

static int do_raw(pakon_ctx *ctx, int alt, int out_ep, int in_ep,
                  const char *hex, unsigned timeout)
{
    uint8_t payload[1024];
    int plen = parse_hex(hex, payload, sizeof(payload));
    if (plen < 0) {
        fprintf(stderr, "--raw: invalid hex string\n");
        return 2;
    }
    if (out_ep < 0) {
        fprintf(stderr, "--raw requires --out 0xNN\n");
        return 2;
    }

    pakon_dev *dev = NULL;
    pakon_result r = pakon_usb_open(ctx, &dev);
    if (r != PAKON_OK) {
        fprintf(stderr, "open failed: %s\n", pakon_result_str(r));
        return 1;
    }

    int rc = 1;
    r = pakon_usb_claim(dev, 0, (uint8_t)alt);
    if (r != PAKON_OK) {
        fprintf(stderr, "claim failed: %s\n", pakon_result_str(r));
        goto out;
    }

    size_t sent = 0;
    printf("sending %d byte(s) on ep 0x%02x (alt %d)...\n", plen, out_ep, alt);
    r = pakon_usb_send(dev, (uint8_t)out_ep, payload, (size_t)plen, &sent,
                       timeout);
    if (r != PAKON_OK) {
        fprintf(stderr, "send: %s (%zu sent)\n", pakon_result_str(r), sent);
        goto out;
    }
    printf("sent %zu byte(s)\n", sent);

    if (in_ep >= 0) {
        uint8_t reply[1024];
        size_t got = 0;
        r = pakon_usb_recv(dev, (uint8_t)in_ep, reply, sizeof(reply), &got,
                           timeout);
        if (r != PAKON_OK && r != PAKON_ERR_TIMEOUT) {
            fprintf(stderr, "recv: %s\n", pakon_result_str(r));
            goto out;
        }
        printf("received %zu byte(s) on ep 0x%02x:\n", got, in_ep);
        for (size_t i = 0; i < got; i++) {
            printf("%02x ", reply[i]);
            if ((i & 15) == 15) printf("\n");
        }
        if (got % 16) printf("\n");
    }
    rc = 0;

out:
    pakon_usb_release(dev);
    pakon_usb_close(dev);
    return rc;
}

/*
 * Auto-sweep: for each alt setting and every OUT/IN endpoint pair present in
 * it, send the documented 36-byte open packet and look for the expected
 * 07 02 10 00 reply. Prints a per-combination result and a summary. This is how
 * we find the command channel without hand-running a matrix over SSH.
 */
static int do_probe_open(pakon_ctx *ctx, unsigned timeout)
{
    static const uint8_t open_pkt[36] = { 0x04, 0x03, 0x10, 0x00, 0x85 };
    static const uint8_t expect[4]    = { 0x07, 0x02, 0x10, 0x00 };

    pakon_dev *dev = NULL;
    pakon_result r = pakon_usb_open(ctx, &dev);
    if (r != PAKON_OK) {
        fprintf(stderr, "open failed: %s\n", pakon_result_str(r));
        return 1;
    }

    pakon_endpoint eps[32];
    size_t n = 0;
    pakon_usb_endpoints(dev, eps, 32, &n);

    int hits = 0;
    for (int alt = 1; alt <= 3; alt++) {
        if (pakon_usb_claim(dev, 0, (uint8_t)alt) != PAKON_OK)
            continue;
        printf("\n=== alt setting %d ===\n", alt);

        for (size_t o = 0; o < n; o++) {
            if (eps[o].altsetting != alt || (eps[o].address & 0x80))
                continue;                       /* want OUT eps in this alt */
            uint8_t out_ep = eps[o].address;

            size_t sent = 0;
            r = pakon_usb_send(dev, out_ep, open_pkt, sizeof(open_pkt),
                               &sent, timeout);
            printf("  out 0x%02x: send %s (%zu)\n", out_ep,
                   pakon_result_str(r), sent);
            if (r != PAKON_OK)
                continue;

            for (size_t in = 0; in < n; in++) {
                if (eps[in].altsetting != alt || !(eps[in].address & 0x80))
                    continue;                   /* want IN eps in this alt */
                uint8_t in_ep = eps[in].address;

                uint8_t reply[512];
                size_t got = 0;
                r = pakon_usb_recv(dev, in_ep, reply, sizeof(reply), &got,
                                   timeout);
                if (r == PAKON_ERR_TIMEOUT && got == 0) {
                    printf("    in 0x%02x: (no reply)\n", in_ep);
                    continue;
                }
                printf("    in 0x%02x: %zu byte(s): ", in_ep, got);
                for (size_t i = 0; i < got && i < 16; i++)
                    printf("%02x ", reply[i]);
                if (got >= 4 && memcmp(reply, expect, 4) == 0) {
                    printf(" <== OPEN REPLY MATCH");
                    hits++;
                }
                printf("\n");
            }
        }
        pakon_usb_release(dev);
    }

    printf("\nsummary: %d endpoint combination(s) returned the expected "
           "open reply (07 02 10 00)\n", hits);
    pakon_usb_close(dev);
    return hits > 0 ? 0 : 1;
}

/* Best-effort hint to help spot the cold FX2 bootloader in --list output.
 * Diagnostic only — NOT used by any matching/classification logic. */
static const char *device_hint(unsigned vid, unsigned pid)
{
    /* PID f235 is the FAMILY bootstrap identity (from the C0 EEPROM), and
     * f135 the F-135-family operational one — neither names the physical
     * model. An F-135+ shows exactly these same IDs; the model is only
     * knowable from the protocol-level PIC presence probes. */
    if (vid == PAKON_WARM_VID && pid == PAKON_WARM_PID)
        return "  <-- operational Pakon (warm; F-135 or F-135+)";
    if (vid == PAKON_COLD_VID && pid == PAKON_COLD_PID)
        return "  <-- Pakon bootstrap (cold, any F-X35 model) — needs firmware load";
    if (vid == PAKON_VID)
        return "  <-- Pakon vendor (other PID)";
    return "";
}

/* Label a Pakon PID (state, not physical model — the PID is firmware-set). */
static const char *pakon_pid_family(uint16_t pid)
{
    switch (pid) {
    case PAKON_WARM_PID: return "operational/f135";
    case PAKON_COLD_PID: return "bootstrap/f235";
    }
    return "unknown";
}

static int do_list(pakon_ctx *ctx)
{
    pakon_usb_devinfo devs[128];
    size_t count = 0;
    pakon_result r = pakon_usb_list(ctx, devs, 128, &count);
    if (r != PAKON_OK) {
        fprintf(stderr, "list failed: %s\n", pakon_result_str(r));
        return 1;
    }

    printf("%zu USB device(s):\n", count);
    printf("  bus addr  vid:pid    class\n");
    size_t shown = count < 128 ? count : 128;
    for (size_t i = 0; i < shown; i++) {
        printf("  %3u %4u  %04x:%04x  0x%02x%s\n",
               devs[i].bus, devs[i].address, devs[i].vid, devs[i].pid,
               devs[i].dev_class, device_hint(devs[i].vid, devs[i].pid));
    }
    printf("\nFor STOP POINT A: power-cycle the scanner, run this BEFORE any\n"
           "driver loads, and note the cold device's vid:pid.\n");
    return 0;
}

static const char *xfer_type(unsigned attr)
{
    switch (attr & 0x03) {
    case 0: return "control";
    case 1: return "isochronous";
    case 2: return "bulk";
    case 3: return "interrupt";
    }
    return "?";
}

static int dump_warm_endpoints(pakon_ctx *ctx)
{
    pakon_dev *dev = NULL;
    pakon_result r = pakon_usb_open(ctx, &dev);
    if (r != PAKON_OK) {
        fprintf(stderr, "open warm device failed: %s\n", pakon_result_str(r));
        return 1;
    }

    uint16_t vid = 0, pid = 0;
    pakon_usb_dev_ids(dev, &vid, &pid);
    printf("opened %04x:%04x (warm Pakon, PID family %s; PID is firmware-set, "
           "not the physical model)\n", vid, pid, pakon_pid_family(pid));

    pakon_endpoint eps[32];
    size_t n = 0;
    r = pakon_usb_endpoints(dev, eps, 32, &n);
    if (r != PAKON_OK) {
        fprintf(stderr, "endpoint query failed: %s\n", pakon_result_str(r));
        pakon_usb_close(dev);
        return 1;
    }

    printf("%zu endpoint(s) across all interfaces/altsettings:\n", n);
    printf("  if alt  ep    dir  type         max\n");
    for (size_t i = 0; i < n && i < 32; i++) {
        printf("  %2u %3u  0x%02x  %s  %-11s  %u\n",
               eps[i].interface, eps[i].altsetting, eps[i].address,
               (eps[i].address & 0x80) ? "IN " : "OUT",
               xfer_type(eps[i].attributes), eps[i].max_packet);
    }
    if (n == 0)
        printf("  (none — run with PAKON_DEBUG=3 to see the interface tree)\n");
    pakon_usb_close(dev);
    return 0;
}

int main(int argc, char **argv)
{
    int want_list = 0, want_probe_open = 0;
    const char *firmware_hex = NULL;
    const char *raw_hex = NULL;
    int alt = 1, out_ep = -1, in_ep = -1;
    unsigned timeout = 1000;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--list")) {
            want_list = 1;
        } else if (!strcmp(argv[i], "--probe-open")) {
            want_probe_open = 1;
        } else if (!strcmp(argv[i], "--load-firmware") && i + 1 < argc) {
            firmware_hex = argv[++i];
        } else if (!strcmp(argv[i], "--raw") && i + 1 < argc) {
            raw_hex = argv[++i];
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            out_ep = (int)strtol(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--in") && i + 1 < argc) {
            in_ep = (int)strtol(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--alt") && i + 1 < argc) {
            alt = (int)strtol(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            timeout = (unsigned)strtoul(argv[++i], NULL, 0);
        } else {
            fprintf(stderr, "unknown/incomplete argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    pakon_ctx *ctx = NULL;
    pakon_result r = pakon_usb_init(&ctx);
    if (r != PAKON_OK) {
        fprintf(stderr, "init failed: %s\n", pakon_result_str(r));
        return 1;
    }

    int exit_code = 0;

    if (want_list) {
        exit_code = do_list(ctx);
        pakon_usb_exit(ctx);
        return exit_code;
    }

    if (want_probe_open) {
        exit_code = do_probe_open(ctx, timeout);
        pakon_usb_exit(ctx);
        return exit_code;
    }

    if (firmware_hex) {
        r = pakon_usb_load_firmware(ctx, firmware_hex);
        if (r != PAKON_OK) {
            fprintf(stderr, "load-firmware: %s\n", pakon_result_str(r));
            exit_code = 1;
        } else {
            printf("load-firmware: firmware sent\n");
        }
        pakon_usb_exit(ctx);
        return exit_code;
    }

    if (raw_hex) {
        exit_code = do_raw(ctx, alt, out_ep, in_ep, raw_hex, timeout);
        pakon_usb_exit(ctx);
        return exit_code;
    }

    /* Default: classify, and if warm, dump the endpoint map. */
    pakon_dev_class cls = PAKON_DEV_UNKNOWN;
    r = pakon_usb_find(ctx, &cls);
    if (r == PAKON_ERR_NO_DEVICE) {
        printf("no device\n");
    } else if (r == PAKON_OK && cls == PAKON_DEV_WARM) {
        printf("found warm Pakon device\n");
        exit_code = dump_warm_endpoints(ctx);
    } else if (r == PAKON_OK && cls == PAKON_DEV_COLD) {
        printf("found cold FX2 device (needs firmware)\n");
    } else {
        printf("enumeration: %s\n", pakon_result_str(r));
        exit_code = 1;
    }

    pakon_usb_exit(ctx);
    return exit_code;
}
