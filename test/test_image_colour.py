"""OEM colour path in tools/pakon_image.py (no hardware). Needs numpy and
Pillow; skipped when they are missing."""

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
try:
    import numpy as np
    from PIL import Image, ImageCms  # noqa: F401
    spec = importlib.util.spec_from_file_location("pakon_image", ROOT / "tools/pakon_image.py")
    img = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(img)
except ImportError:  # pragma: no cover
    img = None


@unittest.skipIf(img is None, "numpy/Pillow not installed")
class OemColourTests(unittest.TestCase):
    def test_rpd_polynomial(self):
        # Diagonal 0.25, offsets 10/20/30: RPD = D/4 + offset, D = LUT[(raw-300)/4].
        m = np.zeros((3, 10), dtype=np.float32)
        for c in range(3):
            m[c, c] = 0.25
            m[c, 9] = 10.0 * (c + 1)
        lut = img._c41_lut()
        raw = np.array([[[300 + 4 * 1638, 300 + 4 * 16000, 300 + 4 * 163]]], dtype=np.uint16)
        out = img.oem_rpd(raw, m, lut)
        expect = [lut[1638] / 4 + 10, lut[16000] / 4 + 20, lut[163] / 4 + 30]
        np.testing.assert_allclose(out[0, 0], expect, rtol=1e-5)
        self.assertAlmostEqual(float(lut[1638]), 3500.0, delta=1.0)   # 1 density decade

    def test_cross_terms_order(self):
        # m6 = RG, m7 = BR, m8 = GB (TLB.dll order).
        lut = img._c41_lut()
        raw = np.array([[[300 + 4 * 1638] * 3]], dtype=np.uint16)
        d = float(lut[1638])
        for k in (6, 7, 8):
            m = np.zeros((3, 10), dtype=np.float32)
            m[0, k] = 1e-4
            self.assertAlmostEqual(float(img.oem_rpd(raw, m, lut)[0, 0, 0]),
                                   1e-4 * d * d, delta=0.5)

    def test_clamped_to_12_bit(self):
        m = np.zeros((3, 10), dtype=np.float32)
        m[:, 9] = [-50.0, 9000.0, 100.0]
        out = img.oem_rpd(np.full((1, 1, 3), 30000, np.uint16), m, img._c41_lut())
        self.assertEqual(out[0, 0].tolist(), [0.0, 4095.0, 100.0])

    def test_roll_balance_keeps_base_moves_median(self):
        # Clear film at the top rows, a "picture" below: R/G/B at different levels.
        m = np.zeros((3, 10), dtype=np.float32)
        for c in range(3):
            m[c, c] = 0.28
            m[c, 9] = [170.0, 450.0, 670.0][c]
        lut = img._c41_lut()
        rng = np.random.default_rng(1)
        # Wide tone range so each median sits well above the film base (a
        # median within 100 RPD of the base hits the divide guard).
        pic = np.stack([rng.integers(1500, 20000, (200, 50)),
                        rng.integers(1200, 18000, (200, 50)),
                        rng.integers(800, 15000, (200, 50))], -1)
        film = np.full((40, 50, 3), [30000, 31000, 28000])
        raw = np.concatenate([film, pic]).astype(np.uint16)
        gain, offset = img.oem_roll_balance(raw, m, lut, strength=0.5)
        base = img.oem_rpd(img.measure_dmin(raw).reshape(1, 1, 3).astype(np.uint16),
                           m, lut)[0, 0]
        np.testing.assert_allclose(base * gain + offset, base, atol=1e-2)
        p = img.oem_rpd(raw, m, lut).reshape(-1, 3)
        lum = p.mean(1)
        med = np.median(p[(lum > np.percentile(lum, 20)) & (p.max(1) < 4000)], 0)
        np.testing.assert_allclose(med * gain + offset,
                                   med + 0.5 * (med.mean() - med), atol=0.5)
        g0, o0 = img.oem_roll_balance(raw, m, lut, strength=0.0)
        np.testing.assert_allclose(g0, 1.0, atol=1e-5)

    @unittest.skipUnless((ROOT / "profiles/rpd.pf").exists(), "profiles/rpd.pf missing")
    def test_rpd_lut_matches_profile_on_nodes(self):
        from PIL import ImageCms
        lut = img.RpdLut(str(ROOT / "profiles/rpd.pf"))
        pts = np.array([[0, 0, 0], [255, 255, 255], [125, 60, 200], [5, 250, 130]],
                       dtype=np.uint8)
        tr = ImageCms.buildTransform(ImageCms.getOpenProfile(str(ROOT / "profiles/rpd.pf")),
                                     ImageCms.createProfile("sRGB"), "RGB", "RGB",
                                     renderingIntent=0)
        direct = np.asarray(ImageCms.applyTransform(
            Image.fromarray(pts.reshape(1, -1, 3), "RGB"), tr)).reshape(-1, 3)
        got = lut.apply(pts.reshape(1, -1, 3).astype(np.float32) / 255.0)[0] * 255.0
        np.testing.assert_allclose(got, direct, atol=0.51)


if __name__ == "__main__":
    unittest.main()
