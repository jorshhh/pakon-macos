# Pakon F-X35 protocol — living document

This file records what we actually know/decode about the wire protocol, updated
phase by phase. Anything not confirmed from documentation or our own captures is
marked **TBD** or **inferred** — do not treat inferred items as ground truth.

## Identities

**Two firmware stages (observed on the F-135 unit):**

1. **Bootstrap / "cold": `0F05:F235`**, class `0xff`, **no string descriptors**.
   Auto-loaded from the onboard EEPROM on power-up. Its job is to receive a
   second-stage firmware download. It does **not** implement the application
   protocol — every bulk-OUT NAKs (see Phase 2 result below).
2. **Operational / "warm": `0F05:F135` "Pakon F135-USB Film Scanner"** (has a
   product string), reached after the working (Windows) driver downloads
   stage-2 firmware. This matches the physical model and the documented PID.

So the correct mapping is **cold = `0F05:F235`, warm = `0F05:F135`**, and the
`F235 → F135` firmware download IS needed for standalone operation (it is not
optional — the EEPROM only gets us to the bootstrap). The mechanism of that
download (standard FX2 `0xA0` to RAM vs something else) is to be confirmed from
the Phase 4 capture. PID still must not be used as a model indicator.
- **Cold (FX2 bootloader):** **TBD** — confirm via `lsusb` (Linux) or
  `pakon_probe --list` / `system_profiler SPUSBDataType` (macOS) on a freshly
  powered scanner before any driver loads (Phase 1, STOP POINT A). Not guessed:
  `PAKON_COLD_VID/PID` in `include/pakon_usb.h` are 0 until confirmed, and the
  firmware-download path hard-refuses to run while they are 0.

## Firmware download (f235 → f135) — decoded from capture

Confirmed from a real capture (`tools/analyze_capture.py`) of the working
driver loading the bootstrap `0F05:F235` device. Standard two-stage EZ-USB
FX2 fxload, all over EP0 vendor control transfers:

- `0xA0 wValue=0xE600 data=01` — write CPUCS, hold the 8051 in reset.
- `0xA0 wValue=0x7F92 data=01` — a firmware-specific byte poked before reset.
- **391× `0xA0`** writes — to internal 8051 RAM.
- **711× `0xA3`** writes — to external RAM (the second-stage loader path used
  for the bulk of the firmware; `0xA3` is implemented by the first-stage code).
- `0xA4 wValue=0x00A1 wLength=0` — finalize / renumerate trigger; the device
  then re-enumerates as operational `0F05:F135`.
- `0xA9 wLength=8` (IN) — an 8-byte readback right after (status/version?).

Implication: standalone firmware load is feasible with the FX35Package `.hex`
blobs + this sequence (extends our current `0xA0`-only stub to add `0xA3` and
the `0xA4` finalize). Still TODO: the actual `.hex` provenance/mapping.

## Command frame (documented)

36-byte fixed frame:

| Offset | Field | Notes |
|-------:|-------|-------|
| 0      | `type`  | packet type byte (see below) |
| 1      | `count` | length of `data`, max 34 |
| 2..35  | `data`  | 34 bytes; `data[0]` is an address byte |

### Address byte (`data[0]`) — documented

| Name                | Value |
|---------------------|------:|
| AD_HOST             | 0x10 |
| AD_PICL             | 0x20 |
| AD_BOOT_PICL        | 0x22 |
| AD_PICM             | 0x24 |
| AD_BOOT_PICM        | 0x26 |
| AD_PICL_PLUS        | 0x40 |
| AD_BOOT_PICL_PLUS   | 0x42 |
| AD_PICM_PLUS        | 0x44 |
| AD_BOOT_PICM_PLUS   | 0x46 |

### Status byte (scanner→host) — CONFIRMED (OEM decompilation)

`0` success, `1` not acked, `2` invalid packet, `3` bad checksum, `4`–`6`
USB-related, `7` host algorithm error, `8` success, `9` bus error.

The OEM error-string table (`TLA.dll`) corroborates this exactly — each status
maps to a named `EC_DRV_PacketHostError*` code:

