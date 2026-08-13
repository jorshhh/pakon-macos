# F-135+ datalogger captures — decoded (2026-08-12)

_Working from the four KK-driver capture sessions in
`~/projects/Pakon Software/Pakon datalogger capture/` (F-135+ serial 16402,
same physical 4-exposure strip, TLXCD at Base 4/8/16 with IR off, plus Base 4
with IR on). Background: `docs/F135_PLUS_NOTES.md`. Tool:
`tools/fx35_datalogger.py`._

## Tooling

`tools/fx35_datalogger.py` parses both capture formats (PLOG ioctl logs,
EP6L image streams):

```sh
# decoded human-readable command timeline
tools/fx35_datalogger.py dump ioctl_log.bin [--grep TEXT]

# convert to a pakon_replay-compatible script; EP6 files interleave as
# M image-read lines via the shared performance-counter clock
tools/fx35_datalogger.py pakscan ioctl_log.bin out.pakscan \
    --ep6 ep6_001.bin ep6_002.bin

# strip EP6L headers -> raw image stream for pakon_image.py
tools/fx35_datalogger.py ep6 ep6_002.bin film.raw
```

The four sessions are converted in `resources/f135plus/*.pakscan`
(base4, base4_ir, base8, base16). Replies were not captured (the KK driver
logs TX only), which is fine: `pakon_replay --scan` sends commands and reads
replies without byte-verifying them.

## Wire-protocol findings (all CONFIRMED against the captures)

- **The IOCTL payload is the wire frame.** The KK driver's
  `Ezusb_Read_Write_Direct` writes `IOCTL_PAKON_SEND_AND_RECEIVE_PACKET`
  input buffers verbatim to the bulk command pipe. So the logged payloads are
  exactly our `[type][count][addr][payload_len][cmd][data…]` frames — the
  owner's `ioctl-capture-analysis.md` header reading `[rx_len][tx_len]` is
  really `[type][count]` (type 04=CMD, 02=WRITE, 01=READ, 03=READ_STATUS).
- **Addresses**: only `0x10` (HOST), `0x40` (PICL+), `0x44` (PICM+) appear.
- **The FX2 bridge layer is identical to the F-135.** The 60-pair vendor
  init is the same `0xA4 (wValue=0x00A5, wIndex=0x1234)` / `0xA9
  (wValue=offset, wIndex=0x1234)` parameter-table read documented in
  `docs/PROTOCOL.md`, same 32-byte chunks, same offsets.
- **The image stream arrives in 20480-byte bulk chunks** — every EP6 packet
  in all 8 stream files, matching the F-135's `0x86` read size exactly.
- The init/config/scan sequence matches the owner's `usb-protocol.md` model:
  per-channel exposure writes, TEC setup, iterative
  `SetCcdGainOffset`/`SetColorMatrix` calibration loop before each scan,
  `HostReady(0x84)`→`AcquireLine(0x8A)` pairs, `EngageFilmDrive(0xA0)` …
  `EndAcquisition(0x92)` + `DisengageFilmDrive(0xA2)`.

## Image stream format (CONFIRMED empirically, autocorrelation + decode)

