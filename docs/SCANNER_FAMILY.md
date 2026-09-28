# The Pakon F-X35 scanner family — model differences

What is known about the four models this project may meet: **F-135**,
**F-135+** ("Plus"/"Hybrid"), **F-235**, **F-335**. Everything here is
sourced; the marker convention is [CONFIRMED] (verified on hardware or read
directly from OEM files/driver source), [DOCUMENTED] (stated in OEM readmes,
INF files, or shipped documentation, not verified by us), and [INFERRED].

Sources: this repo's live runs on a real F-135 and F-135+ (`docs/PROTOCOL.md`,
`docs/F135_PLUS_CAPTURES.md`), the OEM driver package and firmware readmes
(`ReadmeF135/235/335.txt`, `F235usb2.inf`, `FirmwareLoader`), Kai Kaufman's
open-source FX35 driver source, and libpakon's device catalog.

## At a glance

| | F-135 | F-135+ | F-235 | F-335 |
|---|---|---|---|---|
| USB cold (bootstrap) | `0f05:f235` | `0f05:f235` | `0f05:f235` | `0f05:f235` |
| USB warm (operational) | `0f05:f135` | `0f05:f135` | `0f05:35f2` | `0f05:f335` |
| FX2 firmware | Pakon7.hex | Pakon7.hex (same) | Pakon5.hex | Pakon8.hex |
| Personality revision | AA07 | AA07 | AA05 | AA08 |
| PIC bus addresses | 0x20/0x24 | 0x40/0x44 | unknown | unknown |
| Controller boards | PICL (PL) + PICM (PM), PIC16 | PICL+ (NL) + PICM+ (NM), PIC18 | MC, DX, LP, CD, AP boards | MD, LQ, DY, CE, AP boards |
| Illumination | lamp | lamp | lamp (LP board) | **LED** (LQ board) |
| CCD cooling (TEC) | no | **yes** | unknown | unknown |
| DX barcode reading | in PICL | in PICL+ | dedicated DX board | dedicated DY board |
| APS (IX240) film | no | no | yes (AP board) | yes (AP board) |
| Status in this project | fully working | fully working | untested | untested |

## Identity and firmware selection

