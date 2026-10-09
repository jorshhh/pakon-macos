# Pakon F-135 / F-135+ protocol

What we know about the wire protocol, written for the replay-free client.
Rewritten 2026-09-28; the phase-by-phase history is in git. Sources, kept
distinct (see PROVENANCE):

- our own USB captures (`resources/*.pcapng`, `*.pakscan`) and hardware runs on
  an F-135 (serial 3054);
- F-135+ driver-level captures and live runs by Ali Bosworth (serial 16402;
  `resources/f135plus/`, `docs/F135_PLUS_CAPTURES.md`);
- `TLB.dll` 3.1.0.28, the OEM F-135/F-135+ engine (`docs/TLB_FINDINGS.md`);
- [pakon-reference](https://github.com/alibosworth/pakon-reference) for the
  family-wide facts (EEPROM layout, DX, safety).

Register and command tables are in `docs/REGISTERS.md`; image format in
`docs/IMAGING.md`.

## Identities and firmware

| State | VID:PID | Meaning |
|---|---|---|
| cold | `0f05:f235` (rev `aa07`) | FX2 bootstrap from the boot EEPROM; no scanner protocol |
| warm | `0f05:f135` "Pakon F135-USB Film Scanner" | operational after the host loads firmware |

F-135 and F-135+ are identical over USB; the model is only known from the
controller probes. The load is standard FX2: CPUCS `0xE600` hold → `0xA0`
internal RAM + `0xA3` external RAM → `0xA4 wValue=0x00A1` → re-enumerate. Our
tool currently replays the captured sequence (`resources/f135.pakfw`); loading
from the Intel HEX is step 1 of the roadmap in `STATUS.md`. Firmware is RAM
only; a power cycle returns the unit to cold.

## Endpoints

Interface 0, one setting, three bulk endpoints:

| EP | Dir | Role |
|---|---|---|
| `0x01` | OUT | command frame |
| `0x81` | IN | reply |
| `0x86` | IN | image stream (OEM reads 20480-byte transfers; endpoint max packet 512) |

EP0 vendor requests: `0xA0`, `0xA2`–`0xAC` (the OEM driver rejects others).
`0xA4`/`0xA9` read the EEPROMs; **`0xA2` writes the per-unit EEPROM: never
send it.**

## Frames

`[type][count][data × count]`, on-wire length `2 + count`, no checksum, no
padding. `data[0]` is the bus address.

| Type | Use |
|---|---|
| `0x01` | READ: `01 03 <a> <n> <reg>` |
| `0x02` | WRITE: `02 <n+3> <a> <n> <reg> <data…>` |
| `0x03` | STATUS poll: `03 01 <a>` |
| `0x04` | COMMAND: `04 03 <a> 00 <cmd>` |
| `0x07` | reply to COMMAND/WRITE (never sent by the host) |

**Type `0x00` wedges the FX2 until a power cycle. Never send it.**

### Replies

| Request | Reply |
|---|---|
| COMMAND, WRITE | `07 02 <a> <status>` |
| STATUS | `03 02 <a> <flags>` (host replies carry a trailing `0xaa`) |
| READ | `01 <count> <a> <flags> <data…>` |

OEM reply handling (`PPB_CheckReply` in `TLB.dll`) [C]:

- `07` replies: status `0`/`8` OK; `3`, `6`, `9` retry; `1`, `2`, `4`, `5`, `7`
  hard error. Retries: 3 per transfer.
- `01`/`03` replies must echo the request type. Flags byte: bit `0x01` busy
  (retry), `0x10` retry, `0x04` error (except from host), `0x20` error, `0x80`
  event pending.
- Transfer timeout 2 s.

After every COMMAND/WRITE except commands `0x01`, `0x0D`, `0x25`, poll
`03 01 <a>` until flags bit 0 clears (≤ 44 tries, growing delay) [C].

## Bus addresses

| Addr | Device |
|---|---|
| `0x10` | host / FX2 bridge |
| `0x20` / `0x24` | base F-135 LOW / SCN |
| `0x40` / `0x44` | F-135+ LOW / SCN |
| `0x22` `0x26` `0x42` `0x46` | bootloaders: **never address** (type-4 commands `0x0C`–`0x0F` erase flash) |
| `0x28` | focus steppers (only with the OEM focus option) |

Do not send to any other address; unknown addresses can be the I2C EEPROMs.

## Open handshake [C, validated on hardware]

```
04 03 10 00 85        HostReset        -> 07 02 10 00
02 04 10 01 8f 00     HostSetMode 0    -> 07 02 10 00
04 03 44 00 00        probe SCN Plus   -> 07 02 44 <0 present / 1 absent>
04 03 46 00 00        probe boot Plus
04 03 24 00 00        probe SCN base
04 03 26 00 00        probe boot base
```

The bridge answers HostReset/HostSetMode only on the first open after power-on
or firmware load; later opens time out on those two replies while working
normally. Treat them as best effort.

## EEPROM read (`0xA4` / `0xA9`) [C]

Per 32-byte chunk: vendor OUT `0xA4`, `wValue 0x00A5`, `wIndex 0x1234`, no data;
then vendor IN `0xA9`, `wValue` = byte offset, `wIndex 0x1234`, `wLength ≤ 32`.
`wValue 0x00A3` selects the boot EEPROM at `0x51` instead. Read both copies of
both sections and check the CRCs (layout: pakon-reference `calibration.md`).
Our unit: type 1350 (base F-135), serial 3054. `tools/pakon_eeprom.py` reads and
archives all four copies and decodes them; see `docs/EEPROM_BACKUP.md`.

## Calibration reads (motor stopped) [C]

Measured on serial 3054, 2026-10-09 (`pakon_replay --calib-probe`,
`--light-cal`):

- With acquire on (bank `0x82` reg 0 bit 0) and the motor stopped, `0x86`
  streams lines continuously. After changing the light or the A/D settings,
  re-arm with ResetFifos and read.
- A line is `4 × (reg5 − reg4)` u16 samples (8148 with the calibration window
  6..2043): the RGB block (pixels interleaved R, G, B), then the IR block.
- No sample carries the scan-time marker bit, and where a read starts within a
  line varies between reads. Align each read on the optically black pixels:
  the first ~22 pixels of each block are black, then the level ramps up to the
  open-gate level by pixel ~37.
- After an A/D offset write, wait ~300 ms before measuring the dark level;
  without it the reading varied by about ±50 counts (one offset code ≈ 54).

## Event service (the "housekeeping") [C]

The OEM runs a dedicated thread (`Thread_PpbInterrupt`) for the whole session:

1. Poll `03 01 10` every **200 ms**, or every **1 ms at high priority while
   scanning**. Nothing to do unless flags bit `0x80` is set (bit `0x02` = FIFO
   overflow).
2. For LOW, then SCN: read `01 03 <a> 01 02`. If the reply flag byte has
   `0x80`, acknowledge with **`02 05 <a> 02 06 00 <status>`**.
3. LOW status & `0xA4` → read the sensor/DX block `01 03 LOW 1e 90`.
4. LOW status & `0x5B` → lamp status: read `0x83` (flags), `0x84`, `0x88`
   (temperatures) and compare with the warning/fault bands.
5. Update the status LEDs (FPGA bank `0x82` reg 9) from the result.

This is the traffic our captures show at irregular read indices
(`02052002060020`, `0103201e90`). Without it the driven `--scan-sm` died
about 38% into a strip; with it (`pakon_cmd_service_events` after every image
read) a whole strip scanned on serial 3054, 2026-10-09 [C].

In our captures every ack is from LOW, and `0x90` follows 93 of 95 acks
whatever the status (not only on `0xA4`); `0x84`/`0x88` follow every `0x02`
and `0x40` ack.

## Scan (OEM sequence) [C]

`FN_bBeforeScan` → `FN_iScanStrips` → `FN_bAfterScan`:

1. Clear pending events (run the event service once).
2. ResetFifos: `02 04 10 01 84 02`, `04 03 LOW 00 8a`.
3. Calibrate if there is no calibration, it is older than the configured
   interval, or it is forced (motor stopped; `docs/TLB_FINDINGS.md`).
4. FPGA settings: bank `0x82` reg 0 (control, IR bit), reg 4/5 (pixel window
   from the EEPROM Offset), reg 6 (integration period); bank `0x84` gains and
   offsets.
5. LEDs on: LOW `0x80`, `0x81`, `0x82`.
6. Motor: `02 05 SCN 02 a5 <speed16>`, `04 03 SCN 00 a0`.
7. Acquire: bank `0x82` reg 0 = control | 1 (`0x160`→`0x161` on our F-135,
   `0x60`→`0x61` on the F-135+), then `02 06 LOW 03 91 <v16> <mode>`.
8. Read `0x86` continuously into a ring buffer. **No per-line commands.** Keep
   the event service running.
9. End of film is decided on the host (below). Stop acquiring: bank `0x82`
   reg 0 back to control without bit 0.
10. Teardown (`FN_bAfterScan`): `02 04 LOW 01 80 00` (LEDs off) → ResetFifos →
    `04 03 LOW 00 92` → wait 20 ms → `04 03 SCN 00 a2`.

**Stopping the motor.** `a2` alone left the motor running in the August 2026
tests; writing bank `0x82` reg 0 back to its idle value stopped it. That write
was described at the time as a "motor speed register 0"; it is the FPGA
control register, and the value clears the **acquire** bit (`0x61` → `0x60`).
The OEM does the same in step 9, before `a2`. So: clear the acquire bit, then
`a2` [C for the order in captures and `TLB.dll`; the causal link is I].

## End of film [C]

The scanner sends no end-of-roll signal:

- Each line's first word must have LSB = 1 (the marker bit); a miss is
  `LostSync` (or `FifoOverflow` if host flags bit `0x02`).
- The line level (green, [I]) is compared with the calibrated `DetectFilm_G` and
  `DetectWhite_G`, re-based from the first 3 lines: film starts when the level
  drops below DetectFilm and ends when it rises back to DetectWhite.
- `NoFilmTimeOut` (seconds) and `ScanPacketReadyTimeOut` (ms) bound the waits.

## Film advance

The OEM moves film with the motor commands above: set speed (`0xA5`), start
`0xA0` (or `0xA1` reverse), stop `0xA2`. Our tool currently replays
`resources/advance.pakscan` (`pakon_replay advance.pakscan --steps N`), whose
`02 05 24 02 a5 1c 25` is that speed write; replacing it is roadmap step 7.

## Current tool behaviour

| Mode | What it does |
|---|---|
| `pakon_replay --open` | synthesized handshake + probes; prints `model detected: F-135` or `F-135+` |
| `--read-params` | synthesized EEPROM read |
| `--configure` | synthesized FPGA writes (values from our unit) |
| `--calibrate` | driven dark-offset loop (works); gain phase sees no light |
| `--scan FILE` | verbatim replay of a capture; fits a roll the capture's length |
| `--scan FILE --autostop` | verbatim replay, stops at end-of-roll white, then replays the teardown |
| `--scan-sm FILE` | replays setup to motor start, then reads with the event service; scans a whole strip (F-135) |
| `advance.pakscan --steps N` | replayed advance loop; motor address probed per model (`resources/f135plus/advance.pakscan` for the Plus) |
| `--advance [--advance-seconds N]` | timed transport run to push a strip out, standalone or after `--scan`; model-specific captured speed |
| Ctrl-C during `--scan`/`--advance` | first press stops and replays the teardown / stop writes; the run reports failure |

## PROVENANCE

Facts come from (1) our own captures and hardware runs and (2) reverse
engineering of the original Kodak/Pakon Windows software (`TLB.dll`,
`PakonIMAu.dll`) for interoperability, plus (3) the public pakon-reference.
The OEM binaries and decompiler output are third-party copyrighted material
and are **not** in this repository (`pakon-scanning-software/` and `re/` are
git-ignored). Only interface facts needed for interoperability are recorded.