Film-phase rows are **per-pixel interleaved R,G,B** (lag-3 autocorrelation
0.99), with the IR lane as a **trailing per-row block only when IR is on** —
exactly the layout the owner's TLA.dll decompile describes
(`usb-scan-data-decoding.md`: stream = `R[0],G[0],B[0],R[1]…`, IR "not
interleaved, separate location").

| Session | Row layout | Row stride (samples) | Width |
|---|---|---|---|
| Base 16, no IR | `[RGB ×2000]` | 6000 | 2000 px |
| Base 8, no IR | `[RGB ×1500]` | 4500 | 1500 px |
| Base 4, no IR | `[RGB ×1000]` | 3000 | 1000 px |
| Base 4, IR | `[RGB ×1000][IR ×1000]` | 4000 | 1000 px |

Consequences for the decoder (`tools/pakon_image.py`):

- Its default `--linewidth 8000` is just the F-135-with-IR case
  (2000 px × 4). **`--linewidth 6000` decodes the Base 16 no-IR F-135+
  stream correctly end-to-end** — marker/autocrop/framing/C-41 inversion all
  worked; a 4-frame strip rendered with correct colors and skin tones
  (verified visually 2026-08-12; frame pitch 3322 rows at Base 16).
- Stride must therefore be derived from scan settings (or measured), not
  hardcoded: `3 × width`, plus `width` more when IR is on.
- The trailing IR block is only handled for the 8000 case today; small
  follow-up to support `[RGB×W][IR×W]` at arbitrary W (the Base 4 IR
  session's IR lane, split off by hand, shows the expected near-uniform IR
  transparency with dust/scratch specks — the Digital ICE defect channel).
- Trilinear registration's measured leads came back implausible on these
  streams (fell back to fixed G=-8 B=-16 and still rendered clean); possibly
  the Plus firmware already line-aligns. Revisit when replaying live.

## Per-mode parameters (diff of the four sessions)

Fixed across all modes (identical bytes in every session):

- The entire 37-command cold-start init, including `SetLightConfig
  [e8 ff 18 00]`, `SetCcdExposure_G [e0 ff 20 00]`, `_B [f0 00 20 03]`,
  `_R [a0 00 70 03]`, `SetTEC_1 [00]`, `SetTEC_2 [01]`.
- The 60-pair `0xA4`/`0xA9` parameter-table read.

Varying by mode:

| Parameter | Base 4 | Base 4 IR | Base 8 | Base 16 |
|---|---|---|---|---|
| `SetScanLineParams` (0x91) | `07 01 01` (263) | `c5 00 01` (197) | `75 00 01` (117) | `3c 00 01` (60) |
| `SetCcdConfig` (0x80) values seen | 00,01 | 00,01,**02,03** | 00,01 | 00,01 |
| Width (see above) | 1000 | 1000 | 1500 | 2000 |

- `SetScanLineParams` first two bytes (LE16) are the resolution knob —
  roughly doubling as resolution halves (60 → 117 → 263); likely the CCD
  line period / accumulation count driving the downsample ratio.
- `SetCcdConfig` `02`/`03` appear **only with IR on** — matching the
  profile-mode register libpakon found on the F-135 (1 = visible, 2 = IR,
  3 = complete). This is how the 4th channel gets enabled.
- Per-session (not per-mode): `SetMotorCalibration` values, the converged
  `SetCcdGainOffset`/`SetColorMatrix` calibration values, and slight motor
  speed/config differences at scan start.

## Live replay on real hardware (2026-08-12) — IT WORKS

Ran against the owner's F-135+ (serial 16402) plugged into the Mac, macOS
libusb, no Windows anywhere:

1. **Cold enumeration**: `0f05:f235` — same "unloaded family" bootstrap ID as
   the F-135.
2. **Firmware**: the OEM INF maps personality `F235_AA05/AA07/AA08` →
   `Pakon5/7/8.hex` (model byte 05=F235, 07=F135-family, 08=F335). The
   F-135+'s EEPROM personality is `AA07` → **Pakon7.hex, the same FX2
   firmware as the F-135** — the Plus-ness lives entirely in the PIC boards.
   Verified offline that `resources/f135.pakfw` leaves exactly the
   FX35Driver `Pakon7.hex` image in RAM (10355/10355 bytes verbatim; earlier
   apparent mismatches were bootstrap bytes overwritten during load).
   `pakon_probe --load-firmware resources/f135.pakfw` warmed the scanner to
   `0f05:f135` (the final status read "timeout" is just the re-enumeration
   race).
3. **Model detection CONFIRMED**: `04 03 44 00 00` → `07 02 44 00`
   (PICM_PLUS present, status 0) and `04 03 24 00 00` → `07 02 24 01`
   (plain PICM absent) — the exact inversion of the F-135 replies, settling
   the PROTOCOL.md "(present)" mislabel (status 0 = ack/present).
4. **Full init replay**: `pakon_replay --scan resources/f135plus/
   init_base16.pakscan` (the converted OEM init, cut before any film/motor
   activity beyond the standard park) ran **67/67 commands + 60 control
   transfers with real acks** — light config, per-channel exposures, TEC
   enable, color matrix, motor init/park, module-info and sensor reads. Only
   the first two HostReset replies timed out (bridge reset flush; harmless).
5. **RX recovery works as predicted**: `PAKON_DEBUG=4` hexdumps captured
   every reply (`scratch/init_replay_trace.txt`). Observations:
   - READ replies: `01 <count> <addr> 88 <data…>` — a constant `0x88`
     marker/status byte precedes the payload.
   - `03 01 40` status poll returns `03 02 40 80` — `0x80` = host event
     pending (matches libpakon's F-135 semantics).
   - PPB_MINFO 12-byte module info: PICL+ `88 0f 0a 05 00 00 "12345"`,
     PICM+ `08 10 06 05 00 00 "12345"` (version words + placeholder serial).
   - 30-byte sensor read returns idle values with no film inserted.

One-off raw commands (pakon_probe --raw) time out if sent without the
bridge init first — the HostReset/HostSetMode + MINFO sequence is required
before PIC traffic flows.

## What this unlocks / remaining gaps

Ready now: model detection + re-addressed open handshake, then replaying
`resources/f135plus/base16.pakscan` against the real F-135+ over libusb —
which also recovers the missing RX/reply bytes as a side effect (every reply
comes back through `pakon_cmd` and is hexdumped by `pakon_log`).

Still unknown (not in these captures): the Plus's enumeration IDs and
firmware-load phase (captures start after the Windows driver is loaded), and
all reply contents (probe answers, status semantics, `0x90` sensor payload).
First hardware step stays: plug into the Mac, check what `pakon_probe` sees.
