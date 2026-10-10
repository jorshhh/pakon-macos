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
#include "pakon_eeprom.h"
#include "pakon_setup.h"
#include "pakon_lightcal.h"

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
 * the F-135+ captured advance block (motor speed 0xA5 -> engage 0xA0 -> FPGA
 * control reg 0 = acquire), keeps the transport running for `seconds`, then
 * the captured stop (acquire bit cleared -> 0xA2 -> status-LED words). Names
 * per TLB.dll (docs/REGISTERS.md): 0xA5 is the motor speed, bank 0x82 reg 0 is
 * the FPGA control word (bit 0 = acquire), bank 0x82 reg 9 the status LEDs.
 *
 * F-135+ only. The speed 0x647E (25726) is serial 16402's EEPROM base-4
 * MotorSpeed; the base F-135 clamps its motor to 400..9500 and has no captured
 * timed advance, so on a base F-135 this refuses and points at the validated
 * frame-advance script instead of sending another model's values. */
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
    if (picm_addr != AD_PICM_PLUS) {
        fprintf(stderr,
                "advance: F-135 detected. The timed --advance replays F-135+ "
                "captured values\n"
                "  (motor speed 0x647E, above the F-135's 9500 limit) and is not "
                "used on this model.\n"
                "  Use the validated frame advance instead:\n"
                "    pakon_replay resources/advance.pakscan --steps N\n");
        return 1;
    }
    printf("advance: motor PIC at 0x%02x (F-135+), running transport for %us\n",
           picm_addr, seconds);

    /* Captured F-135+ advance block (see docs/F135_PLUS_CAPTURES.md). Frame
     * data is [addr, payload_len, cmd/reg, payload...]; the type byte selects
     * CMD/WRITE/READ_STATUS per the confirmed wire format. */
    const uint8_t motor_speed[] = {picm_addr, 0x02, 0xa5, 0x7e, 0x64};
    const uint8_t engage[]      = {picm_addr, 0x00, 0xa0};
    const uint8_t acquire_on[]  = {picm_addr, 0x03, 0x82, 0x00, 0x63, 0x00};
    const uint8_t status_poll[] = {picm_addr};
    const uint8_t acquire_off[] = {picm_addr, 0x03, 0x82, 0x00, 0x60, 0x00};
    const uint8_t disengage[]   = {picm_addr, 0x00, 0xa2};
    const uint8_t leds_a[]      = {picm_addr, 0x03, 0x82, 0x09, 0x17, 0x02};
    const uint8_t leds_b[]      = {picm_addr, 0x03, 0x82, 0x09, 0x17, 0x00};

    /* From here the motor runs: catch the first Ctrl-C so the stop writes
     * below always execute (a second Ctrl-C kills as usual). */
    scan_interrupted = 0;
    void (*previous_sigint)(int) = signal(SIGINT, scan_sigint_handler);

    int errs = 0;
    errs += advance_frame(dev, PH_WRITE, motor_speed, sizeof(motor_speed), timeout);
    errs += advance_frame(dev, PH_CMD, engage, sizeof(engage), timeout);
    errs += advance_frame(dev, PH_WRITE, acquire_on, sizeof(acquire_on), timeout);
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
    /* Stop: clear the FPGA acquire bit FIRST (0xA2 alone leaves the motor
     * running; the OEM clears it before 0xA2 too), then release the drive and
     * restore the status LEDs as captured. */
    errs += advance_frame(dev, PH_WRITE, acquire_off, sizeof(acquire_off), timeout);
    errs += advance_frame(dev, PH_CMD, disengage, sizeof(disengage), timeout);
    errs += advance_frame(dev, PH_WRITE, leds_a, sizeof(leds_a), timeout);
    errs += advance_frame(dev, PH_WRITE, leds_b, sizeof(leds_b), timeout);
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
 * 0x86 and run the event service (host poll; read + ack controller events).
 * (The earlier re-arm-on-empty design wedged the bus by writing into a live
 * stream.)
 *
 * HOST flags: 0x80 = event pending (serviced here, then keep reading),
 * 0x02 = FIFO overflow. A real read timeout means no data; on timeout with no
 * event pending we count an idle window. Primary stop is end-of-roll white. */