| status | OEM error name |
|---|---|
| 1 | `EC_DRV_PacketHostErrorNoAck` (0x3f3) |
| 2 | `EC_DRV_PacketHostErrorFormat` (0x3f4) |
| 3 | `EC_DRV_PacketHostErrorCkSum` (0x3f5) |
| 4–6 | `EC_DRV_PacketHostError{EndPointFormat,EndPointTimeOut,EndPointLength}` (0x3f6–8) |
| 7 | `EC_DRV_PacketHostErrorAlgo` (0x3f9) |
| 9 | `EC_DRV_PacketHostErrorBus` (0x3fa) |

Plus device-side conditions worth handling: `RingTailOverflow` (0x3ea),
`LostSync` (0x3eb), `FifoOverflow` (0x3ee), `PacketBusy` (0x3ed),
`TransferInProgress` (0x3fe).

### Packet `type` byte — reply semantics CONFIRMED (OEM decompilation)

The names `PH_CMD`, `PH_READ_STATUS`, `PH_INVALID` are documented but their
numeric values are still not individually mapped. Observed on the wire (open
handshake): host command frames begin `0x04`, the reply begins `0x07`.

The OEM command-transaction routine (`TLC.dll FUN_1000c800`, see "Host transport"
below) validates the **reply** `type` byte (`reply[0]`) as:

| `reply[0]` | meaning |
|---|---|
| `7` | **success** (the normal reply, e.g. `07 02 10 00`) |
| `1` or `3` | OK **iff** it echoes the sent `type` byte (status-class reply) |
| anything else | error `EC_DRV_InvalidPacketType` (0x3ec) |

### Checksum — short frames carry a param byte, not a checksum

The OEM error table defines `EC_DRV_PacketChecksumErr` / `…HostErrorCkSum`, so a
checksum mechanism exists in the protocol generally, but the short command frames
we send/replay carry **no trailing checksum** — the OEM transaction sends exactly
`count + 2` bytes (`packet[1] + 2`) with no checksum appended, and the trailing
`0x85` in the open packet is a command/parameter byte (the analogous
`04 03 44 00 00` ends in `00`). No checksum derivation is needed for the frames
in our scan/advance paths.

## Endpoints — CONFIRMED from scan capture (operational `0F05:F135`)

The operational `f135` device exposes interface 0, **single setting, 3 bulk
endpoints** (this is the "3 endpoints" the original note referred to):

| Endpoint | Dir | Role (confirmed) |
|----------|-----|------------------|
| `0x01`   | OUT | **command** channel (host → device) |
| `0x81`   | IN  | **command reply / status** (device → host) |
| `0x86`   | IN  | **image stream** (bulk image data) |

From the 4-frame scan capture (device 13): `BULK OUT 0x01` and `BULK IN 0x81`
= **2218 exchanges** each (one reply per command); `BULK IN 0x86` = **11719**
transfers (the image). So a command is: write a frame on `0x01`, read the reply
on `0x81`; image bytes stream from `0x86`.

> The earlier 6-endpoint / 4-alt-setting map was the **`f235` bootstrap**
> descriptor (and is why `--probe-open` NAK'd — the bootstrap implements no app
> protocol). The operational `f135` is the simple 3-endpoint device above.

## Host transport — CONFIRMED from OEM software decompilation

The original Kodak/Pakon Windows software was reverse-engineered (for
interoperability) with Ghidra to corroborate the capture-derived protocol. See
`PROVENANCE` note at the end of this file. Key structural findings:

### The Windows kernel driver is stock Cypress EZ-USB

`F235Lib.sys` is the DDK "GenericUSB" sample (exports `GenericRegisterForIdleDetection`,
`StartPacket`, `StallRequests`…); `F235Ldr.sys` is the stock EZ-USB `Ezusb_StartDevice`
fxload; `F135usb2.sys`/`F235usb2.sys`/`FX35usb2.sys` are thin per-model PnP dispatch
shims. **The driver carries no Pakon protocol knowledge** — it just wraps bulk and
control transfers behind generic IOCTLs. All protocol logic lives in user space in
the `TLA/TLB/TLC.dll` layer. The device is exposed as the symlink `\\.\Pakon135`
(or `\\.\PakonX35`).

