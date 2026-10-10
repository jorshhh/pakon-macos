# Colour profiles & correction data

Kodak/Pakon colour-correction data extracted from the OEM F-X35 COM SERVER
install (`Config/ColorCorrection/`), used by `tools/pakon_image.py` to reproduce
the OEM's image processing. © Eastman Kodak Company.

## Used today

- **`rpd.pf`** — "RPD Rendering Profile" (a `RPD_dls_3` + `yellow5` ICC cascade).
  The OEM "Use Color Correction (12-bit RPD)" stage. Applied by `--jpeg` via
  littleCMS to render the vibrant positive. Default for `--rpd-profile`.
- **`srgb.pf`** — sRGB IEC61966-2.1 output profile (the OEM output colourspace).
- **`_ClientColNegLut.txt`** — the C-41 negative inversion curve. Fit exactly to
  `out = 3500*log10(16383/in)`; this formula is baked into `pakon_image.py`
  (`_c41_lut`). Kept here as the source of truth / provenance.
- **`_ClientColNegMat.txt`** — ColNeg 3×3 + offset crosstalk matrix (the
  `_C41_MAT`/`_C41_OFF` constants; off by default — over-subtracts G/B here).
- **`Defaults.ini`** — per-film-product colour-slider defaults (mostly neutral).

## Available for future use

- **`satplus03..15.pf` / `satminus03..15.pf`** — saturation-variant rendering
  profiles (could back a `--saturation` option).
- **`romm.pf`, `unity.pf`** — alternate output colourspaces (ROMM / Kodak Lab).
- **`cold_bw.pf`, `warm_bw_ld0_9_22.pf`, `sepia_*.pf`** — B&W / sepia looks.
- **`ColRevLut1.pf`, `ColRevLutS6.lut`** — colour-reversal (slide/positive) path.

Re-extract from the OEM install if needed; see the `[[oem-software-cache]]`
memory for the location.
