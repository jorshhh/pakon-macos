# Firmware blobs

The Pakon scanners are Cypress/Anchor **EZ-USB FX2** devices. On cold power-on
they come up in the FX2 bootloader state and must be handed a firmware image
over the standard FX2 download protocol (vendor request `0xA0` to internal
8051 RAM) before they re-enumerate as the scanner (`0F05:F135`).

The firmware images ship as **Intel HEX** files in the `FX35Package/` folder of
the [`ktkaufman03/FX35`](https://github.com/ktkaufman03/FX35) repository.

## Provenance / legal note

These `.hex` files are **device-bootstrapping artifacts**. They are used as-is
to bring the hardware up and are **not** reverse-engineered or modified. They
are not part of this project's clean-room work; this project only ships a copy
for convenience and documents where it came from. If redistribution terms are
unclear, the build can instead point at a user-supplied path.

## Practical path on the test unit: replay from capture (`.pakfw`)

The F-135 test unit's firmware load was decoded from a real capture: it is the
standard FX2 sequence (`0xA0` internal RAM + `0xA3` external RAM + CPUCS
`0xE600` reset). Rather than depend on the `.hex` blobs, we extract the exact
control-transfer sequence from a capture and replay it:

```sh
# from a capture that includes the f235->f135 load (e.g. a scan capture):
python3 tools/analyze_capture.py CAPTURE.pcapng --extract-firmware resources/f135.pakfw
# then, with the cold 0F05:F235 device attached to the host (not a VM):
sudo ./build/pakon_probe --load-firmware resources/f135.pakfw
```

`pakon_usb_load_firmware()` opens the cold `0F05:F235` device, replays each
control transfer, and waits for re-enumeration to operational `0F05:F135`.

The generated `.pakfw` contains Kodak firmware bytes; this repo currently
ships one at `resources/f135.pakfw` (see the provenance note above — same
status as the `.hex` blobs). Regenerating from your own capture remains the
clean-room route if redistribution is a concern.

## Model → file mapping

Resolved 2026-08-12 from the OEM driver package (`F235usb2.inf`) and Kai
Kaufman's FX35 loader source, and verified on real F-135+ hardware. Selection
works by **personality, not model name**: every family member cold-enumerates
as `0F05:F235`, the driver reads an 8-byte personality via vendor request
`0xA9` (VID, PID, revision word, extra byte), and the registry key
`<PID>_<revision>` picks the image. The revision's low byte is the model
family:

| Personality key | HEX file   | Family | Verified |
|-----------------|------------|--------|----------|
| `F235_AA05`     | Pakon5.hex | F-235  | no |
| `F235_AA07`     | Pakon7.hex | F-135 **and F-135+** | yes — both (F-135: Linux + macOS; F-135+: macOS, serial 16402) |
| `F235_AA08`     | Pakon8.hex | F-335  | no |

The F-135+ shares the F-135's FX2 image: `resources/f135.pakfw` (captured
from an F-135 load) leaves exactly the OEM `Pakon7.hex` image in RAM
(verified byte-for-byte, 10355/10355) and brings a real F-135+ up as
operational `0F05:F135`. The Plus differences live entirely in the PIC
boards (NL/NM firmware), which keep their own flash and are not loaded over
USB at power-on. `PknInit.hex` is the common bootstrap stage downloaded
first (it implements external-RAM writes for the main image). See
`docs/F135_PLUS_CAPTURES.md`.
