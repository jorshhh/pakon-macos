# pakon

A cross-platform (Linux + macOS) driver and web app for the Kodak/Pakon
**F-135** film scanner (and the "Plus" / F-235 / F-335 variants). Unofficial,
independent reimplementation built from documented protocol notes, our own USB
captures, and reverse engineering of the original Windows software for
interoperability (see `docs/PROTOCOL.md` → PROVENANCE).

> **Status:** Full end-to-end scan works on hardware — firmware load, open
> handshake, film advance, scan drive, and image decode are all validated on
> Linux and macOS, on both the **F-135** and the **F-135+** (the Plus uses
> its own scripts — see "F-135+ owners" under Usage). The product is a Python web service (FastAPI + browser UI)
> that wraps the C tools and image pipeline so any machine on the local network
> can drive the scanner.
>
> The decoder handles both the 4-frame (HiRes) and whole-roll (LowRes) scan
> modes: IR-band-aware zone splitting, wrap-order de-interleaving, per-zone
> trilinear registration, fixed per-zone channel order, autocrop, and
> autocorrelation-based frame detection. It also reproduces the OEM colour:
> the recovered **C-41 inversion** (a log-density ColNeg LUT) for a faithful
> positive, and the Kodak **`rpd.pf`** rendering profile for the vibrant JPEG
> look (see `docs/IMAGING.md`). The web UI is a **minilab-style two-stage
> flow** — prescan preview → operator confirms each crop in the browser →
> high-res export.

## Architecture

The hardware driver is two strictly separated C layers:

- **transport** (`pakon_usb`): libusb context, enumeration, FX2 firmware
  download, raw bulk I/O. No packet knowledge.
- **protocol** (`pakon_proto`): the command frame, encode/decode, command
  primitive. No USB knowledge.

`pakon_log` is a shared utility (tracing + the common `pakon_result` type) used
by both layers without coupling them to each other. On top of the driver, the
Python web service (`web/`) drives the C tools and runs the image pipeline
(`tools/pakon_image.py`) to serve the browser UI.

## Building

Requires a C11 compiler, CMake ≥ 3.16, `pkg-config`, and **libusb-1.0**.

### Linux (Debian/Ubuntu)

```sh
sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

### macOS (Homebrew)

```sh
brew install cmake pkg-config libusb
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Produces: static `libpakon`, tools `pakon_probe` and `pakon_replay`, and unit
tests `test_proto` / `test_hex`.

## Usage

### Overview

The scanner has two firmware stages. On power-on it enumerates as a bare FX2
bootloader (`0F05:F235`). The working driver downloads a second-stage firmware
image over USB, after which the device re-enumerates as the operational scanner
(`0F05:F135`). Our tools replicate this sequence.

Two file types drive the process:

- **`.pakfw`** — a firmware replay script extracted from a USB capture of the
  Windows driver performing the firmware load. It contains the exact sequence
  of USB control transfers needed to bring the scanner from cold (`0F05:F235`)
  to operational (`0F05:F135`). One ships at `resources/f135.pakfw` (works
  for the F-135 and F-135+ — same FX2 image); regenerate from your own
  capture with `analyze_capture.py --extract-firmware` if redistribution is
  a concern — see `firmware/README.md`.

- **`.pakscan`** — an operation script extracted from a USB capture of the
  Windows driver. Two kinds:
  - **Scan script** — ordered sequence of commands, control transfers, and
    image reads. `pakon_replay --scan` replays it verbatim to drive a real
    scan. Generate with `analyze_capture.py --extract-scan`.
  - **Advance script** — motor command sequence for film transport.
    `pakon_replay resources/advance.pakscan` replays it and then loops the
    start/poll/finalize sequence for as many frames as needed. Generate by
    capturing an advance operation and extracting with `analyze_capture.py`.

### Step-by-step

**1. List devices**

```sh
./build/pakon_probe --list
```

With the scanner plugged in you should see `0f05:f235` (cold/bootstrap). If
the device is already warm (`0f05:f135`) from a previous session, skip step 2.

**2. Load firmware**

```sh
# Linux (needs privileges for libusb)
sudo ./build/pakon_probe --load-firmware resources/f135.pakfw

# macOS (no sudo needed)
./build/pakon_probe --load-firmware resources/f135.pakfw
```

The scanner re-enumerates as `0f05:f135`. Confirm with `--list`.

