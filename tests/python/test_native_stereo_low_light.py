import unittest
from unittest.mock import patch

import numpy as np

from verify_native_probe import gameplay_grade


class LowLightStereoTests(unittest.TestCase):
    def setUp(self):
        self.image = np.tile(np.arange(512, dtype=np.uint8), (512, 1))
        self.views = [dict(position=[x, 1.7, 0], orientation=[0, 0, 0, 1], fov=[-.8, .8, .8, -.8])
                      for x in (-.032, .032)]

    @staticmethod
    def points(count, vertical=.1):
        return np.array([[200+4*i, 200+2*i, 198+3.7*i, 200+2*i+vertical] for i in range(count)])

    def evaluate(self, first, second):
        with patch("verify_native_probe.correspondences", side_effect=[first, second]) as detect, \
                patch("verify_native_probe.refine_correspondences", side_effect=lambda a, b, p: p):
            result = gameplay_grade(self.image, self.image, self.views)
        return result, detect

    def test_dark_world_retries_detection(self):
        result, detect = self.evaluate(self.points(8), self.points(24))
        self.assertEqual(result["world_matches"], 24)
        self.assertEqual(result["feature_contrast_threshold"], .002)
        self.assertEqual([c.args[2] for c in detect.call_args_list], [.006, .002])

    def test_ordinary_world_keeps_detector(self):
        result, detect = self.evaluate(self.points(24), self.points(8))
        self.assertEqual(result["feature_contrast_threshold"], .006)
        self.assertEqual(detect.call_count, 1)

    def test_low_light_still_requires_twenty_world_points(self):
        with self.assertRaisesRegex(ValueError, "Insufficient textured"):
            self.evaluate(self.points(8), self.points(19))

    def test_low_light_still_rejects_vertical_error(self):
        with self.assertRaisesRegex(ValueError, "vertical stereo alignment"):
            self.evaluate(self.points(8), self.points(24, 5))

    def test_low_light_still_rejects_flat_shift(self):
        flat = self.points(24)
        flat[:, 2] = flat[:, 0]-8
        with self.assertRaisesRegex(ValueError, "depth-varying parallax"):
            self.evaluate(self.points(8), flat)


if __name__ == "__main__":
    unittest.main()
