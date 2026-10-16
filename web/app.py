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
POST /api/scan                    SSE — run a scan (a new order in the work folder; film c41|bw|slide, format full|half|pano)
POST /api/workdir                 set the work folder scans go into
POST /api/eject                   SSE — run the film out of the transport
POST /api/open                    pick a .raw on the host (keeps its crops)
POST /api/process                 SSE — decode the opened .raw or the last scan (film c41|bw|slide, format full|half|pano)
GET  /api/frames                  list decoded frames
GET  /api/frames/{n}/thumb        JPEG thumbnail
GET  /api/frames/{n}/tiff         16-bit TIFF download
POST /api/save                    SSE — save the frames to the work folder (jpeg|tiff|raw|contact)
POST /api/reveal                  show a saved export folder in Finder (macOS)
POST /api/update/open             open the newer release's page in the browser
POST /api/telemetry/consent       the user's crash-report / usage-statistics choice
POST /api/telemetry/page|error    a page opened / a page error (web/telemetry.py)
"""
import asyncio
import json
import os
import shutil
import subprocess
import sys
import time
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from fastapi import Body, FastAPI
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles

from . import dx, eeprom, orders, telemetry, update
from .decode import (WORK_DIR, PREVIEW_PATH, FILMS, FRAME_FORMATS, prescan, export_frames,
                     make_contact_sheet, find_eeprom_dir, clear_work)

# ── Paths ─────────────────────────────────────────────────────────────────────

_REPO = Path(__file__).resolve().parent.parent
_BUILD = Path(os.environ.get("PAKON_BUILD", str(_REPO / "build")))
_RES = Path(os.environ.get("PAKON_RESOURCES", str(_REPO / "resources")))

PROBE_BIN  = _BUILD / "pakon_probe"
REPLAY_BIN = _BUILD / "pakon_replay"
PAKFW      = _RES / "f135.pakfw"      # same FX2 image for F-135 and F-135+
# Firmware from the OEM Intel HEX files when present (firmware/README.md);
# otherwise the captured load above is replayed.
_FW = Path(os.environ.get("PAKON_FIRMWARE", str(_REPO / "firmware")))
FW_STAGE1  = _FW / "PknLdr.hex"
FW_MAIN    = _FW / "Pakon7.hex"
# The unit's EEPROM archive (tools/pakon_eeprom.py backup) is what the
# replay-free scan and the OEM colour path read their per-unit values from.
# The service reads it from the connected scanner by itself (web/eeprom.py)
# and archives it under backups/eeprom/ the first time it sees a unit;
# PAKON_EEPROM_DIR picks an archive instead.
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
    "frames": [],        # list of {"index", "tiff", "thumb"} (after export)
    "prescan": None,     # prescan() metadata (ribbon path, base, pitch, centres…)
    "processing": False,
}


def _eeprom_dir() -> Path | None:
    return find_eeprom_dir()


async def _eeprom_sync_events():
    """Read the connected scanner's EEPROM if not done on this connection
    (web/eeprom.py), yielding a log event when a new archive was made or the
    read failed. Run only between device operations."""
    before = eeprom.status()
    st = await asyncio.to_thread(eeprom.sync)
    if st["created"] and st["archive"] != before["archive"]:
        yield {"type": "log",
               "message": f"EEPROM read from {st['model']} serial {st['serial']}"
                          f" and archived as backups/eeprom/{st['archive']}"}
    elif st["error"] and st["error"] != before["error"]:
        yield {"type": "log", "message": st["error"]}


@app.on_event("startup")
async def _startup_clear():
    # Temp files from an earlier run (the ribbon cache is ~1 GB a roll).
    clear_work(everything=True)


@app.on_event("startup")
async def _startup_telemetry():
    telemetry.start()


@app.on_event("startup")
async def _startup_eeprom():
    # Read a warm scanner's EEPROM at launch, in the background.
    asyncio.create_task(asyncio.to_thread(eeprom.sync))


def _fw_hex_available() -> bool:
    return FW_STAGE1.exists() and FW_MAIN.exists()


# ── Scanner detection ─────────────────────────────────────────────────────────

def _scanner_state() -> tuple[str, str | None]:
    try:
        import usb.core  # pyusb
    except ImportError:
        return "unknown", "PyUSB is missing from the web server's Python environment. Install web/requirements.txt."
    try:
        if usb.core.find(idVendor=0x0F05, idProduct=0xF135) is not None:
            return "warm", None
        if usb.core.find(idVendor=0x0F05, idProduct=0xF235) is not None:
            return "cold", None
        return "disconnected", None
    except Exception as exc:
        return "unknown", f"USB detection failed: {exc}"


# ── SSE helper ────────────────────────────────────────────────────────────────

def _sse(event: dict) -> str:
    return f"data: {json.dumps(event)}\n\n"


_SSE_HEADERS = {"Cache-Control": "no-cache", "X-Accel-Buffering": "no"}


# ── Status ────────────────────────────────────────────────────────────────────

@app.get("/api/status")
async def api_status():
    sp = _state["scan_path"]
    scanner_state, scanner_error = _scanner_state()
    return {
        "state": scanner_state,
        "state_error": scanner_error,
        "scan_file": sp.name if sp else None,
        "scan_bytes": sp.stat().st_size if sp and sp.exists() else 0,
        "work_dir": str(orders.work_dir()),
        "next_order": orders.next_order(),
        "frame_count": len(_state["frames"]),
        "processing": _state["processing"],
        "eeprom": (lambda d: d.name if d else None)(_eeprom_dir()),
        "eeprom_unit": eeprom.status(),
        "firmware_hex": _fw_hex_available(),
        "update": _update_status(),       # a newer release, or None
        "telemetry": telemetry.consent(),
        "version": update.current(),      # the app's, None from a checkout
    }


_update_seen: set = set()


def _update_status():
    st = update.status()
    if st and st["latest"] not in _update_seen:     # once per run and version
        _update_seen.add(st["latest"])
        telemetry.event("update_shown", latest=st["latest"])
    return st


async def _reported(stream, name: str, page: str, **props):
    """Pass an SSE stream through, reporting how it ended: `<name>_done`
    (usage) or a `<Name>Failed` error with the last log lines (crash
    reports). The scanner's own log is the best clue to a failure on
    someone else's unit."""
    t0, tail = time.monotonic(), deque(maxlen=40)
    async for chunk in stream:
        yield chunk
        try:
            ev = json.loads(chunk[len("data: "):])
        except ValueError:
            continue
        if ev.get("type") in ("log", "feed"):
            tail.append(ev.get("message", ""))
            if ev.get("message", "").startswith("Model: "):
                props["model"] = ev["message"].split()[1]
        elif ev.get("type") == "done":
            telemetry.event(f"{name}_done", page, seconds=round(time.monotonic() - t0),
                            **({"mb": ev["mb"]} if "mb" in ev else {}), **props)
        elif ev.get("type") == "error":
            telemetry.error(f"{name.capitalize()}Failed", ev.get("message", ""),
                            "\n".join(tail), page)


