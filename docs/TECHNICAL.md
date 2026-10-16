# pakon: technical guide

This is the developer and power-user side of pakon: building from source, the command-line tools, the web service, how the driver is put together, and where the protocol notes live. If you just want to scan, start with the [README](../README.md) and download the Mac app.

A cross-platform (Linux + macOS) driver and web app for the Kodak/Pakon
**F-135** and **F-135+** film scanners (the F-235 and F-335 are not supported;
see `docs/SCANNER_FAMILY.md` for what differs between the models). Unofficial,
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
> On the **F-135** every step is now **built in code**, nothing replayed:
> firmware from the OEM Intel HEX files, setup, light calibration, scan,
> teardown, film advance and sensor-driven eject (`--load-firmware-hex`,
> `--scan-code`, `--eject`), and the web UI uses that path. It needs the
> unit's EEPROM backup (`tools/pakon_eeprom.py backup`). Verified on one
> F-135 (serial 3054) on macOS, October 2026. The **F-135+** still replays
> captured OEM sequences (`resources/f135plus/*.pakscan`). See
> [Roadmap](#roadmap) for what's next.
>
> The decoder marker-aligns each row, registers the trilinear R/G/B lines,
> applies the OEM's per-column flat field from the light calibration,
> autocrops, and detects frames by autocorrelation. It also reproduces the OEM colour:
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
by both layers without coupling them to each other. Built on them:

- **`pakon_cmd`**: the one place that uses both layers: sends frames and
  generated sequences with the OEM reply rules and busy poll, and runs the
  event service.
- **`pakon_setup`**: builds the command sequences in code (controller init,
  per-mode configure, calibration writes, scan start, teardown, motor,
  event follow-ups), checked byte for byte against the captures.
- **`pakon_lightcal`**: line statistics and the light-calibration steps.
- **`pakon_eeprom`**: per-unit EEPROM decoder. **`pakon_fw`**: FX2
  firmware-load sequence from Intel HEX (parsed by **`pakon_hex`**).
- **`pakon_dx`**: decodes the DX sensor block (the film's edge barcode, read
  by the scanner itself) into the film's product code and frame numbers.

On top of the driver, the Python web service (`web/`) drives the C tools and
runs the image pipeline (`tools/pakon_image.py`) to serve the browser UI.

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

Produces: static `libpakon`, tools `pakon_probe` and `pakon_replay`, and the
hardware-free unit tests (`test_proto`, `test_hex`, `test_calib`,
`test_eeprom_c`, `test_setup`, `test_lightcal`, `test_fw`, `test_dx`). When
CMake finds Python 3, ctest also runs the offline Python tests
(`test_eeprom`, `test_image_colour`, `test_dx_metadata`, `test_oem_sba`); the
last three skip themselves when their Python libraries are missing.

## Usage

### Overview

On power-on the scanner enumerates as a bare FX2 bootloader (`0F05:F235`). The
host downloads the FX2 firmware, after which it re-enumerates as the
operational scanner (`0F05:F135`). Everything after that goes over the command
channel.

On the **F-135** every step is built in code from three inputs: the OEM's FX2
firmware files (shipped in `firmware/`), the unit's **EEPROM** (factory
per-unit values, read once and archived), and a **light calibration** measured
at the start of every scan. Nothing is replayed from a capture.

