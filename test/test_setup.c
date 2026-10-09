/*
 * test_setup — generated init + configure frames vs the captures.
 *
 * For each capture, takes the frames from the SCN 0x97 controller init up to
 * the first acquire (bank 0x82 reg 0 with bit 0 set) or first image read,
 * drops the traffic pakon_setup leaves out (see pakon_setup.h), and requires
 * the generated sequence to match it byte for byte.
 * Usage: test_setup RESOURCES_DIR
 */
#include "pakon_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
        if (cond) { printf("ok   - %s\n", (msg)); }             \
        else { printf("FAIL - %s\n", (msg)); failures++; }      \
    } while (0)

#define MAXF 512

typedef struct { uint8_t b[PAKON_PACKET_SIZE]; size_t n; } frame;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Event-service and status-LED frames that are not part of the sequence. */
static int left_out(const frame *f, uint8_t low, uint8_t scn)
{
    const uint8_t *b = f->b;
    if (f->n == 3 && b[0] == 3 && b[2] == AD_HOST)          /* host poll */
        return 1;
    if (f->n == 5 && b[0] == 1 && (b[2] == low || b[2] == scn) &&
        ((b[3] == 1 && b[4] == 0x02) || (b[3] == 0x1E && b[4] == 0x90) ||
         (b[3] == 1 && b[4] == 0x83) || (b[3] == 2 && b[4] == 0x84) ||
         (b[3] == 4 && b[4] == 0x88)))                       /* event reads */
        return 1;
    return 0;
}

/* Left-out writes whose busy poll goes too: the event ack
 * `02 05 <a> 02 06 00 <s>` and the status-LED word. */
static int left_out_write(const frame *f, uint8_t scn)
{
    const uint8_t *b = f->b;
    if (f->n == 7 && b[0] == 2 && b[3] == 2 && b[4] == 0x06)
        return 1;
    return f->n == 8 && b[0] == 2 && b[2] == scn && b[4] == 0x82 &&
           b[5] == 0x09 && b[6] == 0x13 && b[7] == 0x03;
}

/* Load the comparable window of a .pakscan; returns the frame count. */
static size_t load_window(const char *path, uint8_t low, uint8_t scn,
                          frame *out)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("cannot open %s\n", path);
        return 0;
    }
    char line[512];
    size_t n = 0;
    int started = 0, after_ack = 0;
    while (fgets(line, sizeof line, fp) && n < MAXF) {
        if (line[0] == 'M' && started)
            break;
        if (line[0] != 'O')
            continue;
        frame f = { .n = 0 };
        for (char *p = line + 2; hexval(p[0]) >= 0 && hexval(p[1]) >= 0 &&
             f.n < sizeof f.b; p += 2)
            f.b[f.n++] = (uint8_t)(hexval(p[0]) << 4 | hexval(p[1]));
        if (!started) {
            if (f.n == 6 && f.b[0] == 2 && f.b[2] == scn && f.b[4] == 0x97)
                started = 1;
            else
                continue;
        }
        if (f.n == 8 && f.b[0] == 2 && f.b[2] == scn && f.b[4] == 0x82 &&
            f.b[5] == 0x00 && (f.b[6] & 1))
            break;                                           /* first acquire */
        if (left_out_write(&f, scn)) {
            after_ack = 1;
            continue;
        }
        if (after_ack && f.n == 3 && f.b[0] == 3) {
            after_ack = 0;
            continue;
        }
        after_ack = 0;
        if (!left_out(&f, low, scn))
            out[n++] = f;
    }
    fclose(fp);
    return n;
}

