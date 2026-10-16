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


    def test_pos_matrix_default_and_fixtures(self):
        m = img.load_pos_matrix(None)
        self.assertEqual(m.shape, (3, 10))
        np.testing.assert_allclose(m[:, :3], 0.25 * np.eye(3))
        np.testing.assert_allclose(m[:, 3:], 0.0)
        for unit in ("F135-2233", "F135plus-16402"):
            d = ROOT / "test/fixtures/eeprom" / unit
            np.testing.assert_allclose(img.load_pos_matrix(str(d)), m, atol=1e-6)


@unittest.skipIf(img is None, "numpy/Pillow not installed")
@unittest.skipUnless((ROOT / "profiles/ColRevLut1.pf").exists() and
                     (ROOT / "profiles/romm.pf").exists(), "slide profiles missing")
class SlideTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.path = img.slide_path(None)

    @staticmethod
    def raw_for_density(d):
        """Raw 16-bit samples for per-channel densities (14-bit open gate)."""
        d = np.asarray(d, np.float64)
        return np.minimum(300 + 4 * 16383 * 10.0 ** -d, 65535).astype(np.uint16)

    def test_link_neutral_and_monotone(self):
        link = self.path[2]
        dens = np.array([0.0, 0.3, 1.0, 2.0, 3.0])
        cmy = np.repeat((dens * 875 / 4095)[:, None], 3, 1)[None].astype(np.float32)
        out = link.apply(cmy)[0]
        self.assertGreater(out[0].min(), 0.99)               # clear -> white
        self.assertLess(np.ptp(out, axis=1).max(), 0.01)     # grey stays grey
        self.assertTrue(np.all(np.diff(out[:, 1]) < 0))      # denser -> darker

    def test_render_slide_is_a_positive(self):
        raw = np.stack([self.raw_for_density([0.0] * 3),
                        self.raw_for_density([1.0] * 3),
                        self.raw_for_density([3.0] * 3),
                        self.raw_for_density([1.6, 0.6, 0.6])])[None]
        out = img.render_slide(raw, *self.path).astype(np.int64)[0]
        self.assertGreater(out[0].min(), 64000)              # open gate -> white
        self.assertTrue(out[0].min() > out[1].max() > out[2].max())
        self.assertLess(np.ptp(out[1]), 1500)                # neutral density
        r, g, b = out[3]
        self.assertTrue(g > r and b > r)                     # cyan dye -> cyan

    def test_exact_density_has_no_14_bit_steps(self):
        # Raw 300 + 64..67: one 14-bit code, four distinct exact densities.
        raw = (300 + np.arange(64, 68, dtype=np.uint16))[None, :, None].repeat(3, 2)
        m = img.load_pos_matrix(None)
        table = img.oem_rpd(raw, m, img._c41_lut())[0, :, 1]
        exact = img.oem_rpd(raw, m, img._c41_lut(), exact=True)[0, :, 1]
        self.assertEqual(len(set(table.tolist())), 1)
        self.assertTrue(np.all(np.diff(exact) < 0))
        np.testing.assert_allclose(exact[0], table[0], atol=1.0)

    def test_flare_from_the_gaps(self):
        # Frames with bright detail at 30000 counts, black gaps of 120 rows at
        # 120 / 60 / 50 above black, every 1000 rows.
        gap = np.array([120, 60, 50], np.float32)
        rng = np.random.default_rng(3)
        rgb = (300 + rng.integers(2000, 40000, (4000, 80, 3))).astype(np.uint16)
        for g in (1000, 2000, 3000):
            rgb[g - 60:g + 60] = (300 + gap).astype(np.uint16)
        rgb[1500:1530] = 300 + 40            # a short dark detail: not a gap
        self.assertEqual(img.find_slide_gaps(rgb),
                         [(940, 1059), (1940, 2059), (2940, 3059)])
        floor = 64000 * 10 ** -3.5
        np.testing.assert_allclose(img.measure_slide_flare(rgb, 1.0),
                                   gap - floor, atol=1.0)
        np.testing.assert_allclose(img.measure_slide_flare(rgb, 0.5),
                                   0.5 * (gap - floor), atol=1.0)
        self.assertFalse(img.measure_slide_flare(rgb, 1.0, gaps=[]).any())
        # Capped at D 2: a light "gap" cannot crush the shadows.
        self.assertTrue(np.all(img.measure_slide_flare(
            rgb, 1.0, gaps=[(0, 3999)]) <= 640))
        # Removing it darkens the deep shadows, leaves the highlights alone.
        px = np.array([[[300 + 200] * 3, [300 + 40000] * 3]], np.uint16)
        fl = img.measure_slide_flare(rgb, 1.0)
        a = img.render_slide(px, *self.path).astype(int)
        b = img.render_slide(px, *self.path, flare=fl).astype(int)
        self.assertTrue(np.all(b[0, 0] < a[0, 0]))
        self.assertLess(np.abs(b[0, 1] - a[0, 1]).max(), 300)

    def test_slide_flat_divides_out_the_scan_light(self):
        import json, tempfile, os
        lp, px0 = 12, 2
        ff = {"line_px": lp, "px0": px0, "black_px": 2, "film": "slide",
              "duties": [1000, 1000, 1000, 1300],
              "scan_duties": [1318, 1318, 1318, 1560],
              "dark": [[300.0] * lp] * 4,
              "bright": [[300.0] * 2 + [64300.0] * (lp - 2)] * 4}
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.flat.json")
            with open(p, "w") as f:
                json.dump(ff, f)
            _, g_slide = img.load_flat_field(p, lp - px0)
            ff["film"] = "c41"
            with open(p, "w") as f:
                json.dump(ff, f)
            _, g_neg = img.load_flat_field(p, lp - px0)
        np.testing.assert_allclose(g_slide, g_neg * 1000 / 1318, rtol=1e-6)
        # Film at 1/4 of the open gate, scanned 1.318x brighter -> 16000 again.
        rgb = np.full((1, lp - px0, 3), 300 + int(64000 * 1.318 / 4), np.uint16)
        img.apply_flat_field(rgb, (np.full((lp - px0, 3), 300, np.float32), g_slide))
        self.assertAlmostEqual(float(rgb.mean()), 300 + 16000, delta=3)

    def test_autocrop_keeps_frames_between_dark_gaps(self):
        rng = np.random.default_rng(2)
        rows = []
        for k in range(4):                                   # 4 frames, black gaps
            rows.append(np.full((40, 64, 3), 300, np.uint16))
            rows.append(rng.integers(5000, 60000, (300, 64, 3)).astype(np.uint16))
        rows.append(np.full((40, 64, 3), 300, np.uint16))
        rgb = np.concatenate(rows)
        _, (r0, r1, _, _) = img.autocrop(rgb, positive=True)
        self.assertLess(r0, 60)
        self.assertGreater(r1, rgb.shape[0] - 60)
        _, (r0, r1, _, _) = img.autocrop(rgb)                # negative rules: one frame
        self.assertLess(r1 - r0, 400)


if __name__ == "__main__":
    unittest.main()
