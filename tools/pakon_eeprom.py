#!/usr/bin/env python3
"""Read-only archival backup and offline decoding of the F-135-family EEPROM.

The hardware path requires warm firmware. It sends only the OEM 0xA4 read
select and 0xA9 read, never PPB, firmware downloads, or EEPROM write requests.
See docs/EEPROM_BACKUP.md for scope, provenance, and hardware validation status.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import zlib


# Read fixed documented extents, never an untrusted EEPROM length field.
SECTIONS = {
    "sectionA_primary": (0x000, 398),
    "sectionA_backup": (0x400, 398),
    "sectionB_primary": (0x800, 36),
    "sectionB_backup": (0xA00, 36),
}
MODELS = {1350: "F-135", 1351: "F-135+"}


def filename(name):
    return f"eeprom_0x52_{name}.bin"


def inspect_section(data, expected_length):
    """Length and CRC are independent of any other scanner's stored values."""
    result = {"bytes_read": len(data), "expected_length": expected_length,
              "declared_length": None, "stored_crc32": None,
              "computed_crc32": None, "valid": False}
    if len(data) < 8:
        result["error"] = "missing header"
        return result
    length, stored = struct.unpack_from("<II", data)
    result.update(declared_length=length, stored_crc32=f"{stored:08x}")
    if len(data) != expected_length or length != expected_length:
        result["error"] = "unexpected section length"
        return result
    actual = zlib.crc32(data[8:]) & 0xFFFFFFFF
    result.update(computed_crc32=f"{actual:08x}", valid=stored == actual)
    if stored != actual:
        result["error"] = "CRC mismatch"
    return result


def decode_sections(raw):
    """Decode only CRC-valid copies. Return JSON-safe data without applying it."""
    report = {"schema_version": 1, "sections": {}, "selected": {},
              "copies_equal": {}, "decoded": {}, "warnings": []}
    for name, (offset, length) in SECTIONS.items():
        data = raw.get(name)
        if data is None:
            # Never read or absent from the archive: no file, so no file hash.
            report["sections"][name] = {
                "offset": offset, "file": None, "sha256": None, "missing": True,
                **inspect_section(b"", length), "error": "section not present",
            }
            continue
        report["sections"][name] = {
            "offset": offset, "file": filename(name),
            "sha256": hashlib.sha256(data).hexdigest(), "missing": False,
            **inspect_section(data, length),
        }
    chosen = {}
    for section in ("A", "B"):
        primary, backup = f"section{section}_primary", f"section{section}_backup"
        pvalid = report["sections"][primary]["valid"]
        bvalid = report["sections"][backup]["valid"]
        # Missing or partial files are not a complete copy comparison.
        complete = all(len(raw.get(n, b"")) == SECTIONS[n][1]
                       for n in (primary, backup))
        equal = raw.get(primary) == raw.get(backup) if complete else None
        report["copies_equal"][section] = equal
        selected = primary if pvalid else backup if bvalid else None
        report["selected"][section] = selected
        if selected:
            chosen[section] = raw[selected]
        if not pvalid and not bvalid:
            report["warnings"].append(f"Section {section}: neither copy validates")
        elif not pvalid or not bvalid:
            invalid = backup if pvalid else primary
            report["warnings"].append(
                f"Section {section}: {invalid} invalid; {selected} selected")
        elif not equal:
            report["warnings"].append(
                f"Section {section}: valid copies differ; primary selected")
    decoded = report["decoded"]
    semantic_ok = True
    if "A" in chosen:
        data = chosen["A"]
        hw, model, serial = struct.unpack_from("<III", data, 8)
        decoded.update(hardware_version=hw, scanner_type=model,
                       model=MODELS.get(model, "unknown"), serial=serial)
        if model not in MODELS:
            semantic_ok = False
            report["warnings"].append("Unknown scanner type; no model profile selected")
        decoded["resolution_bases"] = {
            str(base): dict(zip(("offset", "motor_speed", "motor_speed_ir"),
                               struct.unpack_from("<HHH", data, offset)))
            for base, offset in ((4, 0x14), (8, 0x1A), (16, 0x20))
        }
        for name, offset in (("negative_matrix", 0x26), ("positive_matrix", 0x9E)):
            values = struct.unpack_from("<30f", data, offset)
            if not all(math.isfinite(v) for v in values):
                semantic_ok = False
                report["warnings"].append(f"{name}: nonfinite coefficients")
            # Preserve exact bytes in the archive; JSON null represents NaN/Inf.
            values = [v if math.isfinite(v) else None for v in values]
            decoded[name] = [values[i:i + 10] for i in (0, 10, 20)]
    if "B" in chosen:
        data = chosen["B"]
        decoded["motor_adjustments"] = {
            str(base): dict(zip(("normal", "drag", "ir", "drag_ir"),
                               struct.unpack_from("<4H", data, offset)))
            for base, offset in ((4, 8), (8, 16), (16, 24))
        }
        decoded["section_b_unknown_u32"] = struct.unpack_from("<I", data, 32)[0]
    report["decodable"] = len(chosen) == 2 and semantic_ok
    report["all_copies_valid"] = all(s["valid"] for s in report["sections"].values())
    return report


