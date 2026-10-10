# Decision log

Decisions that shape the project, newest first: what was decided, what else
was considered, and why. Facts and how-tos live in the other docs; this is the
reasoning. Status: **decided**, **deferred** (revisit later), or **superseded**.

---

## 2026-10-09 — Ansel scene balance: run the OEM code under emulation

**Status:** decided. Supersedes the deferral below.

**Context.** A roll whose film base was off-nominal (red ~0.15 density low)
came out strongly blue: the NegMatrix offsets push blue up by ~500 RPD codes
and `rpd.pf` is neutral on equal RPD, so the roll balance alone could not
recover it, while a fix tuned on that roll turned another roll cream.

**Decision.** Run the OEM balance itself: `oem/PakonIMAu.dll`'s plain-C SBA,
FOS and Preference functions under Unicorn (`tools/oem_sba.py`), wired into
the web export with the roll balance as fallback. The needed OEM files are
committed under `oem/`.

**Why not the other options.** Wine (option A below) needs a 32-bit Windows
runtime and the DLL's undocumented C++ interface; emulating only the C core
needs neither. A Python port (option B) is still possible and can now be
verified bit for bit against the emulator.

**Result.** By eye on two rolls (serial 3054): the blue roll comes out
neutral with natural skin and whites; the gold roll keeps its colour and gains
per-frame exposure. No OEM reference yet.

---

## 2026-10-09 — Ansel scene balance: deferred, reimplement rather than run the DLL

**Status:** superseded (above).

**Context.** The OEM colour engine (`PakonIMAu.dll`) is built on Kodak's
Ansel library: ~48 stages driven by 333 data files (`docs/IMAGING.md`, "The
OEM Ansel cascade"). For the F-135 the TLX export comes down to three toggles:
colour correction (the `rpd.pf` render, now implemented), **colour scene
balance** (SBA/DSBA, a per-image automatic colour and density balance), and
manual adjustments (neutral by default). The scene balance is the part we
lack; `oem_roll_balance` is a simpler stand-in.

**Options.**

| Option | Effort | Against |
|---|---|---|
| A. Run the OEM's Ansel: call `PakonIMAu.dll` through Wine with a small Windows helper | several days | 32-bit Windows DLL with an undocumented interface, fragile on macOS; OEM code the user must supply; release builds would need Wine |
| B. Reimplement SBA/DSBA from the decompilation, reading its data files from the user's OEM install | ~1 day to scope, then 1–3 days | the balance logic is code, not data; Kodak's data files stay on the user's machine |
| C. Keep the roll balance only | none | no per-image balance; mixed-light rolls stay uneven |

**Decision.** C for now; when revisited, **B**, starting with a scoping pass
(find the SBA/DSBA functions, their inputs, the data files they read). Only
the scene balance, not the whole cascade.

**Revisit when** colour is the next priority. A TLX reference scan of a strip
we can rescan would let the result be checked, not just followed.

---

## 2026-10-09 — Colour: OEM F-135 path with a two-point roll balance

**Status:** decided.

**Context.** The old decode (measured film base → density LUT → sRGB, then
`rpd.pf` and per-channel auto-levels for the JPEG) looked off and
oversaturated. `rpd.pf` turned out to be an input profile expecting 12-bit
RPD (the NegMatrix output), and it was being fed an sRGB positive.

**Decision.** Follow `TLB.dll`: density LUT → the unit's EEPROM NegMatrix →
`rpd.pf`, with no auto-levels (`render_oem`). Add a roll-level balance
(`oem_roll_balance`): per channel, the film base stays put and the roll's
picture median moves half way to neutral.

**Alternatives tried** (by eye, two rolls on serial 3054; no OEM reference):

- No balance: blue cast.
- Per-frame neutral balance: turned a blue sky white.
- Per-channel offset (roll level): blue shadows remained.
- Film base made neutral too: one roll went blue, the other cream, so
  `rpd.pf` appears to expect an unbalanced base.
- Midtone strength 0 / 0.5 / 1: the owner chose 0.5 (`--oem-balance`).

**Revisit** with the Ansel scene balance above, or against a TLX reference.