### Two device IOCTLs map directly to our libusb calls

**Command frames — `IOCTL 0x222090`** (`TLC.dll FUN_1000c800`): a single
bidirectional `DeviceIoControl` that the driver fulfils as an EP1 OUT→IN round
trip:

```
DeviceIoControl(dev, 0x222090,
                packet, packet[1] + 2,   // input  = frame, length = count + 2
                reply,  0x40,            // output = reply, <= 64 bytes
                &nret, OVERLAPPED);
WaitForSingleObject(replyEvent, 2000);   // 2 s reply timeout
// then validate reply[0] per the "type byte" table above (7 = success)
```

- Independently **confirms our wire length = `count + 2`** (no 36-byte padding).
- Maps to libusb: bulk OUT `0x01` (len `count+2`) then bulk IN `0x81` (≤64 B) —
  exactly what `pakon_cmd` does. The 2 s reply timeout is the OEM value.

**EP0 vendor/class control — `IOCTL 0x222059`** = `IOCTL_EZUSB_VENDOR_OR_CLASS_REQUEST`
(`TLC.dll FUN_1001db50`): a 10-byte setup buffer (`[4]=bRequest`, `[5..6]=wValue`,
`[7..8]=wIndex`), data stage ≤ `0x5000` bytes. The code **validates
`bRequest ∈ {0xA0} ∪ {0xA2..0xAC}`** — this defines the entire vendor-request code
space (previously "undocumented, do not guess"). `0xA0` = FX2 RAM load; the
`0xA4`/`0xA9` calibration-table pair sits in range. Maps to `pakon_usb_control()`.

### Parameters are written via a generic `WriteRegister` helper

Scan/calibration parameters are **not** bespoke packets. They funnel through a
register-write helper family (`TLA.dll FUN_1000e510(commObj, ctx, bank, reg,
value16, flags)`, short form `FUN_1002f880(this, ctx, reg, value, flags)`) that
emits `type=0x02` frames `02 <count> <addr> 03 <bank> <reg> <value16>` (banks
`0x82` = CCD timing/exposure, `0x84` = CCD analog front-end; writes are read-back
verified). So the advance write `02 05 24 02 a5 1c 25` is the same family targeting
the motor (`WriteRegister(addr=0x24 PICM, reg=0x02, value=24-bit 0x251ca5)`).
**The full decoded register map (gain/offset/exposure/height) is in
`docs/REGISTERS.md`** — the foundation for a capture-free driven backend.

### Scan engine is a producer/consumer ring buffer (not a poll loop)

`CiScanner::bScanStrips` (`TLC.dll FUN_10032420`) spawns two threads — a
driver-read thread doing **free-running overlapped `ReadFile` on bulk-IN `0x86`**
(priority raised to above-normal; `ERROR_IO_PENDING` is the normal return) and a
write-to-disk thread — backed by a ring buffer. State variables (from its debug
template): `iStopScan, StopDriver, DriverRunning, StopTransfer, TransferInProgress,
overflow, EndTime, Reading, ToRead, Writing, NF`; events `EventScanPacketReady`,
`EventScanWriteToDisk`. It stops on a device end-signal / `StopScan`, **not** on a
fixed byte count — consistent with our end-of-roll-white auto-stop approach.

### OEM-enforced scan parameter ranges (use as scan-option bounds)

From the scan-request validator in `TLA.dll`:

| param | range |
|---|---|
| `iHeight` | 100 – 2114 (0x842) |
| `iOffset` | 6–650, or 3–325 (mode-dependent) |
| `iMotorSpeed` | 60–2200 (0x3c–0x898), or 200–3700 |
| `iStepperLens` | 0 – 2046 (0x7fe) |
| `iStepperCCD` (+adjust) | ∈ [0, 0x4d9) |

Full calibration block (per channel R/G/B + Ir, with open-gate variants):
`Gain_*`, `Offset_*`, `Current_*`, `DutyCycle_*`, `CcdExposure_*`, `IrLEDStartTime`,
`LampLevel`, `uiCcdIntegrationTime`, `SpliceDarkness`, `DetectWhite_G`, `DetectFilm_G`.

