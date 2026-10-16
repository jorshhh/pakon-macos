# Explore: consolidating Pakon into a single Rust binary

**Status:** exploration / decision doc — no code committed. Written 2026-06-01.

## The idea

Today the project is three languages glued together:

- **C** (`libpakon` + `pakon_probe` / `pakon_replay`) — libusb transport, the
  36-byte protocol, firmware load, scan replay. ~3.7k LOC.
- **Python** (`tools/pakon_image.py`, `tools/analyze_capture.py`, `web/`) — the
  image pipeline (deinterleave → register → C-41 invert → rpd.pf render),
  capture analysis, and a FastAPI web service. ~2.3k LOC.
- **HTML/JS** (`web/static/index.html`) — the minilab two-stage UI. ~840 LOC.

The web server **shells out** to the C binaries (`asyncio.create_subprocess_exec`
of `pakon_probe`/`pakon_replay`) and **imports** the Python decode module. So a
working install needs: a C toolchain + CMake build, a Python venv with
numpy/tifffile/Pillow/pyusb/fastapi/uvicorn, ImageMagick (`magick`), and the two
compiled binaries discoverable on disk.

**Proposal:** rewrite the whole thing in Rust as one statically-linked binary
that owns libusb directly (via a Rust USB crate), runs the image pipeline
natively, and serves both the JSON/SSE API and the existing HTML. No
subprocess calls, no Python runtime, no ImageMagick. Ship one file.

## Why this is attractive here

1. **Distribution.** A film-scanning appliance wants to be `scp` one binary to
   the Linux box and run it. Right now we ship a build tree + a venv + system
   packages. The cross-language seam (`web/app.py:108` spawns `pakon_probe`,
   `:157` spawns `pakon_replay`, `web/decode.py:26` imports `pakon_image`) is
   exactly the brittle part.
2. **Kill the IPC.** Scan progress is currently inferred by **polling the output
   file's size** (`web/app.py:164-177`) because the C process writes a raw file
   and the Python side can't see inside it. In-process, the USB read loop can
   emit real byte/line/frame progress directly to the SSE channel.
3. **One memory model for the image.** The ribbon is built in Python, cached to
   a `ribbon.npy` on disk (`web/decode.py:109`), then reloaded mmap'd for export
   (`:147`). A single process can hold the ribbon in memory (or one mmap) across
   prescan → confirm → export without the `.npy` round-trip.
4. **Type safety on the protocol.** The 36-byte frame, address/status enums, and
   wire-length rules are exactly the kind of thing Rust's enums + `#[repr]`
   structs encode well, with the same `static_assert(sizeof==36)` discipline.

## Resolved: the web minilab is the product (no SANE backend)

The project originally aimed at a SANE backend, which is why the early C
layering rules and a (now-deleted) `backend/` shim existed. **That goal has
been dropped** — the web minilab is the product. The old SANE backend stub and
the SANE-framed plan have been removed from the repo. Everything below assumes a
single product: the browser-driven scanning + decoding service.

## Component-by-component migration

| Current piece | LOC | Rust replacement | Difficulty | Notes |
|---|---|---|---|---|
| `pakon_usb.c` (libusb transport) | ~570 | `nusb` (pure-Rust) or `rusb` (libusb binding) | **Low–med** | API maps almost 1:1 to the header. `nusb` removes the libusb C dep entirely → truly static. Control transfers, bulk in/out, claim/alt-set, clear_halt all supported. |
| `pakon_proto.c` + `pakon_cmd.c` | ~140 | plain Rust structs/enums | **Low** | 36-byte frame, `2+count` wire length, address/status enums. Natural fit. |
| `pakon_hex.c` (Intel HEX / FX2 load) | ~190 | `ihex` crate or hand-rolled | **Low** | Firmware load is replaying a `.pakfw` control-transfer script — mostly EP0 writes. |
| `pakon_calib.c` | ~410 | Rust | **Low–med** | Calibration table parsing; straightforward byte work. |
| `pakon_replay.c` (scan driver) | ~1270 | Rust | **Med** | The state machine: open → param read → configure → scan + 0x86 image reads + `--autostop` EOR logic. The autostop arming/`film_seen` logic (recent bug area) needs careful porting. |
| `pakon_image.py` (decode pipeline) | ~800 | `ndarray` + `image` + `tiff` + `lcms2` | **HIGH** | The real work. See below. |
| `analyze_capture.py` (pcapng parsing) | ~550 | optional — leave in Python | **N/A** | A dev/forensics tool, not in the runtime path. No need to port. |
| `web/app.py` (FastAPI + SSE) | ~500 | `axum` + `tokio` + SSE | **Med** | Routes are simple; SSE is `axum::response::sse`. File uploads, static serving, zip export all have crate support. |
| `web/static/index.html` | ~840 | **keep verbatim** | **None** | Served by axum `ServeDir`. This is the whole point — the frontend doesn't change. |

### The hard part: `pakon_image.py`

This is ~800 lines of numpy doing real DSP/color science, and it's the part most
likely to introduce subtle regressions:

- **Deinterleave**: strided slicing of a `<u2` memmap into R/G/B planes
  (`web/decode.py:62-63`). `ndarray` with custom strides handles this, but the
  ergonomics are worse than numpy's `img[:, 0:n3:3]`.
