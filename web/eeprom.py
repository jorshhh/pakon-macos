"""
The connected scanner's EEPROM archive, read automatically.

The replay-free F-135 scan, the eject and the OEM colour path all need the
unit's EEPROM archive (tools/pakon_eeprom.py backup). Instead of asking the
operator to run the backup by hand, the web service reads the EEPROM itself
the first time it sees a warm scanner on a USB connection (app start, after
a firmware load, or before a scan/eject), with the same read-only OEM
requests the backup tool uses (0xA4 select, 0xA9 read). A new archive is
kept only when none exists yet for that serial; otherwise the fresh read is
discarded and the existing archive is used.

Callers run sync() between device operations, never during a scan: it is
called from the request handlers before they start pakon_replay, and at
startup.
"""
import importlib.util
import json
import os
import shutil
import threading
from datetime import datetime
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
ROOT = _REPO / "backups" / "eeprom"
_PRIMARY = "eeprom_0x52_sectionA_primary.bin"

_lock = threading.Lock()
# What the last read found: the USB connection it was made on (bus,
# address; a power cycle or firmware load re-enumerates), the unit, and the
# archive in use.
_session: dict = {"key": None, "serial": None, "model": None,
                  "archive": None, "created": False, "error": None}


def _tool():
    spec = importlib.util.spec_from_file_location(
        "pakon_eeprom", _REPO / "tools" / "pakon_eeprom.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _report(d: Path) -> dict | None:
    try:
        return json.loads((d / "report.json").read_text())
    except (OSError, ValueError):
        return None


def archives() -> list[Path]:
    """Decodable archives under backups/eeprom/, newest first."""
    if not ROOT.is_dir():
        return []
    found = [d for d in ROOT.iterdir()
             if d.is_dir() and not d.name.startswith(".")
             and (d / _PRIMARY).exists()
             and (_report(d) or {}).get("decodable", True)]
    return sorted(found, key=lambda d: d.stat().st_mtime, reverse=True)


def archive_serial(d: Path) -> int | None:
    return ((_report(d) or {}).get("decoded") or {}).get("serial")


def connected_serial() -> int | None:
    """Serial of the scanner the last sync() read, or None."""
    return _session["serial"]


def status() -> dict:
    s = _session
    return {"serial": s["serial"], "model": s["model"],
            "archive": s["archive"].name if s["archive"] else None,
            "created": s["created"], "error": s["error"]}


def sync() -> dict:
    """Read the EEPROM of the one warm scanner (0f05:f135) if it has not been
    read on this USB connection yet, and make sure an archive exists for it.
    Returns status(). Does nothing without a warm scanner or with
    PAKON_EEPROM_DIR set."""
    if os.environ.get("PAKON_EEPROM_DIR"):
        return status()
    with _lock:
        try:
            import usb.core
            import usb.util
        except ImportError:
            _session["error"] = "PyUSB is missing (pip install -r web/requirements.txt)"
            return status()
        try:
            devices = list(usb.core.find(find_all=True, idVendor=0x0F05,
                                         idProduct=0xF135))
        except Exception as exc:
            _session["error"] = f"USB detection failed: {exc}"
            return status()
        if len(devices) != 1:
            return status()
        dev = devices[0]
        key = (dev.bus, dev.address)
        if _session["key"] == key:
            return status()

        ROOT.mkdir(parents=True, exist_ok=True)
        tmp = ROOT / f".reading-{os.getpid()}-{datetime.now():%Y%m%d%H%M%S%f}"
        try:
            identity = {"vid": "0f05", "pid": "f135", "bus": dev.bus,
                        "address": dev.address, "bcd_device": dev.bcdDevice}
            report, code = _tool().backup(dev, tmp, 2000, identity)
        except Exception as exc:
            shutil.rmtree(tmp, ignore_errors=True)
            _session.update(error=f"EEPROM read failed: {exc}")
            return status()
        finally:
            usb.util.dispose_resources(dev)

        decoded = report.get("decoded") or {}
        serial, model = decoded.get("serial"), decoded.get("model")
        if code != 0 or serial is None:
            shutil.rmtree(tmp, ignore_errors=True)
            _session.update(key=key, error="EEPROM read incomplete or not "
                            "decodable: " + str(report.get("acquisition_error")
                                                or report.get("warnings")))
            return status()

        match = [d for d in archives() if archive_serial(d) == serial]
        if match:
            shutil.rmtree(tmp, ignore_errors=True)
            archive, created = match[0], False
        else:
            tag = (model or "unknown").replace("-", "").replace("+", "PLUS")
            archive = ROOT / f"{tag}-{serial}-{datetime.now():%Y%m%d-%H%M%S}"
            tmp.rename(archive)
            created = True
        _session.update(key=key, serial=serial, model=model, archive=archive,
                        created=created, error=None)
        return status()
