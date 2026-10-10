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
| Firmware load (cold `0f05:f235` → warm `0f05:f135`) | from the OEM Intel HEX files (`--load-firmware-hex`), or replay of `resources/f135.pakfw` | yes |
| Open handshake + model detection | synthesized in C; probes report F-135 / F-135+ | yes |
| EEPROM read (`0xA4`/`0xA9`) | `tools/pakon_eeprom.py backup` (all four copies, validated on serial 3054); decoded in C by `pakon_eeprom` | yes |
| Setup / configure | built in code (`pakon_setup`), both models; run on the F-135 | yes (F-135) |
| Light calibration | built in code (`--light-cal`), F-135 | yes (F-135) |
| Scan | `--scan-code` (F-135, Base 16 + IR): nothing replayed, whole strip, 2026-10-09; replay modes `--scan`/`--scan-sm` remain | yes (F-135) |
| Teardown | built in code; stops the motor on the F-135 (2026-10-09); the replay modes still replay the captured tail | yes (F-135) |
| Film advance / eject | F-135: `--advance-code`, `--eject` (DX sensors), in code; F-135+: timed `--advance` (captured speed) | yes (F-135) |
| Decode (`tools/pakon_image.py`) | independent; row layout auto-detected per raw | yes |
| Web UI (`web/`) | F-135 with an EEPROM backup: HEX firmware, `--scan-code` (feed prompt), `--eject`; else / F-135+: replayed scripts | yes (F-135) |

Known limits of replay: a scan fits the capture's length (the F-135+ script
is a 4-frame strip); open-loop replay skips the OEM's film-exit wait, so the
strip can stay in the transport (`--advance` pushes it out on the F-135+;
on the F-135 use `resources/advance.pakscan --steps N`); modes without a
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

## What the two models' captures tell us (2026-09-28)

- **One command set for both models.** With the controller addresses mapped
  (`0x40`/`0x44` → `0x20`/`0x24`), the F-135+ Base 16 scan and our F-135 scan
  use the same 55 commands. Only our capture has one extra, the probe of the
  Plus bootloader address `0x46`. There is no separate Plus protocol: one
  address-parameterised sequence serves both.
- **The per-mode values are known.** On the F-135+ (serial 16402):

  | Mode | `0x91` value | Motor speed `0xA5` | = EEPROM field |
  |---|---|---|---|
  | Base 16 | `0x003c` | 5900 | Base 16 MotorSpeed |
  | Base 8 | `0x0075` | 11434 | Base 8 MotorSpeed |
  | Base 4 | `0x0107` | 25726 | Base 4 MotorSpeed |
  | Base 4 + IR | `0x00c5` | 19278 | Base 4 MotorSpeed_Ir |

  Motor speed is exactly the EEPROM value (adjust factor 1000), confirming the
  `TLB.dll` formula on real captures. The `0x91` values match the
  pakon-reference table. Our F-135 captures use `0x0010` and speed 1557;
  1557 is close to another base F-135's Base 16 IR speed (1530) and our rows
  are the 8000-sample IR layout, so they are most likely Base 16 + IR.
- **The OEM waits on events; replay can't.** The 18-minute Base 4 capture
  replays badly because the OEM waits on lamp temperature (up to 300 s) and
  calibration. A driven client waits on the event service instead.
- **Reply bytes are recoverable.** `PAKON_DEBUG=4` against real hardware logs
  every reply, which the Windows captures lack; use it to check each generated
  sequence.

## Next steps

Each step works on its own; the capture it replaces becomes a unit-test
fixture (generated bytes must match it).

1. ~~**Read our EEPROM**~~ — done 2026-10-09: all four copies valid and
   equal; serial 3054, type 1350. Base 16 `MotorSpeed_Ir` = **1557**, so our
   F-135 captures are Base 16 + IR and motor speed from the EEPROM is proven
   on both models. Archive in `backups/eeprom/F135-3054-cycle1` (git-ignored);
   the C decoder matches the Python one on it. Original plan: **Read our EEPROM** (`python3 tools/pakon_eeprom.py backup
   backups/eeprom/F135-3054-cycle1`, see `docs/EEPROM_BACKUP.md`) and check
   that 1557 is our Base 16 `MotorSpeed_Ir`. About 5 minutes on hardware. If it
   is, motor speed from the EEPROM is proven on both models. This is also the
   first hardware test of that tool's read path; `pakon_replay --read-params`
   remains the fallback but reads only the primary copies.
