"""OEM (TLX) picture framing in tools/pakon_image.py (no hardware). Needs
numpy; skipped when it is missing. docs/TLB_FINDINGS.md "Picture framing"."""

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
try:
    import numpy as np
    spec = importlib.util.spec_from_file_location("pakon_image", ROOT / "tools/pakon_image.py")
    img = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(img)
except ImportError:  # pragma: no cover
    img = None


@unittest.skipIf(img is None, "numpy not installed")
class TlxFramingTests(unittest.TestCase):
    def test_geometry_per_base(self):
        # 375 blocks per picture, ~400 per pitch, at every base.
        for width, (block, length, pitch) in img.TLX_GEOMETRY.items():
            self.assertEqual(length // block, 375)
            self.assertEqual(pitch // block, 400)
            self.assertEqual(length * 2 // 3, width)          # 3:2

    def test_real_strip_profile(self):
        # scan-code-2.raw (Base 16, serial 3054): 5 pictures between the gaps.
        prof = np.load(ROOT / "test/fixtures/framing/scan_code_2_profile.npy")
        starts = img.tlx_place(prof.astype(np.int32), 375, 400)
        self.assertEqual(starts, [1439, 1836, 2234, 2646, 3048])

    def test_lost_gap_and_leader(self):
        # Clear leader, then 6 pictures; the gap between 3 and 4 is missing.
        block, length, pitch = img.TLX_GEOMETRY[2000]
        first = 4000
        rgb = np.full((first + pitch * 6 + 1000, 2000, 3), 60000, np.uint16)
        for k in range(6):
            s = first + k * pitch
            rgb[s:s + length] = 20000
        rgb[first + 3 * pitch - (pitch - length):first + 3 * pitch] = 20000
        starts, got_len, got_pitch = img.tlx_frame_grid(rgb)
        self.assertEqual((got_len, got_pitch), (length, pitch))
        self.assertEqual(len(starts), 6)
        for k, s in enumerate(starts):
            self.assertLessEqual(abs(s - (first + k * pitch)), block)

    def test_format_geometry(self):
        self.assertEqual(img.tlx_geometry(2000, "full"), (8, 3000, 3206))
        self.assertEqual(img.tlx_geometry(2000, "half"), (8, 1500, 1603))
        self.assertEqual(img.tlx_geometry(2000, "pano"), (8, 5530, 5736))  # XPanPreScan.au3
        self.assertEqual(img.tlx_geometry(1500, "pano"), (6, 4148, 4303))
        self.assertIsNone(img.tlx_geometry(1234, "half"))

    def test_half_and_pano_strips(self):
        # Every picture found, within one block, only with the roll's format.
        rng = np.random.default_rng(1)
        for fmt, n in (("half", 12), ("pano", 4)):
            block, length, pitch = img.tlx_geometry(2000, fmt)
            first = 4000
            rgb = np.full((first + pitch * n + 1500, 2000, 3), 60000, np.uint16)
            for k in range(n):
                s = first + k * pitch
                rgb[s:s + length] = rng.integers(8000, 30000, (length, 1, 3))
            starts, got_len, _ = img.tlx_frame_grid(rgb, fmt=fmt)
            self.assertEqual(got_len, length)
            self.assertEqual(len(starts), n, fmt)
            for k, s in enumerate(starts):
                self.assertLessEqual(abs(s - (first + k * pitch)), block, fmt)
            # Full-frame geometry misplaces them.
            full = img.tlx_frame_grid(rgb)[0]
            self.assertNotEqual(len(full), n, fmt)

    def test_unknown_width(self):
        self.assertIsNone(img.tlx_frame_grid(np.zeros((100, 1234, 3), np.uint16)))


if __name__ == "__main__":
    unittest.main()
