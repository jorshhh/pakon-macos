# EEPROM backup and decoding

`tools/pakon_eeprom.py` archives the four documented calibration sections of
the F-135/F-135+ EEPROM and decodes CRC-valid copies. It preserves each original
copy, including corrupt ones. It never writes or repairs EEPROM.

## Current validation

Offline tests cover published base F-135 and F-135+ dumps, including a corrupt
primary copy, and mocked USB failures. The new hardware path has **not yet
been validated on a physical scanner**. Passing these tests does not establish
successful USB access or reliable reads on a particular Mac.

This tool is separate from the older `pakon_replay --read-params` diagnostic,
which does not archive all four copies. Use this tool for a complete backup of
the documented sections. Neither tool archives host-side light calibration.

## Decode existing files without a scanner

Only Python 3's standard library is needed:

```sh
python3 tools/pakon_eeprom.py decode test/fixtures/eeprom/F135-2233
python3 tools/pakon_eeprom.py decode test/fixtures/eeprom/F135plus-16402
python3 test/test_eeprom.py
```

`decode` accepts a directory with these files:

| File | EEPROM offset | Bytes including header |
|---|---|---|
| `eeprom_0x52_sectionA_primary.bin` | `0x000` | 398 |
| `eeprom_0x52_sectionA_backup.bin` | `0x400` | 398 |
| `eeprom_0x52_sectionB_primary.bin` | `0x800` | 36 |
| `eeprom_0x52_sectionB_backup.bin` | `0xA00` | 36 |

It prints JSON and makes no USB calls. If `SHA256SUMS` exists, every listed
file is verified, and every present section file must be listed. Hash failure
stops decoding. An archive without a manifest can still be decoded, with
`hash_manifest_verified: false`.

## First hardware backup

Close other software that accesses the scanner. Connect exactly one scanner.
The scanner must already be operational (`0f05:f135`), with its normal RAM
firmware loaded using the existing supported procedure. This tool deliberately
does not load firmware, reset the device, change its configuration, or send
PPB commands. Reading vendor EP0 requests without additional initialization
is a hardware assumption to verify during the first test.

The hardware mode requires libusb and PyUSB. PyUSB is already listed in
`web/requirements.txt`; run with that environment's Python. No Python USB
dependency is needed for offline decoding or tests.

Choose a fresh directory for each power cycle:

```sh
mkdir -p backups/eeprom
python3 tools/pakon_eeprom.py backup backups/eeprom/F135-cycle1
```

Confirm the decoded model and serial belong to your scanner. Inspect every
copy's length/CRC result, `selected`, `copies_equal`, and `warnings`; exit zero
does not imply that every copy was good. A bad primary with a good backup is
reported explicitly and can still supply valid decoded data.

Power-cycle the scanner, load its normal RAM firmware again, and make one
second backup under a different name:

```sh
python3 tools/pakon_eeprom.py backup backups/eeprom/F135-cycle2
```

Compare the four binary files across cycles, not `report.json` (its timestamp
and USB address may change). Avoid repeated reads within a power cycle; this
is an operator procedure, not something the tool can reliably enforce across
processes. A difference needs investigation, not an EEPROM repair. Keep both
archives somewhere separate from the development machine. Archive directories
are created with owner-only permissions; the suggested `backups/` location is
git-ignored.

## Read sequence and failure behavior

For each chunk the only transfers are:

1. Vendor OUT `bmRequestType=0x40`, `bRequest=0xA4`, `wValue=0x00A5`,
   `wIndex=0x1234`, zero data bytes (select EEPROM `0x52` for reading).
2. Vendor IN `bmRequestType=0xC0`, `bRequest=0xA9`, `wValue=offset`,
   `wIndex=0x1234`, up to 32 bytes.

The eight-byte header is read first, followed by the fixed documented payload
extent. A corrupt length cannot expand a read. There are no retries and no
`0xA2` EEPROM-write requests. The path sends no motor, LED, TEC, or controller
bootloader commands.

On a select failure, read failure, short transfer, or Ctrl-C, further transfers
stop. Bytes already returned are saved, including any short final chunk, and
the report marks acquisition incomplete. Sections never read are invalid, not
filled with fabricated EEPROM bytes; the report lists them with
`missing: true` and a null `file` and `sha256`. Disk write failures return an error;
an incomplete on-disk archive may lack its final report or manifest.

Every new archive contains raw section files, `report.json`, and `SHA256SUMS`
covering the files actually written and the report. Existing directories are
refused. This is a backup of the four known sections, **not the entire EEPROM
chip or the boot-personality EEPROM**.

Exit status: `0` means both sections have a decodable, CRC-valid selected copy;
`1` means acquisition, archive integrity, decoding, or I/O failure; `2` is an
argument-parsing error; `130` indicates an interrupted acquisition whose
partial archive was saved. Warnings remain relevant even with exit `0`.

## Decoding policy

Each header contains little-endian `u32 length` and `u32 CRC-32`. Validate the
expected size and compute zlib/PKZIP CRC-32 over the payload only. Choose a
valid primary, otherwise a valid backup, independently for A and B. If both
valid copies differ, retain both, warn, and select the primary to follow the
OEM preference. If neither validates, do not decode that section.

Section A exposes hardware version, model/type, serial, resolution-base
offsets and motor speeds, and both 3×10 float matrices. Section B exposes
motor-adjust words and its trailing unknown `u32`. Values are reported as
stored; they are not clamped, applied to hardware, or replaced by another
unit's defaults. Unknown model types or nonfinite matrix coefficients prevent
a successful decode result. JSON represents nonfinite floats as `null`; raw
bytes preserve their exact encoding.

The matrix rows are raw coefficient arrays. The newer `docs/TLB_FINDINGS.md`
reports cross-term order RG, BR, GB, differing from the older reference;
this tool does not apply a color transform. EEPROM does not contain the LED
currents/duties held in the OEM registry.

## Provenance

Layout and read procedure: Ali Bosworth's
[calibration reference](https://github.com/alibosworth/pakon-reference/blob/76d9cd0a38f523398c3a19392070539ee80b75cf/docs/calibration.md)
and [backup procedure](https://github.com/alibosworth/pakon-reference/blob/76d9cd0a38f523398c3a19392070539ee80b75cf/docs/per-unit-data-and-safety.md).
Motor-adjust field interpretation follows this project's `docs/TLB_FINDINGS.md`.
Fixture attribution is in `test/fixtures/eeprom/README.md`.
