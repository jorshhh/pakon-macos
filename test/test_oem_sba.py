"""OEM scene balance (tools/oem_sba.py): oem/PakonIMAu.dll's SBA/FOS code
under emulation. Skipped when numpy, unicorn or pefile are missing."""

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
try:
    import numpy as np
    import oem_sba
    AVAILABLE = oem_sba.available()
except ImportError:  # pragma: no cover
    AVAILABLE = False


def synthetic_roll(seed=1, red=0, frames=5):
    """Textured frames in 12-bit RPD with a mask-like R < G < B offset."""
    rng = np.random.default_rng(seed)
    out = []
    for _ in range(frames):
        level = rng.uniform(1100, 1800)
        f = np.empty((128, 192, 3), np.int16)
        tex = rng.normal(0, 250, (128, 192))
        for c, off in enumerate((-300 + red, 0, 200)):
            f[..., c] = np.clip(level + off + tex + rng.normal(0, 40, (128, 192)), 0, 4095)
        out.append(f)
    return out


@unittest.skipUnless(AVAILABLE, "unicorn/pefile or oem/ missing")
class OemSbaTests(unittest.TestCase):
    def test_f80_round_trip(self):
        for x in (0.0, 1.5, -0.33, 1e-300, 12345.678, -2.0 ** 40):
            self.assertEqual(oem_sba._f80_to_float(oem_sba._float_to_f80(x)), x)

    def test_roll_shifts_pinned(self):
        # The emulated OEM output for a fixed input; a change means the
        # emulation (or the structures fed to it) changed.
        got = oem_sba.roll_shifts(synthetic_roll())
        np.testing.assert_array_equal(got, [[506, 189, -19], [362, 45, -163],
                                            [601, 284, 75], [326, 11, -199],
                                            [715, 398, 191]])

    def test_less_red_gets_more_red(self):
        a = oem_sba.roll_shifts(synthetic_roll())
        b = oem_sba.roll_shifts(synthetic_roll(red=-150))
        d = (b - a).mean(axis=0)
        self.assertGreater(d[0] - d[2], 120)
        self.assertLess(abs(d[1] - d[2]), 20)

    def test_short_roll_balances_each_frame(self):
        # Fewer than mff frames: no FOS (the OEM would use scan history).
        got = oem_sba.roll_shifts(synthetic_roll()[:2])
        np.testing.assert_array_equal(got, [[507, 186, -18], [363, 42, -162]])

    def test_analysis_image_is_landscape(self):
        def rpd(raw, negmat, lut):
            return raw.astype(np.float32) / 16
        tall = np.full((3000, 2000, 3), 32000, np.uint16)
        img = oem_sba.analysis_image(tall, None, None, rpd)
        self.assertEqual(img.shape, (oem_sba.ANALYSIS_H, oem_sba.ANALYSIS_W, 3))
        self.assertTrue(np.all(img == 2000))


if __name__ == "__main__":
    unittest.main()
