/*
 * test_lightcal — line statistics and calibration steps (no hardware).
 *
 * Duty values come from resources/scan.pakscan's calibration (serial 3054,
 * period 0x742): calibration caps B 359 / IR 1856 / R 1335 / G 741; starting
 * duties after the current search (currents B 3, IR 2, R 2, G 3) B 239 /
 * IR 930 / R 668 / G 494; converged B 278 / IR 1353 / R 896 / G 708; scan
 * duties B 1442 / IR 1352 / R 1247 / G 1775. Our formulas, from TLB.dll's
 * description with the published densities, land within 3 ticks (0.4%) of
 * every captured value; the residual is not explained yet.
 */
#include "pakon_lightcal.h"

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
        if (cond) { printf("ok   - %s\n", (msg)); }             \
        else { printf("FAIL - %s\n", (msg)); failures++; }      \
    } while (0)

static int near(long a, long b, long tol)
{
    return labs(a - b) <= tol;
}

int main(void)
{
    char msg[160];

    /* Synthetic stream: 1 lead sample, IR block, then lines of RGB + IR.
     * Pixel p of line l: R = 1000 + p, G = 2000 + p, B = 3000 + l, IR = 400. */
    {
        enum { PX = 50, LINES = 5 };
        size_t n = 1 + PX + LINES * 4 * PX;
        uint16_t *s = calloc(n, sizeof *s);
        size_t o = 1 + PX;
        for (size_t l = 0; l < LINES; l++) {
            uint16_t *rgb = s + o + l * 4 * PX;
            for (size_t p = 0; p < PX; p++) {
                rgb[3 * p] = (uint16_t)(1000 + p);
                rgb[3 * p + 1] = (uint16_t)(2000 + p);
                rgb[3 * p + 2] = (uint16_t)(3000 + l);
                if (o + l * 4 * PX + 3 * PX + p < n)
                    rgb[3 * PX + p] = 400;
            }
        }
        pakon_lc_stats st;
        int r = pakon_lc_stats_range(s, n, PX, o, 10, 20, &st);
        CHECK(r == 0 && st.lines == LINES, "whole lines counted");
        CHECK(st.mean[PAKON_LC_R] == 1014.5 && st.peak[PAKON_LC_R] == 1019,
              "R mean and column peak over pixels 10..19");
        CHECK(st.mean[PAKON_LC_G] == 2014.5, "G mean");
        CHECK(st.mean[PAKON_LC_B] == 3002.0, "B mean over lines");
        CHECK(st.mean[PAKON_LC_IR] == 400.0, "IR block follows the RGB block");
        CHECK(pakon_lc_stats_range(s, n, PX, o, 20, 20, &st) == -1,
              "empty pixel range refused");
        CHECK(pakon_lc_stats_range(s, 10, PX, o, 0, 5, &st) == -1,
              "no whole line refused");
        free(s);
    }

    /* Origin search: lines of 8148 samples, RGB block then IR block, each
     * block 20 black pixels then lit; the read starts 1234 samples in. */
    {
        enum { PX = 2037, L = 4 * PX, LINES = 8, START = 1234 };
        uint16_t *line = malloc(L * sizeof *line);
        for (size_t i = 0; i < L; i++) {
            int ir = i >= 3 * PX;
            size_t p = ir ? i - 3 * PX : i / 3;
            line[i] = (uint16_t)(p < 20 ? 300 : ir ? 40000 : 60000);
        }
        uint16_t *s = malloc(LINES * L * sizeof *s);
        for (size_t i = 0; i < LINES * L; i++)
            s[i] = line[(START + i) % L];
        long o = pakon_lc_find_origin(s, LINES * L, PX, 3000);
        snprintf(msg, sizeof msg, "origin found at %ld (expect %d)", o, L - START);
        CHECK(o == L - START, msg);
        for (size_t i = 0; i < L; i++)   /* IR block only: still aligns */
            line[i] = (uint16_t)(i >= 3 * PX && i - 3 * PX >= 20 ? 40000 : 300);
        for (size_t i = 0; i < LINES * L; i++)
            s[i] = line[(START + i) % L];
        CHECK(pakon_lc_find_origin(s, LINES * L, PX, 3000) == L - START,
              "origin found from the IR block alone");
        for (size_t i = 0; i < LINES * L; i++)
            s[i] = 300;
        CHECK(pakon_lc_find_origin(s, LINES * L, PX, 3000) == -1,
              "no light: no origin");
        free(s);
        free(line);
    }

    /* Dark offset. */
    CHECK(pakon_lc_offset_next(-38, 300.0) == -38, "offset unchanged at target");
    CHECK(pakon_lc_offset_next(10, 3884.0) == -38, "offset step: 3584 counts = 48 codes");
    CHECK(pakon_lc_offset_next(-250, 0.0) == -246, "offset step up when too dark");
    CHECK(pakon_lc_offset_next(-250, 60000.0) == -255, "offset clamped to -255");
    CHECK(pakon_lc_dark_ok(332) && pakon_lc_dark_ok(268) && !pakon_lc_dark_ok(333),
          "dark window 300 +/- 32");

    /* Caps and windows. */
    CHECK(!pakon_lc_peak_over_cap(PAKON_LC_G, 64000) &&
          pakon_lc_peak_over_cap(PAKON_LC_G, 64001) &&
          !pakon_lc_peak_over_cap(PAKON_LC_B, 65500) &&
          pakon_lc_peak_over_cap(PAKON_LC_IR, 40001), "current-search caps");
    CHECK(pakon_lc_peak_ok(PAKON_LC_R, 63936) && pakon_lc_peak_ok(PAKON_LC_R, 64000) &&
          !pakon_lc_peak_ok(PAKON_LC_R, 63935) && !pakon_lc_peak_ok(PAKON_LC_R, 64001) &&
          pakon_lc_peak_ok(PAKON_LC_IR, 39936) && !pakon_lc_peak_ok(PAKON_LC_IR, 40001),
          "refine windows");

    /* Duties against the capture (order R, G, B, IR). */
    const uint16_t period = 0x742;
    const long cap_cap[4] = { 1335, 741, 359, 1856 };
    const unsigned cur[4] = { 2, 3, 3, 2 };
    const long start_cap[4] = { 668, 494, 239, 930 };
    const uint16_t conv[4] = { 896, 708, 278, 1353 };
    const long scan_cap[4] = { 1247, 1775, 1442, 1352 };
    for (int c = 0; c < 4; c++) {
        const char *nm[4] = { "R", "G", "B", "IR" };
        uint16_t cap = pakon_lc_duty_cap(period, pakon_lc_density_c41[c]);
        snprintf(msg, sizeof msg, "%s calibration cap %u ~ captured %ld", nm[c],
                 cap, cap_cap[c]);
        CHECK(near(cap, cap_cap[c], 3), msg);
        uint16_t st = pakon_lc_duty_start((uint16_t)cap_cap[c], cur[c]);
        snprintf(msg, sizeof msg, "%s start duty %u ~ captured %ld", nm[c], st,
                 start_cap[c]);
        CHECK(near(st, start_cap[c], 3), msg);
        uint16_t sc = pakon_lc_scan_duty(conv[c], pakon_lc_density_c41[c], period);
        snprintf(msg, sizeof msg, "%s scan duty %u ~ captured %ld", nm[c], sc,
                 scan_cap[c]);
        CHECK(near(sc, scan_cap[c], 3), msg);
    }

    /* Refine step. */
    CHECK(pakon_lc_duty_next(PAKON_LC_B, 239, 53830, period) == 284,
          "refine: 239 at peak 53830 -> 284 (the captured second B duty)");
    CHECK(pakon_lc_duty_next(PAKON_LC_IR, 1000, 39968, period) == 1000,
          "refine: IR at target unchanged");
    CHECK(pakon_lc_duty_next(PAKON_LC_G, 1800, 30000, period) == period - 2,
          "refine clamped to period - 2");
    CHECK(pakon_lc_duty_next(PAKON_LC_G, 100, 0, period) == period - 2,
          "refine with no light goes to the maximum");

    /* Settled: in window, or within one tick of the refine target. */
    CHECK(pakon_lc_duty_settled(PAKON_LC_B, 273, 63958, period),
          "settled: in window");
    CHECK(pakon_lc_duty_settled(PAKON_LC_B, 273, 63899, period),
          "settled: B 273 at 63899 (next step 273)");
    CHECK(!pakon_lc_duty_settled(PAKON_LC_B, 239, 54966, period),
          "not settled: B 239 at 54966 (next step 278)");
    CHECK(!pakon_lc_duty_settled(PAKON_LC_G, period - 2, 50000, period),
          "not settled: duty at its maximum and short of light");

    if (failures) {
        printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall tests passed\n");
    return 0;
}