static void sm_scan_loop(pakon_dev *dev, FILE *img, unsigned long long *img_bytes,
                         unsigned long *nimg, unsigned long *ncmd, unsigned long *errs,
                         unsigned timeout, unsigned long max_mb, int *film_seen_out)
{
    unsigned events = 0, overflows = 0;

    uint8_t buf[20480];
    int film_seen = 0;
    unsigned trail_white = 0, idle = 0;
    unsigned long long max_bytes = (unsigned long long)max_mb * 1024u * 1024u;

    printf("  [sm] driven image phase: read 0x86 + event service (no re-arm); stop on "
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
            /* Event service after every read (the OEM polls every 1 ms while
             * scanning); unacknowledged events are the suspected cause of
             * the stream dying part-way. --scan-sm is F-135 only. */
            uint8_t hf = 0;
            if (pakon_cmd_service_events(dev, AD_PICL, AD_PICM, timeout, &hf,
                                         &events) != PAKON_OK)
                (*errs)++;
            if (hf & PAKON_HOST_FLAG_OVERFLOW)
                overflows++;
            (*ncmd)++;
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
        if (pakon_cmd_service_events(dev, AD_PICL, AD_PICM, timeout, &st,
                                     &events) != PAKON_OK)
            (*errs)++;
        (*ncmd)++;
        if (st & PAKON_HOST_FLAG_EVENT) continue;   /* serviced; keep reading */

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
    printf("  [sm] event service: %u events acknowledged, %u FIFO-overflow flags\n",
           events, overflows);
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
/* ---- Controller init + mode configure built in code (no replay) ----------
 *
 * Detects the model with the PIC probes, takes the unit's Base 16 Offset from
 * an EEPROM archive (tools/pakon_eeprom.py backup; the chip is not read
 * again), then sends pakon_setup_init + pakon_setup_configure with the OEM
 * reply rules. Stops before the first acquire: LEDs off, motor released.
 * Only the modes the captures verified: F-135 Base 16 + IR, F-135+ Base 16.
 * With --teardown, then sends pakon_setup_teardown. Ends with two event-
 * service passes (the second should find nothing pending).
 */
static int load_eeprom_archive(const char *dir, pakon_eeprom *e)
{
    static const char *names[PAKON_EEPROM_NCOPIES] = {
        "sectionA_primary", "sectionA_backup", "sectionB_primary", "sectionB_backup"
    };
    static uint8_t buf[PAKON_EEPROM_NCOPIES][PAKON_EEPROM_A_LEN + 1];
    const uint8_t *data[PAKON_EEPROM_NCOPIES];
    size_t len[PAKON_EEPROM_NCOPIES];
    for (int i = 0; i < PAKON_EEPROM_NCOPIES; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/eeprom_0x52_%s.bin", dir, names[i]);
        FILE *f = fopen(path, "rb");
        data[i] = NULL;
        len[i] = 0;
        if (!f)
            continue;
        len[i] = fread(buf[i], 1, sizeof buf[i], f);
        data[i] = buf[i];
        fclose(f);
    }
    return pakon_eeprom_decode(data, len, e) == PAKON_OK;
}

/* Open the warm scanner, detect the model, check it against the EEPROM
 * archive and send the code-built setup (Base 16 / Base 16 + IR). On success
 * returns 0 with the device open and claimed; on failure returns 1 with
 * everything closed. */
static int setup_session(unsigned timeout, const char *eeprom_dir,
                         pakon_ctx **ctx_out, pakon_dev **dev_out,
                         pakon_eeprom *e, pakon_setup_state *st)
{
    if (!eeprom_dir || !load_eeprom_archive(eeprom_dir, e)) {
        fprintf(stderr, "setup: need --eeprom-dir with a decodable EEPROM "
                "archive (python3 tools/pakon_eeprom.py backup DIR)\n");
        return 1;
    }
    printf("eeprom: %s serial %u, Base 16 Offset %u\n",
           pakon_eeprom_model(e->scanner_type), e->serial,
           e->base[PAKON_EEPROM_BASE16].offset);

    pakon_ctx *ctx = NULL;
    pakon_dev *dev = NULL;
    if (pakon_usb_init(&ctx) != PAKON_OK)
        return 1;
    if (pakon_usb_open(ctx, &dev) != PAKON_OK) {
        fprintf(stderr, "open device failed (need warm 0F05:F135)\n");
        pakon_usb_exit(ctx);
        return 1;
    }
    if (pakon_usb_claim(dev, 0, 0) != PAKON_OK) {
        fprintf(stderr, "claim failed\n");
        pakon_usb_close(dev);
        pakon_usb_exit(ctx);
        return 1;
    }

    pakon_bridge_open(dev, timeout);
    pakon_pic_state plus = pakon_probe_pic(dev, AD_PICM_PLUS, NULL, timeout);
    pakon_pic_state base = pakon_probe_pic(dev, AD_PICM, NULL, timeout);
    uint8_t low, scn;
    uint16_t led_period;
    pakon_scan_mode m = { 0 };
    if (plus == PAKON_PIC_PRESENT && base == PAKON_PIC_ABSENT) {
        low = AD_PICL_PLUS;
        scn = AD_PICM_PLUS;
        led_period = PAKON_SETUP_LED_PERIOD_F135_PLUS;
        m.trigger = 0x003C;                 /* Base 16 */
        m.integration = 0x0FFD;
        printf("model: F-135+ -> Base 16\n");
    } else if (base == PAKON_PIC_PRESENT && plus == PAKON_PIC_ABSENT) {
        low = AD_PICL;
        scn = AD_PICM;
        led_period = PAKON_SETUP_LED_PERIOD_F135;
        m.trigger = 0x0010;                 /* Base 16 + IR */
        m.ir = 1;
        m.integration = 0x0C1A;
        printf("model: F-135 -> Base 16 + IR\n");
    } else {
        fprintf(stderr, "setup: model detection failed (plus=%d base=%d)\n",
                plus, base);
        goto fail;
    }
    if ((low == AD_PICL) != (e->scanner_type == PAKON_EEPROM_TYPE_F135)) {
        fprintf(stderr, "setup: EEPROM archive is for a %s, scanner is not\n",
                pakon_eeprom_model(e->scanner_type));
        goto fail;
    }
    pakon_setup_pixel_window(e->base[PAKON_EEPROM_BASE16].offset, 0,
                             &m.pixel_start, &m.pixel_end);

    pakon_seq seq;
    pakon_seq_init(&seq);
    if (pakon_setup_init(&seq, low, scn, led_period, st) != PAKON_OK ||
        pakon_setup_configure(&seq, st, &m) != PAKON_OK) {
        fprintf(stderr, "setup: could not build the sequence\n");
        goto fail;
    }
    printf("sending %zu frames...\n", seq.n);
    pakon_result r = pakon_cmd_run_seq(dev, &seq, timeout < 2000 ? 2000 : timeout);
    printf("setup: %s\n", r == PAKON_OK ? "OK, configured (not acquiring)"
                                        : pakon_result_str(r));
    if (r != PAKON_OK)
        goto fail;
    *ctx_out = ctx;
    *dev_out = dev;
    return 0;
fail:
    pakon_usb_release(dev);
    pakon_usb_close(dev);
    pakon_usb_exit(ctx);
    return 1;
}

static void session_close(pakon_ctx *ctx, pakon_dev *dev)
{
    pakon_usb_release(dev);
    pakon_usb_close(dev);
    pakon_usb_exit(ctx);
}

static int do_setup(unsigned timeout, const char *eeprom_dir, int teardown)
{
    pakon_ctx *ctx;
    pakon_dev *dev;
    pakon_eeprom e;
    pakon_setup_state st;
    if (setup_session(timeout, eeprom_dir, &ctx, &dev, &e, &st))
        return 1;

    unsigned t = timeout < 2000 ? 2000 : timeout;
    pakon_result r = PAKON_OK;
    if (teardown) {
        pakon_seq seq;
        pakon_seq_init(&seq);
        pakon_setup_teardown(&seq, &st);
        printf("teardown: sending %zu frames...\n", seq.n);
        r = pakon_cmd_run_seq(dev, &seq, t);
        printf("teardown: %s\n", r == PAKON_OK ? "OK" : pakon_result_str(r));
    }
    for (int pass = 1; r == PAKON_OK && pass <= 2; pass++) {
        uint8_t flags = 0;
        unsigned serviced = 0;
        r = pakon_cmd_service_events(dev, st.low, st.scn, t, &flags, &serviced);
        printf("events (pass %d): host flags 0x%02x, %u acknowledged%s\n", pass,
               flags, serviced, r == PAKON_OK ? "" : pakon_result_str(r));
    }
    session_close(ctx, dev);
    return r == PAKON_OK ? 0 : 1;
}

/* ---- Static light probe (step 7, first milestone) ------------------------
 *
 * Motor stopped, open gate (no film). After the code-built setup: A/D gains
 * 13 and serial 3054's converged dark offsets, acquire, read dark lines
 * (LEDs off), then LEDs on at the OEM's converged currents and duties from
 * scan.pakscan, read lit lines. Prints black-pixel means and active-pixel
 * column peaks per channel; the OEM's targets are R/G 64000, B 65500 cap,
 * IR 40000. Ends with the code-built teardown. F-135 only (the LED values
 * are serial 3054's).
 */
#define PROBE_CHUNK    20480u
#define PROBE_CHUNKS   40u     /* chunks measured (~51 lines) */

static int probe_read(pakon_dev *dev, const pakon_setup_state *st, unsigned t,
                      uint16_t *samples, size_t *nsamples)
{
    uint8_t buf[PROBE_CHUNK];
    size_t n = 0;
    for (unsigned i = 0; i < PROBE_CHUNKS; i++) {
        size_t got = 0;
        pakon_result r = pakon_usb_recv(dev, PAKON_EP_IMAGE_IN, buf, sizeof buf,
                                        &got, SCAN_IMG_TIMEOUT_MS);
        if (r != PAKON_OK || got == 0) {
            fprintf(stderr, "probe: image read %u: %s (%zu bytes)\n", i,
                    pakon_result_str(r), got);
            return 1;
        }
        if (pakon_cmd_service_events(dev, st->low, st->scn, t, NULL, NULL) != PAKON_OK)
            fprintf(stderr, "probe: event service failed at read %u\n", i);
        for (size_t k = 0; k + 1 < got; k += 2)
            samples[n++] = (uint16_t)(buf[k] | buf[k + 1] << 8);
    }
    *nsamples = n;
    return 0;
}

/* Skip this many whole lines after a re-arm before measuring. */
#define PROBE_SKIP_LINES 4u
/* Minimum black-to-lit step (counts) for a line alignment to count. */
#define LC_MIN_CONTRAST 3000.0

static void probe_report(const char *label, const uint16_t *s, size_t n,
                         size_t line_px, unsigned offset)
{
    const char *dump = getenv("PAKON_PROBE_DUMP");   /* debug: raw samples */
    if (dump) {
        char path[1024];
        snprintf(path, sizeof path, "%s/probe-%c.raw", dump, label[0]);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(s, sizeof *s, n, f);
            fclose(f);
        }
    }
    long o = pakon_lc_find_origin(s, n, line_px, LC_MIN_CONTRAST);
    if (o < 0) {
        printf("%s: no light to align lines on\n", label);
        return;
    }
    size_t origin = (size_t)o + PROBE_SKIP_LINES * 4 * line_px;
    size_t act0 = offset > 6 ? offset - 6 + 3 : PAKON_LC_BLACK_PX + 20;
    pakon_lc_stats black, active;
    if (pakon_lc_stats_range(s, n, line_px, origin, 0, PAKON_LC_BLACK_PX, &black) ||
        pakon_lc_stats_range(s, n, line_px, origin, act0, line_px - 10, &active)) {
        printf("%s: not enough data (%zu samples)\n", label, n);
        return;
    }
    printf("%s: %zu lines of %zu px\n", label, active.lines, line_px);
    printf("  black px [0,%u)    mean R %.0f G %.0f B %.0f IR %.0f\n",
           PAKON_LC_BLACK_PX, black.mean[0], black.mean[1], black.mean[2],
           black.mean[3]);
    printf("  active px [%zu,%zu) mean R %.0f G %.0f B %.0f IR %.0f\n", act0,
           line_px - 10, active.mean[0], active.mean[1], active.mean[2],
           active.mean[3]);
    printf("  active px          peak R %u G %u B %u IR %u\n",
           active.peak[0], active.peak[1], active.peak[2], active.peak[3]);
}

/* ResetFifos re-arm: the read that follows starts at a fixed line phase. */
static pakon_result probe_rearm(pakon_dev *dev, const pakon_setup_state *st,
                                unsigned t)
{
    const uint8_t host[4] = { AD_HOST, 0x01, 0x84, 0x02 };
    const uint8_t arm[3] = { st->low, 0x00, 0x8A };
    const uint8_t poll[1] = { st->low };
    pakon_seq seq;
    pakon_seq_init(&seq);
    pakon_packet_build(&seq.pkt[seq.n++], PH_WRITE, host, sizeof host);
    pakon_packet_build(&seq.pkt[seq.n++], PH_CMD, arm, sizeof arm);
    pakon_packet_build(&seq.pkt[seq.n++], PH_READ_STATUS, poll, 1);
    return pakon_cmd_run_seq(dev, &seq, t);
}

/* ---- Light calibration in code (step 7) ---------------------------------
 *
 * TLB.dll FN_bCalibrateLEDs on the open gate, motor stopped (F-135, Base 16
 * + IR): dark offsets on the RGB black pixels (TLB.dll has the LEDs off; we
 * keep the IR LED on so each read can be aligned, see pakon_lightcal.h), then the LED current search, then
 * the duty refine. Every pass re-arms (ResetFifos) and reads ~46 lines.
 * Currents never exceed the board ceiling (pakon_setup_leds refuses), duties
 * never exceed period - 2. Prints the result next to the OEM's from
 * scan.pakscan (currents B 3 / IR 2 / R 2 / G 3, duties B 278 / IR 1353 /
 * R 896 / G 708). Ends with the code-built teardown.
 */
static int lc_measure(pakon_dev *dev, const pakon_setup_state *st, unsigned t,
                      size_t line_px, unsigned offset, uint16_t *samples,
                      pakon_lc_stats *black, pakon_lc_stats *active)
{
    size_t n = 0;
    if (probe_rearm(dev, st, t) != PAKON_OK || probe_read(dev, st, t, samples, &n))
        return 1;
    long o = pakon_lc_find_origin(samples, n, line_px, LC_MIN_CONTRAST);
    if (o < 0) {
        fprintf(stderr, "light-cal: no light to align lines on\n");
        return 1;
    }
    size_t origin = (size_t)o + PROBE_SKIP_LINES * 4 * line_px;
    size_t act0 = offset > 6 ? offset - 6 + 3 : PAKON_LC_BLACK_PX + 20;
    return pakon_lc_stats_range(samples, n, line_px, origin, 0, PAKON_LC_BLACK_PX,
                                black) ||
           pakon_lc_stats_range(samples, n, line_px, origin, act0, line_px - 10,
                                active);
}

/* Wait after an A/D offset write: without it the dark level read right
 * after varied by ~+/-50 counts pass to pass (one offset code ~54). */
#define LC_AFE_SETTLE_MS 300u

static void lc_sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

typedef struct {
    int      gain[3], off[3];   /* R, G, B */
    unsigned cur[4];            /* R, G, B, IR */
    uint16_t duty[4];           /* open-gate duties, R, G, B, IR */
    /* Fixed-pattern tables (TLB.dll phases 4 and 7): per-pixel means over
     * the whole calibration line, [channel][pixel], R, G, B, IR. NULL when
     * not captured. */
    size_t   line_px, lines_dark, lines_bright;
    double  *dark, *bright;
} lc_result;

static void lc_result_free(lc_result *res)
{
    free(res->dark);
    free(res->bright);
    res->dark = res->bright = NULL;
}

/* Lines per fixed-pattern table (TLB.dll averages 128). */
#define LC_TABLE_LINES 128u

/* Per-pixel means over >= LC_TABLE_LINES aligned lines, read the way
 * lc_measure reads (re-arm, skip PROBE_SKIP_LINES, align each read). */
static int lc_capture_columns(pakon_dev *dev, const pakon_setup_state *st,
                              unsigned t, size_t line_px, uint16_t *samples,
                              double *mean, size_t *lines)
{
    *lines = 0;
    memset(mean, 0, PAKON_LC_NCH * line_px * sizeof *mean);
    for (int read = 0; read < 8 && *lines < LC_TABLE_LINES; read++) {
        size_t n = 0;
        if (probe_rearm(dev, st, t) != PAKON_OK || probe_read(dev, st, t, samples, &n))
            return 1;
        long o = pakon_lc_find_origin(samples, n, line_px, LC_MIN_CONTRAST);
        if (o < 0) {
            fprintf(stderr, "light-cal: no light to align lines on\n");
            return 1;
        }
        pakon_lc_column_accum(samples, n, line_px,
                              (size_t)o + PROBE_SKIP_LINES * 4 * line_px, mean, lines);
    }
    if (*lines < LC_TABLE_LINES)
        return 1;
    for (size_t i = 0; i < PAKON_LC_NCH * line_px; i++)
        mean[i] /= (double)*lines;
    return 0;
}

/* Mean of pixels [p0, p1) of channel c in a [channel][pixel] table. */
static double lc_table_mean(const double *t, size_t line_px, int c,
                            size_t p0, size_t p1)
{
    double s = 0;
    for (size_t p = p0; p < p1; p++)
        s += t[c * line_px + p];
    return p1 > p0 ? s / (double)(p1 - p0) : 0;
}

/* The calibration itself, on a session after setup_session (F-135, Base 16
 * + IR, period 0x742). Leaves acquire on and the LEDs at the open-gate
 * values. With `with_tables`, also captures the fixed-pattern tables into
 * res (free with lc_result_free). Returns 0 on convergence. */
static int light_cal_run(pakon_dev *dev, pakon_setup_state *st, unsigned t,
                         unsigned offset, int with_tables, lc_result *res)
{
    size_t line_px = (size_t)(st->reg5 - st->reg4);
    uint16_t *samples = malloc((size_t)PROBE_CHUNKS * PROBE_CHUNK);
    const uint16_t period = 0x742;   /* 0x0C1A * 0.6 (Base 16 + IR) */
    const char *nm[4] = { "R", "G", "B", "IR" };
    int gain[3], off[3];
    pakon_lc_stats black, active;
    pakon_seq seq;

    memset(res, 0, sizeof *res);
    if (!samples)
        return 1;
    pakon_cmd_service_events(dev, st->low, st->scn, t, NULL, NULL);

    /* 1. Dark offsets, LEDs off. */
    for (int c = 0; c < 3; c++) {
        gain[c] = PAKON_LC_GAIN_START;
        off[c] = PAKON_LC_OFFSET_START;
    }
    /* IR LED alone (current 1, half duty) so every read can be aligned on
     * the IR block; the RGB black pixels stay dark. */
    pakon_seq_init(&seq);
    pakon_led_values ir_only = { 0, 0, 0, 1 };
    if (pakon_setup_leds(&seq, st, 0x02, &ir_only,
                         (const uint16_t[]){ 0, 0, 0, (period - 2) / 2 },
                         period) != PAKON_OK)
        goto fail;
    pakon_setup_acquire(&seq, st, 1);
    if (pakon_cmd_run_seq(dev, &seq, t) != PAKON_OK)
        goto fail;
    int dark_ok = 0;
    for (int it = 1; it <= PAKON_LC_DARK_ITERS && !dark_ok; it++) {
        pakon_seq_init(&seq);
        pakon_setup_afe(&seq, st, gain, off);
        if (pakon_cmd_run_seq(dev, &seq, t) != PAKON_OK)
            goto fail;
        lc_sleep_ms(LC_AFE_SETTLE_MS);
        if (lc_measure(dev, st, t, line_px, offset, samples, &black, &active))
            goto fail;
        printf("dark %d: offsets %d/%d/%d -> black means %.0f/%.0f/%.0f\n", it,
               off[0], off[1], off[2], black.mean[0], black.mean[1], black.mean[2]);
        dark_ok = 1;
        for (int c = 0; c < 3; c++)
            if (!pakon_lc_dark_ok(black.mean[c])) {
                dark_ok = 0;
                off[c] = pakon_lc_offset_next(off[c], black.mean[c]);
            }
    }
    if (!dark_ok) {
        fprintf(stderr, "light-cal: dark offsets did not converge\n");
        goto fail;
    }

    /* 2. LED current search at the calibration duty caps. */
    uint16_t cap[4], duty[4];
    unsigned curv[4] = { 1, 1, 1, 1 };
    int done[4] = { 0 };
    const pakon_led_values *ceil = &pakon_led_ceiling_f135[1];
    const unsigned ceilv[4] = { ceil->r, ceil->g, ceil->b, ceil->ir };
    for (int c = 0; c < 4; c++)
        duty[c] = cap[c] = pakon_lc_duty_cap(period, pakon_lc_density_c41[c]);
    for (int pass = 1; pass <= 8; pass++) {
        pakon_led_values cur = { (uint8_t)curv[0], (uint8_t)curv[1],
                                 (uint8_t)curv[2], (uint8_t)curv[3] };
        pakon_seq_init(&seq);
        if (pakon_setup_leds(&seq, st, 0x03, &cur, duty, period) != PAKON_OK ||
            pakon_cmd_run_seq(dev, &seq, t) != PAKON_OK)
            goto fail;
        lc_sleep_ms(100);
        if (lc_measure(dev, st, t, line_px, offset, samples, &black, &active))
            goto fail;
        printf("current %d: R%u G%u B%u IR%u -> peaks %u/%u/%u/%u\n", pass,
               curv[0], curv[1], curv[2], curv[3], active.peak[0], active.peak[1],
               active.peak[2], active.peak[3]);
        int all = 1;
        for (int c = 0; c < 4; c++) {
            if (done[c])
                continue;
            if (pakon_lc_peak_over_cap(c, active.peak[c]) || curv[c] >= ceilv[c])
                done[c] = 1;
            else
                curv[c]++;
            all &= done[c];
        }
        if (all)
            break;
    }
    for (int c = 0; c < 4; c++)
        duty[c] = pakon_lc_duty_start(cap[c], curv[c]);

    /* 3. Duty refine: two converged passes 200 ms apart. */
    int converged = 0;
    for (int it = 1; it <= PAKON_LC_REFINE_ITERS && converged < 2; it++) {
        pakon_led_values cur = { (uint8_t)curv[0], (uint8_t)curv[1],
                                 (uint8_t)curv[2], (uint8_t)curv[3] };
        pakon_seq_init(&seq);
        if (pakon_setup_leds(&seq, st, 0x03, &cur, duty, period) != PAKON_OK ||
            pakon_cmd_run_seq(dev, &seq, t) != PAKON_OK)
            goto fail;
        lc_sleep_ms(converged ? 200 : 100);
        if (lc_measure(dev, st, t, line_px, offset, samples, &black, &active))
            goto fail;
        printf("refine %d: duties %u/%u/%u/%u -> peaks %u/%u/%u/%u\n", it,
               duty[0], duty[1], duty[2], duty[3], active.peak[0], active.peak[1],
               active.peak[2], active.peak[3]);
        int ok = 1;
        for (int c = 0; c < 4; c++) {
            if (pakon_lc_duty_settled(c, duty[c], active.peak[c], period))
                continue;
            ok = 0;
            uint16_t next = pakon_lc_duty_next(c, duty[c], active.peak[c], period);
            if (next == duty[c] && next == period - 2) {
                /* Duty at its maximum and still short: more current, else gain. */
                if (curv[c] < ceilv[c])
                    curv[c]++;
                else if (c < 3 && gain[c] < PAKON_LC_GAIN_MAX) {
                    gain[c]++;
                    pakon_seq_init(&seq);
                    pakon_setup_afe(&seq, st, gain, off);
                    pakon_cmd_run_seq(dev, &seq, t);
                } else {
                    fprintf(stderr, "light-cal: insufficient light on %s\n", nm[c]);
                    goto fail;
                }
            }
            duty[c] = next;
        }
        converged = ok ? converged + 1 : 0;
    }
    if (converged < 2) {
        fprintf(stderr, "light-cal: duties did not converge\n");
        goto fail;
    }

    /* 4. Fixed-pattern tables: dark (RGB LEDs off; the IR LED stays on as in
     * the dark-offset loop so reads can be aligned, so the IR dark table is
     * not a dark table), then bright at the calibrated values, which also
     * leaves the LEDs where the scan start expects them. */
    if (with_tables) {
        res->line_px = line_px;
        res->dark = malloc(PAKON_LC_NCH * line_px * sizeof *res->dark);
        res->bright = malloc(PAKON_LC_NCH * line_px * sizeof *res->bright);
        if (!res->dark || !res->bright)
            goto fail;
        pakon_led_values cur = { (uint8_t)curv[0], (uint8_t)curv[1],
                                 (uint8_t)curv[2], (uint8_t)curv[3] };
        pakon_seq_init(&seq);
        if (pakon_setup_leds(&seq, st, 0x02, &ir_only,
                             (const uint16_t[]){ 0, 0, 0, (period - 2) / 2 },
                             period) != PAKON_OK ||
            pakon_cmd_run_seq(dev, &seq, t) != PAKON_OK)
            goto fail;
        lc_sleep_ms(200);
        if (lc_capture_columns(dev, st, t, line_px, samples, res->dark,
                               &res->lines_dark)) {
            fprintf(stderr, "light-cal: dark table capture failed\n");
            goto fail;
        }
        pakon_seq_init(&seq);
        if (pakon_setup_leds(&seq, st, 0x03, &cur, duty, period) != PAKON_OK ||
            pakon_cmd_run_seq(dev, &seq, t) != PAKON_OK)
            goto fail;
        lc_sleep_ms(200);
        if (lc_capture_columns(dev, st, t, line_px, samples, res->bright,
                               &res->lines_bright)) {
            fprintf(stderr, "light-cal: bright table capture failed\n");
            goto fail;
        }
        size_t act0 = offset > 6 ? offset - 6 : PAKON_LC_BLACK_PX + 20;
        const char *nm3[3] = { "R", "G", "B" };
        for (int c = 0; c < 3; c++) {
            double bd = lc_table_mean(res->dark, line_px, c, 0, PAKON_LC_BLACK_PX);
            double ad = lc_table_mean(res->dark, line_px, c, act0, line_px);
            double ab = lc_table_mean(res->bright, line_px, c, act0, line_px);
            printf("tables %s: %zu/%zu lines, dark black %.0f active %.0f "
                   "(IR leak %+.0f), bright active %.0f\n", nm3[c],
                   res->lines_dark, res->lines_bright, bd, ad, ad - bd, ab);
        }
    }

    for (int c = 0; c < 4; c++) {
        res->cur[c] = curv[c];
        res->duty[c] = duty[c];
    }
    for (int c = 0; c < 3; c++) {
        res->gain[c] = gain[c];
        res->off[c] = off[c];
    }
    free(samples);
    return 0;
fail:
    lc_result_free(res);
    free(samples);
    return 1;
}

/*
 * Flat-field file (JSON) for the decoder (tools/pakon_image.py): the
 * calibration-line tables, the black-pixel means, the smear coefficients and
 * where the scan window sits in the line (scan column j = line pixel px0 +
 * j; the scan window starts at the EEPROM Offset, calibration at pixel 6).
 */
static int lc_write_flat(const char *path, unsigned serial, unsigned offset,
                         const lc_result *res)
{
    size_t lp = res->line_px, px0 = offset > 6 ? offset - 6 : 0;
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "cannot write '%s'\n", path);
        return 1;
    }
    char when[32];
    time_t now = time(NULL);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", localtime(&now));
    fprintf(f, "{\n  \"version\": 1,\n  \"source\": \"pakon_replay light-cal\",\n");
    fprintf(f, "  \"date\": \"%s\",\n  \"serial\": %u,\n  \"mode\": \"base16-ir\",\n",
            when, serial);
    fprintf(f, "  \"eeprom_offset\": %u,\n  \"line_px\": %zu,\n  \"px0\": %zu,\n"
               "  \"black_px\": %u,\n", offset, lp, px0, PAKON_LC_BLACK_PX);
    fprintf(f, "  \"lines_dark\": %zu,\n  \"lines_bright\": %zu,\n",
            res->lines_dark, res->lines_bright);
    fprintf(f, "  \"currents\": [%u, %u, %u, %u],\n  \"duties\": [%u, %u, %u, %u],\n",
            res->cur[0], res->cur[1], res->cur[2], res->cur[3], res->duty[0],
            res->duty[1], res->duty[2], res->duty[3]);
    fprintf(f, "  \"afe_gains\": [%d, %d, %d],\n  \"afe_offsets\": [%d, %d, %d],\n",
            res->gain[0], res->gain[1], res->gain[2], res->off[0], res->off[1],
            res->off[2]);
    fprintf(f, "  \"smear\": [");
    for (int c = 0; c < 3; c++) {
        double bd = lc_table_mean(res->dark, lp, c, 0, PAKON_LC_BLACK_PX);
        double bb = lc_table_mean(res->bright, lp, c, 0, PAKON_LC_BLACK_PX);
        double ad = lc_table_mean(res->dark, lp, c, px0, lp);
        double ab = lc_table_mean(res->bright, lp, c, px0, lp);
        fprintf(f, "%s%u", c ? ", " : "", pakon_lc_smear(bb, bd, ab, ad));
    }
    fprintf(f, "],\n");
    const char *key[2] = { "dark", "bright" };
    const double *tab[2] = { res->dark, res->bright };
    for (int k = 0; k < 2; k++) {
        fprintf(f, "  \"%s\": [\n", key[k]);
        for (int c = 0; c < PAKON_LC_NCH; c++) {
            fprintf(f, "    [");
            for (size_t p = 0; p < lp; p++)
                fprintf(f, "%s%.1f", p ? "," : "", tab[k][c * lp + p]);
            fprintf(f, "]%s\n", c + 1 < PAKON_LC_NCH ? "," : "");
        }
        fprintf(f, "  ]%s\n", k ? "" : ",");
    }
    fprintf(f, "}\n");
    int bad = ferror(f);
    if (fclose(f) || bad) {
        fprintf(stderr, "write error on '%s'\n", path);
        return 1;
    }
    printf("flat field -> %s\n", path);
    return 0;
}

