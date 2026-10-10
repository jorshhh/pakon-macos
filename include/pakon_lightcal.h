/*
 * pakon_lightcal.h — F-135 line statistics for light calibration (pure).
 *
 * During calibration (motor stopped, bank 0x82 reg 4 = 6, reg 5 = Offset +
 * 2000) a line is line_px = reg5 - reg4 pixels: line_px R,G,B triplets and
 * line_px IR samples, 4 * line_px u16 LE samples in all (8148 on serial
 * 3054). No sample carries the scan-time marker bit, and where a read
 * starts within a line varies from read to read (measured on serial 3054,
 * 2026-10-09), so each read is aligned from its own data:
 * pakon_lc_find_origin looks for the black-to-lit step at the start of the
 * RGB and IR blocks. That needs light in at least one block; the dark-offset
 * loop therefore runs with the IR LED alone, which leaves the RGB black
 * pixels dark (330/301/317 vs 319/316/319 with all LEDs off).
 *
 * The first ~22 pixels of each block are optically black; the level then
 * ramps up to the open-gate level by pixel ~37 (= Offset - 6, where
 * TLB.dll's active columns start). Calibration (TLB.dll FN_bCalibrateLEDs,
 * docs/TLB_FINDINGS.md) uses black-pixel means and the column peak of the
 * active pixels (per-column mean over the lines, then the maximum).
 */
#ifndef PAKON_LIGHTCAL_H
#define PAKON_LIGHTCAL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PAKON_LC_R = 0, PAKON_LC_G, PAKON_LC_B, PAKON_LC_IR, PAKON_LC_NCH };

#define PAKON_LC_BLACK_PX  20u   /* fully black pixels at the start of a block */

/*
 * Line origin (start of an RGB block, 0 <= origin < 4 * line_px) of the
 * lines in `s`: the offset maximising, over the mean of up to 64 lines, the
 * contrast between each block's black pixels and the rest of that block
 * (RGB block plus IR block). Returns -1 if fewer than 2 lines or the best contrast is
 * under `min_contrast` counts (no light to align on).
 */
long pakon_lc_find_origin(const uint16_t *s, size_t nsamples, size_t line_px,
                          double min_contrast);

typedef struct {
    size_t   lines;                  /* whole lines used */
    double   mean[PAKON_LC_NCH];     /* mean over lines and pixels */
    uint16_t peak[PAKON_LC_NCH];     /* max over pixels of the per-pixel mean */
} pakon_lc_stats;

/*
 * Statistics over pixels [px0, px1) of every whole line from `origin` (RGB
 * block, then the next IR block). Returns 0 on success, -1 if the range is
 * empty or no whole line fits.
 */
int pakon_lc_stats_range(const uint16_t *s, size_t nsamples, size_t line_px,
                         size_t origin, size_t px0, size_t px1,
                         pakon_lc_stats *out);

/*
 * Per-column sums for the fixed-pattern tables (TLB.dll FUN_1001f550): adds
 * every pixel of every whole line from `origin` to `sum`, laid out
 * [channel][pixel] (PAKON_LC_NCH * line_px doubles, RGB block then the IR
 * block), and adds the number of lines to *lines. Call once per read to
 * build up 128+ lines over several reads. Returns the lines added (0 if no
 * whole line fits).
 */
size_t pakon_lc_column_accum(const uint16_t *s, size_t nsamples, size_t line_px,
                             size_t origin, double *sum, size_t *lines);

/*
 * Flat-field gain for one column (TLB.dll FUN_1001f550, as means instead of
 * 128-line sums): 64000 / ((bright - dark) - (black_bright - black_dark)),
 * the factor that brings the open gate to 64000 after dark subtraction.
 * The black-pixel term removes drift of the optically black pixels between
 * the two passes. Capped at 0x3ffff / 65536 (~4.0) like the OEM's 16.16
 * table; 0 when the denominator is not positive.
 */
double pakon_lc_flat_gain(double bright, double dark, double black_bright,
                          double black_dark);

/*
 * Smear coefficient (16.16, TLB.dll): 65536 * (black_bright - black_dark) /
 * (active_bright - active_dark), kept only within 1..699, else 0.
 */
unsigned pakon_lc_smear(double black_bright, double black_dark,
                        double active_bright, double active_dark);

/* ---- Calibration steps (TLB.dll FN_bCalibrateLEDs, docs/TLB_FINDINGS.md) */

#define PAKON_LC_GAIN_START     13      /* A/D gain code (g = 1.2) */
#define PAKON_LC_GAIN_MAX       0x3E
#define PAKON_LC_OFFSET_START   10
#define PAKON_LC_DARK_TARGET    300.0
#define PAKON_LC_DARK_TOL       32.0
#define PAKON_LC_DARK_ITERS     8
#define PAKON_LC_REFINE_ITERS   32

/* Colour-negative film-base densities (FUN_10020230), order R, G, B, IR:
 * the calibration duty is capped at (period - 2) / 10^D and the scan duty is
 * the open-gate duty * 10^D. */
extern const double pakon_lc_density_c41[PAKON_LC_NCH];

/* Next A/D offset from a black-pixel mean: off + round((mean - 300) * -0.0133929)
 * (TLB.dll). When that rounds to no change while the mean is still outside
 * 300 +/- 32 (e.g. 334: step -0.46), step one code toward the target instead;
 * one code moves the level ~54 counts. */
int pakon_lc_offset_next(int offset, double black_mean);
int pakon_lc_dark_ok(double black_mean);       /* within 300 +/- 32 */

/* Current-search stop level: column peak above R/G 64000, B 65500, IR 40000. */
int pakon_lc_peak_over_cap(int ch, unsigned peak);

/* Duty-refine window: R/G/B [63936, 64000], IR [39936, 40000]. */
int pakon_lc_peak_ok(int ch, unsigned peak);

/* Calibration duty cap: (period - 2) / 10^D. */
uint16_t pakon_lc_duty_cap(uint16_t period, double density);

/* Duty to start refining from after the current search ends at `current`:
 * cap * (n - 1) / n (cap / 2 when n is 1). */
uint16_t pakon_lc_duty_start(uint16_t cap, unsigned current);

/* Refine step: duty * 63968 / peak (IR 39968), clamped to [1, period - 2]. */
uint16_t pakon_lc_duty_next(int ch, uint16_t duty, unsigned peak, uint16_t period);

/* Settled: peak in the refine window, or the refine step would move the
 * duty by at most one tick. At small duties one tick is wider than the
 * 64-count window (B at 273: ~234 counts), so the window alone is not
 * reached reliably with ~+/-50 counts of measurement noise. */
int pakon_lc_duty_settled(int ch, uint16_t duty, unsigned peak, uint16_t period);

/* Scan duty for film: open-gate duty * 10^D, clamped to period - 2. */
uint16_t pakon_lc_scan_duty(uint16_t open_duty, double density, uint16_t period);

#ifdef __cplusplus
}
#endif

#endif /* PAKON_LIGHTCAL_H */