def read_section(device, offset, length, timeout_ms, received):
    """Read one fixed extent, preserving bytes even if a later transfer fails.

    The first chunk is the 8-byte header; following chunks are at most 32
    bytes. No retries: reread only in a separate power cycle.
    """
    if (offset, length) not in SECTIONS.values():
        raise ValueError("unsupported EEPROM extent")
    while len(received) < length:
        size = min(8 if not received else 32, length - len(received))
        selected = device.ctrl_transfer(0x40, 0xA4, 0x00A5, 0x1234,
                                        b"", timeout=timeout_ms)
        if selected != 0:
            raise OSError("EEPROM select returned an unexpected length")
        data = bytes(device.ctrl_transfer(0xC0, 0xA9, offset + len(received),
                                          0x1234, size, timeout=timeout_ms))
        received.extend(data)
        if len(data) != size:
            raise OSError(f"short/oversized EEPROM read: expected {size}, got {len(data)}")


def write_report(directory, report):
    # New archive only; these names are never reused for another session.
    with (directory / "report.json").open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    names = sorted(p.name for p in directory.iterdir()
                   if p.name.endswith(".bin") or p.name == "report.json")
    with (directory / "SHA256SUMS").open("x", encoding="ascii") as stream:
        for name in names:
            digest = hashlib.sha256((directory / name).read_bytes()).hexdigest()
            stream.write(f"{digest}  {name}\n")


def backup(device, directory, timeout_ms=2000, identity=None):
    """Create an exclusive archive, including partial data on transfer failure."""
    directory = Path(directory)
    directory.mkdir(mode=0o700)  # Existing directory is an error, not overwrite.
    raw = {}
    error = None
    interrupted = False
    for name, (offset, length) in SECTIONS.items():
        received = bytearray()
        try:
            read_section(device, offset, length, timeout_ms, received)
        except (Exception, KeyboardInterrupt) as exc:
            error = f"{name}: {type(exc).__name__}: {exc}"
            interrupted = isinstance(exc, KeyboardInterrupt)
        raw[name] = bytes(received)
        with (directory / filename(name)).open("xb") as stream:
            stream.write(received)
        if error:
            break  # Do not issue more transfers after an uncertain read.
    report = decode_sections(raw)
    report.update(created_utc=datetime.now(timezone.utc).isoformat(),
                  usb=identity or {}, acquisition_complete=error is None,
                  acquisition_error=error,
                  scope="Four documented sections of EEPROM 0x52, not the entire chip",
                  tool="pakon_eeprom.py schema 1")
    write_report(directory, report)
    code = 130 if interrupted else 1 if error or not report["decodable"] else 0
    return report, code


def read_archive(directory):
    """Offline decode; when a hash manifest exists, verify it before decoding."""
    directory = Path(directory)
    if not directory.is_dir():
        raise ValueError("archive directory does not exist")
    manifest = directory / "SHA256SUMS"
    verified = set()
    if manifest.exists():
        for line in manifest.read_text(encoding="ascii").splitlines():
            digest, name = line.split("  ", 1)
            if Path(name).name != name or name in (".", ".."):
                raise ValueError("invalid manifest filename")
            if name in verified:
                raise ValueError("duplicate manifest filename")
            actual = hashlib.sha256((directory / name).read_bytes()).hexdigest()
            if actual != digest:
                raise ValueError(f"SHA-256 mismatch: {name}")
            verified.add(name)
    raw = {}
    for name in SECTIONS:
        path = directory / filename(name)
        if path.exists():
            if manifest.exists() and path.name not in verified:
                raise ValueError(f"file missing from SHA256SUMS: {path.name}")
            raw[name] = path.read_bytes()
    if not raw:
        raise ValueError("no EEPROM section files found")
    report = decode_sections(raw)
    report["hash_manifest_verified"] = manifest.exists()
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    live = sub.add_parser("backup", help="read all four sections from an already-warm scanner")
    live.add_argument("directory", type=Path, help="new archive directory (must not exist)")
    live.add_argument("--timeout-ms", type=int, default=2000)
    offline = sub.add_parser("decode", help="decode an existing archive without USB access")
    offline.add_argument("directory", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "decode":
            report = read_archive(args.directory)
            code = 0 if report["decodable"] else 1
        else:
            if args.timeout_ms <= 0:
                raise ValueError("timeout must be positive")
            if args.directory.exists():
                raise ValueError("archive directory already exists; choose a new name")
            # Offline mode and tests need only the Python standard library.
            import usb.core
            import usb.util
            devices = list(usb.core.find(find_all=True, idVendor=0x0F05, idProduct=0xF135))
            if len(devices) != 1:
                raise ValueError("connect exactly one warm F-135/F-135+ (0f05:f135); "
                                 "this tool does not load firmware")
            device = devices[0]
            try:
                # EP0 vendor requests need no PPB handshake or configuration change.
                # Do not set_configuration, reset, detach a driver, or claim an interface.
                identity = {"vid": "0f05", "pid": "f135", "bus": device.bus,
                            "address": device.address, "bcd_device": device.bcdDevice}
                report, code = backup(device, args.directory, args.timeout_ms, identity)
            finally:
                usb.util.dispose_resources(device)
        print(json.dumps(report, indent=2, allow_nan=False))
        return code
    except ImportError:
        print("backup requires PyUSB (already listed in web/requirements.txt) and libusb", file=sys.stderr)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