static int do_light_cal(unsigned timeout, const char *eeprom_dir,
                        const char *flat_out)
{
    pakon_ctx *ctx;
    pakon_dev *dev;
    pakon_eeprom e;
    pakon_setup_state st;
    if (setup_session(timeout, eeprom_dir, &ctx, &dev, &e, &st))
        return 1;
    unsigned t = timeout < 2000 ? 2000 : timeout;
    const uint16_t period = 0x742;
    const char *nm[4] = { "R", "G", "B", "IR" };
    int rc = 1;
    lc_result res = { 0 };
    pakon_seq seq;

    if (st.low != AD_PICL) {
        fprintf(stderr, "light-cal: F-135 only for now\n");
        goto out;
    }
    if (light_cal_run(dev, &st, t, e.base[PAKON_EEPROM_BASE16].offset, 1, &res))
        goto stop;
    printf("\n=== light calibration (serial %u, Base 16 + IR) ===\n", e.serial);
    printf("        current  open-gate duty  scan duty (C-41)\n");
    for (int c = 0; c < 4; c++)
        printf("  %-3s   %7u  %14u  %16u\n", nm[c], res.cur[c], res.duty[c],
               pakon_lc_scan_duty(res.duty[c], pakon_lc_density_c41[c], period));
    printf("  gains %d/%d/%d, offsets %d/%d/%d\n", res.gain[0], res.gain[1],
           res.gain[2], res.off[0], res.off[1], res.off[2]);
    printf("  OEM (scan.pakscan): currents R2 G3 B3 IR2, duties R896 G708 B278 "
           "IR1353, offsets -38/-31/-31\n");
    char flat_path[1024];
    if (!flat_out) {
        snprintf(flat_path, sizeof flat_path, "%s/flatfield.json", eeprom_dir);
        flat_out = flat_path;
    }
    rc = lc_write_flat(flat_out, e.serial, e.base[PAKON_EEPROM_BASE16].offset, &res);
stop:
    pakon_seq_init(&seq);
    pakon_setup_teardown(&seq, &st);
    printf("teardown: %s\n", pakon_cmd_run_seq(dev, &seq, t) == PAKON_OK
                             ? "OK" : "FAILED");
out:
    lc_result_free(&res);
    session_close(ctx, dev);
    return rc;
}

