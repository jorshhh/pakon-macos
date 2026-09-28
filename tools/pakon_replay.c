/*
 * pakon_replay — replays/extends captured handshakes against the device.
 *
 * --open replays the open handshake decoded from a real scan capture (the EP1
 * command exchanges that bring the device to Idle) and verifies each reply,
 * exercising the pakon_cmd primitive end to end. --scan (Phase 5) is still a
 * stub pending the image-stream decode.
 *
 * NOTE: this requires the OPERATIONAL device (0F05:F135). The f235 bootstrap
 * does not implement the command protocol.
 */
#include "pakon_usb.h"
#include "pakon_proto.h"
#include "pakon_cmd.h"
#include "pakon_calib.h"
#include "pakon_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

/* One step of the captured open handshake: the bytes to send, and the reply
 * bytes observed in the capture (for verification). Lengths are 2+count. */
typedef struct {
    const char *label;
    uint8_t out[8];
    size_t  out_len;
    uint8_t expect[8];
    size_t  expect_len;
} open_step;

/* From the device-13 scan capture (see docs/PROTOCOL.md). The HOST steps are
 * byte-verified (identical on every model). The PIC probes that followed in
 * the capture are NOT fixed expectations: they are the OEM's model detection
 * (status 0 = that PIC acked/present, 1 = absent), and an F-135+ answers
 * them exactly inverted vs an F-135 — confirmed on real hardware 2026-08-12
 * (docs/F135_PLUS_CAPTURES.md). So the probes are evaluated, not compared. */
static const open_step OPEN_SEQ[] = {
    {"open",            {0x04,0x03,0x10,0x00,0x85}, 5, {0x07,0x02,0x10,0x00}, 4},
    {"open-2",          {0x02,0x04,0x10,0x01,0x8f,0x00}, 6, {0x07,0x02,0x10,0x00}, 4},
};

/* Presence probes, same frames the OEM sends after the open: CMD 0x00 to each
 * candidate PIC address. Reply is 07 02 <addr> <status>. */
static const struct { const char *label; uint8_t addr; } PIC_PROBES[] = {
    {"PICM_PLUS (0x44)",      0x44},
    {"BOOT_PICM_PLUS (0x46)", 0x46},
    {"PICM (0x24)",           0x24},
};

static void hex(const char *tag, const uint8_t *b, size_t n)
{
    printf("%s", tag);
    for (size_t i = 0; i < n; i++)
        printf(" %02x", b[i]);
}

/* First Ctrl-C during a motor-running phase (--scan replay, --advance):
 * finish cleanly (captured teardown / motor stop writes) instead of dying
 * with the drive engaged. The handler resets to default so a second Ctrl-C
 * kills immediately. */
static volatile sig_atomic_t scan_interrupted;

static void scan_sigint_handler(int signum)
{
    (void)signum;
    scan_interrupted = 1;
    signal(SIGINT, SIG_DFL);
}

static int do_open(unsigned timeout)
{
    pakon_ctx *ctx = NULL;
    if (pakon_usb_init(&ctx) != PAKON_OK) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    pakon_dev *dev = NULL;
    pakon_result r = pakon_usb_open(ctx, &dev);
    if (r != PAKON_OK) {
        fprintf(stderr, "open device failed: %s "
                "(need operational 0F05:F135 on the host)\n",
                pakon_result_str(r));
        pakon_usb_exit(ctx);
        return 1;
    }

    uint16_t vid = 0, pid = 0;
    pakon_usb_dev_ids(dev, &vid, &pid);
    printf("device %04x:%04x\n", vid, pid);

    r = pakon_usb_claim(dev, 0, 0);
    if (r != PAKON_OK) {
        fprintf(stderr, "claim failed: %s\n", pakon_result_str(r));
        pakon_usb_close(dev);
        pakon_usb_exit(ctx);
        return 1;
    }

    int failures = 0;
    size_t n = sizeof(OPEN_SEQ) / sizeof(OPEN_SEQ[0]);
    for (size_t i = 0; i < n; i++) {
        const open_step *s = &OPEN_SEQ[i];
        pakon_packet cmd, reply;
        /* out[0]=type, out[1]=count, out[2..]=data */
        pakon_packet_build(&cmd, s->out[0], s->out + 2, s->out[1]);

        r = pakon_cmd(dev, &cmd, &reply, timeout);
        printf("[%-20s] ", s->label);
        hex("send", s->out, s->out_len);
        if (r == PAKON_ERR_TIMEOUT) {
            /* HostReset/HostSetMode reply only on the FIRST open after
             * power-on or firmware load; later opens get no reply while the
             * bridge keeps working (observed on F-135+ hardware). The OEM
             * fires HostReset in clusters of three and ignores the replies,
             * so a missing reply here is not a failure — the PIC probes
             * below are the real health check. */
            printf("  -> no reply (normal after first open)\n");
            continue;
        }
        if (r != PAKON_OK) {
            printf("  -> ERROR %s\n", pakon_result_str(r));
            failures++;
            continue;
        }
        uint8_t got[PAKON_PACKET_SIZE];
        size_t glen = 0;
        pakon_packet_serialize(&reply, got, sizeof(got), &glen);
        hex("  recv", got, glen);
        if (glen == s->expect_len && memcmp(got, s->expect, glen) == 0) {
            printf("  OK\n");
        } else {
            hex("  (expected", s->expect, s->expect_len);
            printf(")  MISMATCH\n");
            failures++;
        }
    }

    /* Model detection via the shared probe primitive. A probe ERROR (transport
     * failure, malformed reply, or a non-ack error status like bus-error) is
     * distinct from ABSENT: it is a fault, never evidence about the model. */
    pakon_pic_state probe_state[sizeof(PIC_PROBES) / sizeof(PIC_PROBES[0])];
    size_t nprobes = sizeof(PIC_PROBES) / sizeof(PIC_PROBES[0]);
    for (size_t i = 0; i < nprobes; i++) {
        uint8_t status = PS_NONE;
        probe_state[i] = pakon_probe_pic(dev, PIC_PROBES[i].addr, &status,
                                         timeout);
        printf("[probe %-14s] ", PIC_PROBES[i].label);
        switch (probe_state[i]) {
        case PAKON_PIC_PRESENT: printf("present\n"); break;
        case PAKON_PIC_ABSENT:  printf("absent\n"); break;
        case PAKON_PIC_ERROR:
            printf("ERROR (%s)\n", status == PS_NONE
                   ? "no/malformed reply" : pakon_status_str(status));
            failures++;
            break;
        }
    }

    pakon_usb_release(dev);
    pakon_usb_close(dev);
    pakon_usb_exit(ctx);

    /* PIC_PROBES order: [0]=PICM_PLUS 0x44, [1]=BOOT_PICM_PLUS 0x46,
     * [2]=PICM 0x24. Exactly one of the motor PICs answers per model. */
    const char *model = NULL;
    if (probe_state[0] == PAKON_PIC_PRESENT &&
        probe_state[2] == PAKON_PIC_ABSENT)
        model = "F-135+ (Plus PICs at 0x40/0x44)";
    else if (probe_state[2] == PAKON_PIC_PRESENT &&
             probe_state[0] == PAKON_PIC_ABSENT)
        model = "F-135 (PICs at 0x20/0x24)";
    if (probe_state[1] == PAKON_PIC_PRESENT) {
        /* The boot PIC answering means the motor PIC is stuck in its
         * bootloader (firmware-update state) — not safe to drive. */
        printf("\nERROR: PICM boot PIC (0x46) answered — device in "
               "firmware-update state, not driving it\n");
        failures++;
    }

    if (model)
        printf("\nmodel detected: %s\n", model);
    else {
        printf("\nmodel detection FAILED (probes: PLUS=%d plain=%d; "
               "0=present 1=absent 2=error)\n",
               probe_state[0], probe_state[2]);
        failures++;
    }

    printf("open handshake: %s\n",
           failures ? "INCOMPLETE" : "reached Idle");
    return failures ? 1 : 0;
}

static int hexbytes(const char *s, uint8_t *out, size_t max);

/* ---- Film eject: drive the transport until the strip is out ---------------
 * The OEM ends a scan by polling the film out of the transport; a verbatim
 * script replay skips that wait and leaves the strip half-fed. This replays
 * the captured advance block (motor cal -> engage -> advance speed), keeps
 * the motor running for `seconds`, then sends the captured stop pattern
 * (disengage + reg9 speed writes). Works on both models: the motor PIC
 * address is probed (0x44 Plus, 0x24 non-plus), same probe the OEM uses. */
/* Motor-running phases must survive Ctrl-C: the first SIGINT skips the rest
 * of the run and falls through to the stop writes instead of killing the
 * process with the drive engaged. Shared with the --scan handler. */
static int advance_frame(pakon_dev *dev, uint8_t frame_type,
                         const uint8_t *frame_data, size_t data_len,
                         unsigned timeout)
{
    pakon_packet reply;
    return pakon_cmd_raw(dev, frame_type, frame_data, data_len, &reply,
                         timeout) == PAKON_OK ? 0 : 1;
}

