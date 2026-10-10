# Decision log

Decisions that shape the project, newest first: what was decided, what else
was considered, and why. Facts and how-tos live in the other docs; this is the
reasoning. Status: **decided**, **deferred** (revisit later), or **superseded**.

---

## 2026-10-09 — Ansel scene balance: deferred, reimplement rather than run the DLL

**Status:** deferred.

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
