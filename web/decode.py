"""
Decode pipeline for the Pakon web service — minilab-style two-stage flow.

Stage 1 (prescan): build the long negative ribbon once, quick-invert a
downscaled copy into a single long PREVIEW strip, and auto-detect the initial
frame centres + pitch. The full-res ribbon is cached to disk (ribbon.npy).

Stage 2 (export): the user confirms/positions each crop in the browser; the
confirmed centres come back and we crop the cached ribbon at full resolution,
run the OEM C-41 inversion + rpd.pf render, and write per-frame TIFF/JPEG.

This mirrors a dedicated minilab: detect → operator confirms frames → export.
"""
import os
import sys
from pathlib import Path

import numpy as np
import tifffile
from PIL import Image

# Pull the decode functions from the CLI tool without installing it.
_TOOLS = Path(__file__).resolve().parent.parent / "tools"
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))

from pakon_image import (  # noqa: E402
    detect_row_layout,
    find_frame_grid,
    invert_c41,
    marker_align,
    measure_dmin,
    measure_leads,
    register_zones,
    render_jpeg,
    validated_leads,
    _c41_lut,
)

WORK_DIR = Path("/tmp/pakon_web")
# Row layout is auto-detected per raw (F-135 vs the F-135+ per-resolution
# strides); PAKON_LINEWIDTH + PAKON_IR_LANE=0/1 override the detection.
_LINEWIDTH_OVERRIDE = os.environ.get("PAKON_LINEWIDTH")
_IR_LANE_OVERRIDE = os.environ.get("PAKON_IR_LANE")
_REPO = Path(__file__).resolve().parent.parent
RPD_PROFILE = _REPO / "profiles" / "rpd.pf"
RIBBON_PATH = WORK_DIR / "ribbon.npy"
PREVIEW_PATH = WORK_DIR / "preview.jpg"
_PREVIEW_H = 300               # target short-axis (frame height) of the preview
_FRAME_W = 3000                # fixed crop width (long axis), capped at pitch


def _build_ribbon(raw_path, emit):
    """Marker-align rows → deinterleave (R,G,B triplets at stride 3, IR trailing)
    → trilinear registration. Returns the FULL assembled negative ribbon
    (rows=long axis, cols=width).

    Aligning to the per-scanline marker bit fixes the triplet phase regardless of
    where the 0x86 capture started, so the colour cast is consistent across scans
    (otherwise it flips purple/turquoise). IR is a single trailing line, so there
    is one visible zone — no wrap-split.

    Autocrop is intentionally NOT applied: it kept only the single largest run
    of image rows, which silently dropped every frame past an interior dark
    exposure / dense rebate (a 36-exp roll came out as ~13). Frames are now
    placed manually in the web UI, so we show the whole ribbon end to end."""
    emit("Loading", 0.0)
    raw = np.memmap(str(raw_path), dtype="<u2", mode="r")

    emit("Detecting row layout", 0.05)
    if _LINEWIDTH_OVERRIDE:
        lw = int(_LINEWIDTH_OVERRIDE)
        has_ir = (_IR_LANE_OVERRIDE != "0") if _IR_LANE_OVERRIDE is not None \
            else (lw == 8000)
    else:
        layout = detect_row_layout(raw)
        if layout is None:
            raise ValueError(
                "could not detect the row layout of this raw (no known stride "
                "matched); set PAKON_LINEWIDTH (+ PAKON_IR_LANE=0/1) to force")
        lw, has_ir = layout
    width = lw // 4 if has_ir else lw // 3
    emit(f"Layout: {lw} samples/row, {width} px "
         f"({'RGB+IR' if has_ir else 'RGB'})", 0.08)

    lines = raw.size // lw
    img = raw[: lines * lw].reshape(lines, lw)

    emit("Aligning rows", 0.10)
    img, _ = marker_align(img)
    nvis = width * 3
    chans = {"r": img[:, 0:nvis:3], "g": img[:, 1:nvis:3], "b": img[:, 2:nvis:3]}

    emit("Registering channels", 0.30)
    leads, _ = validated_leads(measure_leads(chans, 0, width))
    rgb = register_zones(chans, [(0, width)], [leads],
                         correct_seam=False, zone_perms=[None])
    return rgb