static int timed_advance_on(pakon_dev *dev, unsigned timeout, unsigned seconds)
{
    /* PIC traffic only flows after the bridge is opened; on a fresh power
     * session the probes below would otherwise all time out. Reply timeouts
     * from the open itself are normal (first-open-only replies). */
    pakon_bridge_open(dev, timeout);

    uint8_t picm_addr = 0;
    const uint8_t motor_candidates[] = {AD_PICM_PLUS, AD_PICM};
    for (size_t i = 0; i < sizeof(motor_candidates); i++) {
        if (pakon_probe_pic(dev, motor_candidates[i], NULL, timeout) ==
            PAKON_PIC_PRESENT) {
            picm_addr = motor_candidates[i];
            break;
        }
    }
    if (!picm_addr) {
        fprintf(stderr, "advance: no motor PIC answered (0x44/0x24)\n");
        return 1;
    }
    printf("advance: motor PIC at 0x%02x (%s), running transport for %us\n",
           picm_addr, picm_addr == AD_PICM_PLUS ? "F-135+" : "F-135", seconds);

    /* Captured advance block (see docs/F135_PLUS_CAPTURES.md). Frame data is
     * [addr, payload_len, cmd, payload...]; the type byte selects CMD/WRITE/
     * READ_STATUS per the confirmed wire format. */
    const uint8_t motor_cal[]  = {picm_addr, 0x02, 0xa5, 0x7e, 0x64};
    const uint8_t engage[]     = {picm_addr, 0x00, 0xa0};
    const uint8_t speed_run[]  = {picm_addr, 0x03, 0x82, 0x00, 0x63, 0x00};
    const uint8_t status_poll[] = {picm_addr};
    const uint8_t speed_idle[] = {picm_addr, 0x03, 0x82, 0x00, 0x60, 0x00};
    const uint8_t disengage[]  = {picm_addr, 0x00, 0xa2};
    const uint8_t stop_a[]     = {picm_addr, 0x03, 0x82, 0x09, 0x17, 0x02};
    const uint8_t stop_b[]     = {picm_addr, 0x03, 0x82, 0x09, 0x17, 0x00};

    /* From here the motor runs: catch the first Ctrl-C so the stop writes
     * below always execute (a second Ctrl-C kills as usual). */
    scan_interrupted = 0;
    void (*previous_sigint)(int) = signal(SIGINT, scan_sigint_handler);

    int errs = 0;
    errs += advance_frame(dev, PH_WRITE, motor_cal, sizeof(motor_cal), timeout);
    errs += advance_frame(dev, PH_CMD, engage, sizeof(engage), timeout);
    errs += advance_frame(dev, PH_WRITE, speed_run, sizeof(speed_run), timeout);
    for (unsigned elapsed = 0; elapsed < seconds && !scan_interrupted;
         elapsed++) {
        struct timespec pause = {1, 0};
        nanosleep(&pause, NULL);
        /* keep-alive */
        advance_frame(dev, PH_READ_STATUS, status_poll, sizeof(status_poll),
                      timeout);
    }
    if (scan_interrupted)
        printf("advance: interrupted — stopping the motor before exit\n");
    /* Stop: reg0 back to its idle value FIRST (the run/stop state lives
     * there; 0xA2 only releases the drive), then the captured stop tail. */
    errs += advance_frame(dev, PH_WRITE, speed_idle, sizeof(speed_idle), timeout);
    errs += advance_frame(dev, PH_CMD, disengage, sizeof(disengage), timeout);
    errs += advance_frame(dev, PH_WRITE, stop_a, sizeof(stop_a), timeout);
    errs += advance_frame(dev, PH_WRITE, stop_b, sizeof(stop_b), timeout);
    signal(SIGINT, previous_sigint);
    printf("advance: done (%d command errors)\n", errs);
    return (errs || scan_interrupted) ? 1 : 0;
}

static int do_timed_advance(unsigned timeout, unsigned seconds)
{
    pakon_ctx *ctx = NULL;
    if (pakon_usb_init(&ctx) != PAKON_OK) { fprintf(stderr, "init failed\n"); return 1; }
    pakon_dev *dev = NULL;
    pakon_result r = pakon_usb_open(ctx, &dev);
    if (r != PAKON_OK) {
        fprintf(stderr, "open device failed: %s (need operational f135)\n",
                pakon_result_str(r));
        pakon_usb_exit(ctx);
        return 1;
    }
    if (pakon_usb_claim(dev, 0, 0) != PAKON_OK) {
        fprintf(stderr, "claim failed\n");
        pakon_usb_close(dev); pakon_usb_exit(ctx); return 1;
    }
    int rc = timed_advance_on(dev, timeout, seconds);
    pakon_usb_release(dev);
    pakon_usb_close(dev);
    pakon_usb_exit(ctx);
    return rc;
}

/* ---- Film advance: replay an advance script (.pakscan, O/C lines only) ---- */

static int do_advance(const char *script, unsigned timeout, unsigned limit_sec,
                      unsigned long steps_count)
{
    FILE *fp = fopen(script, "r");
    if (!fp) { fprintf(stderr, "cannot open script '%s'\n", script); return 1; }

    pakon_ctx *ctx = NULL;
    if (pakon_usb_init(&ctx) != PAKON_OK) { fclose(fp); return 1; }
    pakon_dev *dev = NULL;
    pakon_result r = pakon_usb_open(ctx, &dev);
    if (r != PAKON_OK) {
        fprintf(stderr, "open device failed: %s (need operational f135)\n",
                pakon_result_str(r));
        fclose(fp); pakon_usb_exit(ctx); return 1;
    }
    if (pakon_usb_claim(dev, 0, 0) != PAKON_OK) {
        fprintf(stderr, "claim failed\n");
        pakon_usb_close(dev); fclose(fp); pakon_usb_exit(ctx); return 1;
    }

    char line[1024];
    uint8_t buf[65536];
    unsigned long ncmd = 0, errs = 0;
    int rc = 0;

    /* Replay the captured command sequence. */
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        if (*p == 'O') {
            int n = hexbytes(p + 1, buf, sizeof(buf));
            if (n < 0) { fprintf(stderr, "bad O line\n"); rc = 1; break; }
            size_t sent = 0;
            r = pakon_usb_send(dev, PAKON_EP_CMD_OUT, buf, (size_t)n, &sent, timeout);
            if (r == PAKON_OK) {
                size_t got = 0;
                r = pakon_usb_recv(dev, PAKON_EP_CMD_IN, buf, sizeof(buf), &got, timeout);
            }
            if (r != PAKON_OK) errs++;
            ncmd++;
        } else if (*p == 'C') {
            unsigned brt, breq, wval, widx, wlen; int consumed = 0;
            if (sscanf(p + 1, "%x %x %x %x %x%n", &brt,&breq,&wval,&widx,&wlen,&consumed) != 5) {
                fprintf(stderr, "bad C line\n"); rc = 1; break;
            }
            int dn = hexbytes(p + 1 + consumed, buf, sizeof(buf));
            if (dn < 0) dn = 0;
            size_t got = 0;
            r = pakon_usb_control(dev, (uint8_t)brt, (uint8_t)breq, (uint16_t)wval,
                                  (uint16_t)widx, buf, (uint16_t)wlen, &got, timeout);
            if (r != PAKON_OK) errs++;
        }
    }
    fclose(fp);

    /* Each advance step: a0 (start) → poll HOST until PS_SUCCESS (frame in
     * position) → a2 (finalize/stop).  Mirrors what the captured script does
     * in a single step, repeated for steps_count frames. */
    if (!rc) {
        /* The motor PIC address differs per model (0x24 F-135, 0x44 F-135+);
         * probe for it instead of assuming AD_PICM. */
        uint8_t picm_addr = AD_PICM;
        const uint8_t motor_candidates[] = {AD_PICM_PLUS, AD_PICM};
        for (size_t i = 0; i < sizeof(motor_candidates); i++) {
            if (pakon_probe_pic(dev, motor_candidates[i], NULL, timeout) ==
                PAKON_PIC_PRESENT) {
                picm_addr = motor_candidates[i];
                break;
            }
        }
        printf("  [advance] motor PIC at 0x%02x (%s)\n", picm_addr,
               picm_addr == AD_PICM_PLUS ? "F-135+" : "F-135");

        /* start-advance: type=0x04, data=[PICM, 0x00, 0xa0] */
        uint8_t adv_data[] = {picm_addr, 0x00, 0xa0};
        pakon_packet adv_pkt, adv_reply;
        pakon_packet_build(&adv_pkt, 0x04, adv_data, 3);

        /* finalize-advance: type=0x04, data=[PICM, 0x00, 0xa2] */
        uint8_t fin_data[] = {picm_addr, 0x00, 0xa2};
        pakon_packet fin_pkt, fin_reply;
        pakon_packet_build(&fin_pkt, 0x04, fin_data, 3);

        /* HOST status poll: type=0x03, data=[HOST] */
        uint8_t host_data[] = {AD_HOST};
        pakon_packet host_pkt, host_reply;
        pakon_packet_build(&host_pkt, 0x03, host_data, 1);

        time_t t0 = time(NULL);
        unsigned long steps = 0;
        printf("  [advance] %lu step(s), limit %u s\n",
               steps_count, limit_sec);

        for (unsigned long s = 0; s < steps_count; s++) {
            time_t elapsed = time(NULL) - t0;
            if (limit_sec > 0 && (unsigned long)elapsed >= limit_sec) {
                printf("  [advance] time limit reached after %ld s\n", (long)elapsed);
                rc = 1;
                break;
            }

            /* Start the step. */
            r = pakon_cmd(dev, &adv_pkt, &adv_reply, timeout);
            ncmd++;
            if (r != PAKON_OK) { errs++; break; }

            /* Poll HOST until it signals frame in position (PS_SUCCESS). */
            for (int p = 0; p < 5000; p++) {
                r = pakon_cmd(dev, &host_pkt, &host_reply, timeout);
                ncmd++;
                if (r != PAKON_OK) { errs++; break; }
                uint8_t st = pakon_packet_status(&host_reply);
                if (st == PS_SUCCESS) break;
            }

            /* Finalize/stop the step. */
            r = pakon_cmd(dev, &fin_pkt, &fin_reply, timeout);
            ncmd++;
            if (r != PAKON_OK) errs++;

            steps++;
            printf("  step %lu/%lu done (%ld s elapsed)\n",
                   steps, steps_count, (long)(time(NULL) - t0));
        }
        printf("  [advance] %lu/%lu steps in %ld s\n",
               steps, steps_count, (long)(time(NULL) - t0));
    }

    pakon_usb_release(dev);
    pakon_usb_close(dev);
    pakon_usb_exit(ctx);
    printf("advance done: %lu commands (%lu errors)\n", ncmd, errs);
    return rc || errs ? 1 : 0;
}

