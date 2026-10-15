"""
pakon_ice.py -- infrared dust and scratch removal ("Digital ICE") for Pakon
scans.

The F-135 scans every line with an infrared LED as well (the trailing block of
each 8000-sample row). Film dyes are nearly transparent to infrared, dust and
scratches are not, so the IR plane is a map of the defects; the masked pixels
are rebuilt from the clean film around them.

The pipeline is a numpy port of nkscan's `src/dust.rs`
(https://github.com/activexray/nkscan, MIT OR Apache-2.0, Copyright (c) the
nkscan authors), which implements the pipeline a6o documented for openICE
(https://github.com/a6o/openICE, GPL-3.0), a reimplementation of Applied
Science Fiction's Digital ICE as tuned for the Nikon Coolscan LS-5000/9000.
The Coolscan constants are kept as they are; what is Pakon-specific here is
getting the IR plane: aligning it to the visible lines and normalising it
(`ir_plane`).

Steps (numbers as in dust.rs):
  1. log-density of the samples, D(v) = M/16 * log2(v + 1);
  2. calibrate: clear-film IR reference and the red -> IR dye leakage slope,
     from a reduced copy of the frame;
  3. gate: IR density with the leakage taken out, g, which responds to
     defects only;
  4. confidence w in [w_floor, 1] that a pixel is clean;
  5. decide which pixels to rebuild;
  6-8. rebuild them from normalised convolutions at four scales, the detail
     dead-zoned against the IR contrast, dithered, never darker than scanned
     (Normal quality);
  9. back to linear.

numpy only (box sums from row-wise cumulative sums), float32.
"""
import numpy as np

M = 65535.0                 # full scale of a 16-bit sample
TAU = 8847.23               # raw IR above which film counts as "clear"
SLOPE_LIMIT = 0.2           # dye leakage slopes considered (+/-)
W_FLOOR = 0.02              # confidence floor
B_ANCHOR = 0.98             # clean-film margin anchor
PHI_ANCHOR = 0.065          # dust floor anchor
MIN3_DPI = 550              # above this, w uses the min of 3 neighbours
DETAIL_GAIN = {"normal": 1.25, "fine": 1.0}
ALPHA = (0.015, 0.015, 0.025)   # dither amplitude per channel

# Coolscan profiles ("kinds" in ICE). Each: gate bias theta, IR-reference gain
# gamma, dither band edges (fractions of full scale), the resolution at or
# below which beta is measured at the centre only, the weight-ramp anchor, and
# the soft-threshold coefficients a[channel][band] = (a_hi, a_lo).
PROFILES = {
    "ls9000": dict(theta=0.0, gamma=1.100, dither=(0.04, 0.96), contrast_dpi=950,
                   ramp=-960.42,
                   a=[[(1.360, 1.320), (1.370, 1.300), (1.340, 1.250)],
                      [(1.370, 1.300), (1.350, 1.290), (1.300, 1.240)],
                      [(1.340, 1.250), (1.320, 1.250), (1.250, 1.210)]]),
    "ls5000": dict(theta=1.0, gamma=1.100, dither=(0.01, 0.99), contrast_dpi=1600,
                   ramp=-960.42,
                   a=[[(1.210, 1.090), (1.170, 1.080), (1.040, 0.960)],
                      [(1.230, 1.130), (1.140, 1.050), (0.930, 0.840)],
                      [(1.130, 1.040), (1.080, 1.020), (0.970, 0.890)]]),
    "ls50": dict(theta=1.0, gamma=1.000, dither=(0.01, 0.99), contrast_dpi=2500,
                 ramp=-960.52,
                 a=[[(2.210, 2.090), (2.170, 2.080), (2.040, 1.960)],
                    [(2.230, 2.130), (2.140, 2.050), (1.930, 1.840)],
                    [(2.130, 2.040), (2.080, 2.020), (1.970, 1.890)]]),
}

# Gate shift for Pakon IR: a defect counts from ~3.5% below the clear-film
# level instead of Nikon's ~2%. At 1.0 the mask also took single pixels of IR
# noise and the thin residue the dye-leak subtraction leaves at sharp edges
# (our IR line is a little softer than the red one): 1.7-2.6% of each frame.
# At 0.985 it shrank 7-18x on a C-41 roll and a slide strip (serial 3054,
# 2026-10-15) and the defects were still removed (a scratch and hairs).
PAKON_METERING = 0.985