- **Every model cold-enumerates as `0f05:f235`** — the "unloaded family"
  bootstrap identity from a C0-type FX2 EEPROM (VID/PID/revision/personality
  only; the host must download firmware). [CONFIRMED for F-135 and F-135+ on
  hardware; DOCUMENTED for F-235/F-335 via the shared driver package and
  libpakon's catalog]
- The driver reads an **8-byte personality** with vendor request `0xA9`
  (`id 0xC0, wVendorId, wProductId, wRevision, extra`), then selects the FX2
  image by registry key `<PID>_<revision>`: `F235_AA05` → Pakon5.hex,
  `F235_AA07` → Pakon7.hex, `F235_AA08` → Pakon8.hex, with `PknInit.hex` as
  the shared bootstrap stage. [CONFIRMED from `F235usb2.inf` + the FX35
  loader source; the AA07 path verified live on an F-135+]
- **The F-135 and F-135+ share everything at the USB level**: same cold and
  warm IDs, same FX2 firmware (`resources/f135.pakfw` works for both), same
  endpoints (0x01 cmd OUT, 0x81 reply IN, 0x86 image IN), same 20480-byte
  image chunks, same `0xA4`/`0xA9` parameter-table read. The model cannot be
  told apart from USB descriptors. [CONFIRMED on both models]
- **Why the Plus shares the F-135's personality (AA07)**: the personality
  only selects the FX2 image, and the FX2 is a plain USB-to-bus bridge that
  is identical on both models — everything Plus-specific sits behind it on
  the PIC boards and is discovered by the driver's presence probes at
  session open. Consistent with this, the OEM `FirmwareLoader/Personalities/`
  folder has no Plus-specific EEPROM image, and the INF has no Plus key.
  [DOCUMENTED; nobody has read the raw personality bytes off an F-135+
  EEPROM directly — the strong indirect proof is that the personality-driven
  OEM loader runs the Plus with Pakon7.hex]
- Warm PIDs for the larger models: F-235 `0f05:35f2`, F-335 `0f05:f335`.
  [DOCUMENTED: OEM INF/notes and libpakon's catalog agree]

## Telling models apart: the PIC presence probes

The OEM opens a session and probes the candidate controller addresses with
`04 03 <addr> 00 00`; reply `07 02 <addr> <status>` with status 0 = that PIC
acked (present), 1 = absent. [CONFIRMED on both F-135 family models]

| Address | Meaning |
|---|---|
| 0x10 | HOST (FX2/PPB bridge, all models) |
| 0x20 / 0x22 | F-135 PICL / its boot loader |
| 0x24 / 0x26 | F-135 PICM / its boot loader |
| 0x40 / 0x42 | F-135+ PICL+ / boot |
| 0x44 / 0x46 | F-135+ PICM+ / boot |

A real F-135+ answers 0x44 present / 0x24 absent — exactly inverted from an
F-135. `pakon_replay --open` uses this as model detection. Whether the
F-235/F-335 controllers appear at these PPB addresses or others is unknown;
their board architecture differs enough that no assumption is made.

## F-135 vs F-135+ (both verified in this project)

Same command dialect, different execution:

- **Addresses**: every PIC-directed frame targets 0x20/0x24 on the F-135 and
  0x40/0x44 on the Plus. This is the entire reason F-135 capture scripts NAK
  on a Plus. [CONFIRMED]
- **PIC hardware/firmware**: F-135 boards run PL/PM firmware on PIC16; the
  Plus runs NL/NM on PIC18 (its NL050A disassembles as PIC18 with ADC, PWM,
  SPI to the FX2, four timers). [CONFIRMED from firmware disassembly]
- **Plus-only hardware**: a TEC (thermoelectric cooler) on the CCD,
  configured every init (`0xD0` setpoint, `0xD1` enable), and DX-code
  reading integrated into PICL+. [CONFIRMED in captures/live init]
- **Image stream**: both send 16-bit LE samples in 20480-byte chunks; rows
  are per-pixel interleaved R,G,B with an IR block only when IR/ICE is on.
  Width follows resolution on the Plus (2000/1500/1000 px at Base 16/8/4).
  [CONFIRMED; see `docs/F135_PLUS_CAPTURES.md` for the stride table]
- **Throughput**: the Plus is marketed as roughly double the F-135's speed.
  [INFERRED from OEM material; not measured here]
- The OEM PICM firmware readme lists "Add F135 Plus/Hybrid support" as a
  PM revision note, so late F-135 motor firmware knows about the Plus
  variant. [DOCUMENTED]

## F-235 (untested here)

[DOCUMENTED, from `ReadmeF235.txt` and the OEM package unless noted]

- Separate boards with their own firmware files: `MC` motor control (PIC16
  or PIC18 depending on PCB revision), a **dedicated `DX` board** (its own
  microcontroller for DX barcode reading), `LP` lamp board (revisions with
  and without fan tachometer), `CD` CCD board (`CD010F.hex`, "the one and
  only"), and `AP` APS board for IX240 film.
- Wider imaging path than the F-135 family: TLA.dll's frame detection has a
  perforation-based stage that only makes sense for models that image the
  full film width including sprocket holes. [INFERRED from TLA.dll
  decompilation]
- Same host software stack (TLX/TLA) and the same FX2 bridge chip; firmware
  is `Pakon5.hex` via personality `AA05`. Image stream format, PPB
  addressing, and scan protocol are unverified.

## F-335 (untested here)

[DOCUMENTED, from `ReadmeF335.txt` unless noted]

- `MD` motor board ("raised limit on motor speed", June 2006), `LQ` **LED
  illumination board** (the only model not using a lamp), `DY` DX board,
  `CE` CCD board, `AP` APS board.
- Otherwise the same situation as the F-235: shared host stack, firmware
  `Pakon8.hex` via `AA08`, everything protocol-level unverified.

## What would make a F-235/F-335 work here

The transport layer almost certainly carries over (same FX2 bridge, same
driver family, personality-selected firmware already handled by the OEM
files in `FX35Driver/`). The unknowns are the PPB addressing of their
multi-board architecture, their command dialects, and their image stream
geometry. The path is the same one that worked for the F-135+: get a
driver-level or USB capture of the OEM software driving the real machine,
convert with `tools/fx35_datalogger.py` or `tools/analyze_capture.py`, and
replay. If you have one of these scanners and a Windows machine, that
capture is the single most useful contribution.
