"""
Pakon Scanner web service.

Run from the repo root:
    uvicorn web.app:app --host 0.0.0.0 --port 8000

The server must be run with USB access (root on Linux, or with a udev rule
granting access to 0f05:f135 / 0f05:f235).  On macOS no privileges are
needed.

Endpoints
---------
GET  /api/status                  scanner state + current job info
POST /api/firmware                SSE — load firmware (cold device)
POST /api/scan                    SSE — run a scan
POST /api/process                 SSE — decode .raw (upload or last scan)
GET  /api/frames                  list decoded frames
GET  /api/frames/{n}/thumb        JPEG thumbnail
GET  /api/frames/{n}/tiff         16-bit TIFF download
GET  /api/export                  zip of all frames (fmt=raw|tiff|jpeg)
GET  /api/contact                 JPEG contact sheet of all frames
"""
import asyncio
import json
import os
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from fastapi import Body, FastAPI, File, Form, UploadFile
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles

from .decode import (WORK_DIR, PREVIEW_PATH, prescan, export_frames,
                     make_contact_sheet)

# ── Paths ─────────────────────────────────────────────────────────────────────

_REPO = Path(__file__).resolve().parent.parent
_BUILD = Path(os.environ.get("PAKON_BUILD", str(_REPO / "build")))
_RES = Path(os.environ.get("PAKON_RESOURCES", str(_REPO / "resources")))

PROBE_BIN  = _BUILD / "pakon_probe"
REPLAY_BIN = _BUILD / "pakon_replay"
PAKFW      = _RES / "f135.pakfw"      # same FX2 image for F-135 and F-135+
# The scan script is model-specific (the two models' PICs live at different
# bus addresses); the model is detected at scan time via pakon_replay --open.
# PAKON_PAKSCAN overrides the choice with an explicit script path.
PAKSCAN_F135     = _RES / "36frames.pakscan"
PAKSCAN_F135PLUS = _RES / "f135plus" / "base16.pakscan"
PAKSCAN_OVERRIDE = os.environ.get("PAKON_PAKSCAN")

# ── App & state ───────────────────────────────────────────────────────────────

app = FastAPI(title="Pakon Scanner")

_executor = ThreadPoolExecutor(max_workers=1)
_state: dict = {
    "scan_path": None,   # Path to the last .raw file
    "out_dir": Path.home(), # host dir the next scan writes scan.raw into
    "frames": [],        # list of {"index", "tiff", "thumb"} (after export)
    "prescan": None,     # prescan() metadata (ribbon path, base, pitch, centres…)
    "processing": False,
}


# ── Scanner detection ─────────────────────────────────────────────────────────

def _scanner_state() -> str:
    try:
        import usb.core  # pyusb
        if usb.core.find(idVendor=0x0F05, idProduct=0xF135) is not None:
            return "warm"
        if usb.core.find(idVendor=0x0F05, idProduct=0xF235) is not None:
            return "cold"
        return "disconnected"
    except Exception:
        return "unknown"


# ── SSE helper ────────────────────────────────────────────────────────────────

def _sse(event: dict) -> str:
    return f"data: {json.dumps(event)}\n\n"


_SSE_HEADERS = {"Cache-Control": "no-cache", "X-Accel-Buffering": "no"}


# ── Status ────────────────────────────────────────────────────────────────────

@app.get("/api/status")
async def api_status():
    sp = _state["scan_path"]
    return {
        "state": _scanner_state(),
        "scan_file": sp.name if sp else None,
        "scan_bytes": sp.stat().st_size if sp and sp.exists() else 0,
        "out_dir": str(_state["out_dir"]),
        "frame_count": len(_state["frames"]),
        "processing": _state["processing"],
    }


# ── Firmware load ─────────────────────────────────────────────────────────────

