# pakon

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
> captured OEM sequences (`resources/f135plus/*.pakscan`). See `STATUS.md`
> for the roadmap.
>
> The decoder marker-aligns each row, registers the trilinear R/G/B lines,
> autocrops, and detects frames by autocorrelation. It also reproduces the OEM colour:
> the recovered **C-41 inversion** (a log-density ColNeg LUT) for a faithful
> positive, and the Kodak **`rpd.pf`** rendering profile for the vibrant JPEG
> look (see `docs/IMAGING.md`). The web UI is a **minilab-style two-stage
> flow** — prescan preview → operator confirms each crop in the browser →
> high-res export.

## Related projects

- [pakon-reference](https://github.com/alibosworth/pakon-reference): public,
  implementation-agnostic reference for the F-X35 family (EEPROM layout, DX,
  safety rules). Several of its hardware facts were measured with this project.
- libpakon (Stefan Dierauf): an independent C++ driver, used as a read-only
  reference (`docs/LIBPAKON_COMPARISON.md`).
- The original OEM Windows software, whose F-135 engine `TLB.dll` was
  reverse-engineered for interoperability (`docs/TLB_FINDINGS.md`). Not
  redistributed here.

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
  firmware-load sequence from Intel HEX.

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
`test_eeprom_c`, `test_setup`, `test_lightcal`, `test_fw`). When CMake finds
Python 3, ctest also runs the offline EEPROM tests (`test_eeprom`).

## Usage

### Overview

On power-on the scanner enumerates as a bare FX2 bootloader (`0F05:F235`). The
host downloads the FX2 firmware, after which it re-enumerates as the
operational scanner (`0F05:F135`). Everything after that goes over the command
channel.

On the **F-135** every step is built in code from three inputs: the OEM's FX2
firmware files (yours, from the OEM install), the unit's **EEPROM** (factory
per-unit values, read once and archived), and a **light calibration** measured
at the start of every scan. Nothing is replayed from a capture.

The **F-135+** still replays captured OEM sequences (`.pakscan` scripts, see
[F-135+ owners](#f-135-owners)); so does the F-135 when no EEPROM backup is
available ([Replay mode](#replay-mode)).

### Step-by-step (F-135)

**1. Firmware files (once)**

Put the two OEM firmware files in `firmware/` (git-ignored). From your OEM install's `FX35Driver` folder:

```sh
python3 tools/extract_fx2_loader.py ".../FX35Driver/F235Ldr.sys" firmware/PknLdr.hex
cp ".../FX35Driver/Pakon7.hex" firmware/
```

See `firmware/README.md` for where they come from.

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
nowhere else. Details in [EEPROM backup](docs/EEPROM_BACKUP.md);
`tools/pakon_eeprom.py decode DIR` decodes an archive without a scanner.
The web service does this step by itself (see [Web service](#web-service)).

**4. Scan**

```sh
./build/pakon_replay --scan-code --eeprom-dir backups/eeprom/F135-SERIAL --image scan.raw
```

Runs setup, the light calibration (about a minute: **keep the film out**, it
measures the open gate), then starts the motor and prints
`motor start (a0) sent -- feed the film now`. **Feed the strip then.** The
scan stops on its own once the open gate follows the film, and the teardown
stops the motor. Base 16 with the IR channel; about 240 MB per 4-frame strip,
up to ~2 GB for a 36-exposure roll (`--max-mb`, default 4096, is only a
runaway guard).

**5. Eject the strip**

```sh
./build/pakon_replay --eject --eeprom-dir backups/eeprom/F135-SERIAL
```

Runs the transport until the DX film sensors say the strip is out, or that it
has stalled at the exit (the tail has left the drive rollers: pull it out by
hand). Waits up to 15 s for film, at most 60 s in all.
`--advance-code SECONDS` runs the transport for a fixed time instead.

Other code-built tools: `--setup [--teardown]` (setup only), `--light-cal`
(calibration only, open gate), `--calib-probe` (static dark/lit levels),
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
- **IR (Digital ICE) block** — the trailing IR samples of each row are dropped
  (we do **not** do scratch removal — see `docs/IMAGING.md`).
- **Marker-bit row alignment** (fixes the R,G,B phase per scan) and **trilinear
  registration** of the R/G/B sensor lines.
- **Frame detection** — autocorrelation pitch → count → phase-locked comb →
  fixed-width centred crops.

Key decoder options:

| Flag | Default | Effect |
|------|---------|--------|
| `--invert-c41` | off | OEM-faithful C-41 positive (ColNeg log LUT + Dmin) |
| `--jpeg` | off | also write the `rpd.pf`-rendered JPEG (needs `profiles/rpd.pf`) |
| `--rotate {90,180,270}` | 0 | rotate each output frame |
| `--frames N` | auto | optional hard override of the auto-detected count |
| `--channel-order {fixed,auto,brg}` | fixed | R/G/B identity after marker alignment (fixed is verified) |
| `--register` / `--no-register` | on | co-register the trilinear R/G/B sensor lines |
| `--autocrop` / `--no-autocrop` | on | strip leader / blank pre-load scan / gate margin; dense negatives (frames under ~6% of full scale) can be cut as leader, use `--no-autocrop` |
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
(`pakon_replay resources/f135plus/advance.pakscan`), and it probes the motor
controller so it runs on either model. `--advance` is **F-135+ only**: it
replays that unit's captured motor speed, which is above the base F-135's
limit, so on an F-135 it refuses and points at `resources/advance.pakscan`. The **web UI supports the F-135+ too**: it detects the model to pick
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
`firmware/PknLdr.hex` + `firmware/Pakon7.hex` when present (see
`firmware/README.md`), else the captured `f135.pakfw`. Without an EEPROM
backup, and on the F-135+, scans replay the captured scripts.

```sh
python3 -m pip install -r web/requirements.txt
python3 -m uvicorn web.app:app --host 0.0.0.0 --port 8000
```

Open `http://<host>:8000`. The UI shows scanner connection state, loads firmware,
triggers a scan with a live progress bar, then runs the **two-stage minilab
flow**:

1. **Process (prescan)** — builds the long negative ribbon once, caches it, and
   renders a single long **preview strip** with auto-detected frame positions.
2. **Confirm frames** — position each crop on the strip (click to place, **+Add /
   −Remove**, keyboard **←/→** nudge, **↑/↓** prev/next, **Enter** next).
3. **Export** — only the confirmed crops are cropped at full resolution and run
   through the C-41 inversion + `rpd.pf` render. Colour and density are balanced
   per frame by the OEM's own scene balance (Kodak SBA + roll analysis), run from
   `oem/PakonIMAu.dll` under emulation (`tools/oem_sba.py`; needs `unicorn` and
   `pefile` from `web/requirements.txt`). Download per-frame raw negative / TIFF /
   JPEG, a zip of any format, or a JPEG contact sheet.

The server calls the compiled `pakon_probe` / `pakon_replay` binaries for hardware
control and runs the Python image pipeline in a thread pool. Build the C tools
first (`cmake --build build`). The `rpd.pf` profile (in `profiles/`) is used for
the rendered JPEGs.

## Firmware

See `firmware/README.md` for provenance and the legal note. Loading from the
OEM Intel HEX files (`pakon_probe --load-firmware-hex firmware/PknLdr.hex
firmware/Pakon7.hex`) is verified on the F-135; the stage-1 loader comes from
the OEM `F235Ldr.sys` via `tools/extract_fx2_loader.py`. The captured `.pakfw`
replay remains as a fallback.

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
