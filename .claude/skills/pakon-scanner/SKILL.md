---
name: pakon-scanner
description: >
  Working knowledge for the pakon project — a cross-platform (Linux + macOS)
  driver, decoder and web app for the Kodak/Pakon F-135 / F-135+ film scanner,
  now working toward a replay-free client. Use when implementing, debugging,
  building, or testing any part of this repo: goal and roadmap, layering rules,
  build & test, the protocol, safety rules, and the related projects.
---

# Pakon F-135 scanner — project guide

## Start here

- `STATUS.md` — goal, what works, the roadmap to a replay-free client, open
  questions. Read first.
- `docs/PROTOCOL.md` — frames, replies, handshake, event service, scan sequence.
- `docs/REGISTERS.md` — every register/command we write, with encodings.
- `docs/TLB_FINDINGS.md` — facts from the OEM F-135 engine (`TLB.dll`).
- `docs/IMAGING.md` — image layout, decoder, colour pipeline.
- `docs/F135_PLUS_CAPTURES.md`, `docs/F135_PLUS_NOTES.md`,
  `docs/SCANNER_FAMILY.md` — F-135+ support and model differences.

## Goal

Replace capture replay (`.pakfw`, `.pakscan`) with commands built in code from
the EEPROM, our own calibration, and the chosen resolution. Keep the captures
as test fixtures: generated command streams must match them byte for byte.

## Related projects (in `~/Desktop/Code/pakon/`)

- **pakon-reference** — public F-X35 reference (CC BY 4.0). Use its facts.
- **libpakon-main** — independent C++ driver. **No licence: read, never copy.**
- **pakon-scanning-software** — the OEM Windows install. `TLB.dll` = F-135/F-135+
  engine; `TLA.dll` = F-235 (do not use it for 135-line facts); `TLC.dll` = F-335.
  Decompiling is for interoperability; never commit OEM binaries or decompiler
  output.

## Golden rules

- **Safety first** (pakon-reference `per-unit-data-and-safety.md`): never send
  type byte `0`; never address the bootloaders `0x22`/`0x26`/`0x42`/`0x46`; never
  write unknown bus addresses; never issue vendor request `0xA2`; replay the OEM
  TEC values, never sweep them; respect the LED current ceilings.
- **Layer separation in the C driver:** transport (`pakon_usb`: libusb,
  enumeration, firmware, bulk/control I/O, no packet knowledge) vs protocol
  (`pakon_proto`: frames, encode/decode, no libusb). `pakon_log` is shared.
- **No invented commands.** Every new frame must come from a capture, the OEM
  engine, or the reference, and should be checked against a capture in a unit
  test. A synthesized teardown built on assumptions once left the unit dark and
  jammed film.
- **Addresses come from the probes**, never constants: base `0x20`/`0x24`, Plus
  `0x40`/`0x44`.
- **Per-unit values come from the EEPROM**, never from our unit (serial 3054).
- Every packet goes through `pakon_log`; `PAKON_DEBUG=0..4` (4 hexdumps).
- Hardware steps need the human: stop and give exact commands to run.

## Key protocol facts

- Frame `[type][count][data]`, length `2 + count`, no checksum; `data[0]` =
  address. Types: 1 READ, 2 WRITE, 3 STATUS, 4 COMMAND, 7 reply.
- Endpoints: `0x01` OUT command, `0x81` IN reply, `0x86` IN image.
- After each command/write, poll `03 01 <addr>` until bit 0 clears.
- Event service: poll host every 1 ms while scanning; on flag `0x80`, read reg 2
  of each controller and ack `02 05 <a> 02 06 00 <status>`.
- LEDs: LOW `0x80` enable, `0x81` currents `[B,IR,R,0,G]`, `0x82` duties.
- Motor: SCN `0xA5` speed, `0xA0` forward, `0xA1` reverse, `0xA2` stop.
- Teardown: LEDs off → ResetFifos (`02 04 10 01 84 02`, `04 03 LOW 00 8a`) →
  `92` → 20 ms → `a2`.
- End of film is detected from pixels on the host, not signalled by the scanner.

## Build & test

Deps: C11, CMake ≥ 3.16, pkg-config, libusb-1.0 (`brew install cmake pkg-config
libusb` / `apt install build-essential cmake pkg-config libusb-1.0-0-dev`).

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Builds static `libpakon`, tools `pakon_probe` and `pakon_replay`, tests
`test_proto`, `test_hex`, `test_calib` (hardware-free). Decoder venv: `numpy
pillow tifffile`. Web: `pip install -r web/requirements.txt`, then `uvicorn
web.app:app --host 0.0.0.0 --port 8000`.

## Hardware and workflow

- Test unit here: base F-135, serial 3054 (EEPROM type 1350), connected to the
  Mac directly; macOS needs no sudo (Linux does).
- F-135+ support was verified by Ali Bosworth on serial 16402 (not on this
  machine). Plus scripts are in `resources/f135plus/`; they carry that unit's
  per-unit values (motor speeds), so treat them as one unit's recording.
- Plus decode: rows have no IR block when IR is off; stride follows the
  resolution (6000/4500/3000 samples at Base 16/8/4). `pakon_image.py
  --linewidth N --no-ir-lane`; the web UI auto-detects the layout.
- Cold `0f05:f235` needs firmware; warm `0f05:f135` is operational. Firmware is
  RAM-only; a power cycle returns to cold.
- A truncated scan without teardown leaves the device dirty; power-cycle.
- Film stuck in the transport: `pakon_replay --advance --advance-seconds 3`
  (model-aware) or `pakon_replay resources/advance.pakscan --steps N`.
- `tools/fx35_datalogger.py` converts OEM driver-level captures (FX35 driver
  with logging) into `.pakscan` scripts.
- Captures and scripts live in `resources/`; large scan outputs (`*.raw`) live
  outside the repo (e.g. `/Volumes/Video/`).
- Commit only when asked; end commit messages with the Co-Authored-By trailer.
