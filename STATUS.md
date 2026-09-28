# Project status — feed this to Claude at the start of next session

Living protocol notes are in `docs/PROTOCOL.md`; imaging details in
`docs/IMAGING.md`; the project skill `.claude/skills/pakon-scanner/SKILL.md` has
the operational guide. This file is the short "where we left off" snapshot.

_Update 2026-08-12 (F-135+ session, on Ali Bosworth's F-135+ serial 16402): the
**F-135+ now scans end-to-end on macOS**. Path: convert Ali Bosworth's Windows
driver-level captures with `tools/fx35_datalogger.py` → `resources/f135plus/
*.pakscan` → `pakon_probe --load-firmware resources/f135.pakfw` (Pakon7.hex
is shared across the F135 family, personality `F235_AA07`; verified
byte-identical) → `pakon_replay --scan resources/f135plus/base16.pakscan`
pulled a full 210 MB strip → `pakon_image.py --linewidth 6000 --no-ir-lane
--invert-c41 --jpeg` gave correct 2000×3000 positives. Plus protocol facts
(addresses 0x40/0x44, inverted presence probes, per-mode row strides) in
`docs/F135_PLUS_CAPTURES.md` + `docs/F135_PLUS_NOTES.md`. OPEN: live replay
verified for Base 16 no-IR only (other modes' scripts convert and their
captured streams decode, but haven't been replayed on hardware); framing
heuristics (`find_frame_grid` pitch bounds, fixed-3000 crop) are tuned for
Base 16 and misframe Base 8/4; open-loop replay skips the OEM's film-exit
polling (eject workaround in F135_PLUS_CAPTURES §6); no scripts exist for
uncaptured modes (e.g. Base 16 + IR) until a driven backend composes them. The
web service now detects the model and auto-detects each raw's row layout, so
its two-stage flow works on the F-135+ as well._

_Last updated: 2026-06-01 (imaging + web session). DONE: recovered the OEM C-41
inversion (ColNeg log LUT `out=3500*log10(16383/in)` + per-channel Dmin
normalisation; NOT the SCP stage) and the vibrant JPEG render (Kodak `rpd.pf` ICC
profile + scene balance + highlight roll-off) — both shipped in
`tools/pakon_image.py` (`--invert-c41`, `--jpeg`) and verified against OEM
reference scans of two rolls. Channel order is a fixed per-zone constant
(purple-cast bug solved). Framing: autocorrelation pitch → auto count → centred
fixed-3000 crops. Web rebuilt as a minilab two-stage flow (prescan preview →
operator confirms crops → high-res export); raw-negative export retained. Kodak
ICC profiles committed under `profiles/` (personal use). See `docs/IMAGING.md`.
OPEN: Digital ICE (scratch/dust) — present in OEM (`DMLDICELib.dll`) but we
detect+discard the IR band; clean-room ICE is a future option.)_

**OEM Windows software reverse-engineered (2026-05-31).** Cloned the original
Kodak/Pakon software (`pakon-scanning-software/`, git-ignored) and decompiled the
user-space stack with Ghidra (workspace in `re/`, git-ignored; findings in
`re/out/FINDINGS.md` and folded into `docs/PROTOCOL.md` → "Host transport"). Net:
- **Kernel driver is stock Cypress EZ-USB** (`F235Lib.sys` = DDK GenericUSB sample);
  all protocol logic is user-space in `TLA/TLB/TLC.dll`. Don't bother with the `.sys`.
- **Command path = `IOCTL 0x222090`** (atomic EP1 OUT→IN), input length `count+2`
  (confirms our wire format), reply `[0]==7` = success, 2 s timeout.
- **EP0 vendor control = `IOCTL 0x222059`** (`IOCTL_EZUSB_VENDOR_OR_CLASS_REQUEST`),
  **bRequest space = `0xA0` / `0xA2..0xAC`** (was "do not guess").
- **Status-byte model confirmed** via the `EC_DRV_*` error table.
- **Params written via a generic `WriteRegister(addr,reg,value)`** emitting `02`
  frames; advance duration = 24-bit value at PICM reg `0x02` (kept as verbatim
  replay — encoding formula still TBD, per decision to leave advance as-is).
- **Scan = producer/consumer ring buffer** (free-running overlapped bulk-IN read),
  stop on device end-signal not byte count — matches our end-of-roll-white autostop.
- **`PakonIMAu.dll` decompiled — "the look" identified (open Q2 ANSWERED).** It's
  Kodak's **Ansel minilab pipeline** (data-driven, ~48 ASCII LUT/param stages):
  `filmLut → SCP (Dmin/orange-mask removal) → DSBA scene balance → flesh →
  tone/contrast/lighting → gamut/colorspace → sRGB`. The C-41 inversion is the
  **SCP `modifyDmin=true`** stage (per-channel base subtraction, density space) —
  NOT naive max-raw. The "look" is the whole cascade, not the inversion. Full
  write-up in `docs/IMAGING.md`. Also found a DX-barcode reader subsystem +
  subsystem address map (see memory). Our `pakon_image.py` still ships raw
  negatives (no inversion/render) — a Pakon-style SCP-Dmin mode would be the
  natural next tool improvement.

**GOAL PIVOT → capture-free independent backend (2026-05-31).** Verbatim replay is
non-reproducible: the OEM auto-calibrates each session (lamp/CCD drift), and a
capture freezes one session's gain/offset/exposure — so the same negative scans
differently each time. Fix: a DRIVEN scan that measures + computes calibration each
session. Plan: OPEN → param-table read → CALIBRATE (measure+adjust) → CONFIGURE
(write computed values) → SCAN. Milestones: (1) register map ✅, (2) calibrate
algorithm, (3) driven C backend.
- **Milestone 1 DONE — register map decoded** (`docs/REGISTERS.md`). WriteRegister
  frame `02 <cnt> <ADDR> 03 <bank> <reg> <v16>` (read-back verified). Bank 0x84 =
  CCD AFE: Gain_R/G/B (regs 2-4, 6-bit), Offset_R/G/B (regs 5-7, sign-mag). Bank
  0x82 = CCD timing: CcdExposure_R/G/B (regs 1-3, 12-bit), Height (reg 6),
  control bitmask (reg 0). Address caveat: TLA uses 0xF0 (CCD1) where our F-135
  wire uses 0x20 (PICL) — same bank/reg semantics, use F-135 addrs.
- **Milestone 2 DONE — CALIBRATE feedback algorithm reversed** (`docs/REGISTERS.md`
  → "The CALIBRATE feedback algorithm"). Core loop is `TLA.c FUN_10022f80`, three
  sequential phases with **no film in the gate**:
  1. **Dark offset** (gate opaque, gain 0): per-channel mean → **target 300 ADU
     ±32**, proportional step `(300−mean)/38.4`, ≤8 iters, writes bank 0x84.5/6/7.
  2. **Gain** (visible gate): per-channel peak → **target 64000 ADU** `[64000,66048]`,
     ratio control `gain = round(factor·64000/peak)` with `factor = 1/(1−gain·k)`,
     ≤4 iters, writes bank 0x84.2/3/4.
  3. **Exposure** by 6-point sweep + per-channel OLS fit → bank 0x82.1/2/3, clamp
     `[0xd,0xfff]`; up to 2 outer passes; thin coverage trims **LampLevel** (lamp
     subsystem **TLA addr 0xf6 reg 0x80**, 16-bit, window ≈11500–14900).
  Measurement primitive `FUN_10021bd0` (accumulate N CCD lines) + reducers
  `FUN_10021b00` (mean) / `FUN_100214d0` (peak); convergence `FUN_100215a0`. Also
  decoded the **filter-wheel/gate** setter `FUN_10033250` (TLA addr 0xf4, position
  codes 0xe3/0xe4 visible, 0xe5 opaque). Open constants (`_DAT_1006f268`≈64000.0,
  gain `k`≈1/63) and the **lamp F-135 wire address** to confirm on hardware.
- **Milestone 3 IMPLEMENTED (2026-05-31) — driven CALIBRATE in C, tests pass,
  awaiting hardware.** New module `src/pakon_calib.c` + `include/pakon_calib.h`:
  - Pure core (unit-tested, hardware-free): `pakon_calib_build_write` builds the
    `02 06 24 03 <bank> <reg> <v16>` frame to **addr 0x24** (test asserts it
    reproduces the OEM seed bytes `02 06 24 03 84 05 33 01` = Offset_R −51, etc.);
    AFE encoders (gain 6-bit, offset sign-mag, exposure 12-bit); B,R,G
    deinterleave + per-channel mean/peak; convergence predicate; offset
    proportional step; gain ratio + linearization factor.
  - Hardware loops `pakon_calib_run`: dark-offset (≤8, mean→300±32) then gain
    (≤4, peak→64000, window `[64000,66048]`). Driver `pakon_replay --calibrate`
    [`--cal-lines N`] [`--cal-verbose`] opens, runs the open handshake, then the
    driven loop, and prints converged gain/offset next to the OEM seeds.
  - `test/test_calib.c` (15+ asserts) wired into ctest; all 3 suites green.
  - `calib_acquire` (kick `8a`+host-arm, read `0x86`); `--prelude FILE` replays a
    captured CCD/lamp setup spine before the loops; `--cal-exposure N` sets the
    gain-phase integration.

- **Milestone 3 HARDWARE-TESTED (2026-05-31, scanner on the Mac directly).**
  Firmware-loaded f235→f135, open handshake → Idle, then ran `--calibrate`:
  - ✅ **DARK-OFFSET PHASE VALIDATED ON HARDWARE.** With `--prelude
    resources/calib_prelude.pakscan`, offsets converge to **−37/−36/−36**
    (OEM seed −45) and dark_mean to ~270 (target 300±32) in ~3 iters. The full
    measure→adjust→write machinery works against the real CCD. Trace: it0
    mean=5719→off −41; it1 mean~130; it2 mean~270 converged.
  - ❌ **GAIN PHASE BLOCKED — no illumination in static calibration.** Open-gate
    peak reads ~480–530 (dark noise) and is **flat across exposure 256/1024/2048**
    (`--cal-exposure` sweep), so it is NOT an exposure problem — there is simply no
    light. The lamp/LED is NOT activated by the static register prelude (the
    spine's PICL `80 01`/`80 00` toggle leaves it off). This matches the STATUS
    "Anatomy of scan.raw" note: the bright open-gate "blank pre-load scan" only
    appears **while the motor runs** ⇒ **F-135 illumination is gated by the running
    scan engine, not a static register.**
  - Side note: writing gain `0x3f` (max) zeroes the readout (AFE saturation quirk);
    harmless since real gain is ~13, but don't drive gain to 63.
- **Milestone 3 GAIN bring-up session (2026-05-31, scanner on Mac) — findings:**
  - Proved the hardware CAN make bright open-gate data: faithfully replaying the
    OEM pre-motor preview (`--scan` of scan.pakscan lines 1–1717) ramps mean
    3840→**54681** (max 65534, 46% of samples >40000). So illumination works.
  - But our driven loop's open-gate level is stuck at **~7880 (gain 0)**, rising
    only to ~8720 at gain 8. **Exposure reg 0x82.1/2/3 is NOT the brightness lever**
    — swept 1024/2048/4095, all identical ~7880. The integration/brightness lever
    is a different register the OEM sets in preview (candidates: reg 9 set to
    0x14/0x17/0x313, the control bitmask value, height reg 6, or PICL 0x20
    `8b/8c/8d/8f`,`d0/d1/80/89`). Gain alone (max 0x3f) can't bridge 7880→64000.
  - **KEY (user/operator insight + decompile):** TLX calibration "takes a while
    the FIRST time, not subsequent times" ⇒ calibration is **cached in EEPROM**
    (decompile: `FN_GetCalibrateEEProm`/`PutCalibrateEEProm`, and the orchestrator's
    "use-cached-calibration" path when recalibrate-flag clear). The slow first run =
    the **thermal-stability wait** (`FUN_10028480`, 20-min timeout, Sleep(500) loop)
    + full multi-phase calibrate. This reframes the goal: the scanner persists its
    own calibration, so reading the **0xA4/0xA9 param table = the cached EEPROM
    calibration** is likely the pragmatic, reproducible path (decode it instead of
    reproducing the live bright-gate measurement).
  - Tooling this session: `--prelude FILE`, `--cal-exposure N`, kick-per-read
    `calib_acquire`, stream-prime, control-strobe (reg0=0x161) write. Dark phase
    can read 0B when the device is left in a dirty state by a truncated `--scan`
    (no teardown) — re-open/replug or run a clean teardown to reset.
- **Milestone 3 → CACHED-CALIBRATION PATH chosen (operator steer) + table dumped
  (2026-05-31).** `pakon_replay --read-params [--params-out FILE]` performs the OEM
  0xA4-trigger / 0xA9-read sequence (wIndex 0x1234, offset in wValue) and dumps the
  scanner's persisted (EEPROM) calibration. Read cleanly on hardware, 0 errors.
  Structure decoded so far (dump saved to /tmp/params.bin this session):
  - Two framed regions; each starts with `u32 length` then `u32 checksum`:
    region 1 = 0x000..0x18e (len 0x18e), region 2 = 0x800..0x824 (len 0x24).
  - Region-1 header ints: 0x0c=1350, 0x10=3054 (likely geometry/DPI). 0x18..0x9f =
    opaque binary (NOT float32) — likely the **ColorMatrix** / per-channel blob.
  - **Region 2 @0x808 = `(1000,1008) × 6` u16 pairs** — almost certainly the LED
    **Current/Duty** per channel (the "Light" calibration).
  - The raw gain/offset/exposure register values are NOT stored verbatim — this is
    a serialized calibration record needing the OEM parser to map fields.
  - **EEPROM schema found** (decompile TLC.c:11429 EEPromDebug.txt header):
    `Current_R/G/B/Ir, Duty_R/G/B/Ir, IntegrationTime` + temps/RPMs; section
    parsers `FN_bEEPromReadSection`, `FN_GetCalibrateInfo{ColorMatrix,Light,Dpi}`
    (TLC.c ~22583+). 0xA4/0xA9 issued via TLC `FUN_1001db50` / TLA `FUN_1001ac60`.
- **Milestone 3 → SYNTHESIZED CONFIGURE works on hardware (2026-05-31).**
  `pakon_calib_config` + `pakon_calib_default_config()` (the OEM's FINAL converged
  values from the capture) + `pakon_calib_configure(dev,cfg)` write the full
  calibration register set to addr 0x24 from C — capture-free, not replayed.
  `pakon_replay --configure [--prelude FILE]` ran on hardware: all 17 register
  writes accepted (gain 13/13/13, offset −38/−31/−31, height 0x0c1a, control
  0x0160, timing 4/5/9/0xa, AFE 0/1). EEPROM **drift detection** (`--read-params`
  checksums vs `PAKON_CALIB_EEPROM_CKSUM_R1/R2`) confirms the defaults match this
  unit. KEY: CcdExposure regs 0x82.1/2/3 are 0 in the OEM final state — integration
  is via timing regs 4/5/9/0xa (why the earlier --cal-exposure sweep did nothing).
  Unit-tested; on main.
- **Milestone 3 → DRIVEN SCAN-OUT WORKS on hardware (2026-06-01).** Fixed the
  long-standing `--scan-sm` stall: two bugs — (1) it took over at the FIRST image
  read after motor-start, *before* the post-motor setup burst (control-strobe
  `reg0=0x0161` + `reg9` integration writes at reads 1–3), leaving integration
  unconfigured; (2) it re-armed `8a` into the live stream (wedge). Fix: replay
  `SM_TAKEOVER_READS`=64 reads past motor-start (captures the burst), then a
  poll-`03 01 10`+read loop with **no re-arm** (status 0x80=busy→wait, 0x00=ready),
  stop on end-of-roll/cap, then **mandatory teardown**. Result open-gate (no film):
  1409 reads, 28.8 MB, continuous stream, **0 transfer errors**, teardown reset to
  idle, and a following `--open` reached Idle (0/5) — device healthy. The
  length-independent driven scan-out is proven.
  - Cadence facts (offline + traced): `03 01 10` reply `03 03 10 00 aa`=ready /
    `…80 aa`=busy; steady state = poll+read, no re-arm; frame-boundary housekeeping
    fires ~4× (trigger TBD, skippable for open-gate / handled by replay-to-takeover);
    teardown (`92` readout-stop + `a2` motor-stop + PICL resets) is MANDATORY —
    killing the host does NOT stop the motor.
- **END-TO-END VALIDATED WITH REAL FILM (2026-06-01).** Drove a real 4-frame C-41
  negative through the **driven** `--scan-sm` path on hardware: firmware load → open
  → driven scan-out (poll+read, no re-arm) → teardown (engines reset to idle). Film
  was detected (`film detected at 28446720 bytes`); decoded with `pakon_image.py`
  (venv: numpy+pillow+tifffile) into 4 recognizable photos (a wedding/event series).
  The capture itself was clean; three POST-CAPTURE rough edges to polish:
  1. **End-of-roll didn't auto-stop** — `sm_chunk_is_white` keys on brightness
     (>40000), but a NEGATIVE transmits bright (orange mask/clear base), so the
     trailing-white stop never armed and it ran to the 400MB cap. Fix: detect
     end-of-roll by loss of detail/variance (blank open gate = low spatial
     variance), not absolute brightness.
  2. **Uneven frame splitting** — gap detector mis-placed boundaries (frames came
     out 2660/1232/3124/1880 rows) on 419MB with lots of blank margin.
  3. Magenta cast = naive linear invert (the separate C-41 orange-mask / "the look"
     work, docs/IMAGING.md).
### >>> NEXT TASK (resume here after a context clear) <<<

**The C / scanning side is DONE. All remaining work is PYTHON decode in
`tools/pakon_image.py`.** Status after the 2026-06-01 LowRes session:

**SOLVED & COMMITTED this session (validated on `/Volumes/Video/fullroll.raw`, the
1.2 GB whole-roll LowRes scan — keep it, it is the LowRes test fixture):**
- **Dual-tap "seam" was MOSTLY a CHANNEL-ORDER BUG, not a gain mismatch.** Zone1
  (the before-IR wrap half, read by the *other* CCD tap) outputs channels in a
  DIFFERENT interleave order than zone0: **zone0 = pos0→B, pos1→R, pos2→G** (the
  known global order) but **zone1 = pos0→G, pos1→B, pos2→R**. With the wrong map
  zone1 showed RGB ghosting + a horizontal shift that made film-edge lines
  discontinuous across the seam, AND a big colour step. Fixed by per-zone channel
  permutation (`measure_leads(...,perm=)` / `register_zones(...,zone_perms=)`);
  zone1 perm = `{"r":"g","g":"b","b":"r"}`. After the fix BOTH zones measure the
  same trilinear leads (G=-8, B=-16) and the leftover tap gain seam is tiny
  (`a≈1.0, b≈+400`). User confirmed "colours are even". Commits `8d6f511`
  (channel order) + `5a37f86` (seam correction, now near-no-op).

- **FRAME SPLITTING for LowRes whole-roll — SOLVED & confirmed (commit
  `35436ed`).** `find_frame_grid` fits a global fixed-pitch grid: scan candidate
  (pitch, phase) pairs and pick the one that puts every cut line in a low-detail
  inter-frame gap at once. On `fullroll.raw` it locks pitch=3210 rows (cuts at
  detail 0.31 vs frame-centers 0.49), excludes the bright pre-roll (open-gate
  white before the neg loads), prunes degenerate end slivers, and emits uniform
  **3000 px** centred crops. Frame count falls out of the grid (25 frames; frame
  25 = unexposed end-of-roll tail, expected). `--frames N` still forces a count.
  This replaced the rejected per-frame measure-then-trim/pad approach (which the
  operator called "dumb / nonsensical").

**THEN, lower priority:**
- **C-41 INVERSION** (the "figure it out" piece). Raw negatives have the C-41
  orange mask; naive `max-raw` comes out magenta. Implement proper inversion:
  per-channel film-base (Dmin) subtraction in DENSITY (log) space + gray-balance +
  tone scale → positive. OEM approach = Ansel **SCP `modifyDmin=true`** (full
  pipeline in `docs/IMAGING.md`). Sample the film base from the real rebate/clear
  unexposed border, NOT the bright blank scene. Decode venv: `numpy pillow
  tifffile`.

Reference (this session's findings):

- **C / SCANNING SIDE IS DONE (2026-06-01).** HiRes and LowRes both capture on
  hardware; driven CONFIGURE / dark-offset calibration / known-good teardown work;
  firmware load + open + advance/eject work. The remaining work is all PYTHON decode.
- **Single-tap vs dual-tap = HiRes vs LowRes (decompile-confirmed).** No "tap"
  toggle exists; the decompile uses `HiRes` / `iResolution` / `Base8Ratio_HR_LR`
  (4 or 8). HiRes reads the CCD through a SINGLE tap (clean color, data-heavy →
  ~4 frames/scan, capped by `HiResMegabytesRoll`/`Total`) = our `scan.pakscan`.
  LowRes reads through TWO taps in parallel (≈4–8× less data, whole roll fits, but
  the two taps have a per-channel gain mismatch → magenta/cyan SEAM down each frame)
  = `scan_fullroll`/`36frames`. Wire tell: PICL `02 05 20 02 06 00 XX` = 0x0200/0x2000
  (HiRes) vs 0x0400/0x4000 (LowRes), a clean 2× readout-rate. Decision: scan LowRes
  for whole rolls and fix the dual-tap seam in `pakon_image.py` (route 1).
- **Dual-tap seam fix (Python, route 1) — DONE 2026-06-01, but the diagnosis
  changed.** The large per-channel seam coefficients in the old spec
  (`a[r]=0.866, a[g]=0.637...`) were measuring the wrong thing: zone1 had a
  **channel-order bug** (pos0→G,pos1→B,pos2→R vs zone0's pos0→B,pos1→R,pos2→G).
  Once zone1 is read with the correct order, the real tap gain mismatch is tiny
  (`a≈1.0, b≈+400`). The remaining `dual_tap_correct` seam-column linear fit is a
  near-no-op safety net. Validated on `/Volumes/Video/fullroll.raw`; user confirmed
  even colours. (The earlier large-coefficient spec is SUPERSEDED.)

- **ROOT CAUSE of driven-scan incompleteness CONFIRMED (2026-06-01).** A real-film
  driven `--scan-sm` scanned only ~3/4 of the strip then went DARK (flat ~317) for
  the rest — the illumination/readout DIED mid-scan, and the last quarter scanned
  black (operator confirmed: not end-of-roll, film left in transport). Brightness
  profiles settle it: **verbatim `--scan` stays lit the whole strip** (mean 15-57k,
  80-100% signal across all 20 windows — it replays every housekeeping write);
  **driven `--scan-sm` dies at ~38%** because the "no re-arm" loop sends NO mid-scan
  writes after takeover. The OEM sends periodic per-frame housekeeping (capture:
  reads 546/1035/5569/9302, irregular = frame-triggered) that keeps the readout
  alive; dropping it kills the scan ~3/4 through. ⇒ End-of-roll detection (bright OR
  dark) is moot — the scan dies before the true end regardless. The fix is the
  housekeeping CADENCE (the long-standing unsolved piece), best from a clean
  `--scan --trace-status` capture correlating the housekeeping writes to poll-status
  at frame boundaries, then replaying them on that trigger. NOT a live-guess task.
  WORKING capture path meanwhile: **verbatim `--scan`** (full strip, fixed length;
  matches a 4-frame neg) + `advance` to eject.

- **REGRESSION + REVERT (2026-06-01).** The synthesized 4-command teardown
  (`sm_teardown`) was a MISTAKE and is reverted. It assumed `02 04 20 01 80 00` =
  "lamp off" (unconfirmed) and replaced the known-good captured 761-command teardown
  replay (`sm_replay_teardown`, which reliably returns the device to Idle 0/5). The
  result: the next scan read **dark (~316, no illumination)** and the engine wasn't
  fully reset (status LEDs kept blinking = stuck in scan mode), so it never
  auto-stopped (ran to cap) and a mid-feed stop **jammed the film** (recovered with
  `pakon_replay resources/advance.pakscan --steps N`). LESSON: do not synthesize
  teardown/illumination commands from assumptions — the lamp-on/off control is still
  an UNKNOWN (see Lamp section). The driven scan-out + captured-teardown replay is
  the known-good baseline (it captured real photos). End-of-roll left at brightness
  60% (IR-aware) — its fair test was confounded by the dark/lamp-off regression;
  re-test once the device is clean (lamp on).

**Milestone 3 — driven backend works; teardown reverted to known-good.**
After the real-film run didn't auto-stop and left the scanner dirty (lamp/LEDs on),
fixed in C (build clean, unit tests pass; validate on next power-cycle):
- **End-of-roll for negatives** — `sm_chunk_is_blank()` keys on UNIFORMITY (mean-abs-
  deviation < `SM_BLANK_MAD`=1200), not brightness, so a negative (bright via orange
  mask) still auto-stops after `SM_TRAIL_BLANK`=16 trailing uniform chunks. Replaces
  the brightness `sm_chunk_is_white` in the driven loop.
- **Robust clean teardown** — `sm_teardown()` (capture-free) halts readout `92` FIRST,
  then motor `a2`, then lamp OFF `02 04 20 01 80 00`, clears strobe (reg0→0x0160),
  drains 0x86. Stops-first works mid-stream (the cause of the lamp-left-on dirty
  state); replaces the replayed captured tail in `--scan-sm`. Runs on every exit.
- Tunables to verify on hardware: `SM_BLANK_MAD` (uniform threshold) and the
  teardown command ORDER (stops-first is a reasoned change from the OEM's lamp-first
  capture order — confirm it leaves LEDs clean).

NEXT: power-cycle, reload firmware, `--scan-sm` a negative; confirm it auto-stops at
end-of-roll AND a following `--open` reaches Idle (0/5) with LEDs normal. Then
frame-split robustness (Python) and C-41 inversion are the only remaining polish. The capture-free driven
backend now works end to end on hardware: OPEN → param-read+drift-check → CONFIGURE
(synthesized) → dark-offset calibrate (validated) → DRIVEN scan-out (validated) →
teardown. What's still *replayed* (acceptable — fixed boilerplate, not calibration):
the PIC/CCD init+setup spine before takeover and the teardown tail (both from
`scan.pakscan`). Optional polish:
- Synthesize the init spine + teardown in C to drop the `.pakscan` dependency entirely.
- Pin the frame-boundary housekeeping trigger (a poll-status flag) for film scans
  longer than the takeover window — capture a CLEAN traced scan WITH film that ejects
  normally (film handling was finicky on 2026-06-01). For open-gate / short scans it's
  not needed.
- Wire CONFIGURE + dark-offset into the driven scan path (currently the scan-out is
  validated standalone via `--scan-sm`; fold our calibration in ahead of it).

(Earlier sub-goal superseded: the full verbatim --scan still works as the reference;
--scan-sm is now the length-independent driven path.)
The pieces now exist capture-free: OPEN handshake (synthesized), param-table read +
drift check (`--read-params`), CONFIGURE (`pakon_calib_configure`, hardware-accepted),
dark-offset live refine (hardware-validated). Remaining: the PIC/CCD **init
boilerplate** before CONFIGURE is still replayed from `resources/calib_prelude.pakscan`
(the `97 01` PIC init, `12345` ID reads, PICL bring-up, `8b/8c/8d/8f` LED geometry,
control toggles) — synthesize that in C too (it's fixed, non-calibration), then drive
the scan/readout + teardown from C. Then milestone 3 (driven backend) is complete.

Optional later: decode the EEPROM table sections (ColorMatrix for rendering "the
look"; LED Current/Duty) — only needed for rendering, not for capturing.

(Earlier sub-goal, superseded: decode the cached-calibration table struct fully —
deferred; the synthesized-CONFIGURE + checksum-drift approach reaches a reproducible
scan without it.) The table is read
(`pakon_replay --read-params`); now map its fields by tracing the OEM parser in the
decompile: `FN_bEEPromReadSection` + `FN_GetCalibrateInfoColorMatrix/Light/Dpi`
(TLC.c ~22583+, and the 0xA9 reader `FUN_1001db50`/`FUN_1001ac60`). Goal: extract
ColorMatrix (region 1 float blob), Light = LED Current/Duty (region 2 (1000,1008)×6)
and IntegrationTime, and geometry (1350/3054). Then drive the backend CONFIGURE from
these cached values instead of a live open-gate calibration. The live-calibration
gain/offset path stays as a fallback (dark-offset already works on hardware).

(Superseded sub-goal: the live GAIN phase is blocked on open-gate illumination — the
brightness lever is some register other than exposure 0x82.1/2/3; deprioritized in
favour of the cached-calibration path.) Dark-offset
is done & hardware-validated. The gain loop needs the open-gate white (~48900) the
OEM sees, but the F-135 lamp does not come on from static register writes. Two
avenues (the scanner connects directly to the Mac now — no SSH/sudo):

1. **Calibrate with the motor running (most promising).** The OEM's bright
   open-gate light is the "blank pre-load scan" — i.e. it scans (motor on, lamp on)
   with no film. Drive a slow advance / scan-engine start (`04 03 24 00 a0` motor +
   the PICL `8a` readout) concurrently with the gain measurement, gate open / no
   film, and measure the streaming `0x86`. Adapt `pakon_calib_run` to start the
   engine for the gain phase (or add a `--gain-with-motor` path).
2. **Find the explicit F-135 lamp-enable** by decompiling the F-135 illumination
   path (TLA's `FUN_10033c70`/addr 0xf6 is F-235/335; the F-135 equivalent lives in
   the PICL `0x20` bank `0x80/0x87/0x89/0xd0/0xd1` writes — instrument which one
   raises the CCD level).

To reproduce the hardware test (scanner on the Mac, gate open, no film):
```sh
./build/pakon_probe                                   # if cold f235:
./build/pakon_probe --load-firmware resources/f135.pakfw   # -> f135
PAKON_DEBUG=2 ./build/pakon_replay --calibrate \
    --prelude resources/calib_prelude.pakscan --cal-verbose
```
`resources/calib_prelude.pakscan` = scan.pakscan up to the first image read
(static CCD setup, no motor); regenerate with
`awk '/^M /{exit}1' resources/scan.pakscan > resources/calib_prelude.pakscan`.

Other deferred items:
- `.data` target constants (`_DAT_1006f268`≈64000, gain `k`≈1/64) — confirm once
  the gain loop sees real light.
- **Exposure phase** (6-pt OLS) not yet ported — add after gain converges.

Environment note: this work mines the **Ghidra dumps `re/out/TLA.c` / `TLC.c`**,
which are **git-ignored and exist only on the Mac** (regenerate via
`re/scripts/DumpDecompiled.java`, see the ghidra-re-setup memory). Do it on the Mac.

Milestone-2 anchors in `re/out/TLA.c` (for re-verification): core loop
`FUN_10022f80` (24739–25373); orchestrator `FUN_10033dd0`; coordinator
`FUN_10026990`; measure `FUN_10021bd0`; reducers `FUN_10021b00`/`FUN_100214d0`;
convergence `FUN_100215a0` (23728); lamp `FUN_10033c70` (addr 0xf6); gate
`FUN_10033250` (addr 0xf4). Register map + address caveat: `docs/REGISTERS.md`.

**Phase 5 WORKS on hardware:** `pakon_replay --scan` drove a full scan from our
code and pulled **239,984,640 image bytes** (4-frame COLOR strip, 11719 reads,
2 late errors). Sample under analysis: `/Volumes/Video/scan.raw`.

**Image layout — CONFIRMED interleaved (not planar):**
- **16-bit LE**, line stride **8000 samples / 16000 bytes** (autocorr peak +
  2x harmonic), ~**14999 lines**.
- **Per-pixel interleaved RGB** (`R,G,B,R,G,B…`), NOT plane-sequential. Proven
  on `scan.raw`: column autocorrelation peaks at lag 3/6/9 (interleave
  fingerprint, lag3 ≈ 1.58× lag1); deinterleave-then-correlate gives high,
  balanced channel correlation (R~G .59 / R~B .90 / G~B .56) while a planar
  split collapses (one "channel" anti-correlates, −0.08). This **supersedes the
  earlier planar hypothesis** in old commits / `raw2pnm.py --planar`.
- 8000 isn't a multiple of 3, so the working model is **2 padding samples per
  line and RGB restarts at R each line → 2666 px wide** (`(8000//3)` triples).
  ⚠️ see OPEN Q1 — this per-line phase reset is the suspected cause of ghosting.
- **Interleave order B,R,G** (position 0 = Blue, 1 = Red, 2 = Green), NOT RGB.
  Green is the middle trilinear line; red is the channel that passes most
  through the orange mask (rebate transmission), blue the most absorbed
  (darkest). Confirmed by natural skin tones across all 6 permutations of a real
  frame (the wrong orders give green or "lomography purple" skin).
- Ribbon comes out rotated 90°; 4 frames stacked along the long (line) axis.

**Anatomy of `scan.raw`** (rows, from `autocrop`): dark leader 0–513; **blank
pre-load scan** ~514–1950 (light through no film — captured before the strip
was loaded, bright + colour-neutral); **film / 4 frames 1951–14825**; blank
tail; uniform gate margin at cols ~2050–2666; orange-base sliver at col 0.

**`tools/pakon_image.py` (committed `ea51cc0`)** decodes interleaved RGB →
16-bit RGB TIFF(s). `--autocrop` (default) trims leader/blank-scan/tail/margin
(dark rows; bright+neutral rows; low-detail columns) → on this scan rows
1925–14868, cols 1–1999. `--frames N` splits along the ribbon **before** rotate;
`--rotate {90,180,270}` per frame; `--invert` is a LINEAR preview only.
Run: `pakon_image.py scan.raw --rotate 90 --frames 4` → four 3236×1999 negs.

**Q1 — RGB ghosting — SOLVED (2026-05-31).** NOT a phase/interleave problem:
the per-line "restart at R" model is correct (period-3 power in per-row channel
means = 0; every row's best phase = 0). The real cause is a **trilinear CCD** —
R/G/B sensor lines are spaced along the scan direction, so the channels are
offset in scan lines. Measured by inter-channel vertical cross-correlation:
sensor order along the scan is **B (0), G (+8), R (+16) lines** (positions
p0/p2/p1; 8-line spacing). Fix: co-register the three lines to a common position
→ edge fringing gone, residual offset 0. Implemented as `pakon_image.py
--register` (default on, auto-measures the leads; `--reg-leads G,B` to force,
`--no-register` to disable).

**Q2 — magenta / "black rebate" — ANSWERED (2026-05-31): stream is NOT
pre-inverted.** Photometric refs prove transmission-sense (more light → higher
value): dark leader ≈ 520, **no-film open gate ≈ 48900 (max)**, orange base
edge R≈11800/G≈5600/B≈4800 (the C-41 mask), film midtones R15223/G18723/B16000.
So it's a true **raw digital negative**; the black rebate the human saw is the
*correct* result of inverting a negative (clear/unexposed border = high
transmission → black), not double-inversion. The **magenta** is purely the
orange mask: naive `max-raw` leaves green lowest → magenta. A mask-aware invert
(per-channel density relative to the film base) fixes it but is **sensitive to
the base estimate** — sampling the bright scene instead of the true rebate
over-corrects to green. TODO (optional): proper C-41 inversion in-tool
(sample the real rebate / per-channel base, density space, gray-balance), OR
keep emitting the registered+cropped raw negative for dedicated film software
(Negative Lab Pro, darktable negadoctor, Grain2Pixel).

TODO after Q1/Q2: frame-boundary auto-detection (gaps); pixels may be non-square
(across-sensor oversampled vs motor step) — Pakon's own output is 3000×2000/
frame, so resampling may be wanted. Then Phase 6 (Swift macOS app).

---

## Full-roll scan decode — findings (2026-05-31)

Full roll scan: `/Volumes/Video/fullroll.raw` (1.2 GB, 83311 lines, 24 frames).
Processed with `pakon_image.py --rotate 90 --frames 24 --resample-to 3000x2000`.

### Scan line layout: `[visible | IR]` with optional wrap

Each scan line is **8000 samples = 2666 RGB pixels**, laid out as:
`[visible image (~2008 px) | IR channel (~658 px)]`

The **IR channel** is a **Digital ICE** channel for dust/scratch removal:
- Perfectly colour-neutral: R=G=B≈32k (a monochrome IR pass stored in all three
  sample slots).
- ~658 px wide, constant brightness down the entire roll.
- Rendered alone it shows the full scene as a grayscale image with dust specks
  and high-frequency edge detail.

**Buffer offset / wrap:** depending on scan mode, the IR band may sit at the
*end* of the line (4-frame scan: cols 2008–2665) or in the *middle* (full-roll
scan: cols 1261–1919). When mid-line, the visible image is wrapped — the two
visible halves are spatially contiguous **at the sensor wrap seam** (col
2665↔col 0, measured continuity corr=0.992) not at the IR-adjacent edges
(corr=0.51). Reassemble in **wrap order: [after-IR | before-IR]**.

| Scan | IR cols | Visible cols | Wrap? |
|------|---------|-------------|-------|
| `scan.raw` (4-frame) | 2008–2665 | 0–2007 | No (IR at edge) |
| `fullroll.raw` (24-frame) | 1261–1919 | 1920–2665 + 0–1260 | Yes |

### Per-zone trilinear registration

The wrap-split halves come from **opposite ends of the sensor readout** and
require **different R/G/B line leads** — using one global lead leaves one half
ghosted (±24 lines mis-registered, strong red/cyan edge fringing):

| Zone | G lead | B lead |
|------|--------|--------|
| Right visible (after-IR, 1920–2665) | −8 | −16 |
| Left visible (before-IR, 0–1260) | +16 | +8 |

The right zone matches `scan.raw`'s single-zone leads exactly. `pakon_image.py`
now measures and applies leads per-zone via `measure_leads()` / `register_zones()`.

### ✅ RESOLVED (2026-06-01): the "dual-tap colour mismatch" was a channel-order bug

The hard tint seam down the middle of every full-roll frame was NOT primarily a
per-tap gain mismatch — it was a **channel-order difference** between the two taps:

- **zone0** (after-IR half): interleave `pos0→B, pos1→R, pos2→G` (global order)
- **zone1** (before-IR half): interleave `pos0→G, pos1→B, pos2→R`

The old "per-channel ratio R:0.60, G:1.33, B:1.15" measurement was really seeing
swapped channels. With the correct per-zone order (`zone_perms` in `register_zones`),
both halves register to the same trilinear leads (G=-8, B=-16) and the residual tap
gain seam is small (`a≈1.0, b≈+400`) — handled by `dual_tap_correct`. Validated on
`/Volumes/Video/fullroll.raw`; operator confirmed even colours. Committed `8d6f511`.

### Aspect ratio

Raw pixels are **non-square**: ~95 steps/mm along film, ~83 px/mm across CCD.
Fix: `--resample-to 3000x2000` (Lanczos, `!`-forced exact size via ImageMagick).
Without resampling, the full-roll frames are ~3430×2007 ≈ 1.7:1 (wrong).
With `--resample-to 3000x2000`: correct 3:2 = 2:3 portrait / landscape. ✓

### Frame boundary detection — SOLVED (fixed-pitch grid, commit `35436ed`)

`pakon_image.py`'s `find_frame_grid` fits a single global grid: it scans
candidate (pitch, phase) pairs and picks the one that places every cut line in a
low-detail inter-frame gap simultaneously — the physically correct model since
35mm frames are a constant width at a consistent pitch. It excludes the bright
pre-roll (open-gate white before the neg loads), prunes degenerate end slivers,
and emits uniform **3000 px** centred crops. Frame count falls out of the grid
(`--frames N` still forces a count). Validated on `fullroll.raw`: pitch 3210
rows, 25 uniform frames, operator-confirmed. (Superseded the per-frame valley +
trim/pad approach, which was rejected.)

### pakon_image.py current command

```sh
python3 tools/pakon_image.py /Volumes/Video/fullroll.raw \
    --rotate 90 --frames 24 --resample-to 3000x2000 -o output/frame
```

Output: `output/frame_1.tif` … `output/frame_24.tif`, 3000×2000 16-bit RGB
raw negatives (orange mask intact). Feed to Negative Lab Pro / negadoctor.

## Phase status

- **Phase 0 (scaffold):** done. CMake build, libpakon, tools, unit tests.
- **Phase 1 (transport / enumerate / firmware-load):** done. Real enumeration,
  warm open, endpoint map, Intel HEX parser (unit-tested), and our own
  `f235→f135` firmware load via captured `.pakfw` replay (awaiting hardware test).
- **Phase 2 (raw bulk I/O):** done. `pakon_usb_claim`/`_release`,
  `pakon_usb_send/recv(dev, ep, …)` dispatching bulk vs interrupt, with
  timeout + stall recovery + tracing.
- **Phase 3 (framing + command primitive + open handshake):** DONE and
  **VALIDATED ON HARDWARE** (2026-05-31). `pakon_replay --open` reached Idle —
  all 5 steps OK, replies byte-for-byte matching the capture. `pakon_proto`
  build/serialize/parse (wire = 2+count); `pakon_cmd.[ch]` over EP1.
  End-to-end proven from our code: load-firmware → enumerate → claim → open.
- **Phase 4 (capture):** essentially done for the command path. `tools/
  analyze_capture.py` parses pcapng natively. Captures live on the Linux box;
  the scan capture was copied to this Mac at `/Volumes/Video/pakon_scan.pcapng`
  (the firmware-only one at `/Volumes/Video/pakon_full.pcapng`).
- **Phase 5 (scan + image):** **VALIDATED ON HARDWARE** (Linux + macOS) via
  **verbatim replay** — `pakon_replay --scan scan_fullroll.pakscan` scans a full
  roll, stops the scanner cleanly, and leaves the device healthy (a following
  `advance` works). `pakon_image.py` decodes the output to 16-bit RGB.
  - **Verbatim is the working path.** It replays every captured command in order,
    including the periodic main-scan housekeeping writes (`0103201e90`,
    `02052002060020`, per-frame `0206..` exposure writes) and the teardown tail.
    Limitation: it fits a roll the **same length** as the capture.
  - **`--scan-sm` (length-independent poll loop) STALLS on hardware.** After
    takeover it only reads `0x86` + re-arms on empty, discarding the housekeeping
    writes — and without them the readout never streams (film feeds through,
    `0x86` stays empty). Making it length-independent needs the **cadence** of
    those housekeeping commands reverse-engineered (when the device expects each),
    best done from a capture. Until then, prefer verbatim `--scan`.
- **Phase 6 (Python web service):** **DONE.** FastAPI server + single-page
  browser UI in `web/`. Wraps C tools and `pakon_image.py`. Run with
  `uvicorn web.app:app --host 0.0.0.0 --port 8000`. This is the product —
  there is no SANE backend (dropped; see `docs/EXPLORE_RUST_MIGRATION.md`).

## Confirmed hardware/protocol facts (from real captures)

- **Two firmware stages.** Physical unit is an **F-135**. It EEPROM-boots to a
  bootstrap **`0F05:F235`** (no strings), then the driver downloads stage-2
  firmware (standard FX2 fxload: `0xA0` internal RAM + `0xA3` external RAM +
  CPUCS `0xE600` reset + `0xA4 wValue=0x00A1` renumerate) and it re-enumerates
  as operational **`0F05:F135` "Pakon F135-USB Film Scanner"**. So
  **cold = f235, warm = f135**. The full firmware byte stream (1102 writes,
  ~14.8 KB) IS present in `pakon_scan.pcapng` (device 12) and is extractable.
- **Operational `f135` endpoints** (interface 0, single setting, 3 bulk eps):
  `0x01` OUT = command, `0x81` IN = reply/status, `0x86` IN = image stream.
- **Command frame wire format:** `[type][count][count data bytes]`, on-wire
  length = **2 + count** (NOT padded to 36). `data[0]` = address byte; in a
  reply `data[1]` = status. The trailing byte is a param, **not a checksum**
  (`04 03 10 00 85` vs `04 03 44 00 00`). No checksum found for short frames.
- **Open handshake (verbatim, on EP1):** `04 03 10 00 85`→`07 02 10 00`;
  `02 04 10 01 8f 00`→`07 02 10 00`; then PIC presence probes
  `04 03 <addr> 00 00`→`07 02 <addr> <present>` (AD_PICM_PLUS 0x44,
  AD_BOOT_PICM_PLUS 0x46, AD_PICM 0x24, …).
- **Command vocabulary** (2218 exchanges in a 4-frame scan): `03 01 10` poll
  (1605×), `01 03 20/24 01 02` PIC writes, `03 01 20`/`03 01 24` polls, etc.
- **Calibration/param read:** EP0 vendor control `0xA4` (OUT trigger,
  wValue=0x00A5, wIndex=0x1234) + `0xA9` (IN) reading 32-byte chunks at
  increasing offsets. To decode in Phase 5.
- **Image data NOT captured by host usbmon** under VirtualBox usbfs passthrough
  (large bulk-IN payloads aren't snapshotted; `0x86` chunks are 20480 bytes).
  Phase 5 image format needs an **in-VM USBPcap** capture.

## Environment / workflow

- Repo: forgejo `origin/main`. Code on this Mac; scanner on a Linux box over
  SSH; sync by commit + push, pull on the box.
- Scanner is passed to a **VirtualBox Windows VM** (USB filter vendor 0f05).
  VirtualBox retains it across the f235→f135 re-enumeration; scans work there.
- Linux needs `sudo` for libusb / usbmon. dumpcap drops privileges → capture to
  `/tmp` then `chown`.

## Firmware load — DONE and VALIDATED on hardware

cold=`0F05:F235`/warm=`0F05:F135`; `analyze_capture.py --extract-firmware
f135.pakfw` extracts the captured FX2 sequence; `pakon_usb_load_firmware`
replays it and waits for re-enumeration. Confirmed working on the box:
`--load-firmware resources/f135.pakfw` brought f235→f135, then `--open` reached Idle.
(a `.pakfw` is committed at `resources/f135.pakfw`. Free the device from the VM
first: shut down the VM / drop the 0f05 USB filter, replug, `lsusb`→0f05:f235.)

To repeat the working test:
```sh
python3 tools/analyze_capture.py /tmp/pakon_scan.pcapng --extract-firmware f135.pakfw
sudo ./build/pakon_probe --load-firmware resources/f135.pakfw
sudo ./build/pakon_probe            # warm f135 + endpoints 0x01/0x81/0x86
sudo ./build/pakon_replay --open    # reaches Idle
```

## Phase 5 — scan replay DONE, validated on Linux and macOS

Scan flow: OPEN → PARAM READ (0xA4/0xA9) → CONFIGURE (PICL/PICM register
writes + polls) → SCAN (interleaved status polls + `0x86` 20480-byte image
reads; ~240 MB / 4 frames). Confirmed on both Linux and macOS: 239,984,640
bytes, 11719 image reads, 2 late transfer errors (normal, ignored).

- `analyze_capture.py --extract-scan OUT.pakscan` — emits the ordered op list:
  `O <hex>` (EP1 cmd+reply), `M <n>` (image read), `C ...` (0xA4/0xA9 control).
  Verified 2218 O / 11719 M / 32 C.
- `pakon_usb_control()` — generic EP0 control transfer (for 0xA4/0xA9).
- `pakon_replay --scan FILE [--image OUT]` — replays each op, writing `0x86`
  payloads to a raw image file. Image-read timeout 5 s.

To run a scan: load film, ensure device is operational `f135`, then:

```sh
sudo ./build/pakon_replay --scan resources/scan.pakscan --image scan.raw  # Linux
./build/pakon_replay --scan resources/scan.pakscan --image scan.raw       # macOS
```

## Film advance — confirmed protocol (2026-05-31)

`advance.pakscan` captures one advance operation from the Windows driver.
Decoded structure: PICL motor-init writes → PICM enable + duration write →
`a0` (start) → HOST polls until `PS_SUCCESS` (frame in position) → `a2`
(finalize/stop). `pakon_replay` now drives this natively:

```sh
./build/pakon_replay resources/advance.pakscan            # 1 step, 60 s limit
./build/pakon_replay resources/advance.pakscan --steps N  # N frame advances
./build/pakon_replay resources/advance.pakscan --steps N --limit SEC
```

The `02 05 24 02 a5 1c 25` write sets the advance duration; the TLX UI accepts
this in **seconds** — exact binary encoding TBD from captures at known
durations. See `docs/PROTOCOL.md § Film advance protocol` for the full command
table.

## Phase 6 — Python web service (2026-05-31)

The Swift macOS app (`MacPakon/`) was abandoned — the image processing
pipeline is too slow in scalar Swift and the app is difficult to distribute.

**Replacement:** `web/` — a FastAPI service that runs on the machine with the
scanner and serves a browser UI to any device on the local network.

**Implemented:**
- `web/app.py` — FastAPI app with SSE streaming for firmware load, scan
  progress (file-size polling), and decode progress (per-step callbacks).
- `web/decode.py` — wraps `tools/pakon_image.py` decode functions; writes
  TIFFs via `tifffile` (no ImageMagick unless `--resample-to` is used);
  generates JPEG thumbnails with Pillow.
- `web/static/index.html` — single-page UI: status badge (pyusb, 2 s poll),
  firmware card (cold only), scan card (warm only), process card with file
  upload + params, scrollable frame grid, per-frame TIFF download, zip export.
- Scanner detection via `pyusb` (VID `0F05`, PID `F135`/`F235`).
- Resource paths: `resources/f135.pakfw`, `resources/scan_fullroll.pakscan`.
  Override via `PAKON_BUILD` / `PAKON_RESOURCES` env vars.

**Start:**
```sh
pip install -r web/requirements.txt
uvicorn web.app:app --host 0.0.0.0 --port 8000
```

**Open items:**
- Dual-tap gain seam fix for full-roll scans (see § dual-tap calibration above).
- Aspect-ratio resampling to 3000×2000 (UI field exists; needs ImageMagick).
- `--rotate 90` already the default in the UI.

## Handy commands

```sh
# build + test (Mac or Linux)
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure

# analyze a capture (native pcapng, no tshark needed)
python3 tools/analyze_capture.py <cap.pcapng> --bus 1               # inventory + timeline
python3 tools/analyze_capture.py <cap.pcapng> --bus 1 --device N --commands

# on the scanner box (Linux needs sudo; macOS does not)
sudo ./build/pakon_probe                         # classify + endpoint map
sudo ./build/pakon_probe --load-firmware resources/f135.pakfw   # cold f235 → warm f135
sudo ./build/pakon_replay --open                 # open handshake to Idle
sudo ./build/pakon_replay --scan resources/scan.pakscan --image scan.raw  # full scan
./build/pakon_replay resources/advance.pakscan --steps N   # advance N frames

# decode
python3 tools/pakon_image.py scan.raw --rotate 90 --frames 4
```