2. ~~**EEPROM decoder in C**~~ — done 2026-10-09: `src/pakon_eeprom.c`
   (`pakon_eeprom_decode`): CRC check, backup fallback, type, serial,
   Offset, MotorSpeed(_Ir), adjust words, both matrices. `test_eeprom_c`
   covers the two in-repo fixtures and the corruption cases; a one-off
   cross-check against `tools/pakon_eeprom.py` matched field for field on
   all four dumps in pakon-reference. Not yet wired into
   `pakon_replay --read-params`.
3. **Setup in code, both models** — done 2026-10-09. Ran on serial 3054
   with `pakon_replay --setup --eeprom-dir backups/eeprom/F135-3054-cycle1`:
   all 77 frames accepted (every write status 0, every busy poll clear on
   the first try; LOW polls carry flag `0x80`, event pending, since no event
   service runs yet). Not yet run on an F-135+. `src/pakon_setup.c` builds the frames from the SCN `0x97` init
   to the first acquire: `pakon_setup_init` (addresses + the model's LED
   period word) and `pakon_setup_configure` (per-mode trigger, IR, dual-tap,
   integration, pixel window, LOW `0x89`; writes only changed registers, as
   the OEM does). `test_setup` matches all five captures byte for byte (F-135
   Base 16 + IR; F-135+ Base 16/8/4/4 + IR), leaving out event-service
   traffic and the status-LED word `0x0313`, whose timing varies. Findings:
   the pixel window during calibration is reg 4 = 6 (3 dual-tap), reg 5 =
   EEPROM Offset + 2000 (1000 dual-tap), on both units; the F-135 and F-135+
   init differ only in the LED period word (`0x0997` / `0x03D6`).
   `pakon_cmd_run_seq` sends a sequence with the OEM reply rules and the busy
   poll (≤ 44 tries); `pakon_replay --setup` runs it (Base 16 only).
   Model detection (probes before `0x97`) was already in C.
4. **Teardown in code** — done 2026-10-09: `pakon_setup_teardown` writes
   bank `0x82` reg 0 back to the configured value (acquire clear), LEDs
   off, ResetFifos, `92`, 20 ms, `a2`. `test_setup` matches the teardown of
   all seven captures (three F-135, four F-135+); only the status-LED words
   differ between them and are left out. `pakon_replay --setup --teardown`
   ran on serial 3054: all 11 frames accepted. In `--scan-code` it stopped
   the running motor at the end of the scan (2026-10-09).
5. **Event service during the scan** — done 2026-10-09:
   `pakon_cmd_service_events` (host poll; LOW then SCN event read; ack
   `02 05 <addr> 02 06 00 <status>`; then `0x90`, plus `0x84`/`0x88` when
   status & `0x5B`). `test_setup` reproduces 91 of the 95 acks in the
   captures (the rest depend on reply data). `--scan-sm` now runs it after
   every image read, and **ran a whole strip on serial 3054**: 5 frames,
   film detected and end-of-roll white reached, 4 events acknowledged, no
   FIFO overflow, 286 MB raw (17920 rows). Before, it died ~38% in. Setup
   and teardown in that run were still the replayed ones.
6. **First scan no replay could do:** compose F-135+ Base 16 + IR (never
   captured) from the per-mode values, on Ali Bosworth's unit.