- **IR band / zone detection**, **trilinear registration** (`measure_leads`,
  `register_zones`), **OEM framing** (`tlx_frame_grid`),
  **Dmin measurement** (`measure_dmin`) — all array reductions, FFTs/correlations,
  percentile ops. numpy's `np.percentile`, `np.correlate`, broadcasting all need
  explicit equivalents (`ndarray-stats`, a correlation/FFT crate like `rustfft`).
- **C-41 inversion** (`invert_c41` + `_c41_lut` + `_C41_MAT`): per-channel Dmin
  normalize → shared log-density LUT (`3500*log10(16383/in)`) → 3×3 matrix →
  sRGB. Pure arithmetic, ports cleanly — but must match numpy's float semantics
  bit-closely enough that output matches the verified OEM reference scans.
- **`render_jpeg`**: applies the Kodak **`rpd.pf` ICC profile**. In Python this
  presumably goes through Pillow/`magick`/littleCMS. In Rust → the `lcms2`
  crate (Little CMS binding). This is the one place a C dep likely sneaks back
  in, but it's well-contained.

Risk: the image math is **empirically tuned and verified against OEM reference
scans** (per the memory notes and `docs/IMAGING.md`). Any port must be validated
against the existing Python output pixel-for-pixel (or within a tight ΔE / PSNR
threshold), not just "looks right." Budget real time for a golden-image test
harness.

## Suggested crate stack

- **USB:** `nusb` (preferred — pure Rust, no libusb C dependency, async) or
  `rusb` (mature, but pulls libusb back in). `nusb` aligns best with the
  "single static binary" goal.
- **Web:** `axum` + `tokio` + `tower-http` (`ServeDir` for static, compression,
  `Body`/multipart for uploads). SSE via `axum::response::sse::Sse`.
- **Arrays:** `ndarray` (+ `ndarray-stats` for percentiles).
- **Image I/O:** `image` (JPEG, PNG, thumbnails), `tiff` (16-bit RGB TIFF —
  verify it writes the `photometric=rgb` 16-bit files we need), `zip` for the
  export archive.
- **Color:** `lcms2` for the `.pf` ICC profile render.
- **FFT/correlation:** `rustfft` if the autocorrelation framing needs it.
- **HEX:** `ihex` or hand-rolled (it's small).

Static linking on Linux: target `x86_64-unknown-linux-musl` for a fully static
binary (with `nusb` + no libusb, this is achievable; `lcms2` is the thing to
check — may need vendoring or a `musl` build).

## Incremental path (don't big-bang it)

A rewrite-in-place is risky given the verified image pipeline. Suggested order
that keeps a working product at every step:

1. **Scaffold** a Rust binary that serves the existing `index.html` via axum and
   proxies/shells to the *current* Python+C for everything else. Prove the web
   shell + SSE plumbing works. (Frontend untouched.)
2. **Port the transport + protocol + replay** (the C side) to Rust behind the
   same `/api/scan` and `/api/firmware` endpoints. Now no C binaries. Validate
   against a real scan on hardware (Linux box).
3. **Port the image pipeline** last, behind a **golden-image test** that diffs
   Rust output against the current Python output for `fullroll.raw` and the
   36-exp sample. This is where most of the effort and risk lives.
4. **Drop Python.** Delete the venv requirement; `analyze_capture.py` can stay as
   an out-of-band dev tool (it never runs in production).

Each phase ships something runnable; you can stop after phase 2 (no Python in
the device path except imaging) if the image port proves too costly.

## Risks & open questions

- **Image-pipeline fidelity** is the dominant risk. numpy → ndarray is not a
  mechanical translation when percentiles, FFT correlation, and float rounding
  feed into verified color output. Mitigation: golden-image regression tests
  before porting a single line.
- **`lcms2` reintroduces a C dependency**, partly undercutting "pure Rust /
  static." It's contained, but note it.
- **Development velocity.** Python's numpy makes iterating on the *still-evolving*
  imaging (Digital ICE is unimplemented; framing heuristics still being tuned)
  much faster than Rust. Freezing imaging into Rust too early could slow the very
  experiments that are still active.
- **Hardware-in-the-loop testing** doesn't get easier: every transport/replay
  change still requires the Linux box + scanner. The Rust port doesn't remove the
  push-pull-rebuild loop on hardware; it just changes the build command.
- **No async-USB win needed.** `pakon_replay`'s scan loop is sequential bulk
  reads; tokio/async buys clean SSE progress but isn't required for the USB
  itself.

## Tentative recommendation

The single-binary goal is **sound and worth it for distribution**, and the
transport/protocol/replay/web layers are a **comfortable, low-risk Rust port**
that immediately removes the subprocess seam and the size-polling hack. The
**image pipeline is the expensive, risky 20%** and is still under active
development — so do it *last*, behind golden-image tests, and don't block the
rest on it.

Recommended: pursue the **incremental path** and treat **phase 2 (no C
binaries, imaging still in Python via PyO3 or a thin sidecar)** as a legitimate
stopping point.

## Decision needed before starting

1. **Acceptable to keep one C dep (`lcms2`) for ICC rendering**, or must it be
   pure Rust (find/port an ICC engine)?
2. **`nusb` (pure Rust) vs `rusb` (libusb)** — defaults to `nusb` unless a
   capability gap shows up on the F-135.
</content>
</invoke>