/* ---- Film sensing experiment (step 9) ------------------------------------
 *
 * Runs the transport at the captured advance speed for `seconds` while the
 * operator feeds a strip through, reading the four DX detector levels (LOW
 * 0x93) every ~100 ms. Prints a line whenever a level moves by more than 8,
 * and once a second otherwise, so the entry and exit transitions of both
 * sensors can be read off. Motor stopped with 0xA2 (acquire is never on).
 * F-135 only.
 */
static int do_film_sense(unsigned timeout, const char *eeprom_dir, unsigned seconds)
{
    pakon_ctx *ctx;
    pakon_dev *dev;
    pakon_eeprom e;
    pakon_setup_state st;
    if (setup_session(timeout, eeprom_dir, &ctx, &dev, &e, &st))
        return 1;
    unsigned t = timeout < 2000 ? 2000 : timeout;
    int rc = 1;
    pakon_seq seq;

    if (st.low != AD_PICL) {
        fprintf(stderr, "film-sense: F-135 only for now\n");
        goto out;
    }
    pakon_seq_init(&seq);
    if (pakon_setup_motor_run(&seq, &st, PAKON_SETUP_ADVANCE_SPEED_F135) != PAKON_OK ||
        pakon_cmd_run_seq(dev, &seq, t) != PAKON_OK) {
        fprintf(stderr, "film-sense: motor start failed\n");
        goto stop;
    }
    printf("motor running -- feed the film now (%u s)\n", seconds);
    fflush(stdout);

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int last[4] = { -1000, -1000, -1000, -1000 };
    double last_print = -1;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double el = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
        if (el >= seconds)
            break;
        pakon_packet pkt, reply;
        pakon_sense_read_frame(&pkt, st.low);
        if (pakon_cmd(dev, &pkt, &reply, t) == PAKON_OK && reply.type == PH_READ &&
            reply.count >= 6 && reply.data[0] == st.low) {
            int v[4] = { reply.data[2], reply.data[3], reply.data[4], reply.data[5] };
            int moved = 0;
            for (int k = 0; k < 4; k++)
                if (abs(v[k] - last[k]) > 8)
                    moved = 1;
            if (moved || el - last_print >= 1.0) {
                printf("%6.2f s  0x93: %3d %3d %3d %3d  (flags 0x%02x)%s\n", el,
                       v[0], v[1], v[2], v[3], reply.data[1], moved ? "  *" : "");
                fflush(stdout);
                memcpy(last, v, sizeof last);
                last_print = el;
            }
        } else {
            printf("%6.2f s  0x93 read failed\n", el);
        }
        pakon_cmd_service_events(dev, st.low, st.scn, t, NULL, NULL);
        lc_sleep_ms(100);
    }
    rc = 0;