/* ---- Phase 5: replay a full scan operation script (.pakscan) ---- */

#define SCAN_IMG_TIMEOUT_MS 5000

/* Parse a hex byte string into out[max]; returns count or -1. */
static int hexbytes(const char *s, uint8_t *out, size_t max)
{
    size_t n = 0;
    int hi = -1;
    for (; *s && *s != '\n' && *s != '\r'; s++) {
        if (*s == ' ') continue;
        int v = (*s >= '0' && *s <= '9') ? *s - '0'
              : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10
              : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
        if (v < 0) return -1;
        if (hi < 0) hi = v;
        else { if (n >= max) return -1; out[n++] = (uint8_t)((hi<<4)|v); hi = -1; }
    }
    return hi < 0 ? (int)n : -1;
}

/* ---- End-of-roll detection (shared by --scan --autostop and --scan-sm) ----
 * The scan ends with the open gate shining through no film: samples near the
 * 16-bit max (~48900). Film -- even clear base, leader, or inter-frame gaps
 * (orange C-41 base) -- is far darker, so a chunk that is almost all "white"
 * means no film. Latch film_seen on the first non-white chunk, then stop after
 * a sustained run of trailing white. See docs/PROTOCOL.md photometry. */
#define SM_WHITE_THRESH   40000u  /* 16-bit sample > this => open-gate "white"
                                   * (open gate ~48900; film midtones ~15-19k; IR
                                   * band ~32k; so only true open-gate visible
                                   * samples clear this) */
#define SM_WHITE_FRAC_PCT 60u     /* chunk is white if >= this %% of samples are.
                                   * 60 not 90: each line is [visible|IR] and the
                                   * ~25% IR band sits at ~32k (< threshold), so an
                                   * open-gate chunk is only ~75% "white" -- 90%
                                   * could never trip and the scan ran to the cap */
