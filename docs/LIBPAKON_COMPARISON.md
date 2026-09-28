# libpakon vs. this project — exploration notes

> **Update 2026-09-28.** Our decoder has since adopted the R,G,B + trailing IR
> layout and marker-bit alignment described below (commit `7474ea9`). libpakon
> has no licence file: it remains a read-only reference. Its sequences are
> transcribed from OEM traces into named functions with computed calibration,
> which is the model for our replay-free roadmap (`STATUS.md`). Facts from the
> OEM F-135 engine are in `docs/TLB_FINDINGS.md`.

_Exploration date: 2026-06-03. Subject repo: `/Users/jorge/Desktop/Code/libpakon`
(`git@github.com:sdierauf/libpakon.git`, branch `main`, HEAD `2035a7e`). This is an
**independent** clean-room driver for the Kodak/Pakon F-135 — not a fork of ours, no
shared history. This file records what it does, how it compares to our reverse-engineered
notes (`docs/PROTOCOL.md`, `docs/IMAGING.md`, the skill guide), and where each project is
ahead. **No code was copied; this is a read-only survey.**_

All line references below are into `/Users/jorge/Desktop/Code/libpakon/libpakon/` unless
noted.

---

## TL;DR

libpakon is **substantially further along on capture, decode, calibration, and framing**,
and is architected as the driven, capture-free backend that was our stated GOAL. It is a
C++20 stack (`pakonctl`) plus a Node "Pakon Studio" web app.

Where **we are ahead**: the **colour pipeline**. libpakon deliberately outputs *raw,
uninverted negatives* (linear RGB16 + gamma 2.2) and explicitly defers C-41 inversion,
orange-mask removal, and ICC rendering to "future Studio work." We have already recovered
and shipped the OEM C-41 inversion (ColNeg log LUT) and the vibrant `rpd.pf` JPEG render.
That is the one piece libpakon lacks and the natural thing to contribute.

Two findings **correct our notes**:

1. **Row layout is 4 channels (R, G, B, IR), 2000 px wide — not 3 channels / 2666 px.**
2. **Trilinear order + channel order is R(0)/G(+8)/B(+16), interleave R,G,B — not our
   B,R,G.** This is almost certainly the root of our recurring "purple cast / channel
   order" bug.

---

## 1. USB transport & wire protocol — *confirms us, and resolves open questions*

`PakonProtocol.cpp/.h`, `PakonDevice.cpp`, `PakonTransport.h`, `LibusbApi.cpp`,
`FirmwareLoader.cpp`, `IntelHex.cpp`, `FirmwareCatalog.cpp`, `PakonCtlF135Protocol.cpp`.

| Aspect | libpakon | Our notes | Verdict |
|---|---|---|---|
| Frame format | `[type][count][payload…]` | `[type][count][data…]` | ✅ match |
| Wire length | `2 + count`, **no 36-byte padding** (`PakonProtocol.cpp:50-54`, max payload 34) | `2 + count`, not padded | ✅ match |
| Checksum | none; trailing byte is a param, relies on USB integrity | none; trailing byte is a param | ✅ match |
| Cmd OUT / reply IN / image IN | `0x01` / `0x81` / `0x86` **or `0x88`** (`PakonDevice.cpp:247-256`) | `0x01` / `0x81` / `0x86` | ✅ match (they also accept `0x88` for the ring) |
| Command path | bulk EP1, atomic OUT→IN | bulk EP1 | ✅ match |

**Resolves our "type byte values unconfirmed" TODO.** `PakonTypes.h:47-52` enumerates them
explicitly:

```cpp
enum class PacketType : uint8_t { Invalid = 0, ReadStatus = 3, Command = 4, Response = 7 };
```

plus observed `0x01` = parameter read, `0x02` = parameter write. This matches our *observed*
`0x04` host cmd / `0x07` reply and our `02`-frame register writes — and pins down the names
(PH_CMD = 4, PH_READ_STATUS = 3, PH_INVALID = 0) we had marked TBD.

**Subsystem addresses** (`PakonCtlF135Protocol.cpp`): `0x10` host, `0x20`/`0x24` lower/drive
PIC (standard F-135), `0x40`/`0x44` lower/drive PIC (F-135 **Plus**). Matches our
`AD_HOST=0x10 / PICL 0x20 / PICM 0x24` + `_PLUS 0x40/0x44`.

**Status byte** at `payload[1]`; `0x00` = ACK/success, `0x80` = host event pending
(`PakonProtocol.cpp:37-41`, `PakonCtlF135Protocol.cpp:206,549`).

### Firmware load — *more general than our verbatim replay*

`FirmwareCatalog.cpp:52-62` is a real device catalog rather than a single captured script:

