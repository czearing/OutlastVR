import unittest
from unittest.mock import patch

from verify_native_probe import recommended_extent_grade
from verify_native_glass import native_hdr_extent


class NativeHeadsetExtentTests(unittest.TestCase):
    def grade(self, width, height, declared, portrait=False):
        frames = [dict(eye_width=width, eye_height=height, source_width=width,
                       source_height=height, world_panel=True, direct_xr_submissions=0)]
        with patch("verify_native_probe.draw_grade", return_value=1):
            return recommended_extent_grade(frames, declared, portrait)

    def test_existing_square_contracts(self):
        for extent in (1024, 2448):
            result = self.grade(extent, extent, extent)
            self.assertEqual((result["source_width"], result["source_height"]), (extent, extent))

    def test_exact_headset_rectangle(self):
        result = self.grade(2688, 2880, None, True)
        self.assertEqual((result["source_width"], result["source_height"]), (2688, 2880))

    def test_cannot_relabel_square_or_transpose_headset(self):
        for width, height in ((2688, 2688), (2880, 2688), (1024, 1024)):
            with self.subTest(width=width, height=height), self.assertRaises(ValueError):
                self.grade(width, height, None, True)

    def test_explicit_shape_and_size_are_required(self):
        for extent, portrait in ((None, False), (2688, False), (1024, True), (None, "true")):
            with self.subTest(extent=extent, portrait=portrait), self.assertRaises(ValueError):
                self.grade(2688, 2880, extent, portrait)

    def test_native_hdr_atlas_follows_explicit_source_extent(self):
        self.assertEqual(native_hdr_extent({}), (7680, 2160))
        for size in (1024, 2448):
            self.assertEqual(native_hdr_extent(dict(recommended_resolution=True, diagnostic_eye_size=size)),
                             (size * 2, size))
        self.assertEqual(native_hdr_extent(dict(recommended_resolution=True, diagnostic_eye_size=None,
                                               headset_extent_2688x2880=True)), (5376, 2880))
        for manifest in (dict(headset_extent_2688x2880=True),
                         dict(recommended_resolution=True, diagnostic_eye_size=1024,
                              headset_extent_2688x2880=True),
                         dict(recommended_resolution=True, diagnostic_eye_size=None),
                         dict(headset_extent_2688x2880="true")):
            with self.subTest(manifest=manifest), self.assertRaises(ValueError):
                native_hdr_extent(manifest)


if __name__ == "__main__":
    unittest.main()