def prescan(raw_path, progress=None) -> dict:
    """Stage 1. Build + cache the ribbon, render the long preview strip, and
    auto-detect initial frame centres. Returns a metadata dict (all JSON-able)."""
    WORK_DIR.mkdir(parents=True, exist_ok=True)

    def emit(step, pct):
        if progress:
            progress(step, round(pct, 3))

    rgb = _build_ribbon(raw_path, emit)
    rows, cols = rgb.shape[:2]

    emit("Measuring film base", 0.62)
    base = measure_dmin(rgb, pct=99.5)

    emit("Detecting frames", 0.68)
    cut_rows, pitch = find_frame_grid(rgb)
    centres = [int((cut_rows[i] + cut_rows[i + 1]) // 2)
               for i in range(len(cut_rows) - 1)]
    if not centres:                       # fallback: single image
        centres = [rows // 2]
        pitch = rows

    emit("Caching ribbon", 0.75)
    np.save(str(RIBBON_PATH), rgb)        # full-res cache for export

    emit("Rendering preview", 0.85)
    ds = max(1, int(round(cols / _PREVIEW_H)))   # ~_PREVIEW_H px tall frames
    small = np.ascontiguousarray(rgb[::ds, ::ds])
    pos = invert_c41(small, base, _c41_lut())          # quick sRGB positive
    strip = np.rot90((pos >> 8).astype(np.uint8), k=1)  # long axis -> horizontal x
    Image.fromarray(strip).save(str(PREVIEW_PATH), "JPEG", quality=85)
    prev_w, prev_h = strip.shape[1], strip.shape[0]     # PIL (w,h)
    scale = prev_w / rows                                # full_row -> preview_x

    emit("Done", 1.0)
    return {
        "ribbon": str(RIBBON_PATH),
        "base": [float(x) for x in base],
        "rows": int(rows), "cols": int(cols),
        "pitch": int(pitch),
        "frame_w": int(min(_FRAME_W, pitch)),
        "centres": centres,                # initial detected centres (full rows)
        "preview_w": int(prev_w), "preview_h": int(prev_h),
        "scale": float(scale),             # preview_x = full_row * scale
    }


def export_frames(ribbon_path, base, centres, widths=None, rotate=90,
                  frame_w=_FRAME_W, progress=None) -> list[dict]:
    """Stage 2. Crop the cached ribbon at each confirmed centre, run the full
    C-41 inversion + rpd.pf render, write TIFF/JPEG.

    `widths` is an optional per-frame crop width (full-res rows) parallel to
    `centres` — lets the operator mix full- and half-frame boxes. When omitted,
    every crop uses `frame_w`."""
    WORK_DIR.mkdir(parents=True, exist_ok=True)

    def emit(step, pct):
        if progress:
            progress(step, round(pct, 3))

    rgb = np.load(str(ribbon_path), mmap_mode="r")
    rows = rgb.shape[0]
    base = np.asarray(base, dtype=np.float32)
    lut = _c41_lut()
    use_rpd = RPD_PROFILE.exists()
    rot_k = (rotate // 90) % 4
    frames = []
    n = len(centres)

    for i, centre in enumerate(centres):
        emit(f"Exporting frame {i + 1}/{n}", (i + 1) / max(1, n))
        fw = int(widths[i]) if widths else frame_w
        half = fw // 2
        r0 = max(0, int(centre) - half)
        r1 = min(rows, r0 + fw)
        r0 = max(0, r1 - fw)
        part = np.ascontiguousarray(rgb[r0:r1])
        if rot_k:
            part = np.ascontiguousarray(np.rot90(part, k=rot_k))

        pos = invert_c41(part, base, lut)             # 16-bit sRGB positive
        rendered = (render_jpeg(pos, str(RPD_PROFILE)) if use_rpd
                    else (pos >> 8).astype(np.uint8))

        raw_tiff = WORK_DIR / f"frame_{i + 1:02d}_raw.tif"
        pos_tiff = WORK_DIR / f"frame_{i + 1:02d}.tif"
        jpg_path = WORK_DIR / f"frame_{i + 1:02d}.jpg"
        thumb_path = WORK_DIR / f"thumb_{i + 1:02d}.jpg"
        # raw negative (uninverted, 16-bit) — for inverting in another tool
        tifffile.imwrite(str(raw_tiff), part, photometric="rgb")
        tifffile.imwrite(str(pos_tiff), pos, photometric="rgb")
        Image.fromarray(rendered).save(str(jpg_path), "JPEG", quality=92)
        step = max(1, max(rendered.shape[0] // 600, rendered.shape[1] // 600))
        Image.fromarray(rendered[::step, ::step]).save(str(thumb_path),
                                                       "JPEG", quality=85)
        frames.append({"index": i + 1, "raw_tiff": raw_tiff, "tiff": pos_tiff,
                       "jpg": jpg_path, "thumb": thumb_path})

    emit("Done", 1.0)
    return frames


def make_contact_sheet(frames, cols: int = 6, cell_w: int = 480,
                       pad: int = 10, bg=(22, 22, 22), fg=(226, 226, 226)):
    """Tile the rendered frame JPEGs into one labelled contact-sheet image."""
    from PIL import ImageDraw, ImageFont

    items = [f for f in frames if f.get("jpg") and Path(f["jpg"]).exists()]
    if not items:
        return None
    with Image.open(items[0]["jpg"]) as im0:
        aspect = im0.height / im0.width
    cell_h = int(cell_w * aspect)
    label_h = 26
    rows = (len(items) + cols - 1) // cols
    W = cols * cell_w + (cols + 1) * pad
    H = rows * (cell_h + label_h) + (rows + 1) * pad
    sheet = Image.new("RGB", (W, H), bg)
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype(
            "/System/Library/Fonts/Supplemental/Arial.ttf", 16)
    except OSError:
        font = ImageFont.load_default()
    for k, f in enumerate(items):
        r, c = divmod(k, cols)
        x = pad + c * (cell_w + pad)
        y = pad + r * (cell_h + label_h + pad)
        with Image.open(f["jpg"]) as im:
            thumb = im.convert("RGB").resize((cell_w, cell_h), Image.LANCZOS)
        sheet.paste(thumb, (x, y))
        draw.text((x + 2, y + cell_h + 4), f"Frame {f['index']:02d}",
                  fill=fg, font=font)
    return sheet