- **Cold/bootstrap:** Cypress `0x0547:0x1002`, `0x04B4:0x8613`, plus Pakon "unloaded family"
  `0x0F05:0xF235`.
- **Warm/loaded:** `0x0F05:0xF135` rev2 (F-135), `0x0F05:0x35F2` (F-235), `0x0F05:0xF335`
  (F-335).

The FX2 sequence (`FirmwareLoader.cpp:107-131`): `cpu_reset(1)` → download bootstrap
(`0xA3` external / `0xA0` internal RAM, split at `0x1B3F`) → `cpu_reset(0)` → **`0xA4`
wValue=`0x00A1` post-bootstrap handshake** → **`0xA9` read 8-byte personality** → *pick the
model HEX from the catalog by personality* → download final firmware → `cpu_reset(1/0)` →
re-enumerate. CPUCS at `0x7F92` (EZ-USB) **and** `0xE600` (FX2). Intel HEX parsed properly
(`IntelHex.cpp`): checksum-validated, type 0x00 split into 16-byte vendor transfers, type
0x02 ignored, type 0x01 EOF.

> **vs. us:** we replay one captured `.pakfw` verbatim and key off `f235→f135` only. libpakon
> reads the device *personality* and selects firmware for F-135/235/335 generically — the
> robust version of what our SKILL notes call out as the eventual goal. The vendor request
> space (`0xA0/0xA3/0xA4/0xA9`) matches our RE exactly.

---

## 2. Bitstream decode — *libpakon is well ahead, and corrects two of our claims*

`RingStream.cpp`, `ScanImage.cpp/.h`, `convert_f135_ring_*.py`.

### 2a. Row layout — **4 channels, 2000 px (CORRECTS US)**

`ScanImage.h:14`, `convert_f135_ring_to_pfs.py:8-10`:

- 16-bit **LE** samples, **8000 samples/row = 16,000 bytes/row** — *this matches our stride
  exactly.*
- **But the 8000 samples are 2000 pixels × 4 interleaved channels: `R, G, B, IR/extra`**,
  not 3-channel RGB.
  - Visible accessor `row[x*3 + c]` for c∈{0,1,2} (`ScanImage.cpp:394-398`).
  - IR/extra lane at `row[width*3 + x]`, width = `row.size()/4` (`ScanImage.cpp:400-404`).

> **This contradicts our notes** (`SKILL.md`, `docs/IMAGING.md`), which treat all 8000
> samples as RGB triplets → **2666 px wide**. But `8000 / 3 = 2666.67` is **not an integer** —
> a latent red flag. `8000 / 4 = 2000` divides cleanly. libpakon's 4-channel (RGB + IR)
> reading is almost certainly correct, and it means **our decoder has been folding the IR
> lane into the visible channels**, which would scramble per-pixel colour and inflate the
> width by 33%. Strong candidate for the source of several imaging headaches.

### 2b. "Properly decode" = row-origin sync via a marker bit (not bit-unpacking)

The commit "properly decode raw bitstream instead of brute forcing it" (`41ac81a`) is **not**
an encoded-format unpack — the stream is raw uint16. It is robust **byte-alignment / row
origin** recovery (`ScanImage.cpp:232-302`, `detect_f135_scanline_marker_origin`):

1. Find the low-saturation packet run = actual film content (`detect_low_saturation_packet_offset`, 157-222).
2. Floor to the 16,000-byte row grid (`:1011`).
3. The scanner sets the **LSB (bit 0) of one fixed word every scanline** as a marker; scan
   ~4096 rows, find the word position whose LSB is set in ≥95% of rows, require an ambiguity
   margin (`:262-266`). That word's byte offset becomes the row origin; all subsequent reads
   align to it.

> **vs. us:** we recover geometry by autocorrelation (lag-3/6/9) and brute-forced stride. The
> marker-bit approach is deterministic and per-scanline — strictly better. Worth adopting.

### 2c. Trilinear CCD — **R(0)/G(+8)/B(+16) (CORRECTS US)**

`ScanImage.h:158-162` / `live-scan-state.js:25-33`:

```cpp
red_row_shift = 0; green_row_shift = 8; blue_row_shift = 16;
```

Output row N = raw row N (R) + N+8 (G) + N+16 (B); edges clamped.

> **This contradicts our `B(0)/G(+8)/R(+16)` claim** (SKILL.md "Q1 ghosting"). Our 8-line
> spacing is confirmed, but **R and B are swapped relative to ours**. Combined with the
> R,G,B (not B,R,G) interleave above, this is very likely the real fix for the recurring
> channel-order / purple-cast bug we kept patching with `--channel-order` heuristics.

### 2d. Colour in decode path

None. `ScanImage.h:17-18,75-79`: linear, uninverted, no crop, no inversion, no orange-mask —
all deferred. (See §5.)