The **F-135+** still replays captured OEM sequences (`.pakscan` scripts, see
[F-135+ owners](#f-135-owners)); so does the F-135 when no EEPROM backup is
available ([Replay mode](#replay-mode)).

### Step-by-step (F-135)

**1. Firmware files**

The two OEM firmware files ship in `firmware/` (`PknLdr.hex`, `Pakon7.hex`);
see `firmware/README.md` for where they come from and how to regenerate them.

**2. Load firmware (every power-on)**

```sh
./build/pakon_probe --list     # 0f05:f235 = cold, needs firmware; 0f05:f135 = ready
./build/pakon_probe --load-firmware-hex firmware/PknLdr.hex firmware/Pakon7.hex
```

The loader refuses to continue unless the scanner reports the F-135/F-135+
personality. Linux needs `sudo` (or a udev rule) for libusb; macOS does not.

**3. Back up the EEPROM (once)**

```sh
python3 -m venv .venv && .venv/bin/pip install pyusb
mkdir -p backups/eeprom
.venv/bin/python tools/pakon_eeprom.py backup backups/eeprom/F135-SERIAL
```

Reads all four copies of the per-unit EEPROM (read-only, the OEM's own read
requests) and archives them with CRC results and SHA-256 hashes. Do it once,
right after a power-on, and keep a copy off the machine: this data exists
nowhere else. Details in [EEPROM backup](EEPROM_BACKUP.md);
`tools/pakon_eeprom.py decode DIR` decodes an archive without a scanner.
The web service does this step by itself (see [Web service](#web-service)).

**4. Scan**

```sh
./build/pakon_replay --scan-code --eeprom-dir backups/eeprom/F135-SERIAL --image scan.raw
```

`--film bw` scans silver B&W film and `--film slide` unmounted slide (E-6)
strips (default `c41`, colour negative): the calibration caps and scan LED
duties use the OEM's film-base densities for that film type.

Runs setup, the light calibration (about a minute: **keep the film out**, it
measures the open gate), then starts the motor and prints
`motor start (a0) sent -- feed the film now`. **Feed the strip then.** The
scan stops on its own once the open gate follows the film, and the teardown
stops the motor. Base 16 with the IR channel; about 240 MB per 4-frame strip,
up to ~2 GB for a 36-exposure roll (`--max-mb`, default 4096, is only a
runaway guard). The calibration's per-column tables are saved next to the
raw as `scan.raw.flat.json`; the decoder picks them up (see
[Flat field](#flat-field)). When the scanner reads the film's DX edge code,
the film's product code is saved as `scan.raw.dx.json` (see
[Film identity (DX)](#film-identity-dx)).

**5. Eject the strip**

```sh
./build/pakon_replay --eject --eeprom-dir backups/eeprom/F135-SERIAL
```

Runs the transport until the DX film sensors say the strip is out, or that it
has stalled at the exit (the tail has left the drive rollers: pull it out by
hand). Waits up to 15 s for film, at most 60 s in all.
`--advance-code SECONDS` runs the transport for a fixed time instead.

Other code-built tools: `--setup [--teardown]` (setup only), `--light-cal`
(calibration only, open gate; writes the flat field to
`DIR/flatfield.json`, or `--flat-out FILE`), `--calib-probe` (static dark/lit levels),
`--film-sense SECONDS` (log the DX sensor levels while the transport runs).
All take `--eeprom-dir`.

### Replay mode

The original path, still used on the F-135+ and as the F-135 fallback:
replaying sequences extracted from USB captures of the OEM driver.

- **`.pakfw`**: the firmware load as captured (`resources/f135.pakfw`, same
  FX2 image for both models):
  `./build/pakon_probe --load-firmware resources/f135.pakfw`.
- **`.pakscan`**: a scan or advance as captured.
  `./build/pakon_replay --scan resources/scan.pakscan --image scan.raw`
  replays an F-135 scan verbatim (insert the film first; `--autostop` stops at
  the end of the film); `./build/pakon_replay resources/advance.pakscan
  --steps N` replays the advance. Generate your own with
  `tools/analyze_capture.py`.
- `./build/pakon_replay --open` replays the open handshake and prints the
  detected model.

### Decode the image

**Option A — web UI (recommended):** start the web service (see below) and
open `http://localhost:8000`. Open the `.raw` (a scan is already loaded), click **Process** (prescan →
long preview strip + detected frames), **confirm/position each crop** on the
strip, then **Export** for high-res output, and save it from the Export page
into the work folder: `jpeg/`, `tiff/` (plain positive) and `raw/` (raw
negative TIFF) subfolders, plus a contact sheet. When the scan read the
film's DX code, each file carries the film name and ISO.

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
- **Flat field** — the per-column dark and gain correction from the light
  calibration (see [Flat field](#flat-field)).
- **IR (Digital ICE) block** — the trailing IR samples of each row are dropped
  (no dust and scratch removal yet: a prototype is in
  [#37](https://github.com/jorshhh/pakon-macos/pull/37), see `docs/IMAGING.md`).
- **Marker-bit row alignment** (fixes the R,G,B phase per scan) and **trilinear
  registration** of the R/G/B sensor lines.
- **Frame detection** — autocorrelation pitch → count → phase-locked comb →
  fixed-width centred crops.

Key decoder options:

| Flag | Default | Effect |
|------|---------|--------|
| `--invert-c41` | off | OEM-faithful C-41 positive (ColNeg log LUT + Dmin) |
| `--bw` | off | silver B&W: with `--invert-c41`, the same inversion then grey (16-bit grey TIFF, grey JPEG) |
| `--slide` | off | slide film: no inversion; OEM positive path (PosMatrix → `ColRevLut1.pf` → `romm.pf`) |
| `--jpeg` | off | also write the `rpd.pf`-rendered JPEG (needs `profiles/rpd.pf`) |
| `--rotate {90,180,270}` | 0 | rotate each output frame |
| `--format {full,half,pano}` | full | picture format the frames are found and cropped in |
| `--channel-order {fixed,auto,brg}` | fixed | R/G/B identity after marker alignment (fixed is verified) |
| `--flat FILE` / `--no-flat` | auto | per-column flat field; default `RAW.flat.json`, else `EEPROM_DIR/flatfield.json` |
| `--register` / `--no-register` | on | co-register the trilinear R/G/B sensor lines |
| `--autocrop` / `--no-autocrop` | on | strip leader / blank pre-load scan / gate margin; dense negatives (frames under ~6% of full scale) can be cut as leader, use `--no-autocrop` |
| `-o PREFIX` | `frame` | output filename prefix |

### Flat field

The F-135's illumination falls off toward the ends of the sensor line: on
serial 3054 a column at the edge gets about a quarter of the light that the
centre gets. Uncorrected, this shows as a bright band along one edge of every
frame (the bottom, after the export's rotation). Like the OEM's `TLB.dll`, the
light calibration measures each column dark and through the empty gate, and
the decoder applies `(raw − dark) · 64000 / (open gate − dark)` per column.

- `--scan-code` saves the tables with each scan (`scan.raw.flat.json`).
- `--light-cal --eeprom-dir DIR` (gate empty) saves `DIR/flatfield.json`,
  which the decoder and the web app use for any raw without its own file, so
  older scans are corrected too.
- Turn it off with `--no-flat` (CLI) or `PAKON_FLAT=0` (web). F-135 Base 16
  only for now; a table that doesn't match the scan's width is skipped.

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
(`pakon_replay resources/f135plus/advance.pakscan`), and it probes the motor
controller so it runs on either model. `--advance` is **F-135+ only**: it
replays that unit's captured motor speed, which is above the base F-135's
limit, so on an F-135 it refuses and points at `resources/advance.pakscan`. The **web UI supports the F-135+ too**: it detects the model to pick
the scan script and auto-detects each raw's row layout, so the two-stage flow
works on both. Everything else F-135+ (protocol differences, stream format,
per-mode parameters) is in `docs/F135_PLUS_CAPTURES.md`.

### Film identity (DX)

35 mm film carries a DX barcode along its edge with the film's product code
and frame numbers. The scanner decodes it itself; the event service reads the
result (PICL register `0x90`, `docs/PROTOCOL.md`), logs it as `dx:` lines at
`PAKON_DEBUG=2`, and a scan ends with a summary such as
`dx: film 82/3, 5 codes (0 rejected), frames 6..4`.

`--scan-code` saves the product code as `scan.raw.dx.json`. The web app looks
it up in the OEM product-code table (`oem/ansel/common/`), shows the film next
to the raw in Process, and writes it into every exported JPEG and TIFF: the
film name as the description, caption and keyword (EXIF and XMP, embedded in
the file) and the ISO. The table was last revised in 2006, so newer films
often have no entry; then nothing is written. Frame numbers are decoded but
not used yet.

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
browser UI for the full workflow: firmware load, scan, eject, and image
processing. Any device on the local network can then open it.

The service reads the connected scanner's EEPROM by itself: at launch, after
**Load firmware**, or before a scan or eject, once per USB connection, with the
same read-only requests as `tools/pakon_eeprom.py backup`. The first time it
sees a unit it archives the EEPROM as `backups/eeprom/<model>-<serial>-<date>`;
after that it reuses the archive for that serial and picks it over other
units' archives. `PAKON_EEPROM_DIR` names an archive instead and turns the
automatic read off. Keep a copy of the archive off the machine: this data
exists nowhere else.

On an F-135 it uses the code-built path once it has the unit's EEPROM
archive:
**Scan** calibrates first (about a minute, keep the film out), then shows
**"Motor running — feed the film now"**; feed the strip then. **Eject film**
runs the transport until the film sensors say the strip is out (or stalled at
the exit, where it has to be pulled by hand). **Load firmware** uses
`firmware/PknLdr.hex` + `firmware/Pakon7.hex` (see `firmware/README.md`),
else the captured `f135.pakfw` if they are missing. Without an EEPROM
backup, and on the F-135+, scans replay the captured scripts.

```sh
python3 -m pip install -r web/requirements.txt
python3 -m uvicorn web.app:app --host 0.0.0.0 --port 8000
```

Open `http://<host>:8000`. The UI opens on **Home** (start a scan or open a
raw) and has a space for each step: **Scan** (connection state, firmware,
work folder and film type, scan with a live progress bar, eject), then the
**two-stage minilab flow**:

1. **Process (prescan)** — builds the long negative ribbon once, caches it, and
   renders a single long **preview strip** with auto-detected frame positions.
2. **Confirm frames** — position each crop on the strip (click to place, **+Add /
   −Remove**, keyboard **←/→** nudge, **↑/↓** prev/next, **Enter** next).
3. **Export** — only the confirmed crops are cropped at full resolution and run
   through the C-41 inversion + `rpd.pf` render. Colour and density are balanced
   per frame by the OEM's own scene balance (Kodak SBA + roll analysis), run from
   `oem/PakonIMAu.dll` under emulation (`tools/oem_sba.py`; needs `unicorn` and
   `pefile` from `web/requirements.txt`). Download a single frame (raw
   negative, TIFF or JPEG), or save every frame of a format, or a contact
   sheet, into the order's folder in the work folder. The film name and ISO
   go into each file when the scan read the DX code.

**Settings** holds the crash-report and usage-statistics switches (see
[Privacy](../README.md#privacy)). On macOS the same service also runs as a standalone
app, `Pakon.app` (`packaging/macos/README.md`).

Processing applies the [flat field](#flat-field) automatically when one is
found (the scan's own, else the EEPROM archive's `flatfield.json`);
`PAKON_FLAT=0` turns it off.

The server calls the compiled `pakon_probe` / `pakon_replay` binaries for hardware
control and runs the Python image pipeline in a thread pool. Build the C tools
first (`cmake --build build`). The `rpd.pf` profile (in `profiles/`) is used for
the rendered JPEGs.

## Roadmap

Open work is tracked in [GitHub issues](https://github.com/jorshhh/pakon-macos/issues);
`STATUS.md` has the background. In short:

- **F-135+ built in code:** scan (#14), light calibration and flat field
  (#15), film advance and eject (#16).
- **Scan options:** resolution Base 4 / 8 / 16 (#17); B&W C-41 film (#18).
- **Image:** Digital ICE dust and scratch removal (#26, prototype in #37); the
  remaining OEM scene-balance stages (#22).
- **Distribution:** installable release builds for macOS, Windows and Linux
  (#25).

## Firmware

See `firmware/README.md` for provenance and the legal note. Loading from the
OEM Intel HEX files (`pakon_probe --load-firmware-hex firmware/PknLdr.hex
firmware/Pakon7.hex`) is verified on the F-135; the stage-1 loader comes from
the OEM `F235Ldr.sys` via `tools/extract_fx2_loader.py`. The captured `.pakfw`
replay remains as a fallback.

## More documentation

| File | What's in it |
|---|---|
| [`../STATUS.md`](../STATUS.md) | Goal, what works, the roadmap, open questions |
| [`PROTOCOL.md`](PROTOCOL.md) | Frames, replies, handshake, event service, DX sensor block, scan sequence |
| [`REGISTERS.md`](REGISTERS.md) | Every register and command we write, with encodings |
| [`TLB_FINDINGS.md`](TLB_FINDINGS.md) | Facts from the OEM F-135 engine (`TLB.dll`) |
| [`IMAGING.md`](IMAGING.md) | Image layout, decoder, colour pipeline |
| [`EEPROM_BACKUP.md`](EEPROM_BACKUP.md) | Reading and archiving the per-unit EEPROM |
| [`SCANNER_FAMILY.md`](SCANNER_FAMILY.md) | F-135 / F-135+ / F-235 / F-335 differences |
| [`F135_PLUS_CAPTURES.md`](F135_PLUS_CAPTURES.md), [`F135_PLUS_NOTES.md`](F135_PLUS_NOTES.md) | F-135+ support |
| [`CAPTURE_GUIDE.md`](CAPTURE_GUIDE.md) | Recording USB captures of the OEM software |
| [`LIBPAKON_COMPARISON.md`](LIBPAKON_COMPARISON.md) | Notes against libpakon |
| [`DECISIONS.md`](DECISIONS.md) | Design decisions |
| [`../packaging/macos/README.md`](../packaging/macos/README.md) | Building and signing `Pakon.app` |

Credits, license and privacy are in the [README](../README.md).
