# Pakon F-X35 imaging pipeline — "the look"

How the raw `0x86` image stream becomes a finished scan, and how our tools
reproduce it. Wire/transport is in `docs/PROTOCOL.md`; this is the image side.

See **PROVENANCE** at the end: the OEM details come from reverse engineering the
original Windows software for interoperability. OEM binaries/data are **not**
committed.

## TL;DR (current)

- The C-41 negative→positive **inversion** is a single shared **log-density
  LUT**, `out = 3500·log10(16383/in)`, recovered exactly. On the **F-135 line**
  the OEM follows it with the unit's own **3×10 NegMatrix** from the EEPROM
  (see "The F-135 OEM colour path" below); the shared 3×3+offset
  `_ClientColNegMat.txt` path is the F-235's and is never called by `TLB.dll`.
- The vibrant **JPEG "look"** is the inverted image run through Kodak's **`rpd.pf`
  ICC rendering profile** + a scene-balance/tone pass. The plain **TIFF** is the
  scene-referred positive (no render).
- `tools/pakon_image.py` now implements **both**: `--invert-c41` (faithful
  positive) and `--jpeg` (rpd.pf render). It also marker-aligns each row,
  registers the trilinear lines, autocrops, and detects frames.
- The web service is a **two-stage minilab flow**: prescan → operator confirms
  crops in the browser → high-res export.

## The F-135 OEM colour path (from `TLB.dll`, 2026-09-28)

What the 135-line engine actually does to colour-negative data
(`FN_bLoadImageFromBuffer`; details in `docs/TLB_FINDINGS.md`) [C]:

1. The raw stream is calibrated per column (dark subtraction + 16.16 gain that
   normalises the open gate to 64000) into the 14-bit domain.
2. **Per-plane density LUT** on planar data: `LUT[i] = 3500·log10(16383/i)`,
   16384 entries, `LUT[0] = 0x3FFF`.
3. **Per-unit 3×10 matrix in density space** (NegMatrix from the EEPROM; PosMatrix
   for slides):
   `out = m0·R + m1·G + m2·B + m3·R² + m4·G² + m5·B² + m6·RG + m7·BR + m8·GB + m9`,
   rounded, clamped to 0–4095 (**12-bit** output). Note the term order RG, BR,
   GB.
4. Scale/rotate, then the 12-bit RPD / sRGB profiles and Ansel scene balance
   (roll-level balance over up to 40 pictures).

Film base is not subtracted in the matrix (its constants are positive): the
light calibration raises scan duty by `10^D` of a nominal base density
(colour negative R 0.144, G 0.40, B 0.715), so the base is largely neutralised
optically before the data reaches the LUT [I].

Our `--invert-c41` does not use the matrix: measured Dmin normalisation → LUT
→ sRGB, which matches the OEM output closely. Adding the per-unit NegMatrix
(read with `--read-params`) is the natural next step for OEM-faithful colour.

## The recovered inversion LUT

Source: decompiled `PakonIMAu.dll` (`re/out/PakonIMAu.c`) + `Config/ColorCorrection/`.
The OEM exposes export toggles (PSI "Other Options", classes `CiColorCorrection`
vs `CiColorCorrectionKodak`):

1. **Color Correction (12-bit RPD)** — `rpd.pf`, a real Kodak KCMS ICC profile
   (a `RPD_dls_3` + `yellow5` cascade). The Kodak colour science.
2. **Color Scene Balance (8-bit sRGB)** — SBA/DSBA, per-scene auto colour+density
   balance (brightness/neutralise/pop).
3. **Color Adjustments** — brightness/contrast/green/blue sliders; `Defaults.ini`
   shows these default to **neutral** for nearly all film products.

### ColNeg LUT + matrix — `Config/ColorCorrection/_ClientColNeg*.txt`

- **`_ClientColNegLut.txt`** — a single shared 14-bit curve, fit *exactly* (0 error
  over all 16384 entries):

  ```
  out = 3500 · log10(16383 / in)        [in = 0 → 16383]
  ```

  i.e. `density = log10(reference / signal)` scaled at 3500 code-values per
  density decade. THIS is the tonal flip. (`pakon_image.py:_c41_lut`.)
- **`_ClientColNegMat.txt`** — a 3×3 + offset dye-crosstalk / orange-mask matrix
  (diag ≈1.1, small negative off-diagonals, offsets [-82.6, -586.9, -707.8]).
  This is the F-235 engine's matrix; the F-135 uses its per-unit NegMatrix.
  (The OEM only loads client LUT/matrix files named without the leading
  underscore, and only when both exist.)

**Correction to the old note:** the **SCP** stage (`AnsSCPLut`, `FUN_10287eb0`) is
NOT the inversion — it's a per-channel **affine** LUT (`out = i·slope − offset`),
a balance/mask-normalisation step inside the separate Ansel "premium" cascade.
A positive-slope linear map can't flip a negative. `filmLut` ships as identity, so
no characterization curve is involved.

## What `tools/pakon_image.py` does today

Marker-align rows → deinterleave 16-bit LE R,G,B (+ trailing IR block) →
trilinear R/G/B line registration → autocrop → detect frames → per-frame crop →
invert/render → TIFF/JPEG.