7. **Calibration in code** — light calibration done 2026-10-09 (F-135):
   `pakon_replay --light-cal --eeprom-dir DIR` runs `FN_bCalibrateLEDs` on the
   open gate with the motor stopped: dark offsets, LED current search, duty
   refine (`src/pakon_lightcal.c`, `test_lightcal`). On serial 3054 it
   converged to offsets −38/−31/−31 (OEM: same), currents R2 G2 B3 IR2 (OEM
   G3), duties R900 G505 B273 IR1357 (OEM R896 G708 B278 IR1353; G differs
   because its current stopped at 2). Findings: during calibration a line is
   4 × (reg5 − reg4) samples (8148 here), RGB block then IR block; no marker
   bit, and the read's start within a line varies, so each read is aligned on
   its black pixels (`pakon_lc_find_origin`), which needs light, so the dark
   loop runs with the IR LED alone (TLB.dll has all LEDs off). The A/D offset
   needs ~300 ms to settle after a write. Convergence: a channel counts as
   settled when its peak is in the window or the next step moves its duty by
   ≤ 1 tick (at B's ~273 one tick is ~234 counts, wider than the 64-count
   window). **Used in a scan:** `pakon_replay --scan-code` (2026-10-09) ran
   setup, this calibration, the scan start (scan window, C-41 scan duties,
   EEPROM motor speed 1557) and the teardown, all built in code, on a whole
   strip: 0 transfer errors, 6 events acknowledged, motor stopped by itself;
   the image matches the replayed scan, and the scan-window line marker is
   back (column 1, every row). **Per-column fixed-pattern tables**
   (2026-10-10, verified on serial 3054): after the duties converge,
   `light_cal_run` averages 128+ dark lines (IR LED alone, for alignment) and
   128+ open-gate lines per pixel; `--light-cal` writes them to
   `DIR/flatfield.json` (or `--flat-out`), `--scan-code` to `OUT.flat.json`.
   The decoder and the web app apply the OEM's `(raw − dark) · gain` per column
   (`apply_flat_field`). Without it the illumination falls to ~25% at scan
   column 0 on serial 3054 (the bottom edge after the export's rotation), the
   same on every roll, which shows as a bright band. On hardware: the empty
   gate shows the same profile column for column (0.23 at column 0, 0.53 at
   10, 0.78 at 40), which confirms scan column j = calibration pixel
   Offset − 6 + j; the IR LED adds 13–17 counts to the RGB dark table; clear
   film then reads 0.92–1.03 across the line. Smear came out 0 (the OEM keeps
   only 1..699). Left: F-135+. The old `pakon_calib` (`--calibrate`, built from the F-235 engine
   `TLA.dll`) is superseded.
8. **Firmware from Intel HEX** — done 2026-10-09: on serial 3054 from cold,
   personality `c0 05 0f 35 f2 07 aa 04`, 1104 transfers with 0 errors, warm
   `0f05:f135`, then `--setup --teardown` all accepted. Command:
   `pakon_probe --load-firmware-hex firmware/PknLdr.hex firmware/Pakon7.hex`.
   The stage-1 loader is a record table inside the OEM `F235Ldr.sys`
   (`tools/extract_fx2_loader.py`), not `PknInit.hex`; the main image is
   `Pakon7.hex`. `pakon_fw_build` reproduces all 1104 transfers of
   `f135.pakfw` (`test_fw`). Refuses to load unless the personality is
   F235_AA07. OEM files stay local (`firmware/README.md`).
9. **Film handling** — done for the F-135, 2026-10-09:
   `pakon_replay --advance-code SECONDS` (timed move at the captured advance
   speed `0x251C` = 9500, matching `advance.pakscan`) and `--eject` (runs
   until the DX sensors say the strip is out or stalled at the exit). From
   a `--film-sense` run: `0x93` bytes 0–1 are the entry DX sensor, 2–3 the
   exit one; empty 190/216/224/202 on serial 3054, film keeps one byte of a
   pair under 170. Once the tail leaves the drive rollers the strip stalls
   under the exit sensor and has to be pulled by hand; `--eject` detects that
   and stops. Not derived: the advance speed (it is not the EEPROM Base 4
   speed on this unit); thresholds are this unit's (DX pots are per unit).
   F-135+ not done.

## Scan options (later): resolution and film type

The OEM TLX software offers Base 4 / 8 / 16 and colour negative / B&W /
slide. Both change the scan itself, not just the decode.

**Resolution (Base 4 / 8 / 16).** Per base: pixel window and motor speed
(EEPROM), integration time (`TLB.dll` table; F-135: Base 4/8/16 =
`0x913`/`0xD9D`/`0xFF0`, with IR `0x60D`/`0x913`/`0xC1A`), LED period
(integration × 0.6), dual-tap at Base 4, and the `0x91` trigger.
`--scan-code` is Base 16 + IR only. The `0x91` value is inferred
(2026-10-09), not yet tested:

- Rule: `0x91` = K × line pitch × (0.01, or 0.005 with IR) ÷ integration
  time; line pitch relative to Base 16 is 1 / 4/3 / 2 for Base 16 / 8 / 4
  (3000 / 2250 / 1500 lines per frame). One K fits all four F-135+ captures
  exactly (Base 16, 8, 4, 4 + IR on serial 16402).
- Cross-check: in every known mode on both models `0x91` ≈ EEPROM motor
  speed ÷ ~98 (`TLB.dll`: default motor speed = 100 × `0x91`).
- F-135 predictions (K from our Base 16 + IR = 16, `TLB.dll` integration):

  | Mode | Formula | EEPROM speed ÷ 98 (serial 3054) | Value |
  |---|---|---|---|
  | Base 16 | 24 | 24.0–24.3 | 24 |
  | Base 8 | 38 | 37.4–37.9 | 37 or 38 |
  | Base 8 + IR | 28 | 28.0–28.4 | 28 |
  | Base 4 | 85 | 84.1–85.3 | 85 (84 possible) |
  | Base 4 + IR | 64 | 63.1–64.0 | 64 |

- To do: `--scan-code --base 4|8|16`; scan one strip at each base and check
  the frame proportions (3:2) — a wrong `0x91` stretches the image ~3%, so
  it also settles Base 8 (37 vs 38). Check the F-135 line layout at Base 8/4
  from the data (the F-135+ narrows to 1500 / 1000 px, dual-tap at Base 4).
  Low risk: `0x91` only sets the line rate.

**Film type.** Scan side: the scan LED duties are the calibrated duties ×
10^density, with `TLB.dll`'s film-base densities (R/G/B/IR): colour negative
0.144/0.40/0.715/0, B&W C-41 0.10/0.25/0.25/0.08, other (slide, silver B&W)
0/0.03/0/0.08; `--scan-code` uses colour negative only. IR/ICE must be off for
silver B&W. Decode side: colour negative uses the EEPROM NegMatrix, slide the
PosMatrix (no inversion), B&W the NegMatrix then grey; `pakon_image.py` has
`--invert-c41` only. About half a day; testing needs a strip of each type.

## Feature work (independent of the replay-free steps)

- **Ansel scene balance (SBA + FOS)** — done 2026-10-09: the web export runs
  the OEM's own code from `oem/PakonIMAu.dll` under emulation
  (`tools/oem_sba.py`, `docs/IMAGING.md`). Left: SCPLut, DX-code DPI
  selection, the CLI (`pakon_image.py`), a Python port verified against the
  emulator, and a TLX reference scan to check against.
- **Web UI: show that RAW/TIFF exports are being generated.** Exporting raw
  negatives or TIFFs takes a while with no feedback: each button just
  navigates to `/api/export?fmt=…`, and the server builds the whole zip
  (deflate-compressing every 16-bit TIFF) before the download starts, so
  the page looks idle. Fix: disable the button and show "Preparing N
  TIFFs…" while it runs, ideally with per-frame progress (build the zip as
  an SSE job like Process/Export, then hand the browser the download), and
  consider `ZIP_STORED` for the TIFFs, since deflating large 16-bit images
  is most of the wait.
