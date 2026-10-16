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
3000x2000`. Frames are found the way the OEM software does it (TLB.dll
picture framing, `tlx_frame_grid`), in the picture format `--format` names.

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


# --- OEM F-135 colour path (TLB.dll FN_bLoadImageFromBuffer, docs/IMAGING.md) --
# raw (open-gate calibrated, ~300 black) -> 14-bit -> the ColNeg density LUT ->
# the unit's own 3x10 NegMatrix from the EEPROM (density -> 12-bit RPD) ->
# Kodak rpd.pf (an input profile: RPD -> Lab) -> sRGB. No auto-levels and no
# per-channel stretch: those were what oversaturated the old --jpeg render.

_RAW_BLACK = 300.0   # dark level the light calibration targets


# --- Per-column flat field (TLB.dll FUN_1001f550 + the per-line MMX pass) ----
# The OEM corrects every scan line per column: (raw - dark[c]) * gain[c], with
# gain[c] = 64000 / ((bright[c] - dark[c]) - (black drift)) from 128 dark and
# 128 open-gate calibration lines, so every column reads 64000 through the
# empty gate. Without it the illumination falls off toward the sensor ends
# (on serial 3054 to ~25% at scan column 0), which inverts to a bright band
# along one edge of every frame. pakon_replay --light-cal / --scan-code write
# the tables as JSON; this applies them in the raw 16-bit domain, keeping the
# ~300 black, so the rest of the pipeline is unchanged.

def find_flat_field(raw_path, eeprom_dir=None):
    """The flat-field file for a raw: its own `<raw>.flat.json` (written by
    pakon_replay --scan-code), else `<eeprom_dir>/flatfield.json` (written by
    pakon_replay --light-cal). None if neither exists."""
    for p in ([f"{raw_path}.flat.json"] +
              ([os.path.join(str(eeprom_dir), "flatfield.json")] if eeprom_dir else [])):
        if os.path.isfile(p):
            return p
    return None


def load_flat_field(path, width):
    """(dark, gain), each (width, 3) float32, for scan columns 0..width-1 from
    a flat-field JSON. Raises ValueError if the file does not cover `width`
    columns (another resolution) or is malformed.

    A slide scan runs its LEDs brighter than the calibration (pakon_replay
    gives slides a film-base density of 0.12, so ~1.32x); the gain is scaled
    by calibration duty / scan duty per channel so the film comes out at its
    true transmission. Other films keep the OEM behaviour (their extra light
    is part of what the NegMatrix expects)."""
    import json
    with open(path) as f:
        ff = json.load(f)
    lp, px0, nb = int(ff["line_px"]), int(ff["px0"]), int(ff["black_px"])
    if lp - px0 != width:
        raise ValueError(f"{path}: covers {lp - px0} columns, the scan has {width}")
    dark = np.asarray(ff["dark"], np.float64)[:3]       # R, G, B; (3, line_px)
    bright = np.asarray(ff["bright"], np.float64)[:3]
    if dark.shape != (3, lp) or bright.shape != (3, lp):
        raise ValueError(f"{path}: tables are not 3 x {lp}")
    drift = bright[:, :nb].mean(1, keepdims=True) - dark[:, :nb].mean(1, keepdims=True)
    den = (bright - dark) - drift
    gain = np.where(den > 0, 64000.0 / np.maximum(den, 1e-9), 0.0)
    gain = np.minimum(gain, 0x3FFFF / 65536.0)          # the OEM 16.16 table cap
    if ff.get("film") == "slide" and "scan_duties" in ff:
        cal = np.asarray(ff["duties"][:3], np.float64)
        scan = np.asarray(ff["scan_duties"][:3], np.float64)
        if np.all(cal > 0) and np.all(scan > 0):
            gain = gain * (cal / scan)[:, None]
    return (dark[:, px0:].T.astype(np.float32),
            gain[:, px0:].T.astype(np.float32))


def apply_flat_field(rgb, flat, chunk=2048):
    """In place on (rows, width, 3) uint16: black + (raw - dark[c]) * gain[c],
    clipped to 0..65535. Returns rgb."""
    dark, gain = flat
    for r0 in range(0, rgb.shape[0], chunk):
        x = rgb[r0:r0 + chunk].astype(np.float32)
        x -= dark
        x *= gain
        x += _RAW_BLACK
        np.clip(x, 0.0, 65535.0, out=x)
        rgb[r0:r0 + chunk] = (x + 0.5).astype(np.uint16)
    return rgb


def _load_matrix(eeprom_dir, key):
    import importlib.util
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location(
        "pakon_eeprom", os.path.join(here, "pakon_eeprom.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    report = mod.read_archive(eeprom_dir)
    m = report.get("decoded", {}).get(key)
    if not report.get("decodable") or m is None:
        raise ValueError(f"no usable {key} in {eeprom_dir}")
    return np.array(m, dtype=np.float32)


def load_neg_matrix(eeprom_dir):
    """The unit's 3x10 NegMatrix (rows R, G, B) from an EEPROM archive made by
    tools/pakon_eeprom.py backup. Raises ValueError if it does not decode."""
    return _load_matrix(eeprom_dir, "negative_matrix")


# The PosMatrix every unit seen so far carries (serials 2233, 3054, 16402):
# 0.25 on the diagonal, nothing else. Used for slides when no EEPROM archive
# is at hand.
POS_MATRIX_DEFAULT = np.hstack([0.25 * np.eye(3, dtype=np.float32),
                                np.zeros((3, 7), np.float32)])


def load_pos_matrix(eeprom_dir=None):
    """The unit's 3x10 PosMatrix (slides) from an EEPROM archive, or the
    factory default when `eeprom_dir` is None."""
    if eeprom_dir is None:
        return POS_MATRIX_DEFAULT.copy()
    return _load_matrix(eeprom_dir, "positive_matrix")


def oem_rpd(rgb16, negmat, lut, exact=False):
    """Raw 16-bit RGB -> 12-bit RPD (float32, 0..4095) the way TLB.dll does it:
    14-bit, the density LUT, then the 3x10 polynomial
    m0 R + m1 G + m2 B + m3 R^2 + m4 G^2 + m5 B^2 + m6 RG + m7 BR + m8 GB + m9.

    exact=True computes the same density in floating point instead of through
    the 14-bit table (for slides: at D 3 one 14-bit step is ~0.026 D, which
    bands the shadows)."""
    if exact:
        x = np.clip((rgb16.astype(np.float32) - _RAW_BLACK) / 4.0, 0.25, 16383.0)
        d = (3500.0 * np.log10(16383.0 / x)).astype(np.float32)
    else:
        x = np.clip((rgb16.astype(np.float32) - _RAW_BLACK) / 4.0, 1.0, 16383.0)
        d = lut[x.astype(np.uint16)]
    r, g, b = d[..., 0], d[..., 1], d[..., 2]
    out = np.empty(d.shape, dtype=np.float32)
    for c in range(3):
        m = negmat[c]
        out[..., c] = (m[0] * r + m[1] * g + m[2] * b + m[3] * r * r +
                       m[4] * g * g + m[5] * b * b + m[6] * r * g +
                       m[7] * b * r + m[8] * g * b + m[9])
    return np.clip(out, 0.0, 4095.0)


class RpdLut:
    """rpd.pf (RPD -> Lab) to sRGB as a 3-D table on exact 8-bit nodes (every
    5th code, 52 per axis), applied with trilinear interpolation so 16-bit
    data keeps its precision (Pillow's colour management is 8-bit only)."""

    def __init__(self, rpd_path):
        from PIL import Image, ImageCms
        nodes = np.arange(0, 256, 5, dtype=np.uint8)
        self.n = len(nodes)
        grid = np.stack(np.meshgrid(nodes, nodes, nodes, indexing="ij"), -1)
        img = Image.fromarray(grid.reshape(1, -1, 3), "RGB")
        tr = ImageCms.buildTransform(ImageCms.getOpenProfile(rpd_path),
                                     ImageCms.createProfile("sRGB"),
                                     "RGB", "RGB", renderingIntent=0)
        out = np.asarray(ImageCms.applyTransform(img, tr), dtype=np.float32)
        self.table = out.reshape(self.n, self.n, self.n, 3) / 255.0

    def apply(self, x01, chunk_rows=256):
        """(H, W, 3) float in [0, 1] -> sRGB float in [0, 1]."""
        return _interp3(self.table, x01, chunk_rows)


def _interp3(t, x01, chunk_rows=256):
    """Trilinear lookup of (H, W, 3) values in [0, 1] in an (n, n, n, 3)
    table whose nodes span [0, 1] evenly."""
    n = t.shape[0]
    out = np.empty(x01.shape, dtype=np.float32)
    for r0 in range(0, x01.shape[0], chunk_rows):
        f = np.clip(x01[r0:r0 + chunk_rows], 0.0, 1.0) * (n - 1)
        i = np.minimum(f.astype(np.int32), n - 2)
        w = f - i
        acc = 0.0
        for dr in (0, 1):
            wr = w[..., 0] if dr else 1.0 - w[..., 0]
            for dg in (0, 1):
                wg = w[..., 1] if dg else 1.0 - w[..., 1]
                for db in (0, 1):
                    wb = w[..., 2] if db else 1.0 - w[..., 2]
                    acc = acc + (wr * wg * wb)[..., None] * t[
                        i[..., 0] + dr, i[..., 1] + dg, i[..., 2] + db]
        out[r0:r0 + chunk_rows] = acc
    return out


class ColRevLink:
    """ColRevLut1.pf, the OEM slide step: a Kodak device link ("Pakon CMY to
    ROMM12", ICC lut16: input curves, 31^3 grid, output curves) from the
    PosMatrix output to 12-bit ROMM RGB. TLB.dll hands it to PakonIMAu.dll
    (PIBegin), and PIColorCorrectColRevPlanar applies it in place after the
    PosMatrix. Evaluated here straight from the tag, all in [0, 1]."""

    def __init__(self, path):
        import struct
        with open(path, "rb") as f:
            d = f.read()
        tags = {}
        for k in range(struct.unpack(">I", d[128:132])[0]):
            sig, off, _ = struct.unpack(">4sII", d[132 + 12 * k:144 + 12 * k])
            tags[sig] = off
        off = tags.get(b"A2B0")
        if off is None or d[off:off + 4] != b"mft2":
            raise ValueError(f"{path}: no lut16 A2B0 tag")
        ni, no, g = d[off + 8], d[off + 9], d[off + 10]
        if ni != 3 or no != 3:
            raise ValueError(f"{path}: expected a 3-in, 3-out link")
        nin, nout = struct.unpack(">HH", d[off + 48:off + 52])
        p = off + 52
        self.inp = np.frombuffer(d, ">u2", 3 * nin, p).reshape(3, nin) / 65535.0
        p += 6 * nin
        self.clut = (np.frombuffer(d, ">u2", g ** 3 * 3, p)
                     .reshape(g, g, g, 3) / 65535.0).astype(np.float32)
        p += 6 * g ** 3
        self.outp = np.frombuffer(d, ">u2", 3 * nout, p).reshape(3, nout) / 65535.0

    @staticmethod
    def _curves(tabs, x):
        n = tabs.shape[1]
        grid = np.linspace(0.0, 1.0, n)
        return np.stack([np.interp(x[..., c], grid, tabs[c]) for c in range(3)],
                        axis=-1).astype(np.float32)

    def apply(self, x01):
        """(H, W, 3) CMY in [0, 1] -> ROMM RGB in [0, 1]."""
        return self._curves(self.outp, _interp3(self.clut, self._curves(self.inp, x01)))


OEM_BALANCE = 0.5   # default roll-balance strength


def oem_roll_balance(rgb16, negmat, lut, strength=OEM_BALANCE):
    """Roll-level two-point balance, a stand-in for the OEM's Ansel roll
    balance (its SCP stage is a per-channel affine LUT). Per channel, in RPD:
    the film base (the clearest film, measure_dmin) stays where it is, and the
    roll's picture median moves `strength` of the way to the neutral mean of
    the three medians; tones between are scaled linearly, so a channel's
    contrast changes but black does not move. Returns (gain, offset), applied
    as rpd * gain + offset. Measured once on the whole ribbon (downsampled is
    fine). Picture pixels: the darkest 20% (clear film, rebate) and clipped
    pixels are left out.

    Chosen by eye on two rolls (serial 3054, 2026-10-09): a plain offset left
    blue shadows; making the film base neutral too pushed one roll blue and
    the other cream (rpd.pf seems to expect an unbalanced base), so the black
    point is left alone; half strength for the midtone."""
    p = oem_rpd(rgb16, negmat, lut).reshape(-1, 3)
    lum = p.mean(axis=1)
    keep = (lum > np.percentile(lum, 20)) & (p.max(axis=1) < 4000)
    if keep.sum() < 100:
        return np.ones(3, np.float32), np.zeros(3, np.float32)
    med = np.median(p[keep], axis=0)
    base = oem_rpd(measure_dmin(rgb16).reshape(1, 1, 3).astype(np.uint16),
                   negmat, lut)[0, 0]
    target = med + strength * (med.mean() - med)
    gain = (target - base) / np.maximum(med - base, 100.0)
    return gain.astype(np.float32), (base * (1.0 - gain)).astype(np.float32)


def render_oem(rgb16, negmat, lut, rpd_lut, balance=None, grey=False):
    """Raw 16-bit RGB frame -> OEM-path positive, 16-bit sRGB. `balance` is
    the (gain, offset) pair from oem_roll_balance (None = none).

    grey=True is silver B&W: TLB.dll runs B&W through the same NegMatrix as
    colour negative; the three RPD channels are then averaged (silver is
    neutral, so they carry the same picture and the mean halves the noise)
    and the neutral rendered through rpd.pf, giving the OEM tone curve. Returns
    (H, W) 16-bit grey instead of (H, W, 3)."""
    rpd = oem_rpd(rgb16, negmat, lut)
    if balance is not None:
        rpd = np.clip(rpd * balance[0] + balance[1], 0.0, 4095.0)
    if grey:
        rpd = np.repeat(rpd.mean(axis=-1, keepdims=True), 3, axis=-1)
    srgb = rpd_lut.apply(rpd / 4095.0)
    out = np.clip(srgb * 65535.0 + 0.5, 0.0, 65535.0).astype(np.uint16)
    return to_grey(out) if grey else out


SLIDE_FLARE = 0.5         # default share of the measured veil to remove
_SLIDE_FLOOR_D = 3.5      # density the black between slide frames should read
_SLIDE_CAP_D = 2.0        # a "gap" lighter than this is not black film


def find_slide_gaps(rgb, min_rows=80, k=3.0):
    """The black film between slide frames, as [(first_row, last_row), ...].

    A gap row is dark across the whole width, so even its brightest pixels
    (95th percentile over the central columns) stay within `k` times the
    level of the darkest rows of the strip; a dark scene almost always has
    something bright in the row. Runs shorter than `min_rows` are dropped
    (gaps are ~120-140 rows at Base 16; a dark detail inside a frame is
    shorter). The leader and tail can show up as runs at the ends."""
    cols = rgb.shape[1]
    lum = rgb[:, cols // 8:cols - cols // 8:5].astype(np.float32).mean(axis=2)
    p95 = np.percentile(lum - _RAW_BLACK, 95, axis=1)
    dark = p95 < k * max(float(np.percentile(p95, 1)), 1.0)
    edges = np.diff(np.concatenate([[0], dark.astype(np.int8), [0]]))
    starts, ends = np.where(edges == 1)[0], np.where(edges == -1)[0] - 1
    return [(int(a), int(b)) for a, b in zip(starts, ends) if b - a + 1 >= min_rows]


def measure_slide_flare(rgb, strength=SLIDE_FLARE, gaps=None):
    """Stray light on a slide strip, per channel, in raw counts above black.

    The black film between frames should read about D 3.5 (~20 counts); on
    serial 3054 it reads D 2.6-3 (70-170 counts), lifted by scattered light
    (and a small black-level offset), which flattens the shadows. Measures
    the median of the gap rows (find_slide_gaps) and returns `strength` x
    (that - the D 3.5 level), to be subtracted from the raw before the
    density step. Zeros when no gap is found; the level is capped at D 2
    (640 counts), so a strip with no real gap cannot crush the shadows."""
    if gaps is None:
        gaps = find_slide_gaps(rgb)
    if strength <= 0 or not gaps:
        return np.zeros(3, np.float32)
    cols = rgb.shape[1]
    c0, c1 = cols // 8, cols - cols // 8
    rows = np.concatenate([np.arange(a, b + 1) for a, b in gaps])
    level = np.median(rgb[rows, c0:c1:4].astype(np.float32).reshape(-1, 3),
                      axis=0) - _RAW_BLACK
    floor = 64000.0 * 10.0 ** -_SLIDE_FLOOR_D
    level = np.minimum(level, 64000.0 * 10.0 ** -_SLIDE_CAP_D)
    return (strength * np.maximum(level - floor, 0.0)).astype(np.float32)


def render_slide(rgb16, posmat, lut, link, romm_lut, flare=None):
    """Raw 16-bit RGB frame of slide film -> 16-bit sRGB, the OEM positive
    path (TLB.dll film type 2): the same density LUT, the unit's PosMatrix
    (0.25 * density, 875 codes per decade, no inversion), the ColRevLut1.pf
    link to 12-bit ROMM, then romm.pf (TLB.dll's "CRInputProfile") to sRGB.
    No scene balance: the OEM runs Ansel in a positive mode that is not
    modelled yet, and a slide is already the finished picture.

    Two departures from the OEM, both for the shadows: the density is
    computed in floating point (oem_rpd exact=True), and `flare` (per-channel
    raw counts from measure_slide_flare) is subtracted first."""
    x = rgb16.astype(np.float32)
    if flare is not None:
        x = x - np.asarray(flare, np.float32)
    cmy = oem_rpd(x, posmat, lut, exact=True) / 4095.0
    srgb = romm_lut.apply(link.apply(cmy))
    return np.clip(srgb * 65535.0 + 0.5, 0.0, 65535.0).astype(np.uint16)


PROFILES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                            "profiles")


def slide_path(eeprom_dir=None, profiles=PROFILES_DIR):
    """(PosMatrix, LUT, ColRevLink, romm.pf table) for render_slide. Without
    an EEPROM archive the factory PosMatrix is used."""
    return (load_pos_matrix(eeprom_dir), _c41_lut(),
            ColRevLink(os.path.join(profiles, "ColRevLut1.pf")),
            RpdLut(os.path.join(profiles, "romm.pf")))


def to_grey(rgb16):
    """(H, W, 3) 16-bit sRGB -> (H, W) 16-bit grey, Rec. 709 luma."""
    y = rgb16.astype(np.float32) @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    return np.clip(y + 0.5, 0.0, 65535.0).astype(np.uint16)


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


def write_tiff(rgb16, path, resize=None):
    """Write an (H,W,3) uint16 array as a 16-bit RGB TIFF via ImageMagick
    ((H,W) as a 16-bit grey TIFF). If resize=(W,H) is given, resample to
    exactly that size."""
    h, w = rgb16.shape[:2]
    fmt = "gray" if rgb16.ndim == 2 else "rgb"
    fd, tmp = tempfile.mkstemp(suffix="." + fmt)
    os.close(fd)
    rgb16.astype(">u2").tofile(tmp)              # big-endian interleaved RGB
    try:
        cmd = ["magick", "-size", f"{w}x{h}", "-depth", "16",
               "-endian", "msb", f"{fmt}:{tmp}"]
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


def autocrop(rgb, positive=False):
    """Trim the dark leader, the blank (no-film) pre/post-load scan, and the
    residual low-detail margins, returning (cropped, (r0, r1, c0, c1)).

    Operates on the already de-wrapped, IR-removed visible image (a single
    contiguous zone). Rows: a row is non-image if it's very dark (leader) OR
    bright *and* neutral (light through no film). We keep the largest contiguous
    run of image rows. After that, further trim uniform (low-detail) rows from
    both ends — this removes blank film base (orange mask, no exposure) that is
    neither dark nor bright-neutral. Cols: trim residual low-detail margins.

    positive=True (slide film): the gaps between frames are as dark as the
    leader, so dark rows are not cut (that would keep a single frame); the
    low-detail trim removes the leader instead.
    """
    full = float(rgb.max()) or 1.0
    sub = rgb[:, ::8, :].astype(np.float32)
    bright = sub.mean((1, 2))
    spread = sub.max(2).mean(1) - sub.min(2).mean(1)
    dark = (bright < 0.06 * full) & (not positive)
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


# OEM picture geometry per sensor width (TLB.dll FUN_10011a60): framing block
# (lines), picture length (lines), pitch (lines). docs/TLB_FINDINGS.md
# "Picture framing".
TLX_GEOMETRY = {2000: (8, 3000, 3206), 1500: (6, 2250, 2405), 1000: (4, 1500, 1603)}

# Picture formats: (length, pitch) as a fraction of the full-frame values.
# TLX has no presets for these; its operator types Width / UnitWidth into
# "Pre-Scan Framing Adjust". Panoramic (XPan) is the Width 5530 / UnitWidth
# 5736 XPan users enter there at Base 16; half frame is half a full frame
# [assumed, not yet checked on a real roll].
FRAME_FORMATS = {
    "full": (1.0, 1.0),
    "half": (0.5, 0.5),
    "pano": (5530 / 3000, 5736 / 3206),
}


def tlx_geometry(width, fmt="full"):
    """(block, length, pitch) in ribbon rows for a sensor width and picture
    format. A width with no OEM entry (e.g. trimmed by autocrop) takes the
    nearest resolution's."""
    block, length, pitch = TLX_GEOMETRY[min(TLX_GEOMETRY, key=lambda w: abs(w - width))]
    kl, kp = FRAME_FORMATS[fmt]
    return block, int(round(length * kl)), int(round(pitch * kp))


def tlx_profile(ribbon, block, positive=False):
    """FUN_10006870: one 8-bit (R+G+B)/3 value per block of lines, inverted
    for negatives so the clear gap is low and the picture high."""
    n = ribbon.shape[0] // block
    v = ribbon[:n * block:block].astype(np.float32).mean((1, 2)) / 257.0
    v = np.clip(v, 0, 255).astype(np.int32)
    return v if positive else 255 - v


def _tlx_threshold(prof):
    """FUN_10005d20 mode 0: the first significant histogram peak (the gap
    level) and a cut just above it."""
    h = np.bincount(prof, minlength=256)
    floor = len(prof) // 150
    peak = pk = 0
    for i in range(0xfa + 2):
        if pk > floor and h[i] < pk * 3 // 5:
            break
        if h[i] > pk:
            pk, peak = int(h[i]), i
    lo = peak
    while lo and h[lo] >= pk // 20:
        lo -= 1
    hi, top = peak, min(255, peak + 50)
    while hi < top and h[hi] >= pk // 10:
        hi += 1
    if (peak - lo) * 4 < hi - peak:
        t = peak
        while t < 255 and h[t] >= pk // 2:
            t += 1
        return t
    return hi + (hi - peak) // 2


def _tlx_runs(ones):
    d = np.diff(np.concatenate([[0], ones, [0]]))
    s, e = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
    return list(zip(s.tolist(), (e - s).tolist()))


def tlx_place(prof, plen, pitch):
    """FN_iFramePicturesSub + FN_iFramingFindPictures on a block profile.
    Returns picture starts in blocks (sorted), each `plen` blocks long."""
    lo, hi = plen * 95 // 100, plen * 115 // 100

    def good(t):
        return sum(lo < l < hi for _, l in _tlx_runs(prof > t))

    # Threshold sweep: +2 / -2 steps while the count of picture-length runs
    # does not drop; keep the best.
    t0 = _tlx_threshold(prof)
    best_n, best_t = good(t0), t0
    for step in (2, -2):
        t = t0
        while 1 <= t + step <= 0xff:
            t += step
            g = good(t)
            if g < best_n:
                break
            if g > best_n:
                best_n, best_t = g, t
    ones = (prof > best_t).astype(np.int32)
    n = len(ones)
    pics = {}                                     # slot (half pitch) -> start

    def put(p):
        pics.setdefault(p * 2 // pitch, p)

    # LookForNicePictures: whole pictures, and two pictures with a lost gap.
    # The excess length is split 1/3 before, 2/3 after.
    for s, l in _tlx_runs(ones):
        if lo < l < hi:
            put(s + (l - plen) // 3)
        elif lo + pitch < l < hi + pitch:
            put(s + (l - pitch - plen) // 3)
            put(s + pitch)
    if not pics:                                  # FramingBlindlyPlacePictures
        off = (pitch - plen) // 2
        return [off + k * pitch for k in range(max(0, (n - 4) // pitch))]

    # LookInBetweenEnds: evenly fill the pictures missing between two found.
    found = sorted(pics.values())
    for a, b in zip(found, found[1:]):
        span = b - a
        k = span // pitch
        if span - k * pitch < pitch // 4:
            k -= 1
        for j in range(1, k + 1):
            put(a + j * span // (k + 1))

    # LookAtEnd / LookAtBeginning: one pitch outward at a time; place the
    # picture where a plen window holds the most film (FUN_100064e0), stop
    # when there is no film left.
    gap = max(1, pitch - plen)
    cum = np.concatenate([[0], np.cumsum(ones)])

    def estimate(start):
        best = None
        for k in range(gap):
            p = start + k
            if p < 0 or p + plen > n:
                continue
            key = (int(cum[p + plen] - cum[p]), -abs(k - gap // 2))
            if best is None or key > best[0]:
                best = (key, p)
        if best is None or best[0][0] < plen // 2:
            return None
        return best[1]

    last = max(pics.values())
    while (q := estimate(last + plen + gap // 2)) is not None and q > last:
        put(q)
        last = q
    first = min(pics.values())
    while (q := estimate(first - pitch - gap // 2)) is not None and q < first:
        put(q)
        first = q
    return sorted(set(pics.values()))


def tlx_frame_grid(ribbon, positive=False, fmt="full"):
    """OEM (TLX) framing. Returns (starts, length, pitch) in ribbon rows:
    each picture is rows [start, start + length). `fmt` is a FRAME_FORMATS
    key."""
    block, length, pitch = tlx_geometry(ribbon.shape[1], fmt)
    starts = tlx_place(tlx_profile(ribbon, block, positive),
                       length // block, pitch // block)
    rows = ribbon.shape[0]
    starts = [min(max(0, s * block), max(0, rows - length)) for s in starts]
    return sorted(set(starts)), length, pitch


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
    ap.add_argument("--flat", default=None, metavar="FILE",
                    help="per-column flat field JSON (pakon_replay --light-cal / "
                         "--scan-code); default RAW.flat.json, else "
                         "EEPROM_DIR/flatfield.json when present")
    ap.add_argument("--no-flat", action="store_true",
                    help="skip the per-column flat field")
    ap.add_argument("--rotate", type=int, default=0, choices=[0, 90, 180, 270])
    ap.add_argument("--format", choices=sorted(FRAME_FORMATS), default="full",
                    help="picture format for the OEM framing: full, half (half "
                         "frame) or pano (XPan panoramic); default full")
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
    ap.add_argument("--bw", action="store_true",
                    help="silver B&W film: with --invert-c41, the same "
                         "inversion (NegMatrix with --eeprom-dir), then grey; "
                         "writes 16-bit grey TIFFs and grey JPEGs")
    ap.add_argument("--slide", action="store_true",
                    help="slide (colour reversal) film: no inversion; density "
                         "LUT -> PosMatrix (the unit's with --eeprom-dir, else "
                         "the factory 0.25) -> ColRevLut1.pf -> romm.pf -> sRGB")
    ap.add_argument("--slide-flare", type=float, default=SLIDE_FLARE,
                    help="with --slide: share of the stray light measured in "
                         "the black between frames to remove, 0 = none, 1 = "
                         f"all down to D 3.5 (default {SLIDE_FLARE:g})")
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
    ap.add_argument("--eeprom-dir", default=None,
                    help="EEPROM archive of the scanner that made the raw "
                         "(tools/pakon_eeprom.py backup). With --invert-c41, "
                         "inverts through the OEM F-135 colour path: density "
                         "LUT -> the unit's NegMatrix -> rpd.pf (TIFF and JPEG)")
    ap.add_argument("--oem-balance", type=float, default=0.5,
                    help="with --eeprom-dir: roll-balance strength, 0 = none, "
                         "1 = roll picture median fully neutral; the film base "
                         "is never moved (default 0.5)")
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
    if args.slide and (args.invert or args.invert_c41 or args.bw):
        sys.exit("--slide is a positive: no --invert, --invert-c41 or --bw")
    if args.bw and not args.invert_c41:
        sys.exit("--bw requires --invert-c41")
    if args.eeprom_dir and not (args.invert_c41 or args.slide):
        sys.exit("--eeprom-dir requires --invert-c41 or --slide")
    if (args.jpeg or args.eeprom_dir) and not args.slide:
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
    flat_path = None if args.no_flat else (
        args.flat or find_flat_field(args.raw, args.eeprom_dir))
    if flat_path:
        try:
            apply_flat_field(rgb, load_flat_field(flat_path, width))
            print(f"flat field: {flat_path}")
        except (OSError, ValueError, KeyError) as exc:
            if args.flat:
                sys.exit(f"--flat: {exc}")
            print(f"flat field: not applied ({exc})")
    elif not args.no_flat:
        print("flat field: none found (run pakon_replay --light-cal); the "
              "edge falloff is left in")
    order_idx = {"r": 0, "g": 1, "b": 2}
    perm = [order_idx[c] for c in args.order.lower()]
    if perm != [0, 1, 2]:
        rgb = rgb[:, :, perm]

    if args.autocrop:
        rgb, (r0, r1, c0, c1) = autocrop(rgb, positive=args.slide)
        print(f"autocrop: rows {r0}-{r1}, cols {c0}-{c1} -> {rgb.shape[1]}x{rgb.shape[0]}")
    if args.crop_width:
        rgb = rgb[:, :args.crop_width]
    if args.invert:
        rgb = rgb.max() - rgb

    # OEM C-41 inversion: measure the film base (Dmin) once on the whole ribbon
    # so every frame inverts against the same white point, then apply per-frame.
    c41 = None
    oem = None
    slide = None
    if args.slide:
        slide = slide_path(args.eeprom_dir)
        print("slide: PosMatrix " + (f"from {args.eeprom_dir}" if args.eeprom_dir
                                     else "factory default (0.25)")
              + ", ColRevLut1.pf, romm.pf")
    elif args.invert_c41 and args.eeprom_dir:
        negmat, lut = load_neg_matrix(args.eeprom_dir), _c41_lut()
        step = max(1, rgb.shape[0] // 4000)
        bal = oem_roll_balance(np.asarray(rgb[::step, ::4]), negmat, lut,
                               args.oem_balance)
        oem = (negmat, lut, RpdLut(rpd_path), bal)
        print(f"C-41 invert: OEM colour path (NegMatrix from {args.eeprom_dir}, "
              f"rpd.pf), roll balance {args.oem_balance:g}: RPD gains "
              f"{np.round(bal[0], 3).tolist()}")
    elif args.invert_c41:
        base = measure_dmin(rgb, pct=args.c41_dmin_pct)
        c41 = (base, _c41_lut())
        print(f"C-41 invert: Dmin (film base) R,G,B = "
              f"{np.round(base).astype(int).tolist()}, matrix={args.c41_matrix}, "
              f"sRGB={'no' if args.c41_no_srgb else 'yes'}")

    rot = (args.rotate // 90) % 4

    # OEM framing: fixed picture length and pitch per resolution, each crop
    # anchored at its picture's start (docs/TLB_FINDINGS.md "Picture framing").
    starts, frame_w, pitch = tlx_frame_grid(rgb, positive=args.slide, fmt=args.format)
    if len(starts) <= 1:
        starts, frame_w = [0], rgb.shape[0]
        print("single frame")
    else:
        print(f"frame grid: pitch={pitch} rows, {len(starts)} frames")
    flare = None
    if slide is not None:
        flare = measure_slide_flare(rgb, args.slide_flare)
        print(f"slide flare: R,G,B {[round(float(v), 1) for v in flare]} counts "
              f"(strength {args.slide_flare:g})")

    fr = len(starts)
    for i, r_start in enumerate(starts):
        part = rgb[r_start:r_start + frame_w]
        if slide is not None:
            part = render_slide(part, *slide, flare=flare)
        elif oem is not None:
            part = render_oem(part, *oem, grey=args.bw)
        elif c41 is not None:
            part = invert_c41(part, c41[0], c41[1], matrix=args.c41_matrix,
                              wb=args.c41_wb, srgb=not args.c41_no_srgb)
            if args.bw:
                part = to_grey(part)
        if rot:
            part = np.rot90(part, k=rot)
        part = np.ascontiguousarray(part)
        suffix = f"_{i+1}" if fr > 1 else ""
        tif = f"{args.out}{suffix}.tif"
        write_tiff(part, tif, resize=resize)
        w_out = resize[0] if resize else part.shape[1]
        h_out = resize[1] if resize else part.shape[0]
        print(f"wrote {tif}  ({w_out}x{h_out}, 16-bit {'grey' if part.ndim == 2 else 'RGB'}"
              f"{' resampled' if resize else ''}"
              f"{' slide' if slide is not None else ' C-41 positive' if (c41 or oem) is not None else (' inverted' if args.invert else ' raw/negative')})")
        if args.jpeg:
            from PIL import Image
            # The OEM-path and slide TIFFs are already rendered.
            if oem is not None or slide is not None or part.ndim == 2:
                rendered = (part >> 8).astype(np.uint8)
            else:
                rendered = render_jpeg(part, rpd_path, gamma=args.jpeg_gamma)
            im = Image.fromarray(rendered, "L" if rendered.ndim == 2 else "RGB")
            if resize:
                im = im.resize(resize, Image.LANCZOS)
            jpg = f"{args.out}{suffix}.jpg"
            im.save(jpg, quality=args.jpeg_quality)
            print(f"wrote {jpg}  ({im.width}x{im.height}, rendered JPEG: "
                  + ("slide path)" if slide is not None else
                     "OEM colour path)" if oem is not None else
                     "RPD profile + scene-balance + highlight roll-off)"))


if __name__ == "__main__":
    main()