- **`--invert-c41`** (the faithful positive): per-channel **Dmin** normalisation
  (orange-mask removal / white balance — the film base is measured robustly as a
  median of local high-percentile bands, never a single whole-roll percentile) →
  the ColNeg log LUT → sRGB encode. **Matrix and gray-world WB are OFF by default**
  — the matrix offsets over-subtract G/B in our scale (red cast), and gray-world
  strips intentional scene warmth (blue lean). Dmin normalisation alone matches
  the OEM. Verified against the OEM reference scans of two rolls.
- **`--jpeg`** (the vibrant render): the sRGB positive → `rpd.pf` via littleCMS →
  an SBA surrogate (per-channel auto-levels + midtone gamma) + a **soft highlight
  shoulder** that, unlike the OEM, does *not* blow out highlights. Flags
  `--rpd-profile`, `--jpeg-gamma`, `--jpeg-quality`. Profiles live in `profiles/`
  (committed; Kodak © — see `profiles/README.md`).

### Row layout and channel order

Each row is `[width × (R,G,B) | width IR samples]`, 16-bit LE, `width =
stride/4` with IR on and `stride/3` without. The phase is fixed by the marker
bit (LSB of one word per line), after which the order is **R, G, B**. Measured
strides (pakon-reference `image-stream.md`, measured with this project on an
F-135+):

| Mode | Samples/row | Visible width |
|---|---|---|
| highest, no IR | 6000 | 2000 |
| medium, no IR | 4500 | 1500 |
| lowest, no IR | 3000 | 1000 |
| lowest, IR on | 4000 | 1000 |
| highest, IR on | 8000 | 2000 (our F-135 captures; unmeasured on the F-135+) |

`--linewidth` defaults to 8000 (our captures); pass the stride for other modes,
with `--no-ir-lane` when the row has no IR block. The web UI detects the
layout per raw (`detect_row_layout`).
The older "per-zone channel order", "B,R,G" and "IR band mid-line / wrap-order"
models were artefacts of a floating row phase and are superseded (commit
`7474ea9`).

### Framing

`find_frame_grid`: never forces a count. Measures the true frame **pitch** by
autocorrelation of the per-row detail profile → count = `round(span/pitch)` →
phase-locked uniform comb → snap each tooth to its gap → drop leader/partial end
cells. The caller crops a **fixed 3000-px window centred between gaps** (the
leftover splits evenly as edge margin; per-gap rebate measurement was unreliable).
In the web flow the detected centres only *seed* the interactive confirmation.

## The OEM "Ansel" cascade (the full minilab look — context)

`PakonIMAu.dll` is built on Kodak's **Ansel** library (build paths `\Atc\ansel\src\`,
classes `CiColorCorrectionAnsel`, `AnsLut`, `AnsImaBuilder`; `anselinstalldir/`).
Data-driven: ~48 stages of ASCII LUT/param files (`dataPathItems/<stage>/`,
`Config/ColorCorrection/`; 333 files in the install). Stage order:

| # | Stage | Role |
|---|-------|------|
| 1 | `filmLut/` | film density → scene (ships identity) |
| 2 | `SCPLut/`  | Scan Color Processing — per-channel **balance** (not the inversion) |
| 3 | `dsba/`,`sba/` | Digital Scene Balance — per-scene auto colour+density balance |
| 4 | `flesh/` | flesh-tone correction |
| 5 | `toneHelper/`,`contrast/`,`lighting/` | tonal rendering per **path** (`CN-Enhanced`, `CN-Premium`, `DC-Premium`, `CP-Balance`) |
| 6 | `fugc/`,`deRender/`,`reRender/` | gamut + RIMM/ROMM colorspace mgmt |
| 7 | `Config/ColorCorrection/*.pf` | output colourspace (`srgb.pf`, `romm.pf`, `satminus15…satplus15`, B&W, `ColRevLut*`) |

Our `--jpeg` path reproduces the *practical* result (RPD profile + balance/tone)
without re-implementing the whole cascade.

## Digital ICE (dust/scratch removal) — NOT implemented

The scanner captures an **IR channel** (separate Ir lamp/exposure;
`WaitForLamp_Ir`, `Current_Ir`, `CcdExposure_Ir`; the IR is a defect map — IR
passes through dye but is blocked by physical dust/scratches). The OEM applies
Digital ICE via **`DMLDICELib.dll`** (loaded by `CN_CiDLLDigitalIce`; PSI "Use
Scratch Removal"; `IrChannelSavedInPlanarFile`, `IrCrossTalkFactor`).

**We do neither.** The decoder drops the trailing IR block (one IR sample per
pixel, `width` samples per row). A clean-room ICE (IR plane → defect mask →
inpaint) is a possible future feature; the IR data is captured but discarded.

## PROVENANCE

OEM imaging details come from reverse engineering the original Kodak/Pakon Windows
software (Ghidra decompilation of `PakonIMAu.dll`/`TLB` (F-135 engine)/`TLA`/`TLC` + inspection of the
`Config/ColorCorrection/` data) **for interoperability**. The OEM binaries and the
decompilation output are third-party copyrighted and are **NOT committed** (working
copies under `pakon-scanning-software/` and `re/`, git-ignored). The Kodak ICC
profiles + ColNeg data needed to reproduce the inversion are committed under
`profiles/` for personal/local use only (see `profiles/README.md`).