- **DX barcode (frame numbers, film identity).** The event service already
  reads the 30-byte `0x90` sensor block (decoded DX entries, layout in
  pakon-reference `dx-barcode.md`) and discards it. First log it during one
  scan to see whether this unit's sensor decodes at all (Ali Bosworth's
  F-135+ decodes nothing); about 30 minutes, no new commands. If it does:
  frame numbers for export labels, and the film product code. Not needed for
  colour: the OEM install ships only default film/colour LUTs; the DX-keyed
  data it does ship is 11 noise-reduction parameter files (inferred from the
  file names, not traced in code).
- **Release CI: installable builds on every release.** Each time a release is
  cut (a `v*` tag / GitHub release), a CI workflow builds executables for
  **macOS**, **Windows** and **Linux (`.deb`)** and publishes them to a
  download page (GitHub Release assets, linked from a GitHub Pages page).
  Points to settle:
  1. What ships: the C tools (`pakon_probe`, `pakon_replay`) need libusb per
     platform (Homebrew on macOS, vcpkg/MSYS2 on Windows, `libusb-1.0-0`
     as a `.deb` dependency); the web app needs Python + its requirements, so
     either a bundled app (e.g. PyInstaller) or an installer that sets up a
     venv.
  2. Windows needs a USB driver binding for libusb (WinUSB via Zadig or an
     `.inf`), and the C code has POSIX bits (`nanosleep`, `usleep`) to port.
  3. The OEM firmware (`firmware/PknLdr.hex`, `firmware/Pakon7.hex`) ships
     in the repo since October 2026, so the app bundles it; users no longer
     supply their own.
  4. macOS signing/notarization, or document the Gatekeeper workaround.