stop:
    pakon_seq_init(&seq);
    pakon_setup_motor_stop(&seq, &st);
    printf("motor stop: %s\n", pakon_cmd_run_seq(dev, &seq, t) == PAKON_OK
                               ? "OK" : "FAILED");
out:
    session_close(ctx, dev);
    return rc;
}

/* ---- Film advance and eject in code (step 9) -----------------------------
 *
 * --advance-code SECONDS: run the transport at the captured advance speed
 * for a fixed time (what advance.pakscan does), event service running.
 *
 * --eject: run it until the strip is out, judged from the DX sensors
 * (pakon_sense_clear): stop when, after film was under the exit sensor, both
 * sensors are clear for 0.5 s ("out"),
 * or when the entry sensor is clear and the exit reading has been flat for
 * 1.5 s with film under it (the tail has left the drive rollers and has to
 * be pulled by hand), or when no film was seen for 15 s, or after
 * max_seconds. F-135 only.
 */
static int read_levels(pakon_dev *dev, const pakon_setup_state *st, unsigned t,
                       uint8_t v[4])
{
    pakon_packet pkt, reply;
    pakon_sense_read_frame(&pkt, st->low);
    if (pakon_cmd(dev, &pkt, &reply, t) != PAKON_OK || reply.type != PH_READ ||
        reply.count < 6 || reply.data[0] != st->low)
        return -1;
    memcpy(v, reply.data + 2, 4);
    return 0;
}