**3. Verify the open handshake**

```sh
./build/pakon_replay --open
```

Should print `OK` for each step and reach `Idle`.

**4. Advance film**

To transport film to the desired position (e.g. to the first frame):

```sh
./build/pakon_replay resources/advance.pakscan            # advance 1 frame
./build/pakon_replay resources/advance.pakscan --steps N  # advance N frames
```

Each step sends the start command, polls until the scanner signals the frame
is in position, then sends the finalize command. `--limit SEC` sets a
wall-clock safety cap (default 60 s). The advance duration (how far each step
moves the film) is a 24-bit value written to PICM register `0x02` (set by the
TLX software in seconds) and is carried verbatim in the `.pakscan` script — see
`docs/PROTOCOL.md`.

**5. Run a scan**

Load film into the scanner, then:

```sh
./build/pakon_replay --scan resources/scan.pakscan --image scan.raw
```

Streams ~240 MB per 4-frame strip, or ~1.2 GB for a whole roll. A couple of
transfer errors at the very end are normal.

**6. Decode the image**

**Option A — web UI (recommended):** start the web service (see below) and
open `http://localhost:8000`. Upload the `.raw`, click **Process** (prescan →
long preview strip + detected frames), **confirm/position each crop** on the
strip, then **Export** for high-res output (raw negative TIFF, plain positive
TIFF, and rendered JPEG per frame, plus a contact sheet).

**Option B — command line:** requires Python 3, `numpy`, `pillow` (with
littlecms, for `--jpeg`), and ImageMagick (`magick`).

```sh
# Faithful C-41 positive (16-bit TIFFs), auto frame count + auto orientation
python3 tools/pakon_image.py scan.raw --invert-c41 --rotate 90

# Also write the vibrant rendered JPEGs (Kodak rpd.pf profile)
python3 tools/pakon_image.py scan.raw --invert-c41 --jpeg --rotate 90

# Raw negatives only (orange mask intact) for inverting in another tool
python3 tools/pakon_image.py scan.raw --rotate 90
```

The frame count is **always auto-detected** (a "36-exposure" roll commonly scans
as 37+ usable frames). The decoder automatically handles:

- **C-41 inversion** (`--invert-c41`) — the recovered OEM ColNeg log-density
  curve `out = 3500·log10(16383/in)` + per-channel film-base (Dmin) normalisation
  (orange-mask removal). Without it, output is the raw negative for external
  inversion (Negative Lab Pro, darktable negadoctor, …).
- **Rendered JPEG** (`--jpeg`) — the Kodak `rpd.pf` ICC profile + scene balance +
  a highlight roll-off that keeps detail the OEM blows out. See `docs/IMAGING.md`.
- **IR (Digital ICE) band** — detected and used to delimit the visible zones,
  then discarded (we do **not** do scratch removal — see `docs/IMAGING.md`).
- **Wrap-order de-interleaving**, **per-zone trilinear registration**, and a
  **fixed per-zone channel order** (the two CCD taps interleave RGB differently).
- **Frame detection** — autocorrelation pitch → count → phase-locked comb →
  fixed-width centred crops.

Key decoder options:

| Flag | Default | Effect |
|------|---------|--------|
| `--invert-c41` | off | OEM-faithful C-41 positive (ColNeg log LUT + Dmin) |
| `--jpeg` | off | also write the `rpd.pf`-rendered JPEG (needs `profiles/rpd.pf`) |
| `--rotate {90,180,270}` | 0 | rotate each output frame |
| `--frames N` | auto | optional hard override of the auto-detected count |
| `--channel-order {fixed,auto,brg}` | fixed | per-zone R/G/B identity (fixed is verified) |
| `--register` / `--no-register` | on | co-register the trilinear R/G/B sensor lines |
| `--autocrop` / `--no-autocrop` | on | strip leader / blank pre-load scan / gate margin |
| `-o PREFIX` | `frame` | output filename prefix |

### F-135+ owners

The F-135+ works end-to-end (verified on real hardware, 2026-08-12) but uses
its own scripts and decoder flags — the F-135 `.pakscan` files above will NAK
on it, because its controllers answer at different bus addresses. The
differences:

```sh
# Firmware load is IDENTICAL (the F-135+ uses the same FX2 image):
./build/pakon_probe --load-firmware resources/f135.pakfw

# The open handshake detects your model:
./build/pakon_replay --open       # prints "model detected: F-135+"

# Scan with the F-135+ scripts (Base 16, highest quality):
./build/pakon_replay --scan resources/f135plus/base16.pakscan --image scan.raw

# Decode: F-135+ rows have no trailing IR block when IR is off, and the
# row stride follows the resolution (Base 16 = 2000 px = 6000 samples):
python3 tools/pakon_image.py scan.raw --linewidth 6000 --no-ir-lane \
    --invert-c41 --jpeg
```

Base 8 / Base 4 scripts are in `resources/f135plus/` too (decode with
`--linewidth 4500` / `3000`); `base4_ir.pakscan` scans with the IR channel
(decode with `--linewidth 4000`, keep the default `--ir-lane`). Insert the
film strip at the feeder *before* starting the scan replay.

The OEM polls the film out of the transport after a scan; verbatim replay
cannot, so the strip can stop short of the exit. Append `--advance` to the
scan command to push it out afterwards (a fixed transport run;
`--advance-seconds` sets the duration, and a strip already at the exit
needs only 1-2 s), or run it standalone any time a strip is left inside:

```sh
./build/pakon_replay --advance --advance-seconds 3
```

Ctrl-C during a scan is safe: the first one stops the scan and replays the
captured teardown (motor and acquisition off) before exiting.

Frame-positioned advancing also works
(`pakon_replay resources/f135plus/advance.pakscan`); both it and
`--advance` probe the motor controller so the same commands work on either
model. The **web UI supports the F-135+ too**: it detects the model to pick
the scan script and auto-detects each raw's row layout, so the two-stage flow
works on both. Everything else F-135+ (protocol differences, stream format,
per-mode parameters) is in `docs/F135_PLUS_CAPTURES.md`.

### Debug logging

Set `PAKON_DEBUG=0..4` to control verbosity. Level 4 hexdumps every packet.

```sh
PAKON_DEBUG=3 ./build/pakon_probe
```

On Linux, pass the env explicitly with `sudo`:

```sh
sudo PAKON_DEBUG=3 ./build/pakon_probe
```

## Web service

The web service runs on the machine with the scanner plugged in and exposes a
browser UI for the full workflow: firmware load, scan, and image processing.
Any device on the local network can then open it.

```sh
pip install fastapi uvicorn python-multipart numpy pillow tifffile
uvicorn web.app:app --host 0.0.0.0 --port 8000
```

Open `http://<host>:8000`. The UI shows scanner connection state, loads firmware,
triggers a scan with a live progress bar, then runs the **two-stage minilab
flow**:

1. **Process (prescan)** — builds the long negative ribbon once, caches it, and
   renders a single long **preview strip** with auto-detected frame positions.
2. **Confirm frames** — position each crop on the strip (click to place, **+Add /
   −Remove**, keyboard **←/→** nudge, **↑/↓** prev/next, **Enter** next).
3. **Export** — only the confirmed crops are cropped at full resolution and run
   through the C-41 inversion + `rpd.pf` render. Download per-frame raw negative
   / TIFF / JPEG, a zip of any format, or a JPEG contact sheet.

The server calls the compiled `pakon_probe` / `pakon_replay` binaries for hardware
control and runs the Python image pipeline in a thread pool. Build the C tools
first (`cmake --build build`). The `rpd.pf` profile (in `profiles/`) is used for
the rendered JPEGs.

## Firmware

See `firmware/README.md` for provenance and the legal note. The `.pakfw` route
(replay from capture) is the practical path for the F-135; the Intel HEX
(`.hex`) route remains available for clean redistribution.

## License

GNU Affero General Public License, version 3 or later (`AGPL-3.0-or-later`).
Full text in `LICENSE`; see `NOTICE` for copyright and third-party terms.

AGPL was chosen because the web app is the product: §13 requires that anyone
who runs a modified pakon as a network service offer its source to the users
of that service, which plain GPL would not.

The Intel HEX firmware blob under `firmware/` is a third-party
device-bootstrapping artifact with its own separate terms and is **not**
covered by this license — see `firmware/README.md`.

## Disclaimer

Reverse-engineered and unofficial. Not affiliated with or endorsed by Kodak or
Pakon. The firmware blob is a third-party device-bootstrapping artifact used
as-is; see `firmware/README.md`.
