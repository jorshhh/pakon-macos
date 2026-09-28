#!/usr/bin/env python3
"""
pakon_image.py — decode a Pakon raw scan stream into 16-bit RGB TIFF(s).

The captured 0x86 image stream (pakon_replay --scan output) is, on this F-135:
  - 16-bit little-endian samples,
  - each scan line is `--linewidth` samples (default 8000 = 16000-byte stride),
    laid out as [ width visible triplets | width IR/extra samples ] where
    width = linewidth//4 (= 2000). The first width*3 samples are the visible
    image, per-pixel interleaved at stride 3 in the order **R, G, B** (pos0=R,
    pos1=G, pos2=B — green is the centre trilinear line); the trailing width
    samples are a single IR (Digital ICE) line, detected and discarded for the
    visible output.

  - the row/triplet phase is recovered per scan from the scanner's per-scanline
    **marker bit** (the LSB of one fixed word, set on every line). The 0x86
    stream is captured in 20480-byte chunks with an arbitrary start, so without
    this the B/R/G samples rotate scan-to-scan and the colour cast flips (purple
    vs turquoise — the old "lomo" inconsistency). Aligning to the marker pins the
    canonical row origin and makes the decode phase-independent (mirrors
    libpakon). `--no-marker-align` falls back to the raw byte-0 phase.

The sensor is **trilinear** (separate R/G/B lines spaced along the scan
direction), so the three channels are offset by a few scan lines and show
colour ghosting at edges if naively combined. By default it **co-registers**
them (auto-measured; the lines are spaced ~8 scan lines apart, so green leads
red by ~8 and blue by ~16 on this F-135); `--no-register` disables it,
`--reg-leads G,B` forces the offsets.

By default it **autocrops** the ribbon: a real scan begins with a dark leader
and often a blank stretch (light through no film, before the strip is loaded),
ends with another blank tail, and carries a uniform gate margin on one side.
`autocrop` drops those (leader = dark rows; blank = bright *and* colour-neutral
rows; margin = low-detail columns) and keeps the film. Pass `--no-autocrop` for
the full raw ribbon.

Structural bright bands (scanner artifacts — persistent bright column ranges
with low variation, e.g. scan-mode light artifacts) are automatically detected
and stitched out of the output; the image zones on either side are concatenated.

This deinterleaves to RGB and writes a **16-bit RGB TIFF**, by default the raw
(uninverted) negative — orange-mask/negative intact — so you can feed it to your
own color-inversion software. `--invert` does a quick linear positive for a
sanity preview only (proper negative inversion is left to dedicated tools).

Use `--resample-to WxH` to correct for non-square raw pixels and get the right
output geometry; for 35mm film at Pakon native resolution use `--resample-to
3000x2000`. The frame splitting (`--frames N`) uses actual inter-frame gap
detection when N > 1, so frames land on real boundaries rather than equal
divisions.

Needs numpy and ImageMagick (`magick`). Writes a small PNG preview alongside.
"""
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np


# --- OEM C-41 negative→positive inversion (recovered from PakonIMAu.dll) -------
# The original Kodak/Pakon software inverts colour negatives via the "ColNeg"
# path (PIColorCorrectColNegPlanarScan), driven by two data files in
# Config/ColorCorrection/:
#   1. _ClientColNegLut.txt — a single shared 14-bit log-density curve. Fit
#      exactly (0 error over all 16384 entries):  out = 3500*log10(16383/in),
#      in=0 -> 16383. This is the tonal flip (density = log10(ref/signal)).
#   2. _ClientColNegMat.txt — a 3x3 + offset dye-crosstalk / orange-mask colour
#      matrix (applied to the inverted positive).
# The OEM scanner's AGC put the clear film base near full-scale before this LUT;
# our raw is not exposure-normalised, so we first normalise each channel by its
# measured film base (Dmin) — this is the per-channel mask removal. The OEM's
# final output-colourspace step (srgb.pf) is the sRGB transfer function, applied
# here for a directly-viewable positive.
_C41_MAT = np.array([[1.11882, -0.10130, -0.01161],
                     [-0.20096, 1.10082, 0.11698],
                     [-0.11657, 0.04834, 1.08274]], dtype=np.float32)
_C41_OFF = np.array([-82.60334, -586.90975, -707.78706], dtype=np.float32)


def _c41_lut():
    """OEM ColNeg shared log-density LUT: out = 3500*log10(16383/in)."""
    ii = np.arange(16384, dtype=np.float64)
    out = np.where(ii < 1.0, 16383.0,
                   3500.0 * np.log10(16383.0 / np.maximum(ii, 1.0)))
    return np.clip(out, 0.0, 16383.0).astype(np.float32)


def _srgb_encode(x01):
    """sRGB transfer function (the OEM srgb.pf output colourspace), x in [0,1]."""
    a = 0.055
    return np.where(x01 <= 0.0031308, x01 * 12.92,
                    (1.0 + a) * np.power(np.clip(x01, 0.0, 1.0), 1.0 / 2.4) - a)


_G_OF_POS = {0: "r", 1: "g", 2: "b"}  # chans label of each interleave position

# Every row layout seen on real hardware: stride in samples -> whether the row
# ends in a trailing IR/Digital-ICE block. F-135 always scans with the IR lane
# (8000 = 2000 px x 4); the F-135+ stride follows the resolution and only
# carries the IR block when ICE is on (docs/F135_PLUS_CAPTURES.md).
KNOWN_ROW_LAYOUTS = {
    8000: True,    # F-135 (2000 px RGB + IR), F-135+ Base 16 with IR
    6000: False,   # F-135+ Base 16, no IR (2000 px)
    4500: False,   # F-135+ Base 8, no IR (1500 px)
    4000: True,    # F-135+ Base 4 with IR (1000 px RGB + IR)
    3000: False,   # F-135+ Base 4, no IR (1000 px)
}