# ── Firmware load ─────────────────────────────────────────────────────────────

async def _firmware_stream():
    if _fw_hex_available():
        args = ["--load-firmware-hex", str(FW_STAGE1), str(FW_MAIN)]
        source = f"Intel HEX ({FW_STAGE1.name} + {FW_MAIN.name})"
    else:
        args = ["--load-firmware", str(PAKFW)]
        source = f"captured load {PAKFW.name} (no HEX files in {_FW})"
    for path, label in [(PROBE_BIN, "pakon_probe binary"),
                        (Path(args[1]), "firmware file")]:
        if not path.exists():
            yield _sse({"type": "error", "message": f"{label} not found: {path}"})
            return
    yield _sse({"type": "log", "message": f"Loading firmware from {source}"})

    proc = await asyncio.create_subprocess_exec(
        str(PROBE_BIN), *args,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    async for line in proc.stdout:
        yield _sse({"type": "log", "message": line.decode().rstrip()})
    await proc.wait()
    if proc.returncode == 0:
        # The scanner re-enumerates as 0f05:f135; read its EEPROM once it does.
        for _ in range(20):
            if _scanner_state()[0] == "warm":
                break
            await asyncio.sleep(0.5)
        async for ev in _eeprom_sync_events():
            yield _sse(ev)
        yield _sse({"type": "done"})
    else:
        yield _sse({"type": "error",
                    "message": f"firmware load failed (exit {proc.returncode})"})


@app.post("/api/firmware")
async def api_firmware():
    return StreamingResponse(_reported(_firmware_stream(), "firmware", "scan"),
                             media_type="text/event-stream",
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


# Lines of `pakon_replay --scan-code` worth showing, and the one that means
# "the motor runs, feed the film".
_SCAN_LOG_KEYS = ("calibrated:", "film detected", "end-of-roll", "stream ended",
                  "no film fed", "safety cap", "event service:", "scan-code:",
                  "teardown", "failed", "did not converge", "insufficient", "] dx:")
_FEED_KEY = "feed the film now"


async def _run_streaming(args: list[str], out_path: Path | None, keys=None):
    """Run pakon_replay with `args`, yielding SSE events: selected output lines
    as 'log', the feed prompt as 'feed', and the image size as 'progress'
    every 0.5 s when `out_path` is given. Ends with the exit code."""
    proc = await asyncio.create_subprocess_exec(
        str(REPLAY_BIN), *args,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    lines: asyncio.Queue = asyncio.Queue()

    async def _reader():
        async for raw in proc.stdout:
            await lines.put(raw.decode(errors="replace").strip())

    reader = asyncio.create_task(_reader())
    while True:
        done = False
        try:
            await asyncio.wait_for(proc.wait(), timeout=0.5)
            done = True
        except asyncio.TimeoutError:
            pass
        if done:
            await reader
        while not lines.empty():
            line = lines.get_nowait()
            if _FEED_KEY in line:
                yield {"type": "feed",
                       "message": "Motor running — feed the film now"}
            elif keys is None or any(k in line for k in keys):
                if line and "0x81: timeout" not in line:
                    yield {"type": "log", "message": line}
        if out_path is not None:
            size = out_path.stat().st_size if out_path.exists() else 0
            yield {"type": "progress", "bytes": size,
                   "mb": round(size / 1_048_576, 1)}
        if done:
            break
    yield {"type": "exit", "code": proc.returncode}


async def _scan_stream(film: str, fmt: str = "full"):
    if not REPLAY_BIN.exists():
        yield _sse({"type": "error",
                    "message": f"pakon_replay binary not found: {REPLAY_BIN}"})
        return

    model = None
    async for ev in _eeprom_sync_events():
        yield _sse(ev)
    eeprom_dir = _eeprom_dir()
    if not PAKSCAN_OVERRIDE:
        yield _sse({"type": "log", "message": "Detecting scanner model..."})
        model = await _detect_model()
    if model == "F-135" and eeprom_dir is not None:
        async for ev in _scan_code_stream(eeprom_dir, film, fmt):
            yield ev
        return
    if film != "c41":
        yield _sse({"type": "log",
                    "message": "The replayed scan is set up for colour negative; "
                               "the film type applies to the decode only"})
    if model == "F-135":
        yield _sse({"type": "log",
                    "message": "No EEPROM archive in backups/eeprom/ (or "
                               "PAKON_EEPROM_DIR): falling back to the "
                               "replayed scan"})

    if PAKSCAN_OVERRIDE:
        pakscan = Path(PAKSCAN_OVERRIDE)
        yield _sse({"type": "log",
                    "message": f"Using PAKON_PAKSCAN override: {pakscan}"})
    else:
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

    out_path, err = _prepare_out()
    if err:
        yield _sse({"type": "error", "message": err})
        return
    yield _sse({"type": "order", "path": str(out_path),
                "order": out_path.parent.name})

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
        _save_film(out_path, film)
        _save_format(out_path, fmt)
        yield _sse({"type": "done", "bytes": size, "mb": mb, "path": str(out_path),
                    "film": film, "format": fmt, "dx": dx.for_raw(out_path)})
    else:
        yield _sse({"type": "error",
                    "message": f"scan failed (exit {proc.returncode})"})


def _prepare_out() -> tuple[Path | None, str | None]:
    """Open the next order (web/orders.py) and return its raw path."""
    try:
        _, out_path = orders.new_order()
    except OSError as exc:
        return None, f"cannot create the order folder in {orders.work_dir()}: {exc}"
    _state["scan_path"] = out_path
    _state["frames"] = []
    return out_path, None


async def _scan_code_stream(eeprom_dir: Path, film: str, fmt: str = "full"):
    """F-135 scan with nothing replayed: setup, light calibration (open gate,
    keep the film out), motor start, then the operator feeds the film."""
    out_path, err = _prepare_out()
    if err:
        yield _sse({"type": "error", "message": err})
        return
    yield _sse({"type": "order", "path": str(out_path),
                "order": out_path.parent.name})
    yield _sse({"type": "log",
                "message": f"Model: F-135 — scan built in code (EEPROM "
                           f"{eeprom_dir.name}). Calibrating: keep the film out "
                           f"until the motor starts."})
    code = 1
    async for ev in _run_streaming(
            ["--scan-code", "--eeprom-dir", str(eeprom_dir), "--image", str(out_path),
             "--film", film],
            out_path, _SCAN_LOG_KEYS):
        if ev["type"] == "exit":
            code = ev["code"]
        else:
            yield _sse(ev)
    size = out_path.stat().st_size if out_path.exists() else 0
    mb = round(size / 1_048_576, 1)
    if code == 0:
        _save_film(out_path, film)
        _save_format(out_path, fmt)
        yield _sse({"type": "done", "bytes": size, "mb": mb, "path": str(out_path),
                    "film": film, "format": fmt, "dx": dx.for_raw(out_path)})
    else:
        yield _sse({"type": "error",
                    "message": f"scan failed (exit {code}) — see the log"})


async def _eject_stream():
    if not REPLAY_BIN.exists():
        yield _sse({"type": "error",
                    "message": f"pakon_replay binary not found: {REPLAY_BIN}"})
        return
    model = await _detect_model()
    if model is None:
        yield _sse({"type": "error",
                    "message": "could not detect the scanner model (is it warm?)"})
        return
    if model == "F-135":
        async for ev in _eeprom_sync_events():
            yield _sse(ev)
        eeprom_dir = _eeprom_dir()
        if eeprom_dir is None:
            yield _sse({"type": "error",
                        "message": "eject needs an EEPROM archive in "
                                   "backups/eeprom/ (or PAKON_EEPROM_DIR)"})
            return
        args = ["--eject", "--eeprom-dir", str(eeprom_dir)]
        yield _sse({"type": "log", "message": "Running the transport until the "
                    "film sensors say the strip is out (at most 60 s)"})
    else:
        args = ["--advance", "--advance-seconds", "15"]
        yield _sse({"type": "log",
                    "message": "F-135+: running the transport for 15 s"})
    code = 1
    keys = ("motor", "eject:", "advance", "failed", "FAILED")
    async for ev in _run_streaming(args, None, keys):
        if ev["type"] == "exit":
            code = ev["code"]
        else:
            yield _sse(ev)
    if code == 0:
        yield _sse({"type": "done"})
    else:
        yield _sse({"type": "error", "message": f"eject failed (exit {code})"})


@app.post("/api/eject")
async def api_eject():
    return StreamingResponse(_reported(_eject_stream(), "eject", "scan"),
                             media_type="text/event-stream",
                             headers=_SSE_HEADERS)


@app.post("/api/scan")
async def api_scan(film: str = Body("c41", embed=True),
                   format: str = Body("full", embed=True)):
    if film not in FILMS:
        return JSONResponse({"error": f"unknown film type: {film}"}, status_code=400)
    if format not in FRAME_FORMATS:
        return JSONResponse({"error": f"unknown format: {format}"}, status_code=400)
    return StreamingResponse(_reported(_scan_stream(film, format), "scan", "scan",
                                       film=film, format=format),
                             media_type="text/event-stream",
                             headers=_SSE_HEADERS)


# ── Work folder (where the order folders go) ────────────────────────────────────

@app.post("/api/workdir")
async def api_workdir(path: str = Body(..., embed=True)):
    target = Path(path).expanduser()
    if not target.is_dir():
        return JSONResponse({"error": f"not a directory: {target}"}, status_code=400)
    if not os.access(target, os.W_OK):
        return JSONResponse({"error": f"not writable: {target}"}, status_code=400)
    orders.set_work_dir(target.resolve())
    return {"work_dir": str(orders.work_dir()), "next_order": orders.next_order()}


# ── Host directory browser (work folder picker) ─────────────────────────────────

@app.get("/api/browse")
async def api_browse(path: str | None = None, raws: bool = False):
    """List subdirectories of a host directory, for picking the work folder
    (and its .raw files with raws=true, for opening one)."""
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
    files = []
    if raws:
        try:
            files = sorted((p.name for p in base.iterdir()
                            if p.is_file() and p.suffix.lower() == ".raw"),
                           key=str.lower)
        except PermissionError:
            pass

    parent = str(base.parent) if base.parent != base else None
    return {"path": str(base), "parent": parent, "dirs": dirs, "files": files}


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

def _run_job(work, on_done, name: str, page: str, stats=None):
    """Run `work(progress)` in the executor, stream progress as SSE, and call
    on_done(result) to build the final 'done' event. Returns a StreamingResponse.
    Reports `<name>_done` with stats(result) (usage) or the exception (crash
    reports)."""
    t0 = time.monotonic()
    loop = asyncio.get_event_loop()
    queue: asyncio.Queue = asyncio.Queue()

    def _progress(step: str, pct: float) -> None:
        loop.call_soon_threadsafe(
            queue.put_nowait, {"type": "progress", "step": step, "pct": pct})

    def _runner() -> None:
        try:
            result = work(_progress)
            loop.call_soon_threadsafe(queue.put_nowait, on_done(result))
            telemetry.event(f"{name}_done", page,
                            seconds=round(time.monotonic() - t0),
                            **(stats(result) if stats else {}))
        except Exception as exc:
            telemetry.exception(exc, name, page)
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
async def api_process(film: str | None = Body(None, embed=True),
                      format: str | None = Body(None, embed=True)):
    """Prescan: cache the ribbon, render the preview strip, detect frame centres.
    `film` (c41 | bw | slide) and `format` (full | half | pano) default to the
    ones recorded for the raw."""
    if _state["processing"]:
        async def _busy():
            yield _sse({"type": "error", "message": "Already processing"})
        return StreamingResponse(_busy(), media_type="text/event-stream",
                                 headers=_SSE_HEADERS)

    if _state["scan_path"] and _state["scan_path"].exists():
        raw_path = _state["scan_path"]
    else:
        async def _no_file():
            yield _sse({"type": "error",
                        "message": "No raw file — run a scan or open a .raw file"})
        return StreamingResponse(_no_file(), media_type="text/event-stream",
                                 headers=_SSE_HEADERS)

    if film is None:
        film = _raw_film(raw_path)
    elif film not in FILMS:
        film = "c41"
    else:
        _save_film(raw_path, film)
    if format is None:
        fmt = _raw_format(raw_path)
    elif format not in FRAME_FORMATS:
        fmt = "full"
    else:
        fmt = format
        _save_format(raw_path, fmt)
    _state["frames"] = []
    _state["prescan"] = None
    _state["processing"] = True

    def _done(meta):
        _state["prescan"] = meta
        # hand the browser everything it needs to place crops (no ribbon path)
        return {"type": "done", "saved": _load_crops(raw_path, meta["rows"]),
                **{k: meta[k] for k in (
                    "rows", "cols", "pitch", "frame_w", "box_w", "centres",
                    "preview_w", "preview_h", "scale", "film", "format")}}

    return _run_job(lambda prog: prescan(raw_path, progress=prog, film=film, fmt=fmt),
                    _done, "process", "process",
                    lambda m: {"film": film, "format": fmt, "frames": len(m["centres"])})


# ── Film type of a raw: <raw>.film beside it ──────────────────────────────────────
# Written by the scan and whenever the operator picks another type in Process;
# a --scan-code raw also records it in <raw>.flat.json. Colour negative when
# neither says.

def _film_path(raw: Path) -> Path:
    return raw.with_name(raw.name + ".film")


def _raw_film(raw: Path) -> str:
    try:
        film = _film_path(raw).read_text().strip()
        if film in FILMS:
            return film
    except OSError:
        pass
    try:
        film = json.loads(raw.with_name(raw.name + ".flat.json").read_text()).get("film")
        if film in FILMS:
            return film
    except (OSError, ValueError, AttributeError):
        pass
    return "c41"


def _save_film(raw: Path, film: str) -> None:
    try:
        _film_path(raw).write_text(film + "\n")
    except OSError as exc:
        print(f"film type not saved: {exc}", flush=True)


# ── Picture format of a raw: <raw>.format beside it ─────────────────────────────
# full | half | pano. Written by the scan and whenever the operator picks
# another format in Process. Full frame when absent.

def _format_path(raw: Path) -> Path:
    return raw.with_name(raw.name + ".format")


def _raw_format(raw: Path) -> str:
    try:
        fmt = _format_path(raw).read_text().strip()
        if fmt in FRAME_FORMATS:
            return fmt
    except OSError:
        pass
    return "full"


def _save_format(raw: Path, fmt: str) -> None:
    try:
        _format_path(raw).write_text(fmt + "\n")
    except OSError as exc:
        print(f"format not saved: {exc}", flush=True)


# ── Saved crop positions: <raw>.crops.json beside the raw ───────────────────────
# Written at export, read back when the same raw is processed again, so the
# operator's boxes come back.

def _crops_path(raw: Path) -> Path:
    return raw.with_name(raw.name + ".crops.json")


def _load_crops(raw: Path, rows: int) -> dict | None:
    path = _crops_path(raw)
    try:
        saved = json.loads(path.read_text())
    except (OSError, ValueError):
        return None
    # another decode of the raw (layout override, new ribbon code) can change
    # the ribbon length; the positions would no longer line up
    if not saved or saved.get("rows") != rows or not saved.get("centres"):
        return None
    return saved


def _save_crops(raw: Path, rows: int, centres, widths) -> None:
    try:
        _crops_path(raw).write_text(json.dumps(
            {"rows": rows, "centres": centres, "widths": widths}, indent=1))
    except OSError as exc:
        print(f"crop positions not saved: {exc}", flush=True)


@app.post("/api/open")
async def api_open(path: str = Body(..., embed=True)):
    """Make a .raw on the host the file Process decodes (in place, no copy)."""
    raw = Path(path).expanduser()
    if raw.suffix.lower() != ".raw" or not raw.is_file():
        return JSONResponse({"error": f"not a .raw file: {raw}"}, status_code=400)
    _state["scan_path"] = raw.resolve()
    _state["frames"] = []
    _state["prescan"] = None
    return {"path": str(_state["scan_path"]), "film": _raw_film(_state["scan_path"]),
            "format": _raw_format(_state["scan_path"]),
            "dx": dx.for_raw(_state["scan_path"])}


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
    raw = _state["scan_path"]

    def _work(prog):
        return export_frames(meta["ribbon"], meta["base"], centres,
                             widths=widths,
                             frame_w=meta["frame_w"], progress=prog,
                             rpd_balance=meta.get("rpd_balance"),
                             roll_centres=meta.get("centres"),
                             film=meta.get("film", "c41"),
                             flare=meta.get("flare"),
                             dx_film=dx.for_raw(raw) if raw is not None else None)

    def _done(result):
        _state["frames"] = result
        if raw is not None:
            _save_crops(raw, meta["rows"], centres,
                        widths or [meta["frame_w"]] * len(centres))
        return {"type": "done", "count": len(result)}

    return _run_job(_work, _done, "export", "frames",
                    lambda r: {"frames": len(r), "film": meta.get("film", "c41")})


# ── Frame access ──────────────────────────────────────────────────────────────

@app.get("/api/frames")
async def api_frames():
    raw = _state["scan_path"]
    return {"folder": str(orders.export_dir(raw)) if raw else None,
            "reveal": sys.platform == "darwin",          # /api/reveal works
            "frames": [{"index": f["index"]} for f in _state["frames"]]}


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
# Saved exports: <export dir>/<subfolder>/frame_NN.<ext> (web/orders.py
# export_dir), from the rendered files in WORK_DIR.
_SAVE_FMTS = {
    "jpeg": ("jpg",      "jpeg", "jpg", "JPEGs"),          # rendered positive
    "tiff": ("tiff",     "tiff", "tif", "TIFFs"),          # 16-bit positive
    "raw":  ("raw_tiff", "raw",  "tif", "raw negatives"),  # 16-bit, uninverted
}


def _save_frames(frames, fmt: str, raw: Path, progress) -> dict:
    out = orders.export_dir(raw)
    if fmt == "contact":
        progress("Making the contact sheet", 0.1)
        sheet = make_contact_sheet(frames)
        if sheet is None:
            raise ValueError("no rendered frames")
        out.mkdir(parents=True, exist_ok=True)
        path = out / "contact_sheet.jpg"
        sheet.save(str(path), "JPEG", quality=90)
        progress("Saved", 1.0)
        return {"folder": str(out), "file": str(path), "count": 1,
                "what": "contact sheet"}

    key, sub, ext, what = _SAVE_FMTS[fmt]
    folder = out / sub
    folder.mkdir(parents=True, exist_ok=True)
    # an earlier save of this roll may have had more frames
    for old in folder.glob(f"frame_*.{ext}"):
        old.unlink()
    n = len(frames)
    for i, f in enumerate(frames):
        progress(f"Saving frame {i + 1}/{n}", i / n)
        shutil.copyfile(f[key], folder / f"frame_{f['index']:02d}.{ext}")
    progress("Saved", 1.0)
    return {"folder": str(folder), "count": n, "what": what}


@app.post("/api/save")
async def api_save(fmt: str = Body(..., embed=True)):
    """Save the exported frames of the current roll into the work folder."""
    frames, raw = _state["frames"], _state["scan_path"]
    if fmt != "contact" and fmt not in _SAVE_FMTS:
        message = f"unknown format: {fmt}"
    elif not frames or raw is None:
        message = "Export the frames first"
    elif _state["processing"]:
        message = "Already processing"
    else:
        message = None
    if message:
        async def _refuse():
            yield _sse({"type": "error", "message": message})
        return StreamingResponse(_refuse(), media_type="text/event-stream",
                                 headers=_SSE_HEADERS)
    _state["processing"] = True
    return _run_job(lambda prog: _save_frames(frames, fmt, raw, prog),
                    lambda result: {"type": "done", **result}, "save", "export",
                    lambda r: {"format": fmt, "frames": r["count"]})


@app.post("/api/reveal")
async def api_reveal(path: str = Body(..., embed=True)):
    """Show a saved export in Finder. Only inside the work folder, macOS only."""
    target = Path(path).resolve()
    base = orders.work_dir().expanduser().resolve()
    if sys.platform != "darwin" or not (target == base or base in target.parents):
        return JSONResponse({"error": "not available"}, status_code=400)
    # a file is selected in its folder; a folder is opened
    subprocess.Popen(["open", "-R", str(target)] if target.is_file()
                     else ["open", str(target)])
    return {"ok": True}


@app.post("/api/update/open")
async def api_update_open():
    """Open the newer release's page in the default browser (the app window
    cannot); elsewhere the page opens the returned URL itself."""
    st = update.status()
    url = st["url"] if st else update.RELEASES + "/latest"
    telemetry.event("update_download", latest=st["latest"] if st else None)
    if sys.platform == "darwin":
        subprocess.Popen(["open", url])
        return {"opened": True, "url": url}
    return {"opened": False, "url": url}


# ── Crash reports and usage statistics (web/telemetry.py) ─────────────────────

@app.post("/api/telemetry/consent")
async def api_telemetry_consent(usage: bool = Body(...), crashes: bool = Body(...)):
    state = telemetry.set_consent(usage, crashes)
    telemetry.event("consent", usage=usage, crashes=crashes)
    return state


@app.post("/api/telemetry/page")
async def api_telemetry_page(page: str = Body(..., embed=True)):
    telemetry.page(page)
    return {"ok": True}


@app.post("/api/telemetry/error")
async def api_telemetry_error(message: str = Body(""), stack: str = Body(""),
                              page: str = Body("")):
    """A JavaScript error in the page."""
    telemetry.error("PageError", message, stack, page)
    return {"ok": True}


# ── Static files (must be last) ───────────────────────────────────────────────

app.mount(
    "/",
    StaticFiles(directory=Path(__file__).parent / "static", html=True),
    name="static",
)
