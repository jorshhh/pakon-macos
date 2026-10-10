/*
 * pakon_lightcal.c — F-135 line statistics. See pakon_lightcal.h.
 */
#include "pakon_lightcal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

long pakon_lc_find_origin(const uint16_t *s, size_t nsamples, size_t line_px,
                          double min_contrast)
{
    size_t L = 4 * line_px;
    size_t lines = L ? nsamples / L : 0;
    if (!s || line_px < 120 || lines < 2)
        return -1;
    if (lines > 64)
        lines = 64;
    /* Mean line, then prefix sums over three copies so windows can wrap. */
    double *c = calloc(3 * L + 1, sizeof *c);
    if (!c)
        return -1;
    for (size_t l = 0; l < lines; l++)
        for (size_t i = 0; i < L; i++)
            c[1 + i] += s[l * L + i];
    for (size_t i = 0; i < L; i++)
        c[1 + i] /= (double)lines;
    for (size_t k = 1; k < 3; k++)
        for (size_t i = 0; i < L; i++)
            c[1 + k * L + i] = c[1 + i];
    for (size_t i = 1; i <= 3 * L; i++)
        c[i] += c[i - 1];
#define WIN(o, a, b) ((c[(o) + (b)] - c[(o) + (a)]) / (double)((b) - (a)))
    size_t ir = 3 * line_px;
    double best = -1e18;
    long arg = -1;
    for (size_t o = 0; o < L; o++) {
        /* Whole-block active windows: a lit IR block alone cannot pass for
         * an RGB block, which is three times longer. */
        double sc = WIN(o, 120, ir - 30) - WIN(o, 0, 60) +
                    WIN(o, ir + 60, L - 10) - WIN(o, ir, ir + 20);
        if (sc > best) {
            best = sc;
            arg = (long)o;
        }
    }
#undef WIN
    free(c);
    return best >= min_contrast ? arg : -1;
}

int pakon_lc_stats_range(const uint16_t *s, size_t nsamples, size_t line_px,
                         size_t origin, size_t px0, size_t px1,
                         pakon_lc_stats *out)
{
    if (!s || !out || px1 <= px0 || px1 > line_px)
        return -1;
    memset(out, 0, sizeof *out);
    size_t line = 4 * line_px;
    size_t lines = nsamples > origin ? (nsamples - origin) / line : 0;
    if (!lines)
        return -1;
    size_t npx = px1 - px0;
    double *col = calloc(PAKON_LC_NCH * npx, sizeof *col);
    if (!col)
        return -1;

    for (size_t l = 0; l < lines; l++) {
        const uint16_t *rgb = s + origin + l * line;
        const uint16_t *ir = rgb + 3 * line_px;
        for (size_t p = px0; p < px1; p++) {
            col[0 * npx + (p - px0)] += rgb[3 * p];
            col[1 * npx + (p - px0)] += rgb[3 * p + 1];
            col[2 * npx + (p - px0)] += rgb[3 * p + 2];
            col[3 * npx + (p - px0)] += ir[p];
        }
    }

    out->lines = lines;
    for (int c = 0; c < PAKON_LC_NCH; c++) {
        double sum = 0, best = 0;
        for (size_t i = 0; i < npx; i++) {
            double m = col[c * npx + i] / (double)lines;
            sum += m;
            if (m > best)
                best = m;
        }
        out->mean[c] = sum / (double)npx;
        out->peak[c] = (uint16_t)(best + 0.5);
    }
    free(col);
    return 0;
}

const double pakon_lc_density_c41[PAKON_LC_NCH] = { 0.144, 0.40, 0.715, 0.0 };

int pakon_lc_offset_next(int offset, double black_mean)
{
    int next = offset + (int)lround((black_mean - PAKON_LC_DARK_TARGET) * -0.0133929);
    /* Outside the window but less than half a code off: the rounded step is
     * 0 and the loop would never move. Step one code toward the target. */
    if (next == offset && !pakon_lc_dark_ok(black_mean))
        next += black_mean > PAKON_LC_DARK_TARGET ? -1 : 1;
    return next < -255 ? -255 : next > 255 ? 255 : next;
}

int pakon_lc_dark_ok(double black_mean)
{
    return fabs(black_mean - PAKON_LC_DARK_TARGET) <= PAKON_LC_DARK_TOL;
}

int pakon_lc_peak_over_cap(int ch, unsigned peak)
{
    static const unsigned cap[PAKON_LC_NCH] = { 64000, 64000, 65500, 40000 };
    return peak > cap[ch];
}

int pakon_lc_peak_ok(int ch, unsigned peak)
{
    unsigned hi = ch == PAKON_LC_IR ? 40000 : 64000;
    return peak >= hi - 64 && peak <= hi;
}

uint16_t pakon_lc_duty_cap(uint16_t period, double density)
{
    return (uint16_t)lround((period - 2) / pow(10.0, density));
}

uint16_t pakon_lc_duty_start(uint16_t cap, unsigned current)
{
    if (current <= 1)
        return (uint16_t)(cap / 2);
    return (uint16_t)lround((double)cap * (current - 1) / current);
}

uint16_t pakon_lc_duty_next(int ch, uint16_t duty, unsigned peak, uint16_t period)
{
    double target = ch == PAKON_LC_IR ? 39968.0 : 63968.0;
    long next = peak ? lround(duty * target / peak) : (long)period - 2;
    if (next < 1)
        next = 1;
    if (next > period - 2)
        next = period - 2;
    return (uint16_t)next;
}

int pakon_lc_duty_settled(int ch, uint16_t duty, unsigned peak, uint16_t period)
{
    if (pakon_lc_peak_ok(ch, peak))
        return 1;
    int next = pakon_lc_duty_next(ch, duty, peak, period);
    return abs(next - (int)duty) <= 1 && next < period - 2;
}

uint16_t pakon_lc_scan_duty(uint16_t open_duty, double density, uint16_t period)
{
    long d = lround(open_duty * pow(10.0, density));
    return (uint16_t)(d > period - 2 ? period - 2 : d);
}
