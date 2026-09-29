# EEPROM decoder fixtures

These eight unmodified section files come from Ali Bosworth's
[pakon-reference](https://github.com/alibosworth/pakon-reference/tree/76d9cd0a38f523398c3a19392070539ee80b75cf/docs/resources/eeprom),
commit `76d9cd0a38f523398c3a19392070539ee80b75cf`, under
`docs/resources/eeprom/<unit>/eeprom/`.

- **F135-2233**: base F-135, read by Mats Fagerberg (`thetalkingdrum`);
  all four copies validate.
- **F135plus-16402**: F-135+, read by Ali Bosworth; section A primary has
  a CRC failure, while its backup and both section B copies validate.

Attribution: © 2026 Ali Bosworth and credited contributor Mats Fagerberg.
Source project license: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
Files are unchanged; only their directory placement differs. The source's
license notice is reproduced in `LICENSE`. `SHA256SUMS` in each unit directory
was generated locally for these four selected files.

These are offline parser fixtures, not operating settings for another scanner.
Do not write them to hardware or use their calibration as another unit's defaults.