#define SM_TRAIL_WHITE    24u     /* consecutive white chunks after film => done
                                   * (generous so a blown-highlight patch within a
                                   * frame doesn't false-stop) */
#define SM_MAX_EMPTY      2u      /* empty windows (~5s each) AFTER film => end of roll */
#define SM_LOAD_WAIT      24u     /* empty windows BEFORE film => waiting for the
                                   * operator to feed the film (~5s each, ~2 min) */
#define SM_FILM_THRESH    8000u   /* 16-bit sample > this => real film-band content.
                                   * Real C-41 film chunks run ~12k-27k mean with
                                   * >=20%% of samples over this; the dim leader/edge
                                   * light (~2.4k mean) has ~0%%. Used to gate
                                   * film_seen so the dim pre-film leader is NOT
                                   * mistaken for film (which armed end-of-roll
                                   * before the open-gate load gap and stopped the
                                   * scan immediately). */
#define SM_FILM_FRAC_PCT  10u     /* chunk has film if >= this %% of samples clear
                                   * SM_FILM_THRESH. 10 sits well between leader (0%%)
                                   * and real film (>=20%%). */

/* Is a 0x86 chunk dominated by open-gate white (no film in the gate)? Counts
 * 16-bit samples above SM_WHITE_THRESH; an open-gate chunk is ~75% white (the IR
 * band is the rest), film chunks ~0% (content is well below threshold). Used by
 * both the driven loop's end-of-roll and verbatim --autostop. */
static int sm_chunk_is_white(const uint8_t *buf, size_t got)
{
    size_t samples = got / 2;
    if (samples == 0) return 0;
    size_t white = 0;
    for (size_t i = 0; i + 1 < got; i += 2) {
        unsigned v = (unsigned)buf[i] | ((unsigned)buf[i + 1] << 8);
        if (v > SM_WHITE_THRESH) white++;
    }
    return white * 100u >= samples * SM_WHITE_FRAC_PCT;
}

/* Does a 0x86 chunk carry real film-band content (vs the dim pre-film leader)?
 * Counts 16-bit samples above SM_FILM_THRESH: real C-41 film (visible 15-19k +
 * IR ~32k) clears it on nearly every sample; the dim leader/edge light (~2.4k)
 * clears it on ~0%%. Used to gate film_seen so the leader is NOT mistaken for
 * film -- the open-gate gap at the film-load point used to trip end-of-roll
 * because the dim leader had already (wrongly) armed it. Open-gate white also
 * clears this threshold, but callers test sm_chunk_is_white() FIRST. */
static int sm_chunk_has_film(const uint8_t *buf, size_t got)
{
    size_t samples = got / 2;
    if (samples == 0) return 0;
    size_t lit = 0;
    for (size_t i = 0; i + 1 < got; i += 2) {
        unsigned v = (unsigned)buf[i] | ((unsigned)buf[i + 1] << 8);
        if (v > SM_FILM_THRESH) lit++;
    }
    return lit * 100u >= samples * SM_FILM_FRAC_PCT;
}

/* After the script ends, keep polling + draining 0x86 until it goes quiet.
 * Used for scans longer than the captured reference (e.g. whole rolls). */
static void drain_image(pakon_dev *dev, FILE *img,
                        unsigned long *ncmd, unsigned long *nimg,
                        unsigned long long *img_bytes, unsigned long *errs,
                        unsigned timeout)
{
    /* 03 01 10 = status poll HOST — the tight loop command from the capture */
    uint8_t poll_data[] = {0x10};
    pakon_packet poll_pkt, poll_reply;
    pakon_packet_build(&poll_pkt, 0x03, poll_data, 1);

    uint8_t buf[20480];
    /* Honor Ctrl-C here too: the drain can outlive the script by minutes,
     * and the caller's teardown must still run afterwards. */
    while (!scan_interrupted) {
        pakon_result r = pakon_cmd(dev, &poll_pkt, &poll_reply, timeout);
        if (r != PAKON_OK) (*errs)++;
        (*ncmd)++;

        size_t got = 0;
        r = pakon_usb_recv(dev, PAKON_EP_IMAGE_IN, buf, sizeof(buf), &got,
                           SCAN_IMG_TIMEOUT_MS);
        if (got) { fwrite(buf, 1, got, img); *img_bytes += got; }
        if (r != PAKON_OK && r != PAKON_ERR_TIMEOUT) (*errs)++;
        if (++(*nimg) % 1000 == 0)
            printf("  ... %lu image reads, %llu bytes\n", *nimg, *img_bytes);
        if (got == 0 || r == PAKON_ERR_TIMEOUT) break;
    }
}

/* Is this command one whose live reply we want to trace? Polls (03 01 xx) and
 * the engine kicks (8a/92 readout, a0/a2 motor). */
static int trace_interesting(const uint8_t *raw, int n)
{
    if (n >= 3 && raw[0] == 0x03 && raw[1] == 0x01) return 1;          /* poll  */
    if (n >= 5 && raw[0] == 0x04 && raw[1] == 0x03 &&
        (raw[4] == 0x8a || raw[4] == 0x92 || raw[4] == 0xa0 || raw[4] == 0xa2))
        return 1;                                                      /* kick  */
    return 0;
}

static long sm_replay_teardown(pakon_dev *dev, const char *script, unsigned timeout,
                               unsigned long *ncmd, unsigned long *errs);

static int do_scan(const char *script, const char *image_path, unsigned timeout,
                   int drain, int trace_status, int autostop)
{
    FILE *fp = fopen(script, "r");
    if (!fp) { fprintf(stderr, "cannot open scan script '%s'\n", script); return 1; }

    pakon_ctx *ctx = NULL;
    if (pakon_usb_init(&ctx) != PAKON_OK) { fclose(fp); return 1; }
    pakon_dev *dev = NULL;
    pakon_result r = pakon_usb_open(ctx, &dev);
    if (r != PAKON_OK) {
        fprintf(stderr, "open device failed: %s (need operational f135)\n",
                pakon_result_str(r));
        fclose(fp); pakon_usb_exit(ctx); return 1;
    }
    if (pakon_usb_claim(dev, 0, 0) != PAKON_OK) {
        fprintf(stderr, "claim failed\n");
        pakon_usb_close(dev); fclose(fp); pakon_usb_exit(ctx); return 1;
    }

    FILE *img = fopen(image_path, "wb");
    if (!img) {
        fprintf(stderr, "cannot open image '%s'\n", image_path);
        pakon_usb_release(dev); pakon_usb_close(dev); fclose(fp);
        pakon_usb_exit(ctx); return 1;
    }

    char line[1024];
    uint8_t buf[65536];
    unsigned long ncmd = 0, nimg = 0, errs = 0;
    unsigned long long img_bytes = 0;
    int rc = 0;
    /* --autostop: watch the image stream and stop when the film actually ends,
     * regardless of the captured length. Replay a full-roll capture and a
     * shorter film just stops earlier. */
    int film_seen = 0, stop_early = 0;
    unsigned trail_white = 0;
    /* End-of-roll detection must stay DISARMED through the pre-scan phase: the
     * captured script first replays ~1889 calibration/positioning image reads in
     * which the gate flashes open (long open-gate WHITE runs) with brief film in
     * between. Those would false-trigger end-of-roll. Arm only once the motor
     * starts (04 03 24 00 a0) -- from there the CCD free-runs the real scan and
     * trailing white genuinely means the film has run out. (This is the "offset"
     * that keyed off motor-start, not the leader.) */
    int scan_armed = 0;

    if (autostop)
        printf("  [autostop] will arm at motor-start, then stop at end-of-roll "
               "white (%u trailing-white chunks after film)\n", SM_TRAIL_WHITE);

    /* Ctrl-C during a scan must not abandon the scanner mid-flight (motor
     * running, CCD acquiring): catch the first SIGINT, break out, and replay
     * the script's captured teardown. A second Ctrl-C kills as usual. */
    scan_interrupted = 0;
    void (*previous_sigint)(int) = signal(SIGINT, scan_sigint_handler);

    while (fgets(line, sizeof(line), fp)) {
        if (scan_interrupted) {
            printf("  [interrupt] Ctrl-C: stopping scan, replaying captured "
                   "teardown...\n");
            stop_early = 1;
            break;
        }
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        if (*p == 'O') {                                  /* command + reply */
            int n = hexbytes(p + 1, buf, sizeof(buf));
            if (n < 0) { fprintf(stderr, "bad O line\n"); rc = 1; break; }
            /* Save the command bytes before the reply overwrites buf. */
            uint8_t snd[8]; int sn = n < (int)sizeof(snd) ? n : (int)sizeof(snd);
            memcpy(snd, buf, (size_t)sn);
            /* Arm end-of-roll detection at motor-start (04 03 <picm> 00 a0):
             * the real continuous scan begins here; everything before is
             * pre-scan. The motor PIC is 0x24 on the F-135 and 0x44 on the
             * F-135+, so accept either — keying on 0x24 alone meant autostop
             * silently never armed on F-135+ scripts. */
            if (autostop && !scan_armed && sn >= 5 &&
                snd[0] == 0x04 && snd[1] == 0x03 &&
                (snd[2] == AD_PICM || snd[2] == AD_PICM_PLUS) &&
                snd[3] == 0x00 && snd[4] == 0xa0) {
                scan_armed = 1;
                printf("  [autostop] motor started at read %lu -> arming end-of-roll\n",
                       nimg);
            }
            int trace = trace_status && trace_interesting(snd, n);
            size_t sent = 0, got = 0;
            r = pakon_usb_send(dev, PAKON_EP_CMD_OUT, buf, (size_t)n, &sent, timeout);
            if (r == PAKON_OK)
                r = pakon_usb_recv(dev, PAKON_EP_CMD_IN, buf, sizeof(buf), &got, timeout);
            if (r != PAKON_OK) errs++;
            if (trace) {
                printf("  [trace @img=%lu] send", nimg);
                for (int k = 0; k < sn; k++) printf(" %02x", snd[k]);
                if (r == PAKON_OK) {
                    printf("  reply");
                    for (size_t k = 0; k < got; k++) printf(" %02x", buf[k]);
                    printf("  (status=%u)", got >= 4 ? buf[3] : 0);
                } else {
                    printf("  -> %s", pakon_result_str(r));
                }
                printf("\n");
            }
            ncmd++;
        } else if (*p == 'M') {                           /* image read */
            unsigned long want = strtoul(p + 1, NULL, 0);
            if (want > sizeof(buf)) want = sizeof(buf);
            size_t got = 0;
            r = pakon_usb_recv(dev, PAKON_EP_IMAGE_IN, buf, want, &got, SCAN_IMG_TIMEOUT_MS);
            if (got) { fwrite(buf, 1, got, img); img_bytes += got; }
            if (r != PAKON_OK && r != PAKON_ERR_TIMEOUT) errs++;
            if (++nimg % 1000 == 0)
                printf("  ... %lu image reads, %llu bytes\n", nimg, img_bytes);
            if (autostop && scan_armed && got) {
                if (sm_chunk_is_white(buf, got)) {
                    if (film_seen && ++trail_white >= SM_TRAIL_WHITE) {
                        printf("  [autostop] end-of-roll white (%u chunks) after "
                               "%lu reads / %llu bytes -> stopping early\n",
                               trail_white, nimg, img_bytes);
                        stop_early = 1;
                        break;
                    }
                } else if (sm_chunk_has_film(buf, got)) {
                    if (!film_seen)
                        printf("  [autostop] film detected at read %lu\n", nimg);
                    film_seen = 1;
                    trail_white = 0;
                }
                /* else: dim leader / dark gap -- neither film nor open-gate, so
                 * don't arm film_seen (its premature latch on the leader was the
                 * "stops immediately at the load gap" bug). */
            }
        } else if (*p == 'C') {                           /* control xfer */
            unsigned brt, breq, wval, widx, wlen; int consumed = 0;
            if (sscanf(p + 1, "%x %x %x %x %x%n", &brt,&breq,&wval,&widx,&wlen,&consumed) != 5) {
                fprintf(stderr, "bad C line\n"); rc = 1; break;
            }
            int dn = hexbytes(p + 1 + consumed, buf, sizeof(buf));
            if (dn < 0) dn = 0;
            size_t got = 0;
            r = pakon_usb_control(dev, (uint8_t)brt, (uint8_t)breq, (uint16_t)wval,
                                  (uint16_t)widx, buf, (uint16_t)wlen, &got, timeout);
            if (r != PAKON_OK) errs++;
        }
    }

    int teardown_ran = 0;
    if (!rc && stop_early) {
        /* We broke out mid-script (end-of-roll or Ctrl-C), so the script's own
         * teardown tail was skipped. Replay it now to stop the engines and
         * reset to idle (same sequence the driver runs at its own end-of-roll). */
        printf("  [teardown] replaying captured teardown (stop + engine reset)...\n");
        long td = sm_replay_teardown(dev, script, timeout, &ncmd, &errs);
        printf("  [teardown] %ld commands replayed\n", td);
        teardown_ran = 1;
    } else if (!rc && drain) {
        printf("  [drain] script exhausted, draining 0x86 until scanner signals done...\n");
        drain_image(dev, img, &ncmd, &nimg, &img_bytes, &errs, timeout);
        if (scan_interrupted) {
            /* Ctrl-C landed during the drain, after the script completed —
             * still stop the engines before exiting. */
            printf("  [interrupt] Ctrl-C during drain: replaying captured "
                   "teardown...\n");
            long td = sm_replay_teardown(dev, script, timeout, &ncmd, &errs);
            printf("  [teardown] %ld commands replayed\n", td);
            teardown_ran = 1;
        }
    }

    signal(SIGINT, previous_sigint);
    pakon_usb_release(dev);
    pakon_usb_close(dev);
    pakon_usb_exit(ctx);
    fclose(fp);
    fclose(img);
    /* An interrupted scan produced a truncated raw: never report success, or
     * shell chains and the web layer will decode the partial file as a
     * finished scan. */
    if (scan_interrupted && !rc)
        rc = 1;
    printf("\nscan replay done: %lu commands, %lu image reads, %llu image bytes "
           "-> %s (%lu transfer errors)%s\n",
           ncmd, nimg, img_bytes, image_path, errs,
           !scan_interrupted ? ""
           : teardown_ran ? " [interrupted by Ctrl-C, teardown sent]"
                          : " [interrupted by Ctrl-C]");
    return rc;
}

/* ---- Phase 5b: poll-driven scan state machine ----------------------------
 *
 * Verbatim replay locks the scan to the captured image-read count, so it only
 * works for a roll the same length as the reference capture. The state machine
 * replays just the deterministic SETUP spine (OPEN -> param table -> the
 * calibration-derived register writes -> motor start, all of which we cannot
 * synthesise) and then DRIVES the image transfer itself, reading 0x86 and
 * re-arming the CCD until the film has fully passed.
 *
 * Done-signal: we cannot mine it offline (the .pakscan stores commands sent,
 * never the poll replies, and test/captures is empty), so we use the IMAGE
 * data: the scan ends with the open gate shining through no film (samples near
 * the 16-bit max, ~48900; film -- even clear base/leader -- is far darker). We
 * latch "film seen" on the first non-white chunk, then stop once we have seen a
 * sustained run of end-of-roll white. Every status poll is logged so a hardware
 * run also reveals the real protocol done-signal for a future tightening.
 *
 * Split marker between replay and takeover = the motor-start command
 * 04 03 24 00 a0 (confirmed). Everything up to the first image read after it is
 * replayed; from there we take over. */

/* (white-detection constants + sm_chunk_is_white live above do_scan, shared.) */
static const uint8_t SM_MOTOR_START[] = {0x04,0x03,0x24,0x00,0xa0};

/* Replay this many image reads PAST motor-start before taking over, so the
 * post-motor setup burst (control-strobe reg0=0x0161 + the reg9 integration
 * writes, which land between reads 1-3 in the capture) is in place — taking over
 * at the very first read leaves integration unconfigured and the stream stalls. */
#define SM_TAKEOVER_READS 64u

/* Build + send a frame whose wire bytes are raw[0..n) (raw[0]=type,[1]=count,
 * [2..]=data). Reads (and discards) the reply on EP1 IN. */
static pakon_result sm_cmd(pakon_dev *dev, const uint8_t *raw, size_t n,
                           pakon_packet *reply, unsigned timeout)
{
    (void)n;   /* count is carried in raw[1]; n is the caller's sizeof sanity */
    pakon_packet pkt;
    pakon_packet_build(&pkt, raw[0], raw + 2, raw[1]);
    return pakon_cmd(dev, &pkt, reply, timeout);
}

/* The poll-driven image phase.
 *
 * CONFIRMED from a full --trace-status scan: re-arming (8a) happens ONLY in the
 * preview/calibration phase (all 21 re-arms at <= the motor-start read index);
 * once the motor runs the CCD streams continuously with ZERO re-arms until the
 * film ends. So here we NEVER send a state-changing command -- we only read
 * 0x86 and issue read-only HOST status polls. (The earlier re-arm-on-empty
 * design wedged the bus by writing into a live stream.)
 *
 * HOST status: 0x00 = ready, 0x80 = busy (buffer momentarily starved). The
 * blocking bulk read absorbs busy (device NAKs, libusb waits), so a real read
 * timeout means no data. On timeout we poll HOST: busy => keep waiting; ready
 * => nothing left. Primary stop is end-of-roll white. */
static void sm_scan_loop(pakon_dev *dev, FILE *img, unsigned long long *img_bytes,
                         unsigned long *nimg, unsigned long *ncmd, unsigned long *errs,
                         unsigned timeout, unsigned long max_mb, int *film_seen_out)
{
    const uint8_t poll_host[] = {0x03,0x01,0x10};

    uint8_t buf[20480];
    int film_seen = 0;
    unsigned trail_white = 0, idle = 0;
    unsigned long long max_bytes = (unsigned long long)max_mb * 1024u * 1024u;
    pakon_packet reply;

    printf("  [sm] driven image phase: poll HOST + read 0x86 (no re-arm); stop on "
           "%u trailing open-gate chunks, %u empty windows after film, or %lu MB cap\n",
           SM_TRAIL_WHITE, SM_MAX_EMPTY, max_mb);

    for (;;) {
        size_t got = 0;
        pakon_result r = pakon_usb_recv(dev, PAKON_EP_IMAGE_IN, buf, sizeof(buf),
                                        &got, SCAN_IMG_TIMEOUT_MS);
        if (got) {
            fwrite(buf, 1, got, img);
            *img_bytes += got;
            (*nimg)++;
            idle = 0;
            if (sm_chunk_is_white(buf, got)) {     /* open-gate bright = no film */
                if (film_seen && ++trail_white >= SM_TRAIL_WHITE) {
                    printf("  [sm] end-of-roll: %u open-gate chunks after film "
                           "-> film fully scanned (%llu bytes)\n",
                           trail_white, *img_bytes);
                    break;
                }
            } else if (sm_chunk_has_film(buf, got)) {  /* real film in the gate */
                if (!film_seen)
                    printf("  [sm] film detected at %llu bytes\n", *img_bytes);
                film_seen = 1;
                trail_white = 0;
            }
            /* else: dim leader / dark gap -- don't latch film_seen on it. */
            if (max_bytes && *img_bytes >= max_bytes) {
                printf("  [sm] hit safety cap %lu MB -> stopping\n", max_mb);
                break;
            }
            continue;
        }
        if (r != PAKON_OK && r != PAKON_ERR_TIMEOUT) (*errs)++;

        /* A read window elapsed with zero bytes. Poll HOST (READ-ONLY) and use
         * its status: 0x80 = busy/starved (more data coming, keep waiting), 0x00
         * = ready/idle (nothing in flight). We do NOT re-arm: once the motor runs
         * the CCD free-runs continuously, and kicking 8a into a live stream wedges
         * the bus (the old re-arm-on-empty bug). The SM_LOAD_WAIT / SM_MAX_EMPTY
         * bounds cap the spin; teardown (Phase 3) is what actually stops it. */
        uint8_t st = 0xff;
        if (sm_cmd(dev, poll_host, sizeof(poll_host), &reply, timeout) == PAKON_OK)
            st = pakon_packet_status(&reply);
        else (*errs)++;
        (*ncmd)++;
        if (st == 0x80) continue;   /* device busy -> data still coming, wait */

        idle++;
        if (!film_seen) {                 /* operator still feeding the film */
            if (idle >= SM_LOAD_WAIT) {
                fprintf(stderr, "  [sm] no film fed within ~%u s -> aborting\n",
                        idle * (SCAN_IMG_TIMEOUT_MS / 1000));
                break;
            }
            printf("  [sm] waiting for film... (%u/%u, HOST st=0x%02x)\n",
                   idle, SM_LOAD_WAIT, st);
            continue;
        }
        /* Film already scanned and now no data -> end of roll. Stop promptly so
         * the motor doesn't run the film out (white detection above is the
         * faster primary stop). */
        printf("  [sm] empty read after film, HOST st=0x%02x (idle %u/%u)\n",
               st, idle, SM_MAX_EMPTY);
        if (idle >= SM_MAX_EMPTY) {
            printf("  [sm] stream ended (no data for %u windows) -> end of roll\n", idle);
            break;
        }
    }
    if (film_seen_out) *film_seen_out = film_seen;
}

/* Replay the captured teardown tail: every O/C line AFTER the last image read.
 * This is the driver's stop + engine-reset sequence (the bare 92/a2 stop plus
 * the PICM/PICL register resets that follow). Without it the engines are left
 * mid-state and the NEXT operation (e.g. advance) hangs. The many trailing
 * idle polls are harmless read-only status reads. Returns commands replayed,
 * 0 if the script has no image reads, -1 on open failure. */
static long sm_replay_teardown(pakon_dev *dev, const char *script, unsigned timeout,
                               unsigned long *ncmd, unsigned long *errs)
{
    FILE *fp = fopen(script, "r");
    if (!fp) return -1;
    char line[1024];
    long ln = 0, lastM = -1;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == 'M') lastM = ln;
        ln++;
    }
    if (lastM < 0) { fclose(fp); return 0; }

    rewind(fp);
    uint8_t buf[65536];
    long cur = 0, replayed = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (cur++ <= lastM) continue;               /* skip up to the last M */
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        if (*p == 'O') {
            int n = hexbytes(p + 1, buf, sizeof(buf));
            if (n < 0) continue;
            size_t sent = 0, got = 0;
            pakon_result r = pakon_usb_send(dev, PAKON_EP_CMD_OUT, buf, (size_t)n, &sent, timeout);
            if (r == PAKON_OK)
                r = pakon_usb_recv(dev, PAKON_EP_CMD_IN, buf, sizeof(buf), &got, timeout);
            if (r != PAKON_OK) (*errs)++;
            (*ncmd)++; replayed++;
        } else if (*p == 'C') {
            unsigned brt, breq, wval, widx, wlen; int consumed = 0;
            if (sscanf(p + 1, "%x %x %x %x %x%n", &brt,&breq,&wval,&widx,&wlen,&consumed) != 5)
                continue;
            int dn = hexbytes(p + 1 + consumed, buf, sizeof(buf)); if (dn < 0) dn = 0;
            size_t got = 0;
            pakon_result r = pakon_usb_control(dev, (uint8_t)brt, (uint8_t)breq,
                                               (uint16_t)wval, (uint16_t)widx, buf,
                                               (uint16_t)wlen, &got, timeout);
            if (r != PAKON_OK) (*errs)++;
            (*ncmd)++; replayed++;
        }
    }
    fclose(fp);
    return replayed;
}