- **Digital ICE (IR dust/scratch removal)** in `tools/pakon_image.py` as
  `--ice`, then a checkbox in the web UI. About 1 day; testable offline on our
  F-135 captures, whose 8000-sample rows already carry the IR block (today it
  is discarded).
  1. Split the IR block per row; align it to the visible image (the OEM
     calibrates an "IR lag", so it may be offset; measure it like the
     trilinear R/G/B leads).
  2. Remove dye crosstalk from the IR (the OEM's `IrCrossTalkFactor`: cyan
     dye absorbs a little IR, so image detail leaks into the IR channel).
  3. Defect mask: coarse (~64 px) local background of the IR, flag pixels
     clearly darker than it, dilate ~6 px.
  4. Fill masked pixels from the smallest surrounding box with enough clean
     pixels (boxes 5 / 17 / 49 px), per channel.
  5. Grey the option out for silver B&W film (IR is blocked by silver; B&W
     C-41 is fine).

  Prior art: [opticfilm135i-linux](https://github.com/cgillinger/opticfilm135i-linux)
  (`of135i/image.py`, same method; GPL-2.0 with no "or later" found, so
  **read only, do not copy** into this AGPL project) and
  [darkoom-infrafill](https://github.com/Inrixia/darkoom-infrafill) (MIT,
  mask only).

## Open questions to settle on hardware

- Stopping: clearing the acquire bit then `a2` stops the motor on the F-135
  (2026-10-09, `--scan-code`). Still to confirm on the F-135+.
- ~~Do the LEDs light with the motor stopped?~~ Yes (2026-10-09): with
  non-zero `0x81`/`0x82` the open gate reads full scale, motor stopped.
- Why our F-135 captures use `0x91` = `0x0010`, outside the reference's
  per-mode table; and the two unresolved inputs of the `0x91` formula
  (`FN_bSetF135`).
- Row stride at the highest resolution with IR on for the F-135+ (expected 8000).

## Safety rules (from pakon-reference, apply to every new command path)

- Never send type byte `0` (wedges the bridge until power cycle).
- Never send to bootloader addresses `0x22`/`0x26`/`0x42`/`0x46` except the
  OEM presence probe `04 03 <addr> 00 00` (used by `--open`); type-4 commands
  `0x0C`–`0x0F` there erase flash.
- Never write unknown bus addresses (can erase the boot EEPROM).
- Never issue vendor request `0xA2` (writes the per-unit EEPROM).
- Replay the OEM TEC values (`0xD0 00`, `0xD1 01`); do not sweep them.
- Respect the LED current ceilings (`docs/REGISTERS.md`).

## Handy commands

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
./build/pakon_probe --load-firmware resources/f135.pakfw   # cold -> warm
./build/pakon_replay --open                                 # handshake to Idle
./build/pakon_replay --read-params --params-out eeprom.bin  # EEPROM dump (primaries)
mkdir -p backups/eeprom                                     # git-ignored archive root
python3 tools/pakon_eeprom.py backup backups/eeprom/NAME    # all four copies + decode
python3 tools/pakon_eeprom.py decode backups/eeprom/NAME    # offline decode
./build/pakon_replay --scan resources/scan.pakscan --image scan.raw
./build/pakon_replay resources/advance.pakscan --steps N
python3 tools/pakon_image.py scan.raw --invert-c41 --rotate 90

# F-135+ (scripts from serial 16402)
./build/pakon_replay --scan resources/f135plus/base16.pakscan --image scan.raw --advance
python3 tools/pakon_image.py scan.raw --linewidth 6000 --no-ir-lane --invert-c41 --jpeg
uvicorn web.app:app --host 0.0.0.0 --port 8000
```
