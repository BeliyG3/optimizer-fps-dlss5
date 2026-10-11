import math
import os
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hdri_prepare as hp  # noqa: E402
from hdri_rgbe import read_hdr, write_hdr  # noqa: E402

H, W = 256, 512
DISC = (60, 400, 3)


def synthetic(disc_value=5000.0, band_value=2.0):
    """Sky gradient 0.2-0.5 over rows, a broad band near the horizon, a small bright disc."""
    row_value = np.linspace(0.2, 0.5, H, dtype=np.float32)
    img = np.repeat(row_value[:, None, None], W, axis=1).repeat(3, axis=2)
    img[118:138] = band_value
    yy, xx = np.mgrid[0:H, 0:W]
    img[np.hypot(yy - DISC[0], xx - DISC[1]) <= DISC[2]] = disc_value
    return img


class FindSunTest(unittest.TestCase):
    def test_finds_the_compact_disc(self):
        row, col, radius = hp.find_sun(synthetic())
        self.assertAlmostEqual(row, 60, delta=1); self.assertAlmostEqual(col, 400, delta=1)
        self.assertTrue(2 <= radius <= 5, radius)

    def test_broad_band_is_not_the_sun(self):
        # a wide bright band (clouds near the horizon) next to the disc: the disc is the answer
        img = synthetic(disc_value=800.0, band_value=300.0)
        row, col, _ = hp.find_sun(img)
        self.assertAlmostEqual(row, 60, delta=1); self.assertAlmostEqual(col, 400, delta=1)

    def test_disc_across_the_seam(self):
        img = np.roll(synthetic(), 112, axis=1)  # the disc now straddles column 0 / W-1
        row, col, _ = hp.find_sun(img)
        self.assertAlmostEqual(row, 60, delta=1)
        self.assertAlmostEqual((col + W / 2) % W - W / 2, 0, delta=1)


class InpaintTest(unittest.TestCase):
    def test_disc_is_replaced_by_the_ring(self):
        img = synthetic(); row, col, radius = hp.find_sun(img)
        out = hp.inpaint_disc(img, row, col, radius)
        yy, xx = np.mgrid[0:H, 0:W]
        d = np.hypot(yy - row, xx - col)
        lum = hp.luminance(out)
        ring = np.median(lum[(d >= 2 * radius) & (d <= 4 * radius)])
        self.assertLessEqual(lum[d <= 1.5 * radius].max(), 3 * ring)
        self.assertLessEqual(hp.residual_peak_ratio(out, row, col, radius), 3)

    def test_outside_the_disc_is_untouched(self):
        img = synthetic(); row, col, radius = hp.find_sun(img)
        out = hp.inpaint_disc(img, row, col, radius)
        yy, xx = np.mgrid[0:H, 0:W]
        far = np.hypot(yy - row, xx - col) > 1.5 * radius + 1
        self.assertTrue(np.array_equal(out[far], img[far]))

    def test_seam(self):
        img = np.roll(synthetic(), 112, axis=1); row, col, radius = hp.find_sun(img)
        out = hp.inpaint_disc(img, row, col, radius)
        self.assertLess(out.max(), 3.0)


class AureoleTest(unittest.TestCase):
    def test_glow_around_the_disc_is_removed_too(self):
        img = synthetic()
        yy, xx = np.mgrid[0:H, 0:W]
        d = np.hypot(yy - DISC[0], xx - DISC[1])
        img += (60.0 / (1.0 + d) ** 2)[..., None].astype(np.float32)  # aureole: 1/r^2 falloff to the sky level
        row, col, disc = hp.find_sun(img)
        radius = hp.removal_radius(img, row, col, disc)
        self.assertGreater(radius, disc)
        out = hp.inpaint_disc(img, row, col, radius)
        self.assertLessEqual(hp.residual_peak_ratio(out, row, col, radius), 3)