static int do_scan_sm(const char *script, const char *image_path, unsigned timeout,
                      unsigned long max_mb)
{
    FILE *fp = fopen(script, "r");
    if (!fp) { fprintf(stderr, "cannot open scan script '%s'\n", script); return 1; }

    pakon_ctx *ctx = NULL;
    if (pakon_usb_init(&ctx) != PAKON_OK) { fclose(fp); return 1; }
    pakon_dev *dev = NULL;
    pakon_result r = pakon_usb_open(ctx, &dev);
    if (r != PAKON_OK) {
        fprintf(stderr, "open device failed: %s (need operational f135)\n",
                pakon_result_str(r));
        fclose(fp); pakon_usb_exit(ctx); return 1;
    }
    if (pakon_usb_claim(dev, 0, 0) != PAKON_OK) {
        fprintf(stderr, "claim failed\n");
        pakon_usb_close(dev); fclose(fp); pakon_usb_exit(ctx); return 1;
    }

    FILE *img = fopen(image_path, "wb");
    if (!img) {
        fprintf(stderr, "cannot open image '%s'\n", image_path);
        pakon_usb_release(dev); pakon_usb_close(dev); fclose(fp);
        pakon_usb_exit(ctx); return 1;
    }

    char line[1024];
    uint8_t buf[65536];
    unsigned long ncmd = 0, nimg = 0, errs = 0;
    unsigned long long img_bytes = 0;
    int rc = 0, passed_motor_start = 0, took_over = 0;
    unsigned reads_after_motor = 0;

    /* Phase 1: replay the deterministic setup spine, including preview/cal image
     * reads and the post-motor setup burst, up to SM_TAKEOVER_READS reads after
     * motor start; then hand off to the poll-driven loop. */
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        if (*p == 'O') {
            int n = hexbytes(p + 1, buf, sizeof(buf));
            if (n < 0) { fprintf(stderr, "bad O line\n"); rc = 1; break; }
            /* Note the motor-start marker before the reply overwrites buf. */
            int is_motor_start = ((size_t)n == sizeof(SM_MOTOR_START) &&
                                  memcmp(buf, SM_MOTOR_START, n) == 0);
            size_t sent = 0;
            r = pakon_usb_send(dev, PAKON_EP_CMD_OUT, buf, (size_t)n, &sent, timeout);
            if (r == PAKON_OK) {
                size_t got = 0;
                r = pakon_usb_recv(dev, PAKON_EP_CMD_IN, buf, sizeof(buf), &got, timeout);
            }
            if (r != PAKON_OK) errs++;
            ncmd++;
            if (is_motor_start) {
                passed_motor_start = 1;
                printf("  [sm] motor start (a0) replayed at cmd %lu\n", ncmd);
            }
        } else if (*p == 'M') {
            if (passed_motor_start && reads_after_motor >= SM_TAKEOVER_READS) {
                printf("  [sm] setup + burst replayed (%lu cmds, %u reads past "
                       "motor); taking over scan\n", ncmd, reads_after_motor);
                took_over = 1;
                break;
            }
            unsigned long want = strtoul(p + 1, NULL, 0);
            if (want > sizeof(buf)) want = sizeof(buf);
            size_t got = 0;
            r = pakon_usb_recv(dev, PAKON_EP_IMAGE_IN, buf, want, &got, SCAN_IMG_TIMEOUT_MS);
            if (got) { fwrite(buf, 1, got, img); img_bytes += got; }
            if (r != PAKON_OK && r != PAKON_ERR_TIMEOUT) errs++;
            nimg++;
            if (passed_motor_start) reads_after_motor++;
        } else if (*p == 'C') {
            unsigned brt, breq, wval, widx, wlen; int consumed = 0;
            if (sscanf(p + 1, "%x %x %x %x %x%n", &brt,&breq,&wval,&widx,&wlen,&consumed) != 5) {
                fprintf(stderr, "bad C line\n"); rc = 1; break;
            }
            int dn = hexbytes(p + 1 + consumed, buf, sizeof(buf));
            if (dn < 0) dn = 0;
            size_t got = 0;
            r = pakon_usb_control(dev, (uint8_t)brt, (uint8_t)breq, (uint16_t)wval,
                                  (uint16_t)widx, buf, (uint16_t)wlen, &got, timeout);
            if (r != PAKON_OK) errs++;
        }
    }
    fclose(fp);

    /* Phase 2: poll-driven image transfer until end-of-roll white. */
    if (!rc) {
        if (!passed_motor_start)
            printf("  [sm] no motor-start (a0) in script; taking over at end of "
                   "script\n");
        int film_seen = 0;
        sm_scan_loop(dev, img, &img_bytes, &nimg, &ncmd, &errs, timeout, max_mb,
                     &film_seen);
        if (!film_seen)
            fprintf(stderr, "  [sm] WARNING: no film ever detected in the stream "
                    "(stale device state / nothing loaded?)\n");

        /* Phase 3: replay the captured teardown (stop + engine reset) -- the
         * driver's full 761-command tail. This is the KNOWN-GOOD reset (returns
         * the device to Idle 0/5). A synthesized 4-command teardown was tried and
         * REVERTED: it assumed 02 04 20 01 80 00 = "lamp off" and left both the
         * illumination off (next scan read dark) and the engine not fully reset.
         * Do not re-synthesize the teardown without knowing each command. */
        printf("  [sm] replaying captured teardown (stop + engine reset)...\n");
        long td = sm_replay_teardown(dev, script, timeout, &ncmd, &errs);
        if (td > 0) {
            printf("  [sm] teardown: %ld commands replayed (engines reset to idle)\n", td);
        } else {
            uint8_t stop_readout[] = {0x04,0x03,0x20,0x00,0x92};
            uint8_t stop_motor[]   = {0x04,0x03,0x24,0x00,0xa2};
            sm_cmd(dev, stop_readout, sizeof(stop_readout), NULL, timeout);
            sm_cmd(dev, stop_motor,   sizeof(stop_motor),   NULL, timeout);
            ncmd += 2;
            printf("  [sm] no teardown tail in script; sent bare stop (92, a2)\n");
        }
    }
    (void)took_over;

    pakon_usb_release(dev);
    pakon_usb_close(dev);
    pakon_usb_exit(ctx);
    fclose(img);
    printf("\nscan-sm done: %lu commands, %lu image reads, %llu image bytes "
           "-> %s (%lu transfer errors)\n",
           ncmd, nimg, img_bytes, image_path, errs);
    return rc;
}