## Command frame — CONFIRMED wire format

`[type][count][count data bytes]` — the on-wire length is **`2 + count`**, NOT
padded to 36 (the 36 is only the max in-memory struct). `data[0]` is the
address byte. Examples from the capture:

```
04 03 10 00 85       type=04 count=3 data=[10(AD_HOST) 00 85]
07 02 10 00          type=07 count=2 data=[10 00]  (reply; data[1]=status)
04 03 44 00 00       query AD_PICM_PLUS  -> 07 02 44 01  (present)
04 03 24 00 00       query AD_PICM       -> 07 02 24 00
```

> **Mislabel CONFIRMED on real hardware (2026-08-12):** per the status-byte
> enum above (0 = success/ack, 1 = not acked), the "(present)" annotation is
> backwards — on this (non-plus) F-135 the PLUS probe (`0x44 -> status 1`)
> means *absent* and the plain PICM probe (`0x24 -> status 0`) *present*.
> These probes are the OEM driver's model detection (F-135 vs F-135+): a
> real F-135+ answers exactly inverted (`0x44 -> 07 02 44 00` present,
> `0x24 -> 07 02 24 01` absent), which is why the verbatim
> `pakon_replay --open` expectations fail on a Plus. See
> `docs/F135_PLUS_NOTES.md` and `docs/F135_PLUS_CAPTURES.md`.

Note the `0x85` in the open packet is a **command/parameter byte, not a
checksum** (the analogous `04 03 44 00 00` ends in `00`). Checksum (if any) is
TBD from the full 2218-command sample set in the capture.

## Open handshake — CONFIRMED (replay verbatim in Phase 3)

Observed on EP1, exactly matching the documentation:

```
host(0x01) -> 04 03 10 00 85       dev(0x81) <- 07 02 10 00
host(0x01) -> 02 04 10 01 8f 00    dev(0x81) <- 07 02 10 00
host(0x01) -> 04 03 44 00 00       dev(0x81) <- 07 02 44 01   ; probe PICs...
... (probes AD_PICM_PLUS 0x44, AD_BOOT_PICM_PLUS 0x46, AD_PICM 0x24, etc.)
```

## Parameter/calibration read — control `0xA4`/`0xA9` (Phase 5)

After the EP1 probe, the driver reads a structured block via EP0 vendor control:
`0xA4` (OUT trigger, `wValue=0x00A5`, `wIndex=0x1234`, no data) paired with
`0xA9` (IN read) pulling **32-byte chunks at increasing offsets** (0x00, 0x08,
0x28, 0x48, …). Looks like a calibration/parameter table. 16 such pairs in the
capture. To be decoded in Phase 5.

## Scan path — mapped from capture (Phase 5)

From the device-13 4-frame scan capture, the phases are:

1. **OPEN** — handshake + PIC presence probes (above). *Replayed & verified.*
2. **PARAM READ** — 16 `0xA4`(OUT trigger, wValue=0x00A5)/`0xA9`(IN) control
   pairs reading a table in 32-byte chunks at offsets 0x00, 0x08, 0x28 … 0x808.
   Likely device capabilities/calibration constants.
3. **CONFIGURE** — register writes on the command channel, each followed by a
   `03 01 <addr>` status poll:
   - PICM (`0x24`): `02 06 24 03 82 <reg> <vN> <vM>` for sub-registers 0x00–0x0a
     and `0x84` (scan geometry/exposure parameters), then `04 03 24 00 a2`.
   - PICL (`0x20`): `02 07 20 04 8x …` and `02 06 20 03 91 …` (more params),
     `02 04 20 01 80 01` / `80 00` (enable/strobe-style toggles).
4. **SCAN / STREAM** — kicked around `04 03 20 00 8a`; then a tight interleaved
   loop: status polls (`03 01 10`/`20`/`24`), register reads (`01 03 20 …`), and
   **bulk image reads on `0x86` in 20480-byte chunks**. In the 4-frame capture:
   ~11719 image reads (~240 MB raw) with ~2070 EP1 commands interleaved.

### Scan start/stop — CONFIRMED from `scan.pakscan` + `scan_fullroll.pakscan`

