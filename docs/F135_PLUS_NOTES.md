# Why the F-135+ didn't work with this client — analysis (now resolved)

> **STATUS 2026-08-12: the F-135+ now works** — firmware load, init, and a
> full Base 16 scan replay all succeeded on real hardware using its own
> converted capture scripts (`docs/F135_PLUS_CAPTURES.md`). This document
> remains the analysis of why the *original F-135 verbatim-replay path*
> fails on a Plus. Of the fixes proposed in §6, model detection is DONE
> (2026-08-13: `pakon_replay --open` evaluates the PIC probes and reports
> the model instead of byte-verifying one F-135's replies); the
> address-parameterised sequences remain the right way to unify the two
> models in one driven client.

_Written 2026-08-12. Sources: this repo's code and `docs/PROTOCOL.md`; Ali
Bosworth's independent RE corpus at `~/projects/Pakon Software/` (F-135+
serial 16402): `notes/usb-protocol.md` (742 MB PPB debug trace), `notes/firmware-files.md`,
`notes/usb-driver-details.md`, `notes/picl-plus-firmware-NL050A.md`,
`notes/ioctl-capture-analysis.md`, and the **driver-level captures in
`Pakon datalogger capture/`**. Markers follow the notes'
[CONFIRMED]/[INFERRED]/[SPECULATIVE] convention._

## TL;DR

This client is a **verbatim replay engine for byte sequences captured from one
specific F-135 (non-plus)**. The F-135+ keeps the same USB transport, framing,
and largely the same PIC command opcodes — but its two PIC controllers answer
at **different bus addresses** (`0x40`/`0x44` instead of `0x20`/`0x24`), run
different firmware (NL/NM "Plus", PIC18), and add Plus-only hardware
(TEC-cooled CCD, integrated DX sensor) that the captured F-135 sequences never
configure. The very first thing `pakon_replay --open` does on a Plus is verify
probe replies that are **exactly inverted** on that model, so it fails at the
open handshake before anything else gets a chance.

Crucially, **Ali Bosworth already has real F-135+ captures** (TX side + full EP6
image stream, four resolution/IR configurations) — see §5. The main remaining
gap is the RX direction (device replies).

## 1. First failure: the open handshake hardcodes "this is a non-plus"

[CONFIRMED in code] `tools/pakon_replay.c` (`OPEN_SEQ`, ~line 34) replays the
captured probes and byte-compares the replies:

```
probe PICM_PLUS       04 03 44 00 00  -> expect 07 02 44 01   (status 1 = not acked / absent)
probe BOOT_PICM_PLUS  04 03 46 00 00  -> expect 07 02 46 01
probe PICM            04 03 24 00 00  -> expect 07 02 24 00   (status 0 = present)
```

These probes are the OEM driver's **model detection** — it asks "is there a
Plus PICM at 0x44? a regular PICM at 0x24?" and branches on the answer. The
replay instead treats one F-135's answers as fixed expectations. On an F-135+
the statuses invert (0x44 present, 0x24 absent), so `--open` reports MISMATCH
and exits non-zero. [SPECULATIVE as to the Plus's exact reply bytes, but the
inversion follows directly from the address table.]

**`docs/PROTOCOL.md` appears to have this mislabeled**: line ~229 annotates
`07 02 44 01` as "(present)", but per its own status enum (0 = success/ack,
1 = not acked) that reply on a *non-plus* should mean the PLUS PICM is
*absent*. A correction note has been added in place in PROTOCOL.md
(see "Suspected mislabel" under "Command frame").

## 2. Even without verification, every PIC command targets the wrong address

[CONFIRMED] All `.pakscan` scripts (scan, advance) were extracted from F-135
captures, so every configure/calibrate/motor/scan command addresses
`AD_PICL 0x20` / `AD_PICM 0x24`. The F-135+ PICs sit at `AD_PICL_PLUS 0x40` /
`AD_PICM_PLUS 0x44` — this is now **hard-confirmed** by Ali Bosworth's IOCTL
captures, where the only addresses ever used are `0x10` (HOST), `0x40`
(PICL+), `0x44` (PICM+) (`ioctl-capture-analysis.md`). On a Plus, frames sent
to 0x20/0x24 should come back status 1 (not acked); only the `AD_HOST 0x10`
(FX2/PPB-bridge) commands would still work, since that chip is common.

## 3. Good news: the command dialect is mostly shared

Cross-referencing this repo's F-135 wire capture against Ali Bosworth's F-135+
traces, the opcodes line up [CONFIRMED on the F-135+ side from the IOCTL
captures; the pairing to F-135 wire bytes is INFERRED]:

| Command | F-135 capture (this repo) | F-135+ (Ali Bosworth's captures) |
|---|---|---|
| Acquire line `0x8A` | `04 03 20 00 8a` to PICL 0x20 | PICL+ CMD 0x8A |
| Light config `0x8F` | `02 07 20 04 8f …` (4-byte write) | PICL+ WRITE 0x8F `[E8 FF 18 00]` |
| CCD config `0x80` | `02 04 20 01 80 01` (1-byte write) | PICL+ WRITE 0x80, 1 byte |
| Scan-line params `0x91` | `02 06 20 03 91 …` (3-byte write) | PICL+ WRITE 0x91, 3 bytes |
| Motor engage/disengage/cal | `a0` / `a2` / `a5` to PICM 0x24 | PICM+ 0xA0 / 0xA2 / 0xA5 |
| End acquisition `0x92` | `92` stop (PROTOCOL.md teardown) | PICL+ CMD 0x92 |

The wire format `[type][count][addr][payload_len][cmd][payload…]` also maps
1:1 onto the IOCTL record format Ali Bosworth decoded
(`[rx_len][tx_len][addr][data_count][cmd][data]`, with op type implicit:
tx_len==3 & count==0 → CMD/type 04, tx_len==3+N → WRITE/type 02, etc.). And
the fixed 60-pair vendor-request init in the F-135+ captures (32-byte chunk
reads at offsets 0x08, 0x28, 0x48 …) matches this repo's F-135
`0xA4`/`0xA9` parameter-table read almost exactly — the FX2 bridge layer is
shared across models. **Re-addressing to 0x40/0x44 gets you most of the way.**

## 4. What re-addressing alone will NOT cover — now with real values

Ali Bosworth's captures show the F-135+ cold-start sequence is byte-identical
across sessions (commands 1–37), including Plus-specific steps an F-135
capture never contains [CONFIRMED]:

- **TEC (thermoelectric CCD cooler)**: `SetTEC_1 [00]`, `SetTEC_2 [01]` every
  init. Plus-only hardware.
- **Per-channel CCD exposure**: `SetExposure_G [E0 FF 20 00]`,
  `_B [F0 00 20 03]`, `_R [A0 00 70 03]`.
- **Color matrix**: 12-byte write `[00×10 D6 03]` (identity/zero-ish).
- **Motor park**: 9× SetMotorSpeed + 2× SetMotorConfig + DisengageFilmDrive.

Remaining model differences beyond the init values: different PIC firmware
(NL050A/NM0506, PIC18 vs PIC16), PCB #125430C, DX sensor integrated into
PICL+, ~2× throughput. Per-resolution parameter values can be diffed directly
from the four capture sessions.

**Firmware selection** [still SPECULATIVE]: the OEM driver picks the FX2 image
via registry key `_PPPP_RRRR` (PID + revision) after reading the EEPROM C0
descriptor (revision word + personality byte). This repo replays a single
captured `f235→f135` `.pakfw` keyed only on cold-PID. Whether the Plus wants a
different FX2 image is unconfirmed — the datalogger captures start after the
driver is loaded, so the enumeration/firmware-load phase isn't in them.
Likewise `pakon_is_warm_id()` (accepts only `0F05:F135`) is probably fine —
Windows exposes the Plus as the same `\\.\Pakon135` device — but the Plus's
actual warm VID/PID/bcdDevice haven't been observed.

## 5. Existing F-135+ captures — what they cover, what's missing

> **UPDATE 2026-08-12: the captures are now decoded — see
> `docs/F135_PLUS_CAPTURES.md`.** `tools/fx35_datalogger.py` converts them
> (`resources/f135plus/*.pakscan`), the image-stream format is confirmed
> (per-pixel RGB, stride 3×width, IR as trailing block when enabled), and a
> Base 16 strip decoded to a correct positive with
> `pakon_image.py --linewidth 6000`. The RX gap and options below still
> stand.

`~/projects/Pakon Software/Pakon datalogger capture/` holds four TLXCD
sessions of the **same 4-exposure strip** on the F-135+ (serial 16402), at
Base 4/8/16 and Base 4 + IR, captured with Ali Bosworth's modified FX35 kernel
driver ("KK driver" with EP6 + IOCTL logging). Each session:
`ep6_001.bin` (calibration pass) + `ep6_002.bin` (film pass) + `ioctl_log.bin`.
See `CAPTURE_CONDITIONS.md` there and `notes/ioctl-capture-analysis.md`.

**Already answered by these captures:**
- The full command stream to drive a Plus: init, calibration, scan, teardown,
  with exact addresses, opcodes, and parameter bytes, per resolution and IR
  mode. This is precisely the TX side a driving client needs to emit.
- The EP6 image stream at every resolution incl. the IR 4th channel — enough
  to verify row stride / lane layout / marker bit against this repo's decoder.
- The FX2 vendor-request init (60 fixed pairs) — cross-confirms the shared
  bridge behavior.

**Still missing (the one real gap): the RX direction.** The driver logs reply
records with empty payloads (METHOD_NEITHER makes `Irp->UserBuffer` unsafe to
touch in the kernel completion path), so probe replies, status-poll values,
`0x90` sensor contents, and PPB_MINFO responses are unknown. The expected
reply *lengths* are known (`rx_len`: 4 for CMD ack, 2 for WRITE ack, 1+data
for READ), just not their contents. Options, in rough order of effort:
1. **Adapt the replay to not need them**: accept any correctly-sized reply,
   and poll `03 01 <addr>` until status 0, per the documented status
   semantics. Riskier but possibly sufficient.
2. **Add RX logging to the KK driver** (copy the buffer at completion in the
   requesting thread's context, or shim to METHOD_BUFFERED).
3. **One USBPcap/usbmon capture** of a TLXCD session — also nails the
   enumeration + firmware-load phase and the Plus's warm/cold USB IDs, closing
   the §4 firmware questions.

## 6. Path to F-135+ support in this client

1. Turn the open-handshake probes into **model detection** (like the OEM):
   probe 0x24/0x44, record which acks, select an address map.
2. Write a small converter: `ioctl_log.bin` → this repo's `.pakscan` format
   (the record→wire mapping in §3 makes this mechanical).
3. Replay the converted Plus init/scan on the real F-135+ with relaxed reply
   verification (§5 option 1), falling back to an RX-capable capture (§5
   options 2–3) if the poll semantics surprise us.
4. Validate the EP6 stream from the captures against `pakon_image.py` before
   ever needing the hardware.