static int do_transport(unsigned timeout, const char *eeprom_dir, int eject,
                        unsigned seconds)
{
    pakon_ctx *ctx;
    pakon_dev *dev;
    pakon_eeprom e;
    pakon_setup_state st;
    if (setup_session(timeout, eeprom_dir, &ctx, &dev, &e, &st))
        return 1;
    unsigned t = timeout < 2000 ? 2000 : timeout;
    int rc = 1;
    pakon_seq seq;
    const char *result = "time limit reached";

    if (st.low != AD_PICL) {
        fprintf(stderr, "transport: F-135 only for now\n");
        goto out;
    }
    pakon_seq_init(&seq);
    if (pakon_setup_motor_run(&seq, &st, PAKON_SETUP_ADVANCE_SPEED_F135) != PAKON_OK ||
        pakon_cmd_run_seq(dev, &seq, t) != PAKON_OK) {
        fprintf(stderr, "transport: motor start failed\n");
        goto stop;
    }
    printf("motor running (%s, at most %u s)\n", eject ? "eject" : "advance", seconds);
    fflush(stdout);

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int film_seen = 0, exit_seen = 0;
    unsigned exit_clear = 0, flat = 0, nothing = 0;
    uint8_t prev[4] = { 0 };
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double el = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
        if (el >= seconds)
            break;
        pakon_cmd_service_events(dev, st.low, st.scn, t, NULL, NULL);
        if (eject) {
            uint8_t v[4];
            if (read_levels(dev, &st, t, v) == 0) {
                int in_clear = pakon_sense_clear(v, PAKON_SENSE_ENTRY);
                int out_clear = pakon_sense_clear(v, PAKON_SENSE_EXIT);
                if (!in_clear || !out_clear)
                    film_seen = 1;
                if (!out_clear)
                    exit_seen = 1;
                /* Out: the exit sensor had film and both are now clear. */
                exit_clear = (exit_seen && in_clear && out_clear) ? exit_clear + 1 : 0;
                nothing = (in_clear && out_clear && !film_seen) ? nothing + 1 : 0;
                int still = abs(v[2] - prev[2]) <= 4 && abs(v[3] - prev[3]) <= 4;
                flat = (in_clear && !out_clear && still) ? flat + 1 : 0;
                memcpy(prev, v, 4);
                if (exit_clear >= 5) {
                    result = "strip is out";
                    break;
                }
                if (flat >= 15) {
                    result = "strip stalled at the exit: pull it out by hand";
                    break;
                }
                if (nothing >= 150) {
                    result = "no film in the transport";
                    break;
                }
            }
        }
        lc_sleep_ms(100);
    }
    rc = 0;