class RotateTest(unittest.TestCase):
    def test_moves_the_disc(self):
        img = synthetic(); out = hp.rotate_azimuth(img, 128)
        _, col, _ = hp.find_sun(out)
        self.assertAlmostEqual(col, (400 + 128) % W, delta=1)

    def test_direction_convention_round_trip(self):
        for row, col in [(10.0, 3.0), (128.0, 256.0), (200.5, 500.25)]:
            r2, c2 = hp.dir_to_rowcol(hp.rowcol_to_dir(row, col, H, W), H, W)
            self.assertAlmostEqual(r2, row, places=6); self.assertAlmostEqual(c2, col, places=6)

    def test_convention_matches_the_shader(self):
        # Sky(): uv = (atan2(z, x) / 2pi + 0.5, acos(y) / pi). +x is the middle of the panorama, +z a quarter further,
        # -z a quarter back, +y the top row, -y the bottom row.
        r, c = hp.dir_to_rowcol([1, 0, 0], H, W)
        self.assertAlmostEqual(c, W * 0.5 - 0.5); self.assertAlmostEqual(r, H / 2 - 0.5)
        _, c = hp.dir_to_rowcol([0, 0, 1], H, W); self.assertAlmostEqual(c, W * 0.75 - 0.5)
        _, c = hp.dir_to_rowcol([0, 0, -1], H, W); self.assertAlmostEqual(c, W * 0.25 - 0.5)
        r, _ = hp.dir_to_rowcol([0, 1, 0], H, W); self.assertAlmostEqual(r, -0.5)
        r, _ = hp.dir_to_rowcol([0, -1, 0], H, W); self.assertAlmostEqual(r, H - 0.5)

    def test_prepared_sun_azimuth_matches_the_bench(self):
        img = synthetic()
        out, report, _ = hp.prepare(img, [0.45, -0.77, 0.45], 1500, 0.12)
        # the in-painted sun is gone; the original sun's texel now sits at the bench sun's azimuth
        target = -np.array([0.45, -0.77, 0.45])
        moved = hp.rowcol_to_dir(60, (400 + report["rotation_cols"]) % W, H, W)
        self.assertAlmostEqual(math.atan2(moved[2], moved[0]), math.atan2(target[2], target[0]), delta=2 * math.pi / W)


class IrradianceTest(unittest.TestCase):
    def test_constant_panorama(self):
        img = np.full((H, W, 3), 0.7, np.float32)
        self.assertAlmostEqual(hp.horizontal_irradiance(img) / (math.pi * 0.7), 1.0, delta=0.01)

    def test_scale_hits_the_ratio(self):
        out, report, _ = hp.prepare(synthetic(), [0.45, -0.77, 0.45], 1500, 0.12)
        expect = 0.12 * 1500 * 0.77 / math.sqrt(0.45 ** 2 * 2 + 0.77 ** 2)
        self.assertAlmostEqual(hp.horizontal_irradiance(out) / expect, 1.0, delta=0.01)


class RgbeTest(unittest.TestCase):
    def test_round_trip_flat(self):
        rng = np.random.default_rng(1)
        img = (rng.random((8, 16, 3)) * 100).astype(np.float32)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "x.hdr"); write_hdr(path, img)
            back = read_hdr(path)
        self.assertLess(np.abs(back - img).max() / img.max(), 0.015)

    def test_reads_rle_scanlines(self):
        # one 8-wide scanline: R = run of 1 x 8 (mantissa 128), G = literal 0..7, B = 0 run, E = run of 129
        scan = bytes([2, 2, 0, 8, 128 + 8, 128, 8, *range(8), 128 + 8, 0, 128 + 8, 129])
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "rle.hdr")
            with open(path, "wb") as f:
                f.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 8\n" + scan)
            img = read_hdr(path)
        self.assertEqual(img.shape, (1, 8, 3))
        self.assertAlmostEqual(float(img[0, 0, 0]), 128.5 * 2.0 ** (129 - 136), places=6)
        self.assertAlmostEqual(float(img[0, 5, 1]), 5.5 * 2.0 ** (129 - 136), places=6)


if __name__ == "__main__":
    unittest.main()