# Pakon Base 16: 2000 px across the 24 mm frame height.
PAKON_BASE16_DPI = round(2000 / (24 / 25.4))

# Row spans (dy, dx_lo, dx_hi) of the 9x9 octagon (max(|dx|,|dy|) <= 4,
# |dx| + |dy| <= 6) and the 5x5 one (<= 2, <= 3).
LEVEL0 = [(-4, -2, 2), (-3, -3, 3), (-2, -4, 4), (-1, -4, 4), (0, -4, 4),
          (1, -4, 4), (2, -4, 4), (3, -3, 3), (4, -2, 2)]
LEVEL1 = [(-2, -1, 1), (-1, -2, 2), (0, -2, 2), (1, -2, 2), (2, -1, 1)]
TENT = (1.0, 2.0, 1.0)


# ----- step 1, density

def density(v):
    """D(v) = M/16 * log2(v + 1), v linear 0..M (array or scalar)."""
    return (np.log2(np.asarray(v, np.float32) + 1.0) * (M / 16.0)).astype(np.float32)


def from_density(d):
    """D^-1: 2^(16 d / M) - 1, clamped to 0..M (float32)."""
    return np.clip(np.exp2(np.asarray(d, np.float32) * (16.0 / M)) - 1.0, 0.0, M)


def _anchor(fraction):
    return float(int(M * fraction))


# ----- step 2, calibrate

def calibrate(red, ir):
    """(c, ir_ref) from a reduced frame: red and IR linear 0..M, same shape.
    c is the red -> IR dye leakage slope, ir_ref the clear-film IR density
    with that leakage removed. None when there is no clear film."""
    red = np.asarray(red, np.float32)
    ir = np.asarray(ir, np.float32)
    d_r, d_ir = density(red), density(ir)
    clear = ir > TAU
    if not clear.any():
        return None
    w = ir[clear].astype(np.float64) ** 2
    r_ref = float((w * d_r[clear]).sum() / w.sum())
    ir_raw = float((w * d_ir[clear]).sum() / w.sum())

    # Leakage: weighted least squares over the 4x4 quadrants of every 8x8
    # tile that is clear film all the way through.
    th, tw = red.shape[0] // 8, red.shape[1] // 8
    c = 0.0
    if th and tw:
        def tiles(a):
            return a[:th * 8, :tw * 8].reshape(th, 8, tw, 8).transpose(0, 2, 1, 3)
        T_r, T_ir, T_raw = tiles(d_r), tiles(d_ir), tiles(ir)
        ok = (T_raw > TAU).all(axis=(2, 3))
        tr = T_r.mean(axis=(2, 3))
        ti = T_ir.mean(axis=(2, 3))
        traw = T_raw.sum(axis=(2, 3))
        num = den = 0.0
        for dy0 in (0, 4):
            for dx0 in (0, 4):
                qr = T_r[:, :, dy0:dy0 + 4, dx0:dx0 + 4].mean(axis=(2, 3)) - tr
                qi = T_ir[:, :, dy0:dy0 + 4, dx0:dx0 + 4].mean(axis=(2, 3)) - ti
                with np.errstate(divide="ignore", invalid="ignore"):
                    slope = qi / qr
                keep = ok & np.isfinite(slope) & (np.abs(slope) <= SLOPE_LIMIT)
                wt = (qr * qr * traw * traw)[keep].astype(np.float64)
                num += float((slope[keep] * wt).sum())
                den += float(wt.sum())
        c = num / den if den > 0 else 0.0
    return c, (ir_raw - c * r_ref) / (1.0 - c)