---

## 3. Calibration & scan engine — *libpakon implements the driven backend that is our GOAL*

`PakonCtlF135Sequences.cpp`, `PakonCtlF135Capture.cpp`, `F135Controller.cpp`, `F135State.cpp`,
`FirmwareCommandMap.cpp`, `PakonCtlInternal.h`.

This is the big architectural gap. Our SKILL "Register map milestone" memory frames the goal
as: *"capture-free driven backend… decoded CCD gain/offset/exposure register map (bank
0x82/0x84); next = calibrate the algorithm."* **libpakon has done the calibration algorithm.**

### Full calibration order (`PakonCtlF135Sequences.cpp:55-196`)

baseline setup → **lamp warmup event** (waits on self-test/service bits) → CCD config + set
calibration window (`0x82/6` span, `0x82/4` offset, `0x82/5` end) → **dark/offset trim**
iteration → **bright open-gate** capture → exposure/trim program → lamp timing ramp → scan
window reprogram (`0x82/4`→`0x0027`, etc.) → motor drive (`0xa5` speed, `0xa0` forward,
`0x82/0`→`0x0161` trigger).

### Register map — *matches and extends ours*

| Reg | Purpose | Cal value | Scan value |
|---|---|---|---|
| `0x82/0` | acquire/scan flags (bit0 = trigger) | `0x0160`/`0x0161`/`0x0061` | `0x0160`→`0x0161` |
| `0x82/4` | CCD window start | `0x0006` | `0x0027` |
| `0x82/5` | CCD window end | `0x07f7` | `0x080e` |
| `0x82/6` | CCD span/timing | `0x09c2` | `0x0ffd` |
| `0x84/2-4` | exposure R/G/B | `0x000d` | from TLA profile |
| `0x84/5-7` | dark trim R/G/B (sign bit `0x0100`) | iterative | from TLA profile |
| `0x80` | profile mode (1=vis, 2=IR, 3=complete) | — | — |
| `0xa5`/`0xa0`/`0xa2` | drive speed / forward / commit-stop | — | — |

Confirms our `bank 0x82/0x84` CCD gain/offset/exposure map. Indexed write wire form
`02 06 <addr> 03 <reg> <idx> <val_le16>` matches our `WriteRegister`-style `02` frames.

### Per-column Fixed Pattern Correction — *the OEM formula we documented, now implemented*

`PakonCtlF135Capture.cpp:620-722` computes per-column gain from **128 dark + 128 bright
calibration lines**:

```
gain[c] = 0x7d00000000 / (bright_mean[c] − dark_mean[c] − bright_prefix_mean + dark_prefix_mean)
          clamped to 0x3ffff
```

Note **`0x7d00000000` = 125 × 2³²** — exactly the `Gain = 125·2³² / (…)` formula we reverse-
engineered into `docs/IMAGING.md` from `CiConfigFixedPatternCorrection`. Independent
confirmation our RE was right. Dark target 300±32; bright target ≈63968.