stop:
    pakon_seq_init(&seq);
    pakon_setup_motor_stop(&seq, &st);
    printf("motor stop: %s\n", pakon_cmd_run_seq(dev, &seq, t) == PAKON_OK
                               ? "OK" : "FAILED");
    if (!rc)
        printf("%s: %s\n", eject ? "eject" : "advance",
               eject ? result : "done");
out:
    session_close(ctx, dev);
    return rc;
}

/* ---- Scan with nothing replayed (F-135, Base 16 + IR) --------------------
 *
 * Code-built setup, light calibration, scan start (scan pixel window, C-41
 * scan duties, EEPROM motor speed, acquire, trigger), the --scan-sm image
 * loop with the event service, then the code-built teardown. Only the
 * firmware load is still a capture. Feed the film once the motor runs.
 */
static int do_scan_code(unsigned timeout, const char *eeprom_dir,
                        const char *image_path, unsigned long max_mb)
{
    pakon_ctx *ctx;
    pakon_dev *dev;
    pakon_eeprom e;
    pakon_setup_state st;
    if (setup_session(timeout, eeprom_dir, &ctx, &dev, &e, &st))
        return 1;
    unsigned t = timeout < 2000 ? 2000 : timeout;
    const uint16_t period = 0x742;
    int rc = 1;
    lc_result res = { 0 };
    pakon_seq seq;
    FILE *img = NULL;

    if (st.low != AD_PICL) {
        fprintf(stderr, "scan-code: F-135 only for now\n");
        goto out;
    }
    if (light_cal_run(dev, &st, t, e.base[PAKON_EEPROM_BASE16].offset, 1, &res)) {
        fprintf(stderr, "scan-code: calibration failed\n");
        goto stop;
    }
    {
        /* The scan's own flat field, next to the raw (the decoder picks it up). */
        char flat_path[1024];
        snprintf(flat_path, sizeof flat_path, "%s.flat.json", image_path);
        lc_write_flat(flat_path, e.serial, e.base[PAKON_EEPROM_BASE16].offset, &res);
    }
    uint16_t scan_duty[4];
    for (int c = 0; c < 4; c++)
        scan_duty[c] = pakon_lc_scan_duty(res.duty[c], pakon_lc_density_c41[c], period);
    uint16_t speed = pakon_setup_motor_speed(
        e.base[PAKON_EEPROM_BASE16].motor_speed_ir,
        e.adjust[PAKON_EEPROM_BASE16].ir, 0);
    printf("calibrated: offsets %d/%d/%d, currents %u/%u/%u/%u, scan duties "
           "%u/%u/%u/%u, motor speed %u\n", res.off[0], res.off[1], res.off[2],
           res.cur[0], res.cur[1], res.cur[2], res.cur[3], scan_duty[0],
           scan_duty[1], scan_duty[2], scan_duty[3], speed);

    img = fopen(image_path, "wb");
    if (!img) {
        fprintf(stderr, "cannot open image '%s'\n", image_path);
        goto stop;
    }
    pakon_seq_init(&seq);
    if (pakon_setup_scan_start(&seq, &st, (uint16_t)e.base[PAKON_EEPROM_BASE16].offset,
                               scan_duty, period, speed, 0x0010) != PAKON_OK ||
        pakon_cmd_run_seq(dev, &seq, t) != PAKON_OK) {
        fprintf(stderr, "scan-code: scan start failed\n");
        goto stop;
    }
    printf("  [sm] motor start (a0) sent -- feed the film now\n");
    fflush(stdout);

    unsigned long long img_bytes = 0;
    unsigned long nimg = 0, ncmd = 0, errs = 0;
    int film_seen = 0;
    sm_scan_loop(dev, img, &img_bytes, &nimg, &ncmd, &errs, timeout, max_mb,
                 &film_seen);
    printf("scan-code: %lu image reads, %llu bytes -> %s (%lu errors)%s\n", nimg,
           img_bytes, image_path, errs, film_seen ? "" : " -- no film seen");
    rc = film_seen ? 0 : 1;
stop:
    pakon_seq_init(&seq);
    pakon_setup_teardown(&seq, &st);
    printf("teardown (code-built): %s\n", pakon_cmd_run_seq(dev, &seq, t) == PAKON_OK
                                          ? "OK" : "FAILED");
out:
    if (img)
        fclose(img);
    lc_result_free(&res);
    session_close(ctx, dev);
    return rc;
}