Two independent engines, each with a symmetric start/stop command of the form
`04 03 <addr> 00 <param>`. Both captures agree byte-for-byte:

| Engine | Addr | START | STOP |
|---|---|---|---|
| **CCD readout** (light + sensor → `0x86`) | PICL `0x20` | `04 03 20 00 8a` | `04 03 20 00 92` |
| **Film transport motor** | PICM `0x24` | `04 03 24 00 a0` | `04 03 24 00 a2` |

- **Re-arms happen ONLY in the preview/calibration phase** (CONFIRMED from a full
  `--scan --trace-status` run): all 21 `8a` re-arms fire at image-read index
  ≤ 1325, and the motor start `a0` is at exactly that index. After the motor
  runs, the **CCD streams continuously with ZERO re-arms** (10,394 reads in the
  4-frame capture) until the film ends. So `8a` is the per-snapshot arm of the
  *stationary* preview phase, not a during-scan primitive. **Physical
  correlation:** while preview/calibration runs the front LED is **orange and
  blinking**; when it turns **green** the operator feeds the film and the motor
  only then starts — matching the trace's preview→`a0`→stream structure.
- Each preview arm is paired with a host-side write `02 04 10 01 84 02` just
  before it, then a PICL poll `03 01 20`.
- `04 03 20 00 92` (stop readout) appears **exactly once**, right after the last
  image read; `04 03 24 00 a2` (stop motor) immediately after it. That two-command
  tail is the scan stop, identical in both captures.
- Ordering: **readout starts first** (`8a`) and grabs calibration/preview lines
  with the film stationary; **then the motor starts** (`a0`) to pull the roll
  through (in `scan.pakscan` ~1325 preview reads precede `a0`, ~10394 follow).
  `a0` is therefore the clean split marker between deterministic setup and the
  open-ended image-transfer loop.

### Live poll status — CONFIRMED from `--scan --trace-status`

Status byte (data[1] of a reply) is a **ready/busy flag**:

| value | meaning |
|---|---|
| `0x00` | ready / data available — proceed |
| `0x80` | busy — buffer momentarily starved, keep waiting |

In a full healthy scan HOST `03 01 10` returned `…00` 1041× and `…80` 564×; the
`0x80` polls cluster at the **end of the roll** (from index ≈11330 onward) as the
film runs out, then 61× at the final read. PICL `03 01 20` showed the same
`00`/`80` split (5× busy, right after preview arms); PICM `03 01 24` was always
`00`. The HOST reply carries a constant trailing byte `0xaa` (a fixed register,
not a counter). **There is no distinct "done" status** — `0x80` is *busy*, not
*finished* — which is why end-of-roll white (below) is the done-signal.

### Auto-stop scanning — `pakon_replay --scan FILE --autostop` (the working path)

The pure poll-driven SM (`--scan-sm`, below) stalls: the main scan needs periodic
**housekeeping** commands interspersed between reads (`02 05 20 02 06 00 20`
write + `01 03 20 1e 90` read, plus per-frame `02 06 ..` exposure writes) to keep
the readout streaming. Their cadence is **device-triggered and irregular**
(~600–4400 reads apart), so it can't be synthesised blind. But all rolls use the
**same method, differing only in length** — a 4-frame and a 24-frame capture have
the identical command vocabulary, just more of it.

So `--autostop` replays a captured script **verbatim** (correct streaming +
housekeeping + teardown) while watching the `0x86` stream, and **stops at
end-of-roll white** — the moment the film is through — then replays the teardown
tail to reset the engines. A shorter film just trips the white detector earlier
and stops. **Strategy: capture once at the maximum length (a 36-exposure roll)
and use that as the canonical script;** any shorter film auto-stops before the
script runs out. This is the length-independent auto-stop the varying-film-length
use case needs, built only from proven behaviour (verbatim streaming + white
detection, both validated on hardware). End-of-roll white tunables shared with
the SM (`SM_TRAIL_WHITE` etc., above `do_scan`).

