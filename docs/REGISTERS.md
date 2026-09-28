# Pakon F-135 / F-135+ register map

The registers and commands a replay-free client writes. Rewritten 2026-09-28
from `TLB.dll` 3.1.0.28 (the F-135/F-135+ engine; details and sources in
`docs/TLB_FINDINGS.md`), cross-checked against our captures. The previous
version of this file was derived from `TLA.dll`, the **F-235** engine; where
the two disagree, this file follows `TLB.dll` and the captures.

Confidence: **[C]** read from the OEM engine or seen in our captures,
**[I]** inferred.

## The two controllers

| Role | Base F-135 | F-135+ | Carries |
|---|---|---|---|
| SCN ("scanner") | `0x24` | `0x44` | FPGA register banks `0x82`/`0x84`, film motor |
| LOW ("lower") | `0x20` | `0x40` | LEDs, lamp temperatures + TEC, DX, FIFO reset, `0x91` scan trigger |
| bootloaders | `0x26` / `0x22` | `0x46` / `0x42` | **never address these** |

Model detection: probe `04 03 <a> 00 00` at `0x44`, `0x46`, `0x24`, `0x26` in
that order; the first set that answers picks the model [C]. The EEPROM type
word (1350 base, 1351 Plus) is the cross-check.

> The historical names PICL/PICM (and "PICM = motor, PICL = light/CCD") are
> misleading: the FPGA banks and the motor share SCN. This repo's older docs
> and the reference use PICL = LOW, PICM = SCN.

## Frames

| Operation | Bytes |
|---|---|
| command | `04 03 <a> 00 <cmd>` |
| write | `02 <n+3> <a> <n> <reg> <data×n>` |
| read | `01 03 <a> <n> <reg>` |
| status poll | `03 01 <a>` |
| FPGA register write | `02 06 SCN 03 <bank> <reg> <v16 LE>` |
| FPGA register read-back | write `02 04 SCN 01 <bank+1> <reg>`, then read `01 03 SCN 02 07` |

After every command or write (except commands `0x01`, `0x0D`, `0x25`) the OEM
polls `03 01 <a>` until bit 0 (busy) clears, up to 44 tries [C]. FPGA writes
accept only banks `0x82` and `0x84` and can be verified by read-back [C].

## FPGA bank 0x84 — CCD analog front end [C]

| Reg | Field | Encoding |
|---|---|---|
| 0 | AFE config | `0x0078` at init |
| 1 | AFE config | `0x0080` at init |
| 2 / 3 / 4 | Gain R / G / B | code = round((1 − 1/g) × 75.6), g ∈ [1, 6], max `0x3F`; 13 (g ≈ 1.2) is the OEM starting value |
| 5 / 6 / 7 | Offset R / G / B | sign-magnitude: bit `0x100` = negative, magnitude ≤ 255 |

Gain `0x3F` zeroes the readout on our unit; stay below it.

## FPGA bank 0x82 — CCD timing and control [C]

| Reg | Field | Notes |
|---|---|---|
| 0 | control | `0x001` acquire, `0x002` dual-tap, `0x100` IR, `0x060` set at init → `0x160`/`0x161` in our F-135 captures, `0x60`/`0x61` (`0x63` during the Plus advance) in the F-135+ captures. Clearing bit 0 before `0xA2` is what stops the motor (see `docs/PROTOCOL.md`) |
| 1 / 2 / 3 | CCD exposure R/G/B | written 0 by `FN_bDrvInitCcd`; not the brightness lever |
| 4 | pixel start | the per-unit EEPROM **Offset** for the resolution base; ≥ 6 (≥ 3 dual-tap) |
| 5 | pixel end | start + height (height 2000); ≤ `0x848` single-tap, `0x424` dual-tap |
| 6 | integration / line period | ≤ `0xFFD`; `0x0C1A` = base 16 with IR |
| 9 | status LEDs | front-panel LED colour word, not brightness |
| 10 (`0xA`) | timing | `0x0400` at init |
| 11 (`0xB`) | timing | written 0 with the integration setting |

## LOW — LEDs [C]

| Cmd | Payload | Meaning |
|---|---|---|
| `0x80` | 1 byte | LED enable: bit 0 visible, bit 1 IR. `02 04 LOW 01 80 00` = all off |
| `0x81` | 5 bytes | LED currents, order **`B, IR, R, 0, G`**, each clamped to the ceiling below |
| `0x82` | 12 bytes | u16 LE **`dutyB, dutyIR, dutyR, 0, dutyG, period`**; period = IntegrationTime × 0.6 (pixel clock 833333.3 Hz); duty ticks ≤ period − 2 |

LED current ceilings (the OEM clamps to these; do not exceed):