/* ---- Phase 6 / milestone 3: driven CALIBRATE -----------------------------
 *
 * Opens the device, replays the OPEN handshake to Idle, then runs the DRIVEN
 * calibration (measure open-gate CCD -> compute gain/offset) instead of
 * replaying frozen values. Prints the converged registers next to the OEM seed
 * values so a hardware run shows immediately whether the loop tracks the OEM.
 *
 * NEEDS-HARDWARE: this assumes the open gate (no film) is presented. The CCD
 * init beyond the OPEN handshake may need extending on hardware (see
 * pakon_calib_run / calib_acquire). Run on the Linux box and iterate. */
/* Replay the O (command) and C (control) lines of a .pakscan-style script,
 * skipping M (image-read) lines. Used as a calibration PRELUDE: the captured
 * CCD + illumination setup spine that lights the lamp and configures the sensor
 * before the driven loops take over. Returns commands replayed, -1 on open. */
static long calib_replay_prelude(pakon_dev *dev, const char *path, unsigned timeout)
{
    FILE *fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "cannot open prelude '%s'\n", path); return -1; }
    char line[1024];
    uint8_t buf[65536];
    long n = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 'O') {
            int nb = hexbytes(p + 1, buf, sizeof(buf));
            if (nb < 2) continue;
            pakon_packet cmd, reply;
            pakon_packet_build(&cmd, buf[0], buf + 2, buf[1]);
            pakon_cmd(dev, &cmd, &reply, timeout);
            n++;
        } else if (*p == 'C') {
            unsigned brt, breq, wval, widx, wlen; int consumed = 0;
            if (sscanf(p + 1, "%x %x %x %x %x%n",
                       &brt,&breq,&wval,&widx,&wlen,&consumed) != 5) continue;
            int dn = hexbytes(p + 1 + consumed, buf, sizeof(buf));
            if (dn < 0) dn = 0;
            size_t got = 0;
            pakon_usb_control(dev, (uint8_t)brt, (uint8_t)breq, (uint16_t)wval,
                              (uint16_t)widx, buf, (uint16_t)wlen, &got, timeout);
            n++;
        }
        /* M (image-read) lines intentionally skipped. */
    }
    fclose(fp);
    return n;
}