async def _firmware_stream():
    for path, label in [(PROBE_BIN, "pakon_probe binary"), (PAKFW, "firmware file")]:
        if not path.exists():
            yield _sse({"type": "error", "message": f"{label} not found: {path}"})
            return

    proc = await asyncio.create_subprocess_exec(
        str(PROBE_BIN), "--load-firmware", str(PAKFW),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    async for line in proc.stdout:
        yield _sse({"type": "log", "message": line.decode().rstrip()})
    await proc.wait()
    if proc.returncode == 0:
        yield _sse({"type": "done"})
    else:
        yield _sse({"type": "error",
                    "message": f"firmware load failed (exit {proc.returncode})"})


@app.post("/api/firmware")
async def api_firmware():
    return StreamingResponse(_firmware_stream(), media_type="text/event-stream",
                             headers=_SSE_HEADERS)


# ── Scan ──────────────────────────────────────────────────────────────────────

async def _detect_model() -> str | None:
    """Run pakon_replay --open and parse the detected model. Returns
    "F-135+"/"F-135", or None when detection fails (device cold/absent)."""
    proc = await asyncio.create_subprocess_exec(
        str(REPLAY_BIN), "--open",
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    output, _ = await proc.communicate()
    text = output.decode(errors="replace")
    if "model detected: F-135+" in text:
        return "F-135+"
    if "model detected: F-135 " in text:
        return "F-135"
    return None


async def _scan_stream(out_dir: Path):
    if not REPLAY_BIN.exists():
        yield _sse({"type": "error",
                    "message": f"pakon_replay binary not found: {REPLAY_BIN}"})
        return

    if PAKSCAN_OVERRIDE:
        pakscan = Path(PAKSCAN_OVERRIDE)
        yield _sse({"type": "log",
                    "message": f"Using PAKON_PAKSCAN override: {pakscan}"})
    else:
        yield _sse({"type": "log", "message": "Detecting scanner model..."})
        model = await _detect_model()
        if model is None:
            yield _sse({"type": "error",
                        "message": "could not detect the scanner model (is it "
                                   "connected and warm?); set PAKON_PAKSCAN to "
                                   "force a scan script"})
            return
        pakscan = PAKSCAN_F135PLUS if model == "F-135+" else PAKSCAN_F135
        yield _sse({"type": "log",
                    "message": f"Model: {model} — scan script {pakscan.name}"})

    if not pakscan.exists():
        yield _sse({"type": "error",
                    "message": f"scan script not found: {pakscan}"})
        return

    try:
        out_dir.mkdir(parents=True, exist_ok=True)
    except Exception as exc:
        yield _sse({"type": "error",
                    "message": f"cannot create output dir {out_dir}: {exc}"})
        return
    if not os.access(out_dir, os.W_OK):
        yield _sse({"type": "error",
                    "message": f"output dir not writable: {out_dir}"})
        return

    out_path = out_dir / "scan.raw"
    _state["scan_path"] = out_path
    _state["out_dir"] = out_dir
    _state["frames"] = []

    # Remove stale output so size polling starts from 0.
    if out_path.exists():
        out_path.unlink()

    proc = await asyncio.create_subprocess_exec(
        str(REPLAY_BIN), "--scan", str(pakscan), "--image", str(out_path),
        "--autostop",
        stdout=asyncio.subprocess.DEVNULL,
        stderr=asyncio.subprocess.DEVNULL,
    )

    while True:
        done = False
        try:
            await asyncio.wait_for(proc.wait(), timeout=0.5)
            done = True
        except asyncio.TimeoutError:
            pass

        size = out_path.stat().st_size if out_path.exists() else 0
        mb = round(size / 1_048_576, 1)
        yield _sse({"type": "progress", "bytes": size, "mb": mb})
        if done:
            break

    size = out_path.stat().st_size if out_path.exists() else 0
    mb = round(size / 1_048_576, 1)
    if proc.returncode == 0:
        yield _sse({"type": "done", "bytes": size, "mb": mb, "path": str(out_path)})
    else:
        yield _sse({"type": "error",
                    "message": f"scan failed (exit {proc.returncode})"})


@app.post("/api/scan")
async def api_scan(out_dir: str | None = Body(default=None, embed=True)):
    target = Path(out_dir).expanduser() if out_dir else Path.home()
    return StreamingResponse(_scan_stream(target), media_type="text/event-stream",
                             headers=_SSE_HEADERS)


# ── Host directory browser (scan output location) ──────────────────────────────

@app.get("/api/browse")
async def api_browse(path: str | None = None):
    """List subdirectories of a host directory, for picking a scan output dir."""
    base = Path(path).expanduser() if path else Path.home()
    try:
        base = base.resolve()
        if not base.is_dir():
            base = Path.home().resolve()
    except Exception:
        base = Path.home().resolve()

    try:
        dirs = sorted(
            (p.name for p in base.iterdir()
             if p.is_dir() and not p.name.startswith(".")),
            key=str.lower,
        )
    except PermissionError:
        dirs = []

    parent = str(base.parent) if base.parent != base else None
    return {"path": str(base), "parent": parent, "dirs": dirs}


@app.post("/api/mkdir")
async def api_mkdir(path: str = Body(...), name: str = Body(...)):
    """Create a new subdirectory under `path` on the host."""
    base = Path(path).expanduser()
    if not base.is_dir():
        return JSONResponse({"error": f"not a directory: {base}"}, status_code=400)
    name = name.strip()
    if not name or "/" in name or name in (".", ".."):
        return JSONResponse({"error": "invalid folder name"}, status_code=400)
    new = base / name
    try:
        new.mkdir(exist_ok=False)
    except FileExistsError:
        return JSONResponse({"error": "folder already exists"}, status_code=400)
    except Exception as exc:
        return JSONResponse({"error": str(exc)}, status_code=400)
    return {"path": str(new)}


# ── Stage 1: prescan (long preview + detected frame centres) ───────────────────

def _run_job(work, on_done):
    """Run `work(progress)` in the executor, stream progress as SSE, and call
    on_done(result) to build the final 'done' event. Returns a StreamingResponse."""
    loop = asyncio.get_event_loop()
    queue: asyncio.Queue = asyncio.Queue()

    def _progress(step: str, pct: float) -> None:
        loop.call_soon_threadsafe(
            queue.put_nowait, {"type": "progress", "step": step, "pct": pct})

    def _runner() -> None:
        try:
            result = work(_progress)
            loop.call_soon_threadsafe(queue.put_nowait, on_done(result))
        except Exception as exc:
            loop.call_soon_threadsafe(
                queue.put_nowait, {"type": "error", "message": str(exc)})

    _executor.submit(_runner)

    async def _stream():
        try:
            while True:
                event = await queue.get()
                yield _sse(event)
                if event["type"] in ("done", "error"):
                    break
        finally:
            _state["processing"] = False

    return StreamingResponse(_stream(), media_type="text/event-stream",
                             headers=_SSE_HEADERS)


@app.post("/api/process")
async def api_process(file: UploadFile | None = File(default=None)):
    """Prescan: cache the ribbon, render the preview strip, detect frame centres."""
    if _state["processing"]:
        async def _busy():
            yield _sse({"type": "error", "message": "Already processing"})
        return StreamingResponse(_busy(), media_type="text/event-stream",
                                 headers=_SSE_HEADERS)

    if file is not None:
        WORK_DIR.mkdir(parents=True, exist_ok=True)
        upload_path = WORK_DIR / "upload.raw"
        with open(upload_path, "wb") as f:
            while chunk := await file.read(1 << 16):
                f.write(chunk)
        raw_path = upload_path
        _state["scan_path"] = upload_path
    elif _state["scan_path"] and _state["scan_path"].exists():
        raw_path = _state["scan_path"]
    else:
        async def _no_file():
            yield _sse({"type": "error",
                        "message": "No raw file — run a scan or upload a .raw file"})
        return StreamingResponse(_no_file(), media_type="text/event-stream",
                                 headers=_SSE_HEADERS)

    _state["frames"] = []
    _state["prescan"] = None
    _state["processing"] = True

    def _done(meta):
        _state["prescan"] = meta
        # hand the browser everything it needs to place crops (no ribbon path)
        return {"type": "done", **{k: meta[k] for k in (
            "rows", "cols", "pitch", "frame_w", "centres",
            "preview_w", "preview_h", "scale")}}

    return _run_job(lambda prog: prescan(raw_path, progress=prog), _done)


@app.get("/api/preview")
async def api_preview():
    if not PREVIEW_PATH.exists():
        return JSONResponse({"error": "no preview"}, status_code=404)
    return FileResponse(PREVIEW_PATH, media_type="image/jpeg")


# ── Stage 2: export the operator-confirmed crops at full resolution ─────────────

@app.post("/api/export_frames")
async def api_export_frames(
    centres: list[int] = Body(..., embed=True),
    widths: list[int] | None = Body(default=None, embed=True),
    rotate: int = Body(default=90, embed=True),
):
    meta = _state["prescan"]
    if not meta:
        async def _no_scan():
            yield _sse({"type": "error", "message": "Run a scan/process first"})
        return StreamingResponse(_no_scan(), media_type="text/event-stream",
                                 headers=_SSE_HEADERS)
    if _state["processing"]:
        async def _busy():
            yield _sse({"type": "error", "message": "Already processing"})
        return StreamingResponse(_busy(), media_type="text/event-stream",
                                 headers=_SSE_HEADERS)

    _state["frames"] = []
    _state["processing"] = True

    def _work(prog):
        return export_frames(meta["ribbon"], meta["base"], centres,
                             widths=widths, rotate=rotate,
                             frame_w=meta["frame_w"], progress=prog)

    def _done(result):
        _state["frames"] = result
        return {"type": "done", "count": len(result)}

    return _run_job(_work, _done)


# ── Frame access ──────────────────────────────────────────────────────────────

@app.get("/api/frames")
async def api_frames():
    return [{"index": f["index"]} for f in _state["frames"]]


def _frame_file(n: int, key: str):
    """Return the Path for frame n's artifact `key`, or None if missing."""
    match = next((f for f in _state["frames"] if f["index"] == n), None)
    if not match:
        return None
    path = match.get(key)
    return path if path and path.exists() else None


@app.get("/api/frames/{n}/thumb")
async def api_thumb(n: int):
    path = _frame_file(n, "thumb")
    if not path:
        return JSONResponse({"error": "not found"}, status_code=404)
    return FileResponse(path, media_type="image/jpeg")


@app.get("/api/frames/{n}/tiff")
async def api_tiff(n: int):
    """Inverted positive TIFF (16-bit)."""
    path = _frame_file(n, "tiff")
    if not path:
        return JSONResponse({"error": "not found"}, status_code=404)
    return FileResponse(path, media_type="image/tiff", filename=f"frame_{n:02d}.tif")


@app.get("/api/frames/{n}/raw")
async def api_raw(n: int):
    """Raw negative TIFF (16-bit, uninverted) — for inverting in another tool."""
    path = _frame_file(n, "raw_tiff")
    if not path:
        return JSONResponse({"error": "not found"}, status_code=404)
    return FileResponse(path, media_type="image/tiff",
                        filename=f"frame_{n:02d}_raw.tif")


@app.get("/api/frames/{n}/jpeg")
async def api_jpeg(n: int):
    """Inverted positive JPEG (8-bit)."""
    path = _frame_file(n, "jpg")
    if not path:
        return JSONResponse({"error": "not found"}, status_code=404)
    return FileResponse(path, media_type="image/jpeg", filename=f"frame_{n:02d}.jpg")


# ── Zip export ────────────────────────────────────────────────────────────────

# fmt -> (state key, file extension, archived name suffix)
_EXPORT_FMTS = {
    "raw":  ("raw_tiff", "tif", "_raw"),  # raw negative TIFF (16-bit, uninverted)
    "tiff": ("tiff",     "tif", ""),      # inverted positive TIFF (16-bit, plain)
    "jpeg": ("jpg",      "jpg", ""),      # rendered positive JPEG (rpd.pf look)
}


@app.get("/api/export")
async def api_export(fmt: str = "tiff"):
    frames = _state["frames"]
    if not frames:
        return JSONResponse({"error": "no frames to export"}, status_code=404)

    fmt = fmt.lower()
    if fmt not in _EXPORT_FMTS:
        return JSONResponse({"error": f"unknown format: {fmt}"}, status_code=400)
    state_key, ext, suffix = _EXPORT_FMTS[fmt]

    zip_path = WORK_DIR / f"export_{fmt}.zip"

    def _make_zip() -> Path:
        with zipfile.ZipFile(str(zip_path), "w", zipfile.ZIP_DEFLATED) as zf:
            for f in frames:
                path = f.get(state_key)
                if path and path.exists():
                    zf.write(str(path), f"frame_{f['index']:02d}{suffix}.{ext}")
        return zip_path

    await asyncio.get_event_loop().run_in_executor(None, _make_zip)
    return FileResponse(
        zip_path,
        media_type="application/zip",
        filename=f"pakon_frames_{fmt}.zip",
    )


@app.get("/api/contact")
async def api_contact():
    """A single JPEG contact sheet of all rendered frames."""
    frames = _state["frames"]
    if not frames:
        return JSONResponse({"error": "no frames"}, status_code=404)

    out = WORK_DIR / "contact_sheet.jpg"

    def _make() -> Path | None:
        sheet = make_contact_sheet(frames)
        if sheet is None:
            return None
        sheet.save(str(out), "JPEG", quality=90)
        return out

    result = await asyncio.get_event_loop().run_in_executor(None, _make)
    if result is None:
        return JSONResponse({"error": "no rendered frames"}, status_code=404)
    return FileResponse(out, media_type="image/jpeg",
                        filename="pakon_contact_sheet.jpg")


# ── Clear / cleanup ─────────────────────────────────────────────────────────────

@app.post("/api/clear")
async def api_clear():
    """Reset the current scan and delete temp artifacts under WORK_DIR.

    Only files inside WORK_DIR (/tmp/pakon_web) are removed — a scan.raw saved
    into a user-chosen output directory is left untouched.
    """
    removed = 0
    if WORK_DIR.exists():
        for p in WORK_DIR.iterdir():
            if p.is_file():
                try:
                    p.unlink()
                    removed += 1
                except OSError:
                    pass

    _state["scan_path"] = None
    _state["frames"] = []
    _state["prescan"] = None
    _state["processing"] = False
    return {"cleared": removed}


# ── Static files (must be last) ───────────────────────────────────────────────

app.mount(
    "/",
    StaticFiles(directory=Path(__file__).parent / "static", html=True),
    name="static",
)