static int do_calib_probe(unsigned timeout, const char *eeprom_dir)
{
    pakon_ctx *ctx;
    pakon_dev *dev;
    pakon_eeprom e;
    pakon_setup_state st;
    if (setup_session(timeout, eeprom_dir, &ctx, &dev, &e, &st))
        return 1;
    unsigned t = timeout < 2000 ? 2000 : timeout;
    int rc = 1;
    size_t cap = (size_t)PROBE_CHUNKS * PROBE_CHUNK / 2, n = 0;
    uint16_t *samples = malloc(cap * sizeof *samples);
    pakon_seq seq;

    if (st.low != AD_PICL) {
        fprintf(stderr, "probe: F-135 only (LED values are serial 3054's)\n");
        goto out;
    }
    pakon_cmd_service_events(dev, st.low, st.scn, t, NULL, NULL);

    /* Dark: gains 13, serial 3054's converged offsets, LEDs off (init). */
    pakon_seq_init(&seq);
    pakon_setup_afe(&seq, &st, (const int[]){ 13, 13, 13 },
                    (const int[]){ -38, -31, -31 });
    pakon_setup_acquire(&seq, &st, 1);
    size_t line_px = (size_t)(st.reg5 - st.reg4);
    if (pakon_cmd_run_seq(dev, &seq, t) != PAKON_OK ||
        probe_rearm(dev, &st, t) != PAKON_OK ||
        probe_read(dev, &st, t, samples, &n))
        goto stop;
    probe_report("dark (LEDs off)", samples, n, line_px,
                 e.base[PAKON_EEPROM_BASE16].offset);

    /* Lit: the OEM's converged values (scan.pakscan calibration). */
    pakon_seq_init(&seq);
    pakon_led_values cur = { .r = 2, .g = 3, .b = 3, .ir = 2 };
    uint16_t pduty[4] = { 0x380, 0x2C4, 0x116, 0x549 };
    unsigned en = 0x03;
    /* Debug override: PAKON_PROBE_LEDS="en cR cG cB cIR dR dG dB dIR". */
    const char *ov = getenv("PAKON_PROBE_LEDS");
    if (ov) {
        unsigned v[9];
        if (sscanf(ov, "%x %u %u %u %u %hu %hu %hu %hu", &en, &v[0], &v[1], &v[2],
                   &v[3], &pduty[0], &pduty[1], &pduty[2], &pduty[3]) == 9) {
            cur = (pakon_led_values){ (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2],
                                      (uint8_t)v[3] };
            printf("LED override: enable 0x%x currents %u/%u/%u/%u duties "
                   "%u/%u/%u/%u\n", en, v[0], v[1], v[2], v[3], pduty[0],
                   pduty[1], pduty[2], pduty[3]);
        }
    }
    if (pakon_setup_leds(&seq, &st, (uint8_t)en, &cur, pduty, 0x742) != PAKON_OK)
        goto stop;
    if (pakon_cmd_run_seq(dev, &seq, t) != PAKON_OK)
        goto stop;
    if (probe_rearm(dev, &st, t) != PAKON_OK ||
        probe_read(dev, &st, t, samples, &n))
        goto stop;
    probe_report("lit (OEM converged LEDs)", samples, n, line_px,
                 e.base[PAKON_EEPROM_BASE16].offset);
    rc = 0;
stop:
    pakon_seq_init(&seq);
    pakon_setup_teardown(&seq, &st);
    printf("teardown: %s\n", pakon_cmd_run_seq(dev, &seq, t) == PAKON_OK
                             ? "OK" : "FAILED");
out:
    free(samples);
    session_close(ctx, dev);
    return rc;
}

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
        "       %s --setup --eeprom-dir DIR [--teardown]  controller init + Base 16 configure, built in code\n"
        "       %s --calib-probe --eeprom-dir DIR         static dark/lit line levels (F-135, open gate)\n"
        "       %s --light-cal --eeprom-dir DIR [--flat-out FILE]  light calibration in code (F-135, open gate)\n"
        "       %s --scan-code --eeprom-dir DIR [--image OUT]  scan with nothing replayed (F-135)\n"
        "       %s --film-sense SECONDS --eeprom-dir DIR  run the transport, log DX levels (F-135)\n"
        "       %s --advance-code SECONDS --eeprom-dir DIR  timed film advance in code (F-135)\n"
        "       %s --eject [--eject-seconds N] --eeprom-dir DIR  run until the strip is out (F-135)\n"
        "\n"
        "  FILE.pakscan  positional: replay advance script, then poll until idle\n"
        "  --limit SEC   wall-clock limit for the advance poll loop (default 60)\n"
        "  --open        replay the captured open handshake, verify replies\n"
        "  --calibrate   run the driven CALIBRATE (measure open-gate CCD ->\n"
        "                compute gain/offset); present the open gate, no film\n"
        "  --cal-lines N  CCD lines to average per measurement (default 32)\n"
        "  --flat-out FILE  where --light-cal writes the flat field (default\n"
        "                DIR/flatfield.json; --scan-code writes OUT.flat.json)\n"
        "  --cal-exposure N  nominal CcdExposure for the gain phase (default 256)\n"
        "  --prelude FILE replay a .pakscan setup spine (CCD+lamp init) before\n"
        "                calibrating -- needed for the gain phase (illumination)\n"
        "  --cal-verbose  log each calibration iteration\n"
        "  --scan FILE   replay a .pakscan scan script verbatim (fixed length)\n"
        "  --scan-sm FILE  replay setup spine, then drive the image transfer and\n"
        "                  stop on end-of-roll white (any roll length)\n"
        "  --image OUT   raw image output for --scan/--scan-sm (default pakon_scan.raw)\n"
        "  --advance     F-135+ only: run the film transport for a fixed time\n"
        "                to push the strip out (standalone, or appended to\n"
        "                --scan). On an F-135 use the FILE.pakscan advance mode\n"
        "                (resources/advance.pakscan --steps N).\n"
        "  --advance-seconds N  how long to run the transport (default 15;\n"
        "                a strip already at the exit needs only 1-2)\n"
        "  --drain       after the scan script ends, keep reading 0x86 until done\n"
        "  --autostop    with --scan: stop at end-of-roll white (film fully\n"
        "                scanned) and replay teardown -- replay a max-length\n"
        "                capture and any shorter film just stops earlier\n"
        "  --trace-status  with --scan: log live poll/kick replies + status, with\n"
        "                  the current image-read index (to learn the cadence)\n"
        "  --max-mb N    --scan-sm/--scan-code safety cap on image MB (default 4096,\n"
        "                0=off); a full 36-exposure roll is up to ~2 GB\n"
        "  --timeout MS  USB per-transfer timeout in ms (default 1000)\n"
        "\n"
        "Set PAKON_DEBUG=0..4 for increasing trace verbosity.\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0,
        argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    /* Line-buffered even into a pipe: the web client reads progress (e.g. the
     * "feed the film now" prompt) as it happens. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    int want_open = 0, drain = 0, trace_status = 0, autostop = 0;
    int want_advance_run = 0;       /* --advance: standalone or after --scan */
    unsigned advance_seconds = 15;
    int want_calibrate = 0, cal_verbose = 0, want_read_params = 0, want_configure = 0;
    int want_setup = 0, want_teardown = 0, want_probe = 0, want_lightcal = 0;
    int want_scan_code = 0;
    unsigned film_sense_s = 0, advance_code_s = 0, eject_s = 60;
    int want_eject = 0;
    const char *eeprom_dir = NULL;
    unsigned long cal_lines = 32;
    unsigned long cal_exposure = 256;
    const char *cal_prelude = NULL;
    const char *params_out = NULL;
    const char *flat_out = NULL;
    const char *scan_file = NULL;
    const char *scan_sm_file = NULL;
    const char *advance_file = NULL;
    const char *image_path = "pakon_scan.raw";
    unsigned timeout = 1000;
    unsigned limit_sec = 60;
    unsigned long steps_count = 1;
    /* Runaway guard only: a 36-exposure roll at Base 16 + IR is up to ~2 GB. */
    unsigned long max_mb = 4096;

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
        } else if (!strcmp(argv[i], "--setup")) {
            want_setup = 1;
        } else if (!strcmp(argv[i], "--advance-code") && i + 1 < argc) {
            advance_code_s = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--eject")) {
            want_eject = 1;
        } else if (!strcmp(argv[i], "--eject-seconds") && i + 1 < argc) {
            eject_s = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--film-sense") && i + 1 < argc) {
            film_sense_s = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--scan-code")) {
            want_scan_code = 1;
        } else if (!strcmp(argv[i], "--flat-out") && i + 1 < argc) {
            flat_out = argv[++i];
        } else if (!strcmp(argv[i], "--light-cal")) {
            want_lightcal = 1;
        } else if (!strcmp(argv[i], "--calib-probe")) {
            want_probe = 1;
        } else if (!strcmp(argv[i], "--teardown")) {
            want_teardown = 1;
        } else if (!strcmp(argv[i], "--eeprom-dir") && i + 1 < argc) {
            eeprom_dir = argv[++i];
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
        !want_setup && !want_probe && !want_lightcal && !want_scan_code &&
        !film_sense_s && !advance_code_s && !want_eject &&
        !scan_file && !scan_sm_file && !advance_file && !want_advance_run) {
        usage(argv[0]);
        return 2;
    }

    if (want_read_params)
        return do_read_params(timeout < 2000 ? 2000 : timeout, params_out);
    if (want_eject)
        return do_transport(timeout, eeprom_dir, 1, eject_s > 120 ? 120 : eject_s);
    if (advance_code_s)
        return do_transport(timeout, eeprom_dir, 0,
                            advance_code_s > 120 ? 120 : advance_code_s);
    if (film_sense_s)
        return do_film_sense(timeout, eeprom_dir, film_sense_s > 120 ? 120 : film_sense_s);
    if (want_scan_code)
        return do_scan_code(timeout, eeprom_dir, image_path, max_mb);
    if (want_lightcal)
        return do_light_cal(timeout, eeprom_dir, flat_out);
    if (want_probe)
        return do_calib_probe(timeout, eeprom_dir);
    if (want_setup)
        return do_setup(timeout, eeprom_dir, want_teardown);
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