**End-of-roll detection MUST stay disarmed through the pre-scan phase.** The
captured script first replays the **calibration/positioning phase** — in
`36frames.pakscan` that is the **first 1889 image reads, before motor-start
`04 03 24 00 a0`** — during which the gate flashes open: long open-gate **white**
runs interspersed with brief film. Arming there false-triggers "end of roll" within
a few percent of the scan (the original `--autostop` "stops immediately" bug). Two
guards, both required:
- **Arm at motor-start.** `--autostop` ignores the `0x86` stream until it replays
  `04 03 24 00 a0`. From there the CCD free-runs the real continuous scan (one long
  film block), so trailing white genuinely means the film ran out. This is the
  motor-start "offset" — keyed off the command, not a timer or read count.
- **Film-band `film_seen`.** `film_seen` latches only on real film content
  (`sm_chunk_has_film`: ≥`SM_FILM_FRAC_PCT` 10% of samples > `SM_FILM_THRESH` 8000),
  not the dim pre-film leader (~2.4k mean, ~0% over 8000). Open-gate white also
  clears 8000, but `sm_chunk_is_white` is tested first. Verified on `scan.raw`:
  old logic stopped at chunk 531/11718; fixed arms at 1889 and stops at 11616, on
  the true end-of-roll tail (final white run begins at 11593). `--scan-sm`'s image
  loop only runs post-motor-start, so it needs only the film-band guard, not the
  arming gate. Do **not** revert to the `sm_chunk_is_blank` variance detector: the
  `[visible | IR]` line layout makes true open-gate *high*-variance.

### Poll-driven scan state machine — `pakon_replay --scan-sm` (shelved — stalls)

Verbatim `--scan` is locked to the captured image-read count, so it only fits a
roll the same length as the reference. `--scan-sm` replays the deterministic
setup spine (OPEN → param table → calibration register writes → motor start
`a0`, none synthesisable) up to the first image read after `a0`, then **drives
the transfer itself**.

The image phase reads `0x86` and **re-arms on every empty window** with the pair
`02 04 10 01 84 02` + `04 03 20 00 8a`. This is empirically required (the
re-arm-less version never streamed once a real green→feed gap was present): when
the film isn't streaming continuously the readout needs the arm to produce the
next block, exactly as the preview phase does. It does NOT contradict the trace
— in the reference capture the film fed continuously, so there were no empty
reads and hence no mid-scan re-arms; re-arm only fires across gaps and at the
ends. If a re-arm write ever fails (command channel unresponsive) the loop
**aborts cleanly** instead of spinning (the old 200×-poll spin is what wedged
the bus on a stale device).

**Done-signal = end-of-roll white.** We can't mine a protocol done-signal (none
exists, and the `.pakscan` has no replies). The scan ends with the open gate
shining through no film (samples ≈ 48900, near 16-bit max; film/base/leader is
far darker). The loop latches `film_seen` on the first non-white chunk, then
stops after a sustained run of trailing white.

**Empty reads mean opposite things before vs after film, and look identical on
the wire (HOST `0x80` busy, read times out):** the CCD streams *nothing* while
the gate is empty — it only starts once film reaches it. So the loop gates on
`film_seen`:
- **before film** — the operator is still feeding the film (it can take ~10 s);
  wait up to `SM_LOAD_WAIT` windows (~2 min) before aborting "no film fed".
- **after film** — the film has passed the gate → end of roll; stop after
  `SM_MAX_EMPTY` (2) empty windows. Must NOT keep waiting on `0x80` busy here or
  the motor runs until the film ejects.

Tunables: `SM_WHITE_THRESH` 40000, `SM_WHITE_FRAC_PCT` 60, `SM_TRAIL_WHITE` 24,
`SM_FILM_THRESH` 8000, `SM_FILM_FRAC_PCT` 10, `SM_MAX_EMPTY` 2, `SM_LOAD_WAIT` 24,
plus `--max-mb`. (`FRAC_PCT` 60 not 90: the ~25% IR band sits at ~32k, below the
40k white threshold, so an open-gate chunk is only ~75% "white" — 90% never
tripped.) End-of-roll white is the
primary/faster stop (fires on trailing white *data*, before the film ejects);
the empty-window backstop covers the case where the device just stops feeding.