static int do_calibrate(unsigned timeout, size_t nlines, int verbose,
                        const char *prelude, unsigned exposure)
{
    pakon_ctx *ctx = NULL;
    if (pakon_usb_init(&ctx) != PAKON_OK) { fprintf(stderr, "init failed\n"); return 1; }
    pakon_dev *dev = NULL;
    pakon_result r = pakon_usb_open(ctx, &dev);
    if (r != PAKON_OK) {
        fprintf(stderr, "open device failed: %s (need operational f135)\n",
                pakon_result_str(r));
        pakon_usb_exit(ctx); return 1;
    }
    if (pakon_usb_claim(dev, 0, 0) != PAKON_OK) {
        fprintf(stderr, "claim failed\n");
        pakon_usb_close(dev); pakon_usb_exit(ctx); return 1;
    }

    if (prelude) {
        /* Replay the captured CCD + illumination setup spine (lamp on, sensor
         * configured) before driving the loops. */
        long np = calib_replay_prelude(dev, prelude, timeout);
        if (np < 0) { pakon_usb_release(dev); pakon_usb_close(dev);
                      pakon_usb_exit(ctx); return 1; }
        printf("prelude '%s' replayed (%ld commands); running driven calibration...\n",
               prelude, np);
    } else {
        /* No prelude: just the open handshake (dark-offset works, but the gain
         * phase needs illumination — pass --prelude resources/scan.pakscan). */
        size_t nseq = sizeof(OPEN_SEQ) / sizeof(OPEN_SEQ[0]);
        for (size_t i = 0; i < nseq; i++) {
            const open_step *s = &OPEN_SEQ[i];
            pakon_packet cmd, reply;
            pakon_packet_build(&cmd, s->out[0], s->out + 2, s->out[1]);
            if (pakon_cmd(dev, &cmd, &reply, timeout) != PAKON_OK)
                fprintf(stderr, "  open step '%s' failed (continuing)\n", s->label);
        }
        printf("open handshake done (no prelude); running driven calibration "
               "(present the open gate, no film)...\n");
    }

    pakon_calib_opts opts = {0};
    opts.nlines = nlines;
    opts.timeout_ms = timeout < 2000 ? 2000 : timeout;
    opts.exposure = exposure;
    opts.do_dark = 1;
    opts.do_gain = 1;
    opts.verbose = verbose;

    pakon_calib_result res;
    r = pakon_calib_run(dev, &opts, &res);

    pakon_usb_release(dev);
    pakon_usb_close(dev);
    pakon_usb_exit(ctx);

    if (r != PAKON_OK) {
        fprintf(stderr, "calibration failed: %s\n", pakon_result_str(r));
        return 1;
    }

    printf("\n=== calibration result (OEM seeds: gain~13, offset~-45) ===\n");
    const char *ch = "RGB";
    for (int k = 0; k < 3; k++)
        printf("  %c: gain=%-3d offset=%-5d  dark_mean=%-6u white_peak=%u\n",
               ch[k], res.gain[k], res.offset[k], res.dark[k], res.white[k]);
    printf("  dark offset converged: %s | gain converged: %s\n",
           res.offset_converged ? "yes" : "NO",
           res.gain_converged ? "yes" : "NO");
    printf("\nNOTE: if dark_mean/white_peak read ~0, the open-gate acquisition "
           "spine needs extending (see calib_acquire). This is the seam to tune.\n");
    return 0;
}

/* ---- read the cached calibration / param table (0xA4/0xA9) ----------------
 *
 * The OEM reads a parameter table over EP0 at session start: a 0xA4 trigger
 * (wValue=0x00a5, wIndex=0x1234) then a 0xA9 IN read (wValue=offset,
 * wIndex=0x1234) per chunk. Per the operator's observation that TLX only
 * calibrates slowly the first time and reuses the result after, this table is
 * the scanner's PERSISTED (EEPROM) calibration. We dump it so we can locate the
 * gain/offset/exposure/lamp fields and drive the backend from the cache instead
 * of reproducing a live open-gate calibration. Offsets/lengths mirror the
 * capture (scan.pakscan). */
static int do_read_params(unsigned timeout, const char *outpath)
{
    struct { uint16_t off, len; } chunks[] = {
        {0x0000,0x08},{0x0008,0x20},{0x0028,0x20},{0x0048,0x20},{0x0068,0x20},
        {0x0088,0x20},{0x00a8,0x20},{0x00c8,0x20},{0x00e8,0x20},{0x0108,0x20},
        {0x0128,0x20},{0x0148,0x20},{0x0168,0x20},{0x0188,0x06},
        {0x0800,0x08},{0x0808,0x1c},
    };
    size_t nchunks = sizeof(chunks)/sizeof(chunks[0]);

    pakon_ctx *ctx = NULL;
    if (pakon_usb_init(&ctx) != PAKON_OK) { fprintf(stderr, "init failed\n"); return 1; }
    pakon_dev *dev = NULL;
    pakon_result r = pakon_usb_open(ctx, &dev);
    if (r != PAKON_OK) {
        fprintf(stderr, "open device failed: %s (need operational f135)\n",
                pakon_result_str(r));
        pakon_usb_exit(ctx); return 1;
    }
    if (pakon_usb_claim(dev, 0, 0) != PAKON_OK) {
        fprintf(stderr, "claim failed\n");
        pakon_usb_close(dev); pakon_usb_exit(ctx); return 1;
    }
    /* open handshake to Idle (the table read follows it in the capture) */
    for (size_t i = 0; i < sizeof(OPEN_SEQ)/sizeof(OPEN_SEQ[0]); i++) {
        const open_step *s = &OPEN_SEQ[i];
        pakon_packet cmd, reply;
        pakon_packet_build(&cmd, s->out[0], s->out + 2, s->out[1]);
        pakon_cmd(dev, &cmd, &reply, timeout);
    }

    /* Assemble into a sparse buffer indexed by offset (covers up to 0x824). */
    static uint8_t table[0x900];
    static uint8_t valid[0x900];
    memset(table, 0, sizeof(table)); memset(valid, 0, sizeof(valid));
    unsigned errs = 0;
    for (size_t i = 0; i < nchunks; i++) {
        size_t got = 0;
        /* trigger */
        pakon_usb_control(dev, 0x40, 0xa4, 0x00a5, 0x1234, NULL, 0, &got, timeout);
        /* read chunk at its offset */
        uint8_t tmp[64];
        r = pakon_usb_control(dev, 0xc0, 0xa9, chunks[i].off, 0x1234,
                              tmp, chunks[i].len, &got, timeout);
        if (r != PAKON_OK) { errs++; continue; }
        for (size_t b = 0; b < got && chunks[i].off + b < sizeof(table); b++) {
            table[chunks[i].off + b] = tmp[b];
            valid[chunks[i].off + b] = 1;
        }
    }

    pakon_usb_release(dev);
    pakon_usb_close(dev);
    pakon_usb_exit(ctx);

    /* annotated hexdump of the valid bytes */
    printf("=== param table (%zu chunks, %u read errors) ===\n", nchunks, errs);
    for (size_t off = 0; off < sizeof(table); off += 16) {
        int any = 0;
        for (int b = 0; b < 16; b++) if (valid[off+b]) { any = 1; break; }
        if (!any) continue;
        printf("%04zx:", off);
        for (int b = 0; b < 16; b++)
            valid[off+b] ? printf(" %02x", table[off+b]) : printf(" --");
        printf("  ");
        for (int b = 0; b < 16; b++) {
            uint8_t c = table[off+b];
            putchar(valid[off+b] && c >= 0x20 && c < 0x7f ? c : '.');
        }
        printf("\n");
    }

    /* drift detection: compare the region checksums to the baseline */
    if (valid[0x04]) {
        uint32_t c1 = (uint32_t)(table[0x04] | (table[0x05]<<8) | (table[0x06]<<16) | (table[0x07]<<24));
        printf("\nregion1 checksum 0x%08x %s baseline 0x%08x\n", c1,
               c1 == PAKON_CALIB_EEPROM_CKSUM_R1 ? "==" : "!=", PAKON_CALIB_EEPROM_CKSUM_R1);
    }
    if (valid[0x804]) {
        uint32_t c2 = (uint32_t)(table[0x804] | (table[0x805]<<8) | (table[0x806]<<16) | (table[0x807]<<24));
        printf("region2 checksum 0x%08x %s baseline 0x%08x  (matches => default_config valid for this unit)\n",
               c2, c2 == PAKON_CALIB_EEPROM_CKSUM_R2 ? "==" : "!=", PAKON_CALIB_EEPROM_CKSUM_R2);
    }

    /* flag the OEM seed values so the fields are easy to spot */
    printf("\n=== candidate field locations (16-bit LE matches of OEM seeds) ===\n");
    struct { const char *name; uint16_t v; } seeds[] = {
        {"Gain=0x000d (13)", 0x000d}, {"Offset_R=0x0133 (-51 sm)", 0x0133},
        {"Offset_G=0x012a (-42)", 0x012a}, {"Offset_B=0x012b (-43)", 0x012b},
        {"Height=0x0c1a", 0x0c1a},
    };
    for (size_t s = 0; s < sizeof(seeds)/sizeof(seeds[0]); s++) {
        for (size_t off = 0; off + 1 < sizeof(table); off++) {
            if (!valid[off] || !valid[off+1]) continue;
            uint16_t v = (uint16_t)(table[off] | (table[off+1] << 8));
            if (v == seeds[s].v)
                printf("  %-26s @ 0x%04zx\n", seeds[s].name, off);
        }
    }

    if (outpath) {
        FILE *f = fopen(outpath, "wb");
        if (f) { fwrite(table, 1, sizeof(table), f); fclose(f);
                 printf("\nraw table written to %s (0x%zx bytes)\n", outpath, sizeof(table)); }
    }
    return errs ? 1 : 0;
}

/* ---- synthesized CONFIGURE (capture-free register programming) ------------
 * Open + (optional) init prelude + write the calibration register set from C
 * defaults (the OEM's validated converged values), instead of replaying frozen
 * captured writes. Proves we can program the calibration independently. */