| Board | IR off R / G / B / IR | IR on R / G / B / IR |
|---|---|---|
| F-135 (SCN `0x24`) | 6 / 8 / 8 / 0 | 8 / 8 / 8 / 8 |
| F-135+ (SCN `0x44`) | 4 / 20 / 20 / 0 | 8 / 24 / 24 / 8 |

The LEDs stay on from before the scan to after it; nothing refreshes them and
the motor does not gate them. The OEM turns them on with the motor stopped for
calibration [C]. Our "no light in static calibration" result is most likely
because `0x81`/`0x82` were never sent with non-zero values [I, to test].

## LOW — temperatures and TEC [C]

| Cmd | Meaning |
|---|---|
| `0x8E` | lamp temperature setpoint (`LampTempWorking`, 37–48 °C) |
| `0x8F` / `0x8C` | lamp warning / fault band |
| `0x8B` / `0x8D` | motherboard warning / fault band |
| `0xD0 00`, `0xD1 01` | TEC setup, sent on both models; replay as-is, never sweep |
| `0x83` (read 1) | lamp status flags |
| `0x84` (read 2) | setpoint read-back |
| `0x88` (read 4) | lamp + motherboard temperatures |

Units: 0.0625 °C per count. These were previously mislabelled "CCD exposure /
LED geometry" in this repo.

## LOW — scan, FIFOs, DX [C]

| Cmd | Meaning |
|---|---|
| `0x8A` (command) | **ResetFifos**, preceded by host `02 04 10 01 84 02`. Not "AcquireLine" |
| `0x91` (write 3) | scan-line trigger: u16 value + mode byte; also resets the DX position counter. Value per resolution/IR in `docs/TLB_FINDINGS.md` |
| `0x92` (command) | end acquisition / DX window |
| `0x90` (read 30) | sensor/DX entries; read only when flagged by the event service |
| `0x93` (read 4) | the four DX detector levels |
| `0x94` / `0x96` | DX hardware setup / DX pot values (calibration only) |

## SCN — film motor [C]

| Cmd | Meaning |
|---|---|
| `0xA5` (write 2) | motor speed, u16 = MotorSpeed(_Ir) × MotorAdjust / 1000 from the EEPROM; clamp base 400–9500, Plus 1000–32766. Captured values: F-135 scan `0x0615`, advance `0x251C`; F-135+ (serial 16402) Base 16 scan `0x170C`, advance `0x647E` |
| `0xA0` | start forward |
| `0xA1` | start reverse |
| `0xA2` | release the drive; clear FPGA reg 0 bit 0 first or the motor keeps running |
| `0x97` | controller init |

## Event registers (both controllers) [C]

| Reg | Use |
|---|---|
| 2 (read 1) | pending-event status byte; reply flag byte bit `0x80` = event pending |
| 6 (write 2) | acknowledge: `00 <status>` clears the bits that were set |

See `docs/PROTOCOL.md` → "Event service".

## Per-unit values (EEPROM `0x52`, read with `0xA4`/`0xA9`)

Layout in pakon-reference `calibration.md`. How the engine uses it [C]:

| Field | Feeds |
|---|---|
| type word `0x00C` (1350 / 1351) | model |
| Offset per base (`0x014`/`0x01A`/`0x020`) | bank `0x82` reg 4 (and reg 5 = Offset + height) |
| MotorSpeed, MotorSpeed_Ir per base | motor `0xA5`, scaled by the adjust words |
| section B words (`0x808`–`0x81E`) | per base: MotorAdjust, MotorAdjustDrag, MotorAdjust_Ir, MotorAdjustDrag_Ir (clamp 900–1100) |
| NegMatrix (`0x026`), PosMatrix (`0x09E`) | colour transform on density data (`docs/IMAGING.md`) |

Light calibration (LED currents/duties, gains, offsets, `DetectWhite_G`,
`DetectFilm_G`) is **not** in the EEPROM; the OEM keeps it in the registry and
re-derives it every 20 h. A replay-free client measures it itself.

## Our unit's converged values (serial 3054, for sanity checks only)

From `pakon_scan.pcapng`; shipped as `pakon_calib_default_config()`. Per-unit;
do not reuse on another scanner.

| Register | Value |
|---|---|
| bank 84 gain R/G/B | 13 / 13 / 13 |
| bank 84 offset R/G/B | −38 / −31 / −31 |
| bank 82 reg 0 | `0x0160` |
| bank 82 reg 4 / 5 | `0x002B` / `0x07FB` |
| bank 82 reg 6 | `0x0C1A` |
| bank 82 reg 9 | `0x001F` (status LEDs) |
| bank 82 reg 10 | `0x0400` |
| bank 84 reg 0 / 1 | `0x0078` / `0x0080` |