**Teardown matters.** After the image phase the loop replays the captured
*teardown tail* — every command after the last image read in the `.pakscan`:
the `92`/`a2` stop **plus** the PICM/PICL register resets (`0206…0917xx`,
`0205 20 02 06 xx`) that follow it. A bare `92`/`a2` leaves the engines
mid-state and **hangs the next operation** (e.g. a following `advance` does
nothing until the scanner is power-cycled). The long run of trailing `03 01 10`
idle polls in the tail is harmless read-only chatter.

### Command verbs (EP1, from frequencies)

- `04 03 <addr> 00 <p>` — query/command to an address (open, PIC probe, kick).
- `03 01 <addr>` — **status poll** of an address (10=HOST, 20=PICL, 24=PICM);
  the driver repeats these waiting for ready (e.g. 36×, 5× runs).
- `01 03 <addr> <reg> <p>` — read a register.
- `02 <n> <addr> <len> <reg> <data…>` — write register(s).

### Image format — partly DECODED (from our own `0x86` capture)

Captured `0x86` stream = **239,984,640 bytes**, 20480-byte chunks, ~240 MB /
4 frames. Decoded from `/Volumes/Video/scan.raw` (see `tools/pakon_image.py`):

- **16-bit little-endian** samples; line stride **8000 samples / 16000 bytes**
  (autocorrelation peak at 8000 + 16000 harmonic); ~14999 lines.
- **Per-pixel interleaved RGB** (`R,G,B,R,G,B…`) — NOT plane-sequential.
  Evidence: column autocorrelation peaks at lag 3/6/9 (lag3 ≈ 1.58× lag1);
  deinterleaving gives balanced channel correlation (R~G .59 / R~B .90 /
  G~B .56) whereas a planar split anti-correlates one "channel" (−0.08). This
  **supersedes** the earlier plane-sequential guess (old `raw2pnm.py --planar`).
- Working geometry: 8000 ∤ 3, so model = **2 pad samples/line, the triple
  restarts each line ⇒ 2666 px wide** (per-line restart confirmed: every row's
  best phase = 0).
- **Interleave order B,R,G** (position 0 = Blue, 1 = Red, 2 = Green), NOT RGB.
  Green is the middle trilinear line; red passes most through the orange mask,
  blue is most absorbed. Confirmed by natural skin tones across all 6 channel
  permutations of a real frame (wrong orders give green or "lomography purple").
- Ribbon is rotated 90°; the **4 frames lie along the long (line) axis**.
- Structure along the ribbon: dark leader, then a **blank "no-film" scan** (the
  feed runs before the strip loads — bright + colour-neutral), then the 4
  frames, then a blank tail; plus a uniform **gate margin** on one side (cols
  ~2050+) and an orange-base sliver at col 0. `pakon_image.py --autocrop`
  isolates the film.
- **Idea (not built): capture-time leading-white trim.** Because film is
  inserted by hand, the scan can stream seconds of open-gate white (≈48900)
  before the strip loads, all written to the `.raw`. We could drop it in
  `pakon_replay --scan` with a latched `--skip-leader`: drop "no-film" lines
  (≥90% of samples > ~40000) until the first non-white line, then write
  everything verbatim. **Hard constraint:** trimming must be done in whole
  **16000-byte line units**, never per `0x86` read — a read is 20480 B (1.28
  lines), so dropping a raw chunk desyncs the line stride and shears the rest of
  the image. Leading-only + latched so it never punches holes mid-image. Low
  priority: `--autocrop` already removes this region in post; the only wins are
  disk and a bounded file regardless of insert delay.

**Resolved photometric/alignment questions (2026-05-31), see STATUS.md:**
- **Q1 ghosting — trilinear CCD.** The per-line "restart" is correct (no phase
  drift); the R/G/B sensor lines are spaced **8 lines apart** along the scan
  (order **B(0), G(+8), R(+16)**), so register the three to a common position →
  fringing gone. `pakon_image.py --register` (auto-measures).
