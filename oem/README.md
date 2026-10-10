# OEM files

Files from the original Kodak/Pakon F-X35 Windows software (`F-X35 COM SERVER`
in the `fx35install` package) that the colour pipeline uses. They are run or
read as-is; nothing here is modified.

| File | What it is | Used for |
|---|---|---|
| `PakonIMAu.dll` | The OEM colour engine (Kodak Ansel), 32-bit x86 | Its plain-C scene-balance code (SBA, FOS, Preference) is run under emulation |
| `ansel/sba/SbaDPI/sba-CN-default.dpi` | SBA parameters for colour negative | Parameter block for SBA/FOS |
| `ansel/sba/SbaDPI/sba-CN-default-*.dpi` | Film-specific variants, chosen by DX product/generation code | Same, when the film's DX code is known |
| `ansel/sba/SbaDPI/sba.map` | Selects the `.dpi` by source type and DX code | DPI selection |
| `ansel/sba/Pcode/pcode-dls_1.7` | SBA parameter tables (binary) | Decoded by the DLL's `SbaDecodePcode` |
| `ansel/sba/Sfs/sfsTable35` | Subject-failure-suppression hue table (35 mm) | Built by the DLL's `Makesfs` |

Paths under `ansel/` mirror `anselinstalldir/dataPathItems/` in the OEM install.
The Kodak ICC profiles (`rpd.pf` and others) live in `profiles/`.