static void check_capture(const char *dir, const char *file, const char *label,
                          uint8_t low, uint8_t scn, uint16_t led_period,
                          const pakon_scan_mode *mode)
{
    char path[1024], msg[256];
    static frame cap[MAXF];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    size_t ncap = load_window(path, low, scn, cap);

    pakon_seq seq;
    pakon_setup_state st;
    pakon_seq_init(&seq);
    pakon_result r1 = pakon_setup_init(&seq, low, scn, led_period, &st);
    pakon_result r2 = pakon_setup_configure(&seq, &st, mode);

    size_t i = 0, mismatch = (size_t)-1;
    for (; i < seq.n && i < ncap; i++) {
        uint8_t wire[PAKON_PACKET_SIZE];
        size_t wl = 0;
        pakon_packet_serialize(&seq.pkt[i], wire, sizeof wire, &wl);
        if (wl != cap[i].n || memcmp(wire, cap[i].b, wl)) {
            mismatch = i;
            break;
        }
    }
    int ok = r1 == PAKON_OK && r2 == PAKON_OK && mismatch == (size_t)-1 &&
             seq.n == ncap && ncap > 0;
    snprintf(msg, sizeof msg, "%s: %zu generated frames match the capture",
             label, seq.n);
    CHECK(ok, msg);
    if (!ok) {
        printf("     generated %zu, captured %zu, first difference at %zu\n",
               seq.n, ncap, mismatch == (size_t)-1 ? i : mismatch);
        if (mismatch != (size_t)-1) {
            printf("     captured: ");
            for (size_t k = 0; k < cap[mismatch].n; k++)
                printf("%02x", cap[mismatch].b[k]);
            printf("\n");
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s RESOURCES_DIR\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    pakon_scan_mode m;
    uint16_t s, e;

    /* Pixel window from the EEPROM Offset: serial 16402's offsets 60/58/30
     * and serial 3054's Base 16 offset 43 give the captured reg 4/5. */
    pakon_setup_pixel_window(60, 0, &s, &e);
    CHECK(s == 6 && e == 0x080C, "16402 Base 16 window 6..0x080c");
    pakon_setup_pixel_window(30, 1, &s, &e);
    CHECK(s == 3 && e == 0x0406, "16402 Base 4 window 3..0x0406");

    /* F-135 serial 3054, Base 16 + IR. */
    m = (pakon_scan_mode){ .trigger = 0x0010, .ir = 1, .integration = 0x0C1A };
    pakon_setup_pixel_window(43, 0, &m.pixel_start, &m.pixel_end);
    check_capture(dir, "scan.pakscan", "F-135 Base 16 + IR",
                  AD_PICL, AD_PICM, PAKON_SETUP_LED_PERIOD_F135, &m);

    /* F-135+ serial 16402. */
    m = (pakon_scan_mode){ .trigger = 0x003C, .integration = 0x0FFD };
    pakon_setup_pixel_window(60, 0, &m.pixel_start, &m.pixel_end);
    check_capture(dir, "f135plus/base16.pakscan", "F-135+ Base 16",
                  AD_PICL_PLUS, AD_PICM_PLUS, PAKON_SETUP_LED_PERIOD_F135_PLUS, &m);

    m = (pakon_scan_mode){ .trigger = 0x0075, .integration = 0x0AFD, .low89 = 1 };
    pakon_setup_pixel_window(58, 0, &m.pixel_start, &m.pixel_end);
    check_capture(dir, "f135plus/base8.pakscan", "F-135+ Base 8",
                  AD_PICL_PLUS, AD_PICM_PLUS, PAKON_SETUP_LED_PERIOD_F135_PLUS, &m);

    m = (pakon_scan_mode){ .trigger = 0x0107, .dual_tap = 1, .integration = 0x0753 };
    pakon_setup_pixel_window(30, 1, &m.pixel_start, &m.pixel_end);
    check_capture(dir, "f135plus/base4.pakscan", "F-135+ Base 4",
                  AD_PICL_PLUS, AD_PICM_PLUS, PAKON_SETUP_LED_PERIOD_F135_PLUS, &m);

    m = (pakon_scan_mode){ .trigger = 0x00C5, .ir = 1, .dual_tap = 1,
                           .integration = 0x04E2 };
    pakon_setup_pixel_window(30, 1, &m.pixel_start, &m.pixel_end);
    check_capture(dir, "f135plus/base4_ir.pakscan", "F-135+ Base 4 + IR",
                  AD_PICL_PLUS, AD_PICM_PLUS, PAKON_SETUP_LED_PERIOD_F135_PLUS, &m);

    /* Integration above the FPGA limit is refused. */
    {
        pakon_seq seq;
        pakon_setup_state st;
        pakon_seq_init(&seq);
        pakon_setup_init(&seq, AD_PICL, AD_PICM, PAKON_SETUP_LED_PERIOD_F135, &st);
        m.integration = 0x1000;
        CHECK(pakon_setup_configure(&seq, &st, &m) == PAKON_ERR_PARAM,
              "integration > 0xFFD refused");
    }

    if (failures) {
        printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall tests passed\n");
    return 0;
}