- **Q2 sense — NOT pre-inverted.** Transmission-sense confirmed: no-film open
  gate ≈ 48900 (max), dark leader ≈ 520. It's a raw negative; the black rebate
  is the correct result of inverting a negative. The magenta cast is the C-41
  orange mask (base ≈ orange, R≫B); proper inversion is mask-aware/density-space
  (left to dedicated film software, or an optional in-tool mode).

Note: film is **motor-fed whole rolls** — design CANCEL to let the feed finish,
not hard-abort.

## Film advance protocol — confirmed from `advance.pakscan` capture

The advance operation (film transport between frames) uses PICM (`0x24`) and
PICL (`0x20`) over the same EP1 command channel as scanning.

### Command sequence for one advance step

```
# --- setup (sent once before the first step) ---
01 03 20 01 02          PICL reg 0x01 = 0x02   (motor init)
01 03 20 1e 90          PICL reg 0x1e = 0x90
01 03 20 01 83          PICL reg 0x01 = 0x83
01 03 20 02 84          PICL reg 0x02 = 0x84
01 03 20 04 88          PICL reg 0x04 = 0x88
01 03 24 01 02          PICM reg 0x01 = 0x02   (PICM enable)
02 05 24 02 a5 1c 25    PICM reg 0x02 = [a5 1c 25]   ← advance duration (see below)
03 01 24                poll PICM status

# --- per-step (repeat for each frame advance) ---
04 03 24 00 a0          PICM command 0xa0 — START advance
03 01 10                poll HOST (type=0x03, data=[AD_HOST])
... (repeat HOST polls until reply status byte = PS_SUCCESS = 0x00)
                        ← PS_SUCCESS on HOST = frame in position
04 03 24 00 a2          PICM command 0xa2 — FINALIZE/STOP advance
```

### Key commands

| Wire bytes             | Meaning |
|------------------------|---------|
| `04 03 24 00 a0`       | Start one advance step (motor on) |
| `04 03 24 00 a2`       | Finalize/stop advance (motor park) |
| `03 01 10` → `… 00`   | HOST status poll; `data[1]=0x00` (PS_SUCCESS) = frame in position |
| `03 01 24`             | PICM status poll |

### Advance duration parameter (`02 05 24 02 a5 1c 25`)

The three bytes `[0xa5, 0x1c, 0x25]` written to PICM register `0x02` encode the
advance duration. This frame is the generic `WriteRegister(addr=0x24, reg=0x02,
value)` (see "Host transport" above), with the value being a **24-bit little-endian
integer** (`0x251ca5` = 2,432,165). The TLX Windows software accepts a value in
**seconds** from the user; the exact seconds→24-bit arithmetic is computed in the
TLA motor controller and is **TBD** (decompilation confirmed the wire shape but not
the formula — pin it by diffing two `advance.pakscan` captures at known durations).
**We keep the current verbatim-replay advance; this encoding is documented, not
required.**

### `pakon_replay` advance mode

```sh
./build/pakon_replay advance.pakscan            # 1 step, 60 s limit
./build/pakon_replay advance.pakscan --steps N  # N steps (frames)
./build/pakon_replay advance.pakscan --steps N --limit SEC
```

Each "step" = `a0` (start) → poll HOST until `PS_SUCCESS` (frame in position)
→ `a2` (finalize). `--limit SEC` is a wall-clock safety cap (default 60 s);
`--timeout MS` is the USB per-transfer timeout (default 1000 ms).

## PROVENANCE

The protocol facts in this file come from two sources, kept distinct:

1. **Our own USB captures** of the working scanner (`tools/analyze_capture.py`,
   `test/captures/`) — the primary, clean source.
2. **Reverse engineering of the original Kodak/Pakon Windows software** (Ghidra
   decompilation of `tlx/TLA/TLB/TLC.dll`), done **for interoperability** to
   corroborate and fill gaps in (1). Sections marked "OEM decompilation" above
   derive from this.

The OEM binaries and the Ghidra decompilation output are **third-party
copyrighted material and are NOT committed to this repository** (the working copy
lives outside the tree, under `pakon-scanning-software/` and `re/`, both
git-ignored). This project is an independent implementation; only
factual interface details (IOCTL numbers, request codes, wire shapes, value
ranges) needed for interoperability are recorded here.