**Crucially, libpakon does the bright pass with the motor running**, via the scan engine —
which is exactly the obstacle our discarded FFC notes flagged ("the F-135 lamp only lights
while the motor runs, so FPC can't be computed from a motor-off pass"). libpakon solves it
by driving the calibration.

### Autostop / end-of-roll

`F135State.cpp:104-254`: classifies lower-PIC event byte — `0x04`/`0x40` = confirmed film-end,
`0x20` = scan progress, `0x02` = service. The scan loop runs until a film-end event, not a
byte count. Matches our "end-of-roll-white autostop" / device-end-signal model, but as a real
poll-until-state machine (our SKILL flags that as the robust follow-up to verbatim replay).

> **vs. our discarded `--ffc`:** interesting overlap. libpakon *also* has a second, roll-local
> "scanner-FPC" derived from unexposed/rebate rows with **cluster-validated separator ranges
> (light-leak rejection), edge-column preservation, and broad-falloff + fine-residual
> refinement** (see §5 / `StudioFrameProcessor`). That is almost exactly the algorithm we just
> deleted. We arrived independently at the same design — but libpakon also has the *primary*
> OEM-style dark/bright calibration that ours lacked.

---

## 4. Frame detection / chopping — *libpakon is more sophisticated*

`StudioFrameLayout.cpp`, `StudioFrameProcessor.cpp`.

libpakon uses a **dynamic-programming cost-minimisation solver** over separator candidates
(`StudioFrameLayout.cpp:92-346`) that jointly fits a sequence of **half / standard /
panoramic** frames, with penalties for mixed-format rolls. Separators come from two detectors
(bright strip edges + dark gaps, `StudioFrameProcessor.cpp:707-817`) with quantile
thresholding, plus oversize-split and median-width normalisation passes for panoramas. B&W
gets adjusted thresholds (commit `2035a7e`).

> **vs. us:** ours is autocorrelation pitch → auto frame count → centred fixed-3000 crops.
> libpakon's solver handles half/pano/mixed rolls and partial fragments — meaningfully more
> capable. (Studio frame output rows default `3050`, gamma `2.2`.)

---

## 5. Colour / imaging — *WE are ahead; libpakon has the gap*

`ScanImage.h:16-18,75-79`, `StudioFrameProcessor.cpp:1502-1609,1713-1766`, Studio
`README.md`.

libpakon **deliberately does no colour rendering**:

- ❌ No C-41 inversion, ❌ no orange-mask removal, ❌ no ICC profiles.
- ✅ Linear RGB16 native TIFF (uninverted negative, orange mask intact).
- ✅ Optional **vendor-TLA + scanner-FPC flat field** (still linear).
- ✅ Frame TIFFs get **gamma 2.2** + rotation + optional **B&W collapse** (RGB averaged to
  mono; commit `d9abaa9`/`2035a7e`). IR lane disabled for B&W.

Studio `README.md` states outright: *"Photographic positive conversion and orange-mask
compensation remain future Studio-layer work."*

> **vs. us — this is our differentiator.** We have recovered and shipped (in the now-rolled-
> back tree, and documented in `docs/IMAGING.md`):
> - the OEM **C-41 inversion** = per-channel Dmin normalisation → shared log-density LUT
>   `out = 3500·log10(16383/in)` → sRGB (`--invert-c41`), verified vs OEM reference scans;
> - the **vibrant JPEG render** = Kodak `rpd.pf` ICC profile + scene balance + highlight
>   roll-off (`--jpeg`).
>
> This is exactly the "future Studio-layer work" libpakon hasn't built. **If we migrate to
> libpakon, our colour pipeline is the thing worth porting in** — it slots cleanly onto their
> linear, uninverted RGB16 output (which is the correct input for a Dmin/log inversion).

### Digital ICE — same status both sides

Both detect the IR/4th lane and currently discard it for the visible image; both leave actual
scratch/dust removal as future work. libpakon keeps the IR lane as an optional gray16 sidecar;
we detect+discard. Consistent with `[[digital-ice-status]]`.

---

## 6. Architecture & build

- **Stack:** C++20 `pakonctl` (clang++, libusb loaded dynamically; `make -C libpakon`) +
  Node "Pakon Studio" (`apps/pakon-studio`, `server.js`, port 4318, SSE live view).
  `./run-pakon-studio.sh` builds and launches.
- **Key CLI:** `pakonctl list` / `load-fw F135` / `scan-f135-strip .` / `studio-frame-detect`
  / `studio-frame-chop`.
- **Formats:** native linear RGB16 TIFF; `.pfs` = TLX planar line-interleaved
  (R|G|B|IR lines, 2000 wide); per-roll folder with `pakon_manifest.json`, previews, logs.
- **Studio UX:** scan-first queue workflow, live preview reconstructed from the growing
  `.raw.bin` using the marker-bit row origin, an error atlas (USB / power / lost-sync codes),
  presets (std-135, panoramic, half-frame, bw-proof).

> Mirrors our two-stage minilab flow (`[[web-minilab-workflow]]`) conceptually, but built on a
> live SSE preview off the real ring stream rather than a prescan pass.

---

## 7. Net assessment & where our work still has value

**Migrate to libpakon for:** transport, firmware (personality-driven, multi-model), the
marker-bit bitstream decode, the **correct 4-channel RGB+IR / R-G-B-(+0/+8/+16) layout**, the
full driven calibration (dark/bright FPC with the confirmed `125·2³²` gain formula), the
poll-until-state scan engine, and the DP frame solver.

**Carry forward from us:**
1. **The colour pipeline** — C-41 inversion (ColNeg log LUT + Dmin) and the `rpd.pf` JPEG
   render. This is libpakon's explicit gap and our strongest contribution.
2. **The decompilation/RE corpus** — `re/out/FINDINGS.md`, the Ansel-pipeline analysis, the
   DX-barcode subsystem map, the OEM IOCTL/vendor-request confirmations. libpakon's code
   independently corroborates much of it (formula, addresses, type bytes), which raises
   confidence in the rest.

**Bugs in our current code this exploration exposed (independent of libpakon migration):**
- 3-channel decode (2666 px) should be **4-channel RGB+IR (2000 px)** — `8000/3` was never
  integer.
- Trilinear/interleave order should be **R(0)/G(+8)/B(+16)**, not B,R,G — likely the real
  cause of the recurring purple-cast/channel-order bug we kept band-aiding.

_No action taken — this is exploration only, per request._
