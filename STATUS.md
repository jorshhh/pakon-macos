# Project status

_Reset 2026-09-28. The previous snapshot (sessions of May–June 2026) is in git
history; everything still true from it has been folded into `docs/`._

## Goal

A **non-USB-replay client** for the Kodak/Pakon F-135 and F-135+: every command
the scanner receives is built by our code from known parameters (per-unit
EEPROM, a calibration we run, the chosen resolution), not replayed from a
captured `.pakscan`/`.pakfw` file. The captures stay in the repo as **test
fixtures**: generated command streams are checked byte-for-byte against them.

## Where things stand

Both models scan end to end on hardware by replaying captures:

- **F-135**: serial 3054 (ours), Linux and macOS, scripts in `resources/`.
- **F-135+**: serial 16402 (Ali Bosworth, PRs #2 and #3, August 2026), macOS,
  scripts in `resources/f135plus/` converted from OEM driver-level captures
  with `tools/fx35_datalogger.py`. Base 16 and Base 8 verified live; Base 4 is
  a poor replay candidate; Base 4 + IR decodes but has not been replayed.
  Coverage matrix in `docs/F135_PLUS_CAPTURES.md`.

| Step | How it works now | Replay-free? |
|---|---|---|
| Firmware load (cold `0f05:f235` → warm `0f05:f135`) | replays `resources/f135.pakfw` (same FX2 image for both models) | no |
| Open handshake + model detection | synthesized in C; probes report F-135 / F-135+ | yes |
| EEPROM read (`0xA4`/`0xA9`) | synthesized in C (`--read-params`) | yes |
| Setup / configure | per-model script replay; synthesized CONFIGURE exists for the F-135 only (`pakon_calib_default_config`, values from serial 3054) | partly |
| Scan | verbatim `--scan` replay of a per-model script; driven `--scan-sm` dies ~38% into a strip | no |
| Teardown | replays the captured tail (also on Ctrl-C) | no |
| Film advance | `advance.pakscan` loop (per model) or timed `--advance` | no |
| Decode (`tools/pakon_image.py`) + web UI (`web/`) | independent; row layout auto-detected per raw | yes |

Known limits of replay: a scan fits the capture's length (the F-135+ script
is a 4-frame strip); open-loop replay skips the OEM's film-exit wait, so the
strip can stay in the transport (`--advance` pushes it out); modes without a
capture (e.g. F-135+ Base 16 + IR) have no script.

## The projects we are working from

Laid out side by side in `~/Desktop/Code/pakon/` (see `../repos.md`):

- **pakon-macos** (this repo): our driver, decoder and web app. AGPL-3.0.
- **pakon-reference**: Ali Bosworth's public, implementation-agnostic
  reference for the F-X35 family (CC BY 4.0). Cites this project for the
  reply direction, per-resolution strides and motor stop behaviour. Source of
  the per-unit EEPROM layout, the safety rules and the DX subsystem.
- **libpakon-main**: Stefan Dierauf's independent C++ driver + "Pakon Studio".
  A driven (trace-transcribed) client for the F-135 line. **No licence file:
  read for reference only, never copy code.** Notes in
  `docs/LIBPAKON_COMPARISON.md`.
- **pakon-scanning-software**: the original Windows XP OEM install (git-ignored
  here). Its `TLB.dll` is the F-135/F-135+ engine; decompiled 2026-09-28 with
  Ghidra. Findings in `docs/TLB_FINDINGS.md`. Our earlier reverse engineering
  used `TLA.dll`, which is the **F-235** engine; facts from it are being
  re-checked against `TLB.dll`.

## Road to replay-free

Each step works on its own; the capture it replaces becomes a unit-test fixture.

1. **Firmware from Intel HEX** instead of `f135.pakfw` (standard FX2 load,
   documented in pakon-reference `usb-identity-and-firmware.md`).
2. **Setup in code:** transcribe `calib_prelude.pakscan` into named functions
   parameterised by the controller addresses (base `0x20`/`0x24`, Plus
   `0x40`/`0x44`). Add the OEM rule: after each command/write, poll `03 01 <addr>`
   until bit 0 clears (≤ 44 tries).
3. **Teardown in code:** the OEM `FN_bAfterScan` is 4 frames (LEDs off,
   ResetFifos, `92`, 20 ms, `a2`).
4. **Event service during the scan:** poll host every 1 ms; on bit `0x80`
   read each controller's reg 2 and ack with `02 05 <addr> 02 06 00 <status>`;
   read `0x90` / temperatures as flagged. Most likely fix for `--scan-sm` dying.
5. **Per-unit values from the EEPROM:** Offset → FPGA bank `0x82` reg 4/5,
   MotorSpeed × MotorAdjust / 1000 → motor reg `0xA5`, model from type word
   (1350/1351). Replaces the constants tied to one unit (the F-135+ scripts
   carry serial 16402's speeds, e.g. Base 16 `0x170C` = 5900 = its EEPROM value).
6. **Calibration in code:** LED current/duty search and dark offset with the
   motor stopped (`FN_bCalibrateLEDs`), per-column fixed-pattern correction.
7. **Film advance** from motor speed + timed move instead of `advance.pakscan`.

## Open questions to settle on hardware

- Stopping: the captures show FPGA bank `0x82` reg 0 going from acquire
  (`0x61`/`0x161`) back to idle (`0x60`/`0x160`) before `a2`. That acquire bit,
  not a motor speed register, is the likely "run/stop state" seen in August.
  Confirm that clearing it then `a2` stops the motor on both models.
- Do the LEDs light with the motor stopped once `0x81`/`0x82` carry non-zero
  currents and duties? (Would explain the old "no light in static calibration".)
- The two unresolved inputs of the `0x91` value (`FN_bSetF135`).
- Row stride at the highest resolution with IR on for the F-135+ (expected 8000).

## Safety rules (from pakon-reference, apply to every new command path)

- Never send type byte `0` (wedges the bridge until power cycle).
- Never send to bootloader addresses `0x22`/`0x26`/`0x42`/`0x46`; type-4
  commands `0x0C`–`0x0F` there erase flash.
- Never write unknown bus addresses (can erase the boot EEPROM).
- Never issue vendor request `0xA2` (writes the per-unit EEPROM).
- Replay the OEM TEC values (`0xD0 00`, `0xD1 01`); do not sweep them.
- Respect the LED current ceilings (`docs/REGISTERS.md`).

## Handy commands

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
./build/pakon_probe --load-firmware resources/f135.pakfw   # cold -> warm
./build/pakon_replay --open                                 # handshake to Idle
./build/pakon_replay --read-params --params-out eeprom.bin  # EEPROM dump
./build/pakon_replay --scan resources/scan.pakscan --image scan.raw
./build/pakon_replay resources/advance.pakscan --steps N
python3 tools/pakon_image.py scan.raw --invert-c41 --rotate 90
uvicorn web.app:app --host 0.0.0.0 --port 8000
```