def detect_row_layout(raw):
    """Measure a raw stream's row stride by autocorrelation against the known
    layouts. Returns (linewidth, has_ir_lane) or None when no candidate wins
    cleanly (e.g. a stream too short or with no image content).

    The true stride and its integer multiples all score high (a lag of two
    rows correlates as well as one), so among candidates within a small margin
    of the best score the SMALLEST wins: on a genuine 8000-sample stream the
    4000 lag lands mid-row and scores low, while on a genuine 4000-sample
    stream both 4000 and 8000 score high and 4000 is chosen."""
    total = raw.size
    window_len = min(2_000_000, total // 2)
    if window_len < 200_000:
        return None
    start = (total - window_len) // 2
    window = raw[start:start + window_len].astype(np.float64)
    window -= window.mean()

    scores = {}
    for lag in KNOWN_ROW_LAYOUTS:
        if lag * 4 > window_len:
            continue
        head, tail = window[:-lag], window[lag:]
        denominator = np.linalg.norm(head) * np.linalg.norm(tail)
        if denominator:
            scores[lag] = float(np.dot(head, tail) / denominator)
    if not scores:
        return None
    best = max(scores.values())
    if best < 0.6:
        return None
    for lag in sorted(scores):
        if scores[lag] >= best - 0.02:
            return lag, KNOWN_ROW_LAYOUTS[lag]
    return None


def marker_align(img, min_frac=0.6, sample_rows=3000):
    """Rotate every row so the scanner's per-scanline marker bit sits at column 0
    — the canonical row origin (after libpakon's detect_f135_scanline_marker_origin).

    The firmware sets the LSB (bit 0) of one fixed word on every scan line. Find
    that word by counting, per column, how often bit 0 is set across rows; the
    column that's set on (almost) every row is the marker. Rolling it to column 0
    fixes the triplet/IR phase regardless of where the 0x86 capture began, so the
    B/R/G channel identity no longer drifts between scans.

    `img` is (rows, linewidth) uint16. Returns (aligned_img, marker_col), or
    (img, None) if no column is reliably set (no marker → leave phase untouched)."""
    sub = img[::max(1, img.shape[0] // sample_rows)]
    frac = (sub & 1).mean(0)
    cm = int(np.argmax(frac))
    if frac[cm] < min_frac:
        return img, None
    return np.roll(img, -cm, axis=1), cm


def _zone_band_bases(chans, c0, c1, sat, pct, nwin, winrows):
    """Per-window per-position film-base percentiles for a zone. Measured on
    *local* row-bands, NOT the whole roll: the orange film-base signal is clean
    in any given band (R>>B), but a whole-roll percentile is dominated by blown
    neutral highlights (~saturation, R≈G≈B) which destroy the ranking."""
    n = chans["b"].shape[0]
    bands = []
    for w in range(nwin):
        r0 = int((w + 0.5) / nwin * n)
        r1 = min(n, r0 + winrows)
        b = {}
        for pos, gname in _G_OF_POS.items():
            col = chans[gname][r0:r1, c0:c1].ravel().astype(np.float32)
            m = col[col < sat]
            b[pos] = float(np.percentile(m if m.size else col, pct))
        bands.append(b)
    return bands


def detect_zone_perm(chans, c0, c1, sat=63000, pct=99.5, nwin=8, winrows=1500):
    """Auto-detect a zone's R/G/B channel identity using the C-41 orange film
    base as physical ground truth.

    The clearest film (rebate / deep shadows) is most transmissive in RED (the
    orange mask passes red, blocks blue), so ranking the three interleave
    positions by their per-position film base gives R (highest) > G > B
    (lowest). The order is a fixed CCD-tap property of the zone, so it is
    measured per local band and **majority-voted** for robustness (blown frames
    that read neutral are outvoted). Replaces hardcoded channel orders (which
    were scan/zone-specific and produced a purple cast / mismatched-cast seam
    when wrong).

    Returns (perm, bases_by_pos): perm maps output r/g/b -> the global `chans`
    key; bases_by_pos is the median {pos: base} for logging."""
    from collections import Counter
    bands = _zone_band_bases(chans, c0, c1, sat, pct, nwin, winrows)
    votes = [tuple(sorted(b, key=lambda p: b[p], reverse=True)) for b in bands]
    order = Counter(votes).most_common(1)[0][0]  # (R_pos, G_pos, B_pos)
    perm = {"r": _G_OF_POS[order[0]], "g": _G_OF_POS[order[1]],
            "b": _G_OF_POS[order[2]]}
    med = {pos: float(np.median([b[pos] for b in bands])) for pos in (0, 1, 2)}
    return perm, med


def measure_dmin(rgb16, pct=99.5, sat=63000, nwin=12, winrows=1500):
    """Per-channel film base (Dmin) = the clear/brightest point of the negative,
    used as the per-channel white point (dividing by it removes the orange mask
    and white-balances).

    Measured as the **median over local row-bands** of each channel's clipped
    high percentile. A single whole-roll percentile is dominated by blown
    neutral highlights (~saturation, R≈G≈B), which leaves the orange mask in
    (warm/purple cast); local bands recover the true orange base (R > G > B).
    Pixels at/above `sat` are excluded (clipped, no mask info). Raw 16-bit."""
    n = rgb16.shape[0]
    bands = []
    for w in range(nwin):
        r0 = int((w + 0.5) / nwin * n)
        seg = rgb16[r0:min(n, r0 + winrows)].reshape(-1, 3).astype(np.float32)
        bands.append([np.percentile(c[c < sat], pct) if (c < sat).any()
                      else np.percentile(c, pct) for c in seg.T])
    base = np.median(np.array(bands), axis=0).astype(np.float32)
    return np.maximum(base, 1.0)


def invert_c41(rgb16, base, lut, matrix="none", wb=False, srgb=True):
    """Apply the OEM C-41 ColNeg inversion to a 16-bit RGB array (one frame).

    `base` = per-channel Dmin (16-bit, from measure_dmin); `matrix` in
    {none, post, pre} controls the crosstalk matrix; `wb` applies a per-frame
    gray-world white balance; `srgb` applies the sRGB display encode. Returns
    16-bit RGB positive.

    The per-channel Dmin normalisation IS the white balance (white point = the
    orange film base) and lands very close to the OEM's own rendering — verified
    against the OEM reference scans of this roll. Gray-world (`wb`) is therefore
    OFF by default: it forces every scene neutral, stripping intentional warmth
    (tungsten/sunset) and leaving a blue lean. The OEM crosstalk matrix is also
    off: its offsets (−82/−586/−707) suit the OEM's pre-balanced scale and
    over-subtract G/B here (extreme red)."""
    in14 = np.clip(rgb16.astype(np.float32) / base * 16383.0, 0.0, 16383.0)
    if matrix == "pre":
        in14 = np.clip(in14 @ _C41_MAT.T + _C41_OFF, 0.0, 16383.0)
    pos = lut[in14.astype(np.uint16)]
    if matrix == "post":
        pos = np.clip(pos @ _C41_MAT.T + _C41_OFF, 0.0, 16383.0)
    if wb:
        # Gray-world: scale each channel so its median matches the luma median.
        # Median (not mean) + clamped gains keep colour-dominant scenes sane.
        med = np.median(pos.reshape(-1, 3), axis=0)
        gain = np.clip(med.mean() / np.maximum(med, 1.0), 0.6, 1.7)
        pos = np.clip(pos * gain, 0.0, 16383.0)
    if srgb:
        pos = _srgb_encode(pos / 16383.0) * 16383.0
    return np.clip(pos * 4.0, 0.0, 65535.0).astype(np.uint16)


def render_jpeg(rgb16_srgb, rpd_path, gamma=1.5, lo_p=0.5, hi_p=99.7, knee=0.85):
    """Render the OEM "vibrant JPEG" look from our sRGB-encoded C-41 positive.

    Reproduces the OEM's JPEG export options (PSI "Other Options"):
      1. **Color Correction (12-bit RPD)** — run through Kodak's `rpd.pf` ICC
         rendering profile (`RPD_dls_3` + `yellow5` cascade) via littleCMS. This
         is the accurate Kodak colour rendering.
      2. **Color Scene Balance** — surrogate for the OEM SBA: per-channel
         auto-levels (neutralise shadow+highlight ends) + a midtone gamma lift.
    DIFFERENCE FROM OEM (deliberate improvement): the OEM blows out highlights;
    we roll them off with a soft exponential shoulder above `knee`, so bright
    areas keep detail instead of clipping to flat white.

    Input is the same 16-bit sRGB frame written to the TIFF. Returns an 8-bit
    (H,W,3) uint8 array. The plain TIFF stays the full-control, unrendered file."""
    from PIL import Image, ImageCms
    src = Image.fromarray((rgb16_srgb >> 8).astype(np.uint8), "RGB")
    rpd = ImageCms.getOpenProfile(rpd_path)
    srgb = ImageCms.createProfile("sRGB")
    tr = ImageCms.buildTransform(rpd, srgb, "RGB", "RGB", renderingIntent=0)
    a = np.asarray(ImageCms.applyTransform(src, tr)).astype(np.float32) / 255.0
    out = np.empty_like(a)
    for c in range(3):
        ch = a[:, :, c]
        lo, hi = np.percentile(ch, lo_p), np.percentile(ch, hi_p)
        x = np.clip((ch - lo) / max(hi - lo, 1e-3), 0.0, None)
        x = np.where(x <= 0, 0.0, x ** (1.0 / gamma))          # midtone lift
        # soft highlight shoulder: compress (knee, inf) -> (knee, 1), no hard clip
        x = np.where(x > knee,
                     knee + (1 - knee) * (1 - np.exp(-(x - knee) / (1 - knee))),
                     x)
        out[:, :, c] = np.clip(x * 255.0, 0, 255)
    return out.astype(np.uint8)


def _vlead(ch, ref, rng=40, rowstep=1, colstep=3):
    """Vertical 'lead' of channel `ch` over `ref` in scan lines: the dy that
    maximizes corr(ch[row], ref[row+dy]). On this F-135 the sensor is trilinear
    (separate R/G/B lines along the scan), so a feature at ref row y appears in
    `ch` at row y-dy. Measured on a central detail band, subsampled for speed."""
    n = ch.shape[0]
    a, b = int(n * 0.35), int(n * 0.65)
    A = ch[a:b:rowstep, ::colstep].astype(np.float32)
    Rf = ref[a:b:rowstep, ::colstep].astype(np.float32)
    rng = min(rng, A.shape[0] // 4)
    base = A[rng:-rng]
    best = (0, -2.0)
    for dy in range(-rng, rng + 1):
        cmp = Rf[rng + dy:Rf.shape[0] - rng + dy]
        x = base.ravel() - base.mean()
        y = cmp.ravel() - cmp.mean()
        d = np.linalg.norm(x) * np.linalg.norm(y)
        if d:
            c = float(np.dot(x, y) / d)
            if c > best[1]:
                best = (dy, c)
    return best[0]


def measure_leads(chans, c0, c1, perm=None):
    """Measure the trilinear G/B line leads (in scan lines) relative to R, on a
    single column zone [c0:c1] of the full-width deinterleaved channels.

    `perm` is an optional dict remapping channel names, e.g. {"r":"g","g":"b","b":"r"}
    for zones whose tap outputs channels in a different order than the global default."""
    ch = {c: chans[perm[c]] if perm else chans[c] for c in ("r", "g", "b")}
    return {"r": 0,
            "g": _vlead(ch["g"][:, c0:c1], ch["r"][:, c0:c1]),
            "b": _vlead(ch["b"][:, c0:c1], ch["r"][:, c0:c1])}


# Trilinear CCD line spacing (scan lines) in the marker-aligned R,G,B frame:
# green is read ~8 lines after red, blue ~16 (matches libpakon's fixed
# red=0/green=8/blue=16; our co-registration sign makes them negative). The
# spacing is a fixed sensor-geometry constant, so it's the default + the sanity
# bound for the auto-measured leads — the cross-correlation returns 0 (or junk)
# on a low-detail band, e.g. when a frame gap lands in the centre, which leaves
# the channels unregistered and the frame ghosted (this happened on neg4).
_TRILINEAR_LEADS = {"r": 0, "g": -8, "b": -16}


def validated_leads(measured):
    """Accept auto-measured trilinear leads only if they're physically plausible
    (close to the fixed spacing); otherwise fall back to the constant. Returns
    (leads, measured_ok)."""
    g, b = measured["g"], measured["b"]
    if -13 <= g <= -3 and -22 <= b <= -9:
        return {"r": 0, "g": g, "b": b}, True
    return dict(_TRILINEAR_LEADS), False


def register_zones(chans, zones, leads_per_zone, correct_seam=False, zone_perms=None):
    """Co-register the trilinear R/G/B planes and assemble the visible image.

    `zones` is a list of (c0, c1) column ranges in *output order*; for a
    wrap-split scan that is [after-IR, before-IR] so the halves rejoin at the
    sensor wrap seam. Each zone has its own R/G/B leads (the two halves of a
    wrap-split image come from opposite ends of the sensor readout and need
    different leads — using one global lead leaves one half ghosted).

    `zone_perms` is an optional list of per-zone channel permutation dicts (same
    format as `measure_leads` perm). Used when a zone's tap outputs channels in a
    different order than the global default (e.g. the second tap in a wrap-split
    scan).

    All zones are row-cropped against a *common* lead span (global max/min over
    every zone) so they stay aligned to the same scan lines, then concatenated
    left-to-right. Returns the assembled (H, W, 3) array."""
    allv = [v for leads in leads_per_zone for v in leads.values()]
    gm, glo = max(allv), min(allv)
    n = chans["r"].shape[0]
    L = n - (gm - glo)
    parts = []
    for i, ((c0, c1), leads) in enumerate(zip(zones, leads_per_zone)):
        perm = zone_perms[i] if zone_perms else None
        src = {c: chans[perm[c]] if perm else chans[c] for c in ("r", "g", "b")}
        out = {c: src[c][gm - leads[c]: gm - leads[c] + L, c0:c1] for c in src}
        parts.append(np.stack([out["r"], out["g"], out["b"]], axis=-1))
    if correct_seam:
        parts = dual_tap_correct(parts)
    return np.concatenate(parts, axis=1)


def dual_tap_correct(parts, seam_cols=50):
    """Correct the dual-tap CCD gain/offset seam for wrap-split (LowRes) scans.

    When the IR band sits in the middle of the line the visible image wraps into
    two zones, each read by a different CCD output tap with its own analogue
    gain and offset. This produces a hard per-channel tint seam at the zone
    junction. Fix: measure per-channel pixel statistics at the seam boundary
    (last seam_cols of zone 0 and first seam_cols of zone 1) and apply a linear
    gain+offset correction to zone 0 so it matches zone 1 at the seam.
    """
    if len(parts) != 2:
        return parts
    z0 = parts[0].astype(np.float64)
    z1 = parts[1].astype(np.float64)
    k = min(seam_cols, z0.shape[1], z1.shape[1])
    s0 = z0[:, -k:, :].reshape(-1, 3)
    s1 = z1[:, :k, :].reshape(-1, 3)
    ch_names = ("R", "G", "B")
    corrected = z0.copy()
    for ch in range(3):
        mu0, mu1 = s0[:, ch].mean(), s1[:, ch].mean()
        sigma0, sigma1 = s0[:, ch].std(), s1[:, ch].std()
        if sigma0 > 1.0:
            a = sigma1 / sigma0
            b = mu1 - a * mu0
        else:
            a, b = 1.0, mu1 - mu0
        print(f"  dual-tap {ch_names[ch]}: a={a:.4f} b={b:+.1f}  "
              f"seam mu {mu0:.0f} -> {mu1:.0f}")
        corrected[:, :, ch] = a * z0[:, :, ch] + b
    return [np.clip(corrected, 0, 65535).astype(np.uint16),
            np.clip(z1, 0, 65535).astype(np.uint16)]


def find_ir_band(chans, full):
    """Locate the IR (Digital ICE) band: a contiguous run of bright,
    colour-neutral columns (R≈G≈B). Returns (ir0, ir1) in deinterleaved column
    space, or None. Measured on a central film slice."""
    n = chans["r"].shape[0]
    a, b = max(0, n // 2 - 5000), n // 2 + 5000
    sl = np.stack([chans["r"][a:b], chans["g"][a:b], chans["b"][a:b]],
                  axis=-1).astype(np.float32)
    col_mean = sl.mean((0, 2))
    col_spread = sl.max(2).mean(0) - sl.min(2).mean(0)
    ir = (col_mean > 0.35 * full) & (col_spread < 0.06 * full)
    runs = _all_runs(ir, min_len=100)
    return runs[0] if runs else None


def write_tiff(rgb16, path, resize=None):
    """Write an (H,W,3) uint16 array as a 16-bit RGB TIFF via ImageMagick.
    If resize=(W,H) is given, resample to exactly that size."""
    h, w, _ = rgb16.shape
    fd, tmp = tempfile.mkstemp(suffix=".rgb")
    os.close(fd)
    rgb16.astype(">u2").tofile(tmp)              # big-endian interleaved RGB
    try:
        cmd = ["magick", "-size", f"{w}x{h}", "-depth", "16",
               "-endian", "msb", f"rgb:{tmp}"]
        if resize:
            rw, rh = resize
            # '!' forces exact dimensions (no aspect preservation — we are
            # deliberately correcting the pixel aspect ratio)
            cmd += ["-filter", "Lanczos", "-resize", f"{rw}x{rh}!", "-depth", "16"]
        cmd.append(path)
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        sys.exit("ImageMagick 'magick' not found (install imagemagick)")
    finally:
        os.unlink(tmp)


def _all_runs(mask, min_len=1):
    """Return list of (start, end) inclusive for all True runs in mask,
    filtered to minimum length min_len, sorted by length descending."""
    idx = np.where(mask)[0]
    if not len(idx):
        return []
    runs = []
    s, p = idx[0], idx[0]
    for i in idx[1:]:
        if i != p + 1:
            if p - s + 1 >= min_len:
                runs.append((int(s), int(p)))
            s = i
        p = i
    if p - s + 1 >= min_len:
        runs.append((int(s), int(p)))
    runs.sort(key=lambda r: r[1] - r[0], reverse=True)
    return runs


def autocrop(rgb):
    """Trim the dark leader, the blank (no-film) pre/post-load scan, and the
    residual low-detail margins, returning (cropped, (r0, r1, c0, c1)).

    Operates on the already de-wrapped, IR-removed visible image (a single
    contiguous zone). Rows: a row is non-image if it's very dark (leader) OR
    bright *and* neutral (light through no film). We keep the largest contiguous
    run of image rows. After that, further trim uniform (low-detail) rows from
    both ends — this removes blank film base (orange mask, no exposure) that is
    neither dark nor bright-neutral. Cols: trim residual low-detail margins.
    """
    full = float(rgb.max()) or 1.0
    sub = rgb[:, ::8, :].astype(np.float32)
    bright = sub.mean((1, 2))
    spread = sub.max(2).mean(1) - sub.min(2).mean(1)
    dark = bright < 0.06 * full
    blank = (bright > 0.55 * full) & (spread < 0.03 * full)
    r0, r1 = _all_runs(~(dark | blank))[0]

    # Trim low-detail (uniform film base) from both ends. Sample every 4 rows;
    # threshold at 25% of the median detail in the central half of the ribbon.
    step = 4
    lum_e = rgb[r0:r1 + 1:step, ::8, :].astype(np.float32)
    row_det = lum_e.std(axis=(1, 2))
    n = len(row_det)
    mid_det = np.median(row_det[n // 4: 3 * n // 4]) if n > 4 else row_det.mean()
    thresh = 0.25 * mid_det
    trim_s = 0
    while trim_s < n and row_det[trim_s] < thresh:
        trim_s += 1
    trim_e = n - 1
    while trim_e > trim_s and row_det[trim_e] < thresh:
        trim_e -= 1
    r0 = r0 + trim_s * step
    r1 = min(r1, r0 + (trim_e - trim_s + 1) * step)

    lum = rgb[r0:r1 + 1:15].astype(np.float32).mean(2)
    cs = lum.std(0)
    c0, c1 = _all_runs(cs > 0.28 * cs.max())[0]
    return rgb[r0:r1 + 1, c0:c1 + 1], (r0, r1, c0, c1)


def find_frame_grid(ribbon, n_frames=None, pitch_lo=2600, pitch_hi=3800):
    """Find the inter-frame boundaries on the ribbon.

    The frame count is ALWAYS auto-detected — never forced (a 36-exposure roll
    commonly scans as 37+ usable frames, and forcing a count makes the cuts drift
    a fraction of a frame each, so the inter-frame gap creeps into the picture).

    Method: (1) measure the true frame **pitch** by autocorrelation of the
    per-row detail profile — robust to where exactly the film starts and to the
    real count; (2) derive the count from span / pitch; (3) pick the global
    phase that lands the regular grid in low-detail gaps; (4) **snap** each
    interior boundary to its local detail minimum (the actual rebate) to absorb
    film-advance jitter. `n_frames` is an optional hard override (rarely needed).

    Returns (cut_rows, pitch): the inter-frame boundary rows (gap positions)
    spanning the film, and the frame period in rows. The caller crops a fixed
    width centred in each [cut_k, cut_{k+1}] cell.
    """
    rows, cols, _ = ribbon.shape

    # Per-row detail + brightness on the central columns (avoids edge stripes).
    step_r = max(1, rows // 8000)
    cen0, cen1 = cols // 4, 3 * cols // 4
    step_c = max(1, (cen1 - cen0) // 200)
    sub = ribbon[::step_r, cen0:cen1:step_c, :].astype(np.float32)
    detail = sub.mean(2).std(1)
    bright = sub.mean((1, 2))
    n = len(detail)

    win = max(3, int(0.01 * n))
    det_s = np.convolve(detail, np.ones(win) / win, mode='same')
    dnorm = (det_s - det_s.min()) / (det_s.max() - det_s.min() + 1e-9)

    # Exclude the bright pre-roll: the leading run of anomalously bright rows
    # (open-gate white, ~2-4x the film-base median) before the film loads.
    med = np.median(bright)
    start = 0
    for i in range(n):
        if bright[i] > 1.6 * med:
            start = i + 1
        elif i - start > 30:
            break
    end = n
    span = end - start

    P_lo, P_hi = max(2, pitch_lo // step_r), pitch_hi // step_r

    # (1) True pitch via autocorrelation of the (detrended) detail profile.
    d = detail[start:end] - detail[start:end].mean()
    if len(d) > P_hi:
        ac = np.correlate(d, d, mode="full")[len(d) - 1:]
        seg = ac[P_lo:P_hi + 1]
        pitch0 = P_lo + int(np.argmax(seg)) if len(seg) else max(2, span // 36)
    else:
        pitch0 = max(2, span // max(1, (n_frames or 36)))
    if n_frames and n_frames > 1:           # optional hard override
        pitch0 = max(2, span // n_frames)

    # (2) Phase-locked uniform comb. Film advance is regular, so the gaps sit at
    # φ + k·P — fit ONE (P, φ) that lands the whole comb in low-detail gaps (use
    # every comb tooth at once; do NOT snap cuts individually — that just chases
    # dark-scene minima and distorts frames). Search P in a narrow band around
    # the autocorr pitch.
    lo = max(P_lo, int(pitch0 * 0.95))
    hi = min(P_hi, int(pitch0 * 1.05))
    best = None
    for P in range(lo, hi + 1):
        for phi in range(0, P, max(1, P // 80)):
            pos = np.arange(start + phi, end, P)
            if len(pos) < 2:
                continue
            score = dnorm[pos].mean()
            if best is None or score < best[0]:
                best = (score, P, phi)
    if best is None:
        return [0, rows], pitch0 * step_r
    _, P, phi = best

    # (3) Lock each comb tooth onto its actual gap (deepest detail dip in a small
    # window — the comb is already close, so this won't reach distant dark-scene
    # minima) for accurate gap positions. These boundaries are trustworthy; the
    # caller crops a fixed width centred between them (gaps vary in width, so we
    # do NOT try to measure each rebate — that's unreliable).
    def snap(g):
        w = max(2, int(0.12 * P))
        a, b = max(0, g - w), min(n, g + w + 1)
        return a + int(np.argmin(det_s[a:b]))

    gaps = [snap(g) for g in range(start + phi, end, P) if start < g < end]
    bounds = sorted(set([start] + gaps + [end]))
    cells = [(bounds[i], bounds[i + 1]) for i in range(len(bounds) - 1)]

    # (4) Drop junk end cells: partial slivers (< 0.55·P, leader/tail fragments)
    # and leading/trailing low-detail cells (the leader/blank produces one — a
    # real frame carries detail; interior dark frames are kept).
    cell_det = [float(dnorm[a:b].mean()) for a, b in cells]
    med = float(np.median(cell_det)) if cell_det else 0.0
    keep = [c[1] - c[0] >= 0.55 * P for c in cells]
    for i in range(len(cells)):                       # leading
        if keep[i] and cell_det[i] < 0.45 * med:
            keep[i] = False
        elif keep[i]:
            break
    for i in range(len(cells) - 1, -1, -1):           # trailing
        if keep[i] and cell_det[i] < 0.45 * med:
            keep[i] = False
        elif keep[i]:
            break
    cells = [c for c, k in zip(cells, keep) if k]
    if not cells:
        return [0, rows], P * step_r

    cut_rows = [int(cells[0][0] * step_r)] + [int(c[1] * step_r) for c in cells]
    return cut_rows, P * step_r


def write_preview(rgb16, path, maxdim=1000):
    try:
        from PIL import Image
    except ImportError:
        return
    a = rgb16
    step = max(1, max(a.shape[0] // maxdim, a.shape[1] // maxdim))
    a8 = (a[::step, ::step] >> 8).astype(np.uint8)
    Image.fromarray(a8).save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("raw")
    ap.add_argument("--linewidth", type=int, default=8000,
                    help="samples per scan line (default 8000)")
    ap.add_argument("--ir-lane", action=argparse.BooleanOptionalAction,
                    default=True,
                    help="row carries a trailing IR/extra block, so width = "
                         "linewidth/4 (F-135 8000-sample rows; F-135+ scans "
                         "with IR on). --no-ir-lane for IR-off F-135+ streams "
                         "where the row is pure RGB and width = linewidth/3.")
    ap.add_argument("--order", default="rgb",
                    help="channel order of the interleave (default rgb)")
    ap.add_argument("--channel-order", default="fixed",
                    choices=["fixed", "auto", "brg"],
                    help="R/G/B identity: 'fixed' (default) = R,G,B at "
                         "interleave positions 0,1,2; 'auto' = detect from the "
                         "orange film base (unreliable on dark/red rolls); "
                         "'brg' = old (wrong) B,R,G order, A/B comparison only")
    ap.add_argument("--marker-align", action=argparse.BooleanOptionalAction,
                    default=True,
                    help="align rows to the per-scanline marker bit so the B/R/G "
                         "triplet phase is independent of the capture start "
                         "(default on; fixes the inconsistent colour cast across "
                         "scans). --no-marker-align uses the raw byte-0 phase")
    ap.add_argument("--rotate", type=int, default=0, choices=[0, 90, 180, 270])
    ap.add_argument("--frames", type=int, default=None,
                    help="split into N frames (default: auto-detect from valley count)")
    ap.add_argument("--crop-width", type=int,
                    help="keep only this many px of width (drop blank overscan)")
    ap.add_argument("--autocrop", action=argparse.BooleanOptionalAction,
                    default=True,
                    help="auto-trim leader/blank-scan/structural-bands (default "
                         "on; --no-autocrop keeps the full raw ribbon)")
    ap.add_argument("--register", action=argparse.BooleanOptionalAction,
                    default=True,
                    help="co-register the trilinear R/G/B sensor lines (default "
                         "on; fixes colour ghosting at edges)")
    ap.add_argument("--seam-correct", action=argparse.BooleanOptionalAction,
                    default=True,
                    help="correct dual-tap CCD gain/offset seam for wrap-split "
                         "(LowRes/full-roll) scans (default on; no-op for "
                         "single-zone HiRes scans)")
    ap.add_argument("--reg-leads",
                    help="force channel leads in lines as 'G,B' (rel. to R), "
                         "e.g. 16,8; default = auto-measure")
    ap.add_argument("--resample-to",
                    help="resample each output frame to WxH (e.g. 3000x2000) to "
                         "correct non-square raw pixels; for 35mm use 3000x2000")
    ap.add_argument("--invert", action="store_true",
                    help="quick linear positive (preview only; not real C-41)")
    ap.add_argument("--invert-c41", action="store_true",
                    help="OEM-faithful C-41 inversion (ColNeg log-density LUT + "
                         "crosstalk matrix + sRGB encode, recovered from "
                         "PakonIMAu.dll). Produces a viewable positive.")
    ap.add_argument("--c41-matrix", default="none", choices=["none", "post", "pre"],
                    help="where to apply the ColNeg crosstalk matrix (default "
                         "none; the OEM matrix offsets over-subtract G/B in our "
                         "scale and cause a red cast)")
    ap.add_argument("--c41-wb", action=argparse.BooleanOptionalAction, default=False,
                    help="per-frame gray-world white balance (default OFF; the "
                         "Dmin normalisation already matches the OEM, and "
                         "gray-world strips intentional scene warmth → blue lean)")
    ap.add_argument("--c41-no-srgb", action="store_true",
                    help="skip the sRGB display encode (leave in ColNeg density space)")
    ap.add_argument("--c41-dmin-pct", type=float, default=99.5,
                    help="per-channel film-base (Dmin) percentile (default 99.5)")
    ap.add_argument("--jpeg", action="store_true",
                    help="also write a rendered .jpg per frame (OEM 'vibrant' "
                         "look: rpd.pf colour profile + scene-balance + highlight "
                         "roll-off). Requires --invert-c41 and the RPD profile.")
    ap.add_argument("--rpd-profile",
                    default=os.path.join(os.path.dirname(__file__), "..",
                                         "profiles", "rpd.pf"),
                    help="path to Kodak rpd.pf ICC profile (default repo "
                         "profiles/rpd.pf; extract from the OEM install)")
    ap.add_argument("--jpeg-gamma", type=float, default=1.5,
                    help="midtone lift for the JPEG render (default 1.5)")
    ap.add_argument("--jpeg-quality", type=int, default=92,
                    help="JPEG quality (default 92)")
    ap.add_argument("-o", "--out", default="frame",
                    help="output path prefix (default 'frame')")
    args = ap.parse_args()

    resize = None
    if args.resample_to:
        try:
            rw, rh = (int(v) for v in args.resample_to.lower().split("x"))
            resize = (rw, rh)
        except ValueError:
            sys.exit("--resample-to must be WxH, e.g. 3000x2000")

    rpd_path = os.path.abspath(args.rpd_profile)
    if args.jpeg:
        if not args.invert_c41:
            sys.exit("--jpeg requires --invert-c41 (it renders the inverted positive)")
        if not os.path.exists(rpd_path):
            sys.exit(f"--jpeg needs the Kodak RPD profile; not found at "
                     f"{rpd_path}\n  extract rpd.pf from the OEM install and pass "
                     f"--rpd-profile, or place it at profiles/rpd.pf")

    raw = np.memmap(args.raw, dtype="<u2", mode="r")
    lw = args.linewidth
    lines = raw.size // lw
    img = raw[:lines * lw].reshape(lines, lw)

    # Align to the per-scanline marker bit BEFORE deinterleaving. The 0x86 image
    # stream is captured in 20480-byte bulk chunks with an arbitrary start, so the
    # row/triplet phase floats from scan to scan; without correcting it the B/R/G
    # samples rotate and the colour cast flips between scans (purple on one scan,
    # turquoise on another — the long-standing "lomo" inconsistency). The scanner
    # marks every scan line by setting the LSB of one fixed word, so finding that
    # word pins the true row origin and makes the phase scan-independent (mirrors
    # libpakon's detect_f135_scanline_marker_origin). Verified phase-invariant by
    # decoding the same scan from many simulated capture starts.
    if args.marker_align:
        img, marker_col = marker_align(img)
        if marker_col is not None:
            print(f"marker align: row origin at raw col {marker_col}")
        else:
            print("marker align: no reliable marker found; using raw phase "
                  "(colour cast may vary)")

    # Wire row layout (libpakon repack_shifted_rows): each marker-aligned row is
    #   [ width per-pixel-interleaved R,G,B triplets | width IR/extra samples ]
    # so the first width*3 = lw*3//4 samples are the visible image at stride 3
    # (pos0=R, pos1=G, pos2=B; green is the centre trilinear line), and the
    # trailing lw//4 samples are a SINGLE IR (Digital ICE) line — NOT a column
    # band that splits the image. Hence one visible zone, no wrap-split. width =
    # lw//4 (= 2000 for the F-135's 8000-sample row). [Supersedes the old B,R,G /
    # 2666-px reading, which assumed phase-0 alignment and folded the IR line into
    # the channels.]
    # F-135+ IR-off streams have no trailing IR block: the row is width*3 pure
    # RGB samples (see docs/F135_PLUS_CAPTURES.md), so width = lw//3 there.
    width = lw // 4 if args.ir_lane else lw // 3
    nvis = width * 3
    chans = {"r": img[:, 0:nvis:3], "g": img[:, 1:nvis:3], "b": img[:, 2:nvis:3]}
    # The trailing lw//4 samples per row (when --ir-lane) are the IR/Digital
    # ICE line; nothing downstream consumes it yet, so it is simply not sliced.
    full = float(max(chans["r"][::997].max(), chans["g"][::997].max(),
                     chans["b"][::997].max())) or 1.0

    # Single visible zone. Marker-aligned, the CCD emits R,G,B at fixed positions
    # 0,1,2; 'fixed' (default) uses that identity. 'auto' = orange-base detection
    # (opt-in, unreliable on dark/red rolls); 'brg' = a swapped order for A/B
    # comparison only.
    zones = [(0, width)]
    if args.channel_order == "auto":
        perm, base = detect_zone_perm(chans, 0, width)
        ranked = sorted(base, key=lambda p: base[p], reverse=True)
        print(f"channel-order: pos-bases {[int(base[p]) for p in (0, 1, 2)]} -> "
              f"R=pos{ranked[0]} G=pos{ranked[1]} B=pos{ranked[2]}")
        zone_perms = [perm]
    elif args.channel_order == "brg":
        # Swapped order (B,R,G at pos0,1,2) — what you get without marker
        # alignment; kept for A/B comparison only.
        zone_perms = [{"r": "g", "g": "b", "b": "r"}]
        print("channel-order: swapped B,R,G (comparison only)")
    else:  # "fixed" (default) — marker-aligned pos0=R, pos1=G, pos2=B
        zone_perms = [None]

    if args.register:
        if args.reg_leads:
            g, b = (int(v) for v in args.reg_leads.split(","))
            leads = {"r": 0, "g": g, "b": b}
            print(f"register: forced trilinear leads R=0 G={g} B={b}")
        else:
            measured = measure_leads(chans, 0, width, perm=zone_perms[0])
            leads, ok = validated_leads(measured)
            if ok:
                print(f"register: trilinear leads R=0 G={leads['g']} B={leads['b']}")
            else:
                print(f"register: measured leads G={measured['g']} B={measured['b']}"
                      f" implausible (low detail?); using fixed G={leads['g']} "
                      f"B={leads['b']}")
        leads_per_zone = [leads]
    else:
        leads_per_zone = [{"r": 0, "g": 0, "b": 0}]

    rgb = register_zones(chans, zones, leads_per_zone, correct_seam=False,
                         zone_perms=zone_perms)  # (lines, width, 3)
    order_idx = {"r": 0, "g": 1, "b": 2}
    perm = [order_idx[c] for c in args.order.lower()]
    if perm != [0, 1, 2]:
        rgb = rgb[:, :, perm]

    if args.autocrop:
        rgb, (r0, r1, c0, c1) = autocrop(rgb)
        print(f"autocrop: rows {r0}-{r1}, cols {c0}-{c1} -> {rgb.shape[1]}x{rgb.shape[0]}")
    if args.crop_width:
        rgb = rgb[:, :args.crop_width]
    if args.invert:
        rgb = rgb.max() - rgb

    # OEM C-41 inversion: measure the film base (Dmin) once on the whole ribbon
    # so every frame inverts against the same white point, then apply per-frame.
    c41 = None
    if args.invert_c41:
        base = measure_dmin(rgb, pct=args.c41_dmin_pct)
        c41 = (base, _c41_lut())
        print(f"C-41 invert: Dmin (film base) R,G,B = "
              f"{np.round(base).astype(int).tolist()}, matrix={args.c41_matrix}, "
              f"sRGB={'no' if args.c41_no_srgb else 'yes'}")

    rot = (args.rotate // 90) % 4

    # Auto-detected frame grid (count never forced; see find_frame_grid).
    cut_rows, pitch = find_frame_grid(rgb, args.frames)
    fr = max(0, len(cut_rows) - 1)
    # Trust the detected boundaries; crop a FIXED width (3000 px, capped at the
    # pitch) centred between each pair of gaps so the leftover splits evenly as
    # edge margin and the rebate stays off both edges. Variable per-gap trimming
    # is unreliable, so everything is the same width.
    frame_w = min(3000, pitch)
    if fr <= 1:
        cut_rows, fr, frame_w = [0, rgb.shape[0]], 1, None
        print("single frame")
    else:
        print(f"frame grid: pitch={pitch} rows, {fr} frames")

    for i in range(fr):
        if frame_w is None:
            r_start, r_end = cut_rows[i], cut_rows[i + 1]
        else:
            centre = (cut_rows[i] + cut_rows[i + 1]) // 2
            r_start = max(0, centre - frame_w // 2)
            r_end = min(rgb.shape[0], r_start + frame_w)
            r_start = max(0, r_end - frame_w)
        part = rgb[r_start:r_end]
        if c41 is not None:
            part = invert_c41(part, c41[0], c41[1], matrix=args.c41_matrix,
                              wb=args.c41_wb, srgb=not args.c41_no_srgb)
        if rot:
            part = np.rot90(part, k=rot)
        part = np.ascontiguousarray(part)
        suffix = f"_{i+1}" if fr > 1 else ""
        tif = f"{args.out}{suffix}.tif"
        write_tiff(part, tif, resize=resize)
        w_out = resize[0] if resize else part.shape[1]
        h_out = resize[1] if resize else part.shape[0]
        print(f"wrote {tif}  ({w_out}x{h_out}, 16-bit RGB"
              f"{' resampled' if resize else ''}"
              f"{' C-41 positive' if c41 is not None else (' inverted' if args.invert else ' raw/negative')})")
        if args.jpeg:
            from PIL import Image
            rendered = render_jpeg(part, rpd_path, gamma=args.jpeg_gamma)
            im = Image.fromarray(rendered, "RGB")
            if resize:
                im = im.resize(resize, Image.LANCZOS)
            jpg = f"{args.out}{suffix}.jpg"
            im.save(jpg, quality=args.jpeg_quality)
            print(f"wrote {jpg}  ({im.width}x{im.height}, rendered JPEG: "
                  f"RPD profile + scene-balance + highlight roll-off)")


if __name__ == "__main__":
    main()