def reduce(a, k=4):
    """Area-average by k in both axes (the 'prescan' calibrate measures)."""
    h, w = a.shape[0] // k * k, a.shape[1] // k * k
    a = np.asarray(a[:h, :w], np.float32)
    return a.reshape(h // k, k, w // k, k, *a.shape[2:]).mean(axis=(1, 3))


# ----- parameters

class Params:
    def __init__(self, cal, dpi=PAKON_BASE16_DPI, model="ls5000", quality="normal",
                 metering_target=0.9, spread_rows=True):
        p = PROFILES[model]
        c, ir_ref = cal
        self.c, self.ir_ref = float(c), float(ir_ref)
        # The gate bias also undoes metering below full scale: the anchors
        # assume the brightest IR is M.
        self.theta = p["theta"] + float(density(_anchor(min(max(metering_target, 0.0), 1.0)))) - M
        self.ramp_bias = self.ir_ref + float(density(_anchor(B_ANCHOR))) - M
        self.ramp_s = 1.0 / p["ramp"]
        self.phi = float(density(_anchor(PHI_ANCHOR)))
        self.eta = tuple(float(density(_anchor(f))) for f in p["dither"])
        self.gamma = p["gamma"]
        self.a = p["a"]
        self.detail_gain = DETAIL_GAIN[quality]
        self.clamp_l3 = quality == "normal"
        self.min3 = dpi > MIN3_DPI
        self.cross_beta = dpi > p["contrast_dpi"]
        self.spread_rows = spread_rows

    def weight(self, g):
        return np.clip(1.0 + (self.ramp_bias - g) * self.ramp_s, W_FLOOR, 1.0)


# ----- steps 3-5

def gate(red, ir, p):
    """g = (D(ir) - c D(red)) / (1 - c) - theta."""
    return ((density(ir) - p.c * density(red)) / (1.0 - p.c) - p.theta).astype(np.float32)


def confidence(g, p):
    """w from the gate; above MIN3_DPI from the min of the pixel and its two
    neighbours along the sensor line (columns). spread_rows also takes the
    neighbours along the scan (rows): a Pakon adaptation, its IR line is a
    little softer than the visible ones in the scan direction, so a defect's
    IR footprint is narrower than its visible one."""
    if not p.min3:
        return p.weight(g).astype(np.float32)
    m = np.minimum(np.minimum(_shift_cols(g, -1), g), _shift_cols(g, 1))
    if p.spread_rows:
        m = np.minimum(np.minimum(_shift_rows(m, -1), m), _shift_rows(m, 1))
    return p.weight(m).astype(np.float32)


def _shift_cols(a, k):
    """a[:, clamp(x + k)]"""
    if k == 0:
        return a
    idx = np.clip(np.arange(a.shape[1]) + k, 0, a.shape[1] - 1)
    return a[:, idx]


def _shift_rows(a, k):
    """a[clamp(y + k), :]"""
    if k == 0:
        return a
    idx = np.clip(np.arange(a.shape[0]) + k, 0, a.shape[0] - 1)
    return a[idx]


def _and3_cols(a, k):
    return _shift_cols(a, -k) & a & _shift_cols(a, k)


def _and3_rows(a, k):
    return _shift_rows(a, -k) & a & _shift_rows(a, k)


def decide(g, w, p):
    """Which pixels to rebuild: not inside a large dark structure (the IR is
    dark there for a reason other than dust), and, in Normal, not fully
    trusted already."""
    dark = g < p.phi
    row_dark = _and3_cols(_and3_cols(dark, 1), 3)
    col_dark = _and3_rows(_and3_rows(dark, 1), 3)
    mask = ~(_shift_rows(row_dark, -4) | _shift_rows(row_dark, 4) |
             _shift_cols(col_dark, -4) | _shift_cols(col_dark, 4))
    if p.clamp_l3:
        mask &= w < 1.0
    return mask


# ----- steps 6-8, rebuild

def _octagon_sum(a, spans):
    """Sum of `a` over an octagon given as row spans; rows past the edge add
    nothing, columns past the edge repeat the edge column (as dust.rs)."""
    H, W = a.shape
    pad = max(max(-lo, hi) for _, lo, hi in spans)
    ap = np.concatenate([np.repeat(a[:, :1], pad, 1), a, np.repeat(a[:, -1:], pad, 1)], 1)
    cs = np.zeros((H, W + 2 * pad + 1), np.float64)
    np.cumsum(ap, axis=1, out=cs[:, 1:])
    out = np.zeros((H, W), np.float64)
    x = np.arange(W) + pad
    for dy, lo, hi in spans:
        span = cs[:, x + hi + 1] - cs[:, x + lo]          # row sums at each x
        if dy > 0:
            out[:-dy] += span[dy:]
        elif dy < 0:
            out[-dy:] += span[:dy]
        else:
            out += span
    return out.astype(np.float32)


def _tent_sum(a):
    """3x3 binomial tent / 16, cells past any edge add nothing."""
    ap = np.pad(a, 1)
    out = np.zeros_like(a, dtype=np.float32)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            k = TENT[dy + 1] * TENT[dx + 1] / 16.0
            out += k * ap[1 + dy:1 + dy + a.shape[0], 1 + dx:1 + dx + a.shape[1]]
    return out


def _levels(g, w, dens):
    """Per level l = 0..3: confidence mass C_l, gate pyramid P_l and per
    channel L_l (normalised convolutions with weight w)."""
    C, P, L = [], [], []
    wg = w * g
    wd = [w * d for d in dens]
    for kind, k in (("oct0", 1.0 / 69.0), ("oct1", 1.0 / 21.0), ("tent", None)):
        if kind == "tent":
            s = _tent_sum
        else:
            spans = LEVEL0 if kind == "oct0" else LEVEL1
            s = (lambda a, sp=spans, kk=k: _octagon_sum(a, sp) * kk)
        cw = s(w)
        inv = np.where(cw > 0, 1.0 / np.maximum(cw, 1e-30), 0.0).astype(np.float32)
        C.append(cw)
        P.append(s(wg) * inv)
        L.append([s(x) * inv for x in wd])
    C.append(w)
    P.append(g)
    L.append(list(dens))
    return C, P, L


def _uniform(idx, ch):
    """Uniform [0, 1) fixed per (pixel, channel): Stafford's Mix13, as dust.rs."""
    x = (idx.astype(np.uint64) << np.uint64(2)) | np.uint64(ch)
    with np.errstate(over="ignore"):
        x = (x ^ (x >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
        x = (x ^ (x >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
    return ((x ^ (x >> np.uint64(31))) & np.uint64(0xFFFFFF)).astype(np.float32) / float(1 << 24)


def _dither(a, idx, ch, p):
    lo, hi = p.eta
    inside = (a > lo) & (a < hi)
    env = 4.0 / (hi - lo) ** 2 * (a - lo) * (hi - a)
    d = env * (_uniform(idx, ch) - 0.5) * ALPHA[ch] * a
    ok = inside & (a + d > lo) & (a + d < hi)
    return np.where(ok, d, 0.0).astype(np.float32)


def rebuild(g, w, dens, mask, p):
    """(rows, cols, per-channel rebuilt densities) for the masked pixels that
    survive the Normal clamp."""
    C, P, L = _levels(g, w, dens)
    D = [P[l] - P[l - 1] for l in (1, 2, 3)]            # gate detail per band
    if p.cross_beta:
        lo, hi = [], []
        for d in D:
            nb = [d]
            H, W = d.shape
            for dy, dx in ((0, -1), (0, 1), (-1, 0), (1, 0)):
                s = np.full_like(d, np.nan)
                ys = slice(max(0, -dy), H - max(0, dy))
                xs = slice(max(0, -dx), W - max(0, dx))
                yd = slice(max(0, dy), H - max(0, -dy))
                xd = slice(max(0, dx), W - max(0, -dx))
                s[ys, xs] = d[yd, xd]                     # neighbour (y+dy, x+dx)
                nb.append(s)
            st = np.stack(nb)
            lo.append(np.nanmin(st, axis=0))
            hi.append(np.nanmax(st, axis=0))
    else:
        lo, hi = D, D

    ys, xs = np.nonzero(mask)
    idx = ys.astype(np.int64) * g.shape[1] + xs
    conf = [np.minimum(2.0 * C[1][ys, xs], 1.0), C[2][ys, xs], C[3][ys, xs] ** 2]
    out = []
    for ch in range(3):
        a = L[0][ch][ys, xs] + p.gamma * (p.ir_ref - P[0][ys, xs])
        for l in (1, 2, 3):
            detail = (L[l][ch][ys, xs] - L[l - 1][ch][ys, xs]) * p.detail_gain
            a_hi, a_lo = p.a[ch][l - 1]
            h = hi[l - 1][ys, xs]
            lo_l = lo[l - 1][ys, xs]
            alo = np.where(h < 0, a_hi, a_lo)
            ahi = np.where(h < 0, a_lo, a_hi)
            lo_t, hi_t = alo * lo_l, ahi * h
            r = np.where(detail < lo_t, detail - lo_t,
                         np.where(detail > hi_t, detail - hi_t, 0.0))
            a = a + r * conf[l - 1]
        out.append((a + _dither(a, idx, ch, p)).astype(np.float32))
    out = np.stack(out, axis=1)
    if p.clamp_l3:
        keep = (out > 0).all(axis=1)
        orig = np.stack([dens[c][ys, xs] for c in range(3)], axis=1)
        out = np.maximum(out, orig)
        ys, xs, out = ys[keep], xs[keep], out[keep]
    return ys, xs, out


# ----- the whole pass

def clean(rgb, ir, cal=None, dpi=PAKON_BASE16_DPI, model="ls5000", quality="normal",
          metering_target=PAKON_METERING, spread_rows=True):
    """Remove dust from one frame. rgb: (H, W, 3) linear 0..M (above black);
    ir: (H, W) linear 0..M. metering_target shifts the gate (nkscan's
    correction for its own auto-exposure); 1.0 leaves the gate as Nikon's,
    so a defect starts to count ~2% below the measured clear-film level
    whatever the exposure. Tying it to our IR exposure instead widened the
    margin by however far clear film sits below full scale, and on a C-41
    roll (clear film at ~0.75) only the densest defects were caught. The
    default, PAKON_METERING, widens Nikon's margin a little (see there).
    Returns
    (cleaned (H, W, 3) float32, mask of rebuilt pixels, (c, ir_ref)), or the
    input unchanged when there is no clear film to calibrate on."""
    rgb = np.asarray(rgb, np.float32)
    ir = np.asarray(ir, np.float32)
    if cal is None:
        cal = calibrate(reduce(rgb[..., 0]), reduce(ir))
    done = np.zeros(ir.shape, bool)
    if cal is None:
        return rgb.copy(), done, None
    p = Params(cal, dpi=dpi, model=model, quality=quality, metering_target=metering_target,
               spread_rows=spread_rows)
    g = gate(rgb[..., 0], ir, p)
    w = confidence(g, p)
    mask = decide(g, w, p)
    dens = [density(rgb[..., c]) for c in range(3)]
    ys, xs, d = rebuild(g, w, dens, mask, p)
    out = rgb.copy()
    lin = from_density(d)
    for c in range(3):
        v = lin[:, c]
        if p.clamp_l3:
            v = np.maximum(v, out[ys, xs, c])
        out[ys, xs, c] = v
    done[ys, xs] = True
    return out, done, cal


# ----- the Pakon side: the IR plane

def measure_ir_shift(red, ir, search=40):
    """Row offset s such that ir[y + s] lines up with red[y] (the IR line
    sits at its own place on the sensor, like the R/G/B leads). Measured on
    high-passed copies of the central columns."""
    cols = red.shape[1]
    sl = slice(cols // 4, 3 * cols // 4)
    r = np.asarray(red[:, sl], np.float32)
    i = np.asarray(ir[:, sl], np.float32)
    r = r - np.convolve(r.mean(1), np.ones(31) / 31, "same")[:, None]
    i = i - np.convolve(i.mean(1), np.ones(31) / 31, "same")[:, None]
    r = np.diff(r, axis=0)
    i = np.diff(i, axis=0)
    best = (-np.inf, 0)
    n = r.shape[0] - 2 * search
    for s in range(-search, search + 1):
        a = r[search:search + n]
        b = i[search + s:search + s + n]
        score = float((a * b).mean() / (a.std() * b.std() + 1e-9))
        best = max(best, (abs(score), s))
    return best[1]


def ir_plane(img, flat, rows, shift, metering_target=0.9):
    """The IR block of marker-aligned 8000-sample rows (img[:, 6000:8000])
    for the visible rows `rows` (indices into img, after registration),
    shifted by `shift`, flat-fielded per column against the calibration's
    open-gate IR (its dark table is taken with the IR LED on, so the black
    level is used instead), and scaled so the open gate reads
    metering_target * M."""
    from pakon_image import _RAW_BLACK
    r = np.clip(np.asarray(rows) + shift, 0, img.shape[0] - 1)
    ir = img[r, 6000:8000].astype(np.float32) - _RAW_BLACK
    bright = np.asarray(flat["bright"][3], np.float32)[int(flat["px0"]):] - _RAW_BLACK
    gain = np.where(bright > 0, metering_target * M / np.maximum(bright, 1.0), 0.0)
    return np.clip(ir * gain[None, :], 0.0, M)