static int do_configure(unsigned timeout, const char *prelude)
{
    pakon_ctx *ctx = NULL;
    if (pakon_usb_init(&ctx) != PAKON_OK) { fprintf(stderr, "init failed\n"); return 1; }
    pakon_dev *dev = NULL;
    pakon_result r = pakon_usb_open(ctx, &dev);
    if (r != PAKON_OK) {
        fprintf(stderr, "open device failed: %s (need operational f135)\n",
                pakon_result_str(r));
        pakon_usb_exit(ctx); return 1;
    }
    if (pakon_usb_claim(dev, 0, 0) != PAKON_OK) {
        fprintf(stderr, "claim failed\n");
        pakon_usb_close(dev); pakon_usb_exit(ctx); return 1;
    }
    if (prelude) {
        long np = calib_replay_prelude(dev, prelude, timeout);
        printf("prelude '%s' replayed (%ld commands)\n", prelude, np);
    } else {
        for (size_t i = 0; i < sizeof(OPEN_SEQ)/sizeof(OPEN_SEQ[0]); i++) {
            const open_step *s = &OPEN_SEQ[i];
            pakon_packet cmd, reply;
            pakon_packet_build(&cmd, s->out[0], s->out + 2, s->out[1]);
            pakon_cmd(dev, &cmd, &reply, timeout);
        }
    }

    pakon_calib_config cfg = pakon_calib_default_config();
    r = pakon_calib_configure(dev, &cfg, timeout < 2000 ? 2000 : timeout);

    pakon_usb_release(dev);
    pakon_usb_close(dev);
    pakon_usb_exit(ctx);

    if (r != PAKON_OK) {
        fprintf(stderr, "configure failed: %s\n", pakon_result_str(r));
        return 1;
    }
    printf("CONFIGURE ok: programmed gain=%d/%d/%d offset=%d/%d/%d height=0x%04x "
           "(all register writes accepted)\n",
           cfg.gain[0], cfg.gain[1], cfg.gain[2],
           cfg.offset[0], cfg.offset[1], cfg.offset[2], cfg.height);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s FILE.pakscan [--steps N] [--limit SEC] advance film\n"
        "       %s --scan FILE [--image OUT] [--drain] [--autostop]  scan replay\n"
        "       %s --scan-sm FILE [--image OUT] [--max-mb N]  poll-driven scan\n"
        "       %s --open                                verify open handshake\n"
        "       %s --calibrate [--cal-lines N] [--cal-verbose]  driven calibration\n"
        "       %s --read-params [--params-out FILE]      dump cached calibration table\n"
        "       %s --configure [--prelude FILE]           program calibration regs from C\n"
        "\n"
        "  FILE.pakscan  positional: replay advance script, then poll until idle\n"
        "  --limit SEC   wall-clock limit for the advance poll loop (default 60)\n"
        "  --open        replay the captured open handshake, verify replies\n"
        "  --calibrate   run the driven CALIBRATE (measure open-gate CCD ->\n"
        "                compute gain/offset); present the open gate, no film\n"
        "  --cal-lines N  CCD lines to average per measurement (default 32)\n"
        "  --cal-exposure N  nominal CcdExposure for the gain phase (default 256)\n"
        "  --prelude FILE replay a .pakscan setup spine (CCD+lamp init) before\n"
        "                calibrating -- needed for the gain phase (illumination)\n"
        "  --cal-verbose  log each calibration iteration\n"
        "  --scan FILE   replay a .pakscan scan script verbatim (fixed length)\n"
        "  --scan-sm FILE  replay setup spine, then drive the image transfer and\n"
        "                  stop on end-of-roll white (any roll length)\n"
        "  --image OUT   raw image output for --scan/--scan-sm (default pakon_scan.raw)\n"
        "  --advance     run the film transport for a fixed time to push the\n"
        "                strip out (standalone, or appended to --scan); probes\n"
        "                the motor PIC so it works on the F-135 and F-135+\n"
        "                alike. For frame-positioned advancing use the\n"
        "                FILE.pakscan mode.\n"
        "  --advance-seconds N  how long to run the transport (default 15;\n"
        "                a strip already at the exit needs only 1-2)\n"
        "  --drain       after the scan script ends, keep reading 0x86 until done\n"
        "  --autostop    with --scan: stop at end-of-roll white (film fully\n"
        "                scanned) and replay teardown -- replay a max-length\n"
        "                capture and any shorter film just stops earlier\n"
        "  --trace-status  with --scan: log live poll/kick replies + status, with\n"
        "                  the current image-read index (to learn the cadence)\n"
        "  --max-mb N    --scan-sm safety cap on image bytes (default 512, 0=off)\n"
        "  --timeout MS  USB per-transfer timeout in ms (default 1000)\n"
        "\n"
        "Set PAKON_DEBUG=0..4 for increasing trace verbosity.\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    int want_open = 0, drain = 0, trace_status = 0, autostop = 0;
    int want_advance_run = 0;       /* --advance: standalone or after --scan */
    unsigned advance_seconds = 15;
    int want_calibrate = 0, cal_verbose = 0, want_read_params = 0, want_configure = 0;
    unsigned long cal_lines = 32;
    unsigned long cal_exposure = 256;
    const char *cal_prelude = NULL;
    const char *params_out = NULL;
    const char *scan_file = NULL;
    const char *scan_sm_file = NULL;
    const char *advance_file = NULL;
    const char *image_path = "pakon_scan.raw";
    unsigned timeout = 1000;
    unsigned limit_sec = 60;
    unsigned long steps_count = 1;
    unsigned long max_mb = 512;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--open")) {
            want_open = 1;
        } else if (!strcmp(argv[i], "--calibrate")) {
            want_calibrate = 1;
        } else if (!strcmp(argv[i], "--read-params")) {
            want_read_params = 1;
        } else if (!strcmp(argv[i], "--configure")) {
            want_configure = 1;
        } else if (!strcmp(argv[i], "--params-out") && i + 1 < argc) {
            params_out = argv[++i];
        } else if (!strcmp(argv[i], "--cal-lines") && i + 1 < argc) {
            cal_lines = strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--prelude") && i + 1 < argc) {
            cal_prelude = argv[++i];
        } else if (!strcmp(argv[i], "--cal-exposure") && i + 1 < argc) {
            cal_exposure = strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--cal-verbose")) {
            cal_verbose = 1;
        } else if (!strcmp(argv[i], "--scan") && i + 1 < argc) {
            scan_file = argv[++i];
        } else if (!strcmp(argv[i], "--scan-sm") && i + 1 < argc) {
            scan_sm_file = argv[++i];
        } else if (!strcmp(argv[i], "--max-mb") && i + 1 < argc) {
            max_mb = strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--image") && i + 1 < argc) {
            image_path = argv[++i];
        } else if (!strcmp(argv[i], "--advance")) {
            want_advance_run = 1;
        } else if (!strcmp(argv[i], "--advance-seconds") && i + 1 < argc) {
            advance_seconds = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--drain")) {
            drain = 1;
        } else if (!strcmp(argv[i], "--trace-status")) {
            trace_status = 1;
        } else if (!strcmp(argv[i], "--autostop")) {
            autostop = 1;
        } else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            timeout = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--limit") && i + 1 < argc) {
            limit_sec = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--steps") && i + 1 < argc) {
            steps_count = strtoul(argv[++i], NULL, 0);
        } else if (argv[i][0] != '-' && !advance_file) {
            advance_file = argv[i];
        } else {
            fprintf(stderr, "unknown/incomplete argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!want_open && !want_calibrate && !want_read_params && !want_configure &&
        !scan_file && !scan_sm_file && !advance_file && !want_advance_run) {
        usage(argv[0]);
        return 2;
    }

    if (want_read_params)
        return do_read_params(timeout < 2000 ? 2000 : timeout, params_out);
    if (want_configure)
        return do_configure(timeout, cal_prelude);
    if (want_calibrate)
        return do_calibrate(timeout, (size_t)cal_lines, cal_verbose, cal_prelude,
                            (unsigned)cal_exposure);
    if (advance_file) {
        if (want_advance_run)
            fprintf(stderr, "note: --advance is ignored with an advance "
                    "script (that mode already drives the film)\n");
        return do_advance(advance_file, timeout, limit_sec, steps_count);
    }
    if (scan_sm_file) {
        int rc = do_scan_sm(scan_sm_file, image_path, timeout, max_mb);
        if (want_advance_run) {
            printf("\n[advance] pushing the film out of the transport...\n");
            int advance_rc = do_timed_advance(timeout, advance_seconds);
            if (!rc)
                rc = advance_rc;
        }
        return rc;
    }
    if (scan_file) {
        int rc = do_scan(scan_file, image_path, timeout, drain, trace_status,
                         autostop);
        if (want_advance_run) {
            printf("\n[advance] pushing the film out of the transport...\n");
            int advance_rc = do_timed_advance(timeout, advance_seconds);
            if (!rc)
                rc = advance_rc;
        }
        return rc;
    }
    if (want_advance_run)
        return do_timed_advance(timeout, advance_seconds);
    return do_open(timeout);
}
