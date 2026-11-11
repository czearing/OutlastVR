import unittest
from unittest.mock import patch

import cv2
import numpy as np

from verify_native_probe import correspondences, gameplay_grade, recommended_extent_grade, refine_correspondences


class NativeCorrespondenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        noise = np.random.default_rng(42).integers(0, 256, (512, 512), dtype=np.uint8)
        cls.left = cv2.GaussianBlur(noise, (5, 5), .8)
        cls.views = [dict(orientation=[0, 0, 0, 1], position=[eye, 0, 0],
                          fov=[-.8, .8, .8, -.8]) for eye in (-.032, .032)]
        cls.right = np.empty_like(cls.left)
        for top, disparity in ((0, 4), (256, 12)):
            cls.right[top:top+256] = cv2.warpAffine(
                cls.left[top:top+256], np.float32([[1, 0, -disparity], [0, 1, 0]]), (512, 256))

    def test_depth_varying_stereo_is_measured(self):
        result = gameplay_grade(self.left, self.right, self.views)
        self.assertGreater(result["world_matches"], 100)
        self.assertGreater(result["world_parallax_spread_px"], 7)
        self.assertLess(result["world_vertical90_px"], .1)
        self.assertGreaterEqual(result["raw_correspondences"], result["refined_correspondences"])

    def test_both_vertical_errors_are_retained_and_rejected(self):
        for offset in (-5, 5):
            with self.subTest(offset=offset):
                right = cv2.warpAffine(
                    self.right, np.float32([[1, 0, 0], [0, 1, offset]]), (512, 512))
                points = refine_correspondences(self.left, right, correspondences(self.left, right))
                self.assertGreater(len(points), 100)
                self.assertGreater(float(np.percentile(np.abs(points[:, 1]-points[:, 3]), 90)), 4.9)
                with self.assertRaisesRegex(ValueError, "vertical stereo alignment"):
                    gameplay_grade(self.left, right, self.views)

    def test_feature_localization_is_refined_without_changing_depth(self):
        points = correspondences(self.left, self.right)
        points[:, 2:] += [.35, -.4]
        refined = refine_correspondences(self.left, self.right, points)
        self.assertGreater(len(refined), len(points)*.7)
        self.assertLess(float(np.percentile(np.abs(refined[:, 1]-refined[:, 3]), 90)), .1)
        disparities = refined[:, 0]-refined[:, 2]
        self.assertLess(float(np.percentile(np.minimum(np.abs(disparities-4), np.abs(disparities-12)), 90)), .1)

    def test_mono_and_flat_shift_remain_rejected(self):
        flat = cv2.warpAffine(self.left, np.float32([[1, 0, -8], [0, 1, 0]]), (512, 512))
        for right in (self.left, flat):
            with self.subTest(mono=right is self.left), self.assertRaises(ValueError):
                gameplay_grade(self.left, right, self.views)

    def test_failed_tracking_does_not_fall_back_to_raw_features(self):
        points = correspondences(self.left, self.right)
        with patch("verify_native_probe.cv2.calcOpticalFlowPyrLK", return_value=(None, None, None)):
            with self.assertRaisesRegex(ValueError, "localization failed"):
                refine_correspondences(self.left, self.right, points)

    def test_insufficient_refined_features_are_rejected(self):
        with patch("verify_native_probe.refine_correspondences", return_value=np.empty((0, 4))):
            with self.assertRaisesRegex(ValueError, "Insufficient textured gameplay"):
                gameplay_grade(self.left, self.right, self.views)

    def test_higher_resolution_keeps_the_two_pixel_alignment_limit(self):
        left, right = [cv2.resize(image, (1024, 1024), interpolation=cv2.INTER_CUBIC)
                       for image in (self.left, self.right)]
        result = gameplay_grade(left, right, self.views)
        self.assertEqual(result["eye_extent"], 1024)
        self.assertEqual(result["vertical90_limit_px"], 2)
        self.assertGreater(result["world_parallax_spread_px"], 14)
        for offset in (-3, 3):
            shifted = cv2.warpAffine(right, np.float32([[1, 0, 0], [0, 1, offset]]), (1024, 1024))
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, "vertical stereo alignment"):
                gameplay_grade(left, shifted, self.views)

    def test_inconsistent_eye_baseline_is_not_a_rendering_alignment_result(self):
        views = [dict(view) for view in self.views]
        views[1]["position"] = [.032, 0, .004]
        with self.assertRaisesRegex(ValueError, "rectified submitted eye rig"):
            gameplay_grade(self.left, self.right, views)

    def test_missing_eye_poses_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "Missing submitted stereo eye poses"):
            gameplay_grade(self.left, self.right, [])

    def extent_frames(self, size):
        return [dict(eye_width=size, eye_height=size, source_width=size, source_height=size,
                     world_panel=False, direct_xr_submissions=899, views=self.views,
                     logical_geometry_draws=1, driver_draws=[dict(indexed=True, instances=2)])
                for _ in range(900)]

    def test_actual_source_and_declared_recommendation_both_match(self):
        for size in (1024, 2448):
            result = recommended_extent_grade(self.extent_frames(size), size)
            self.assertEqual(result["source_width"], size)
            self.assertEqual(result["actual_paired_draws"], 900)

    def test_correct_final_eye_image_does_not_hide_wrong_native_source(self):
        frames = self.extent_frames(1024)
        frames[0]["source_width"] = 3840
        with self.assertRaisesRegex(ValueError, "Actual engine viewport"):
            recommended_extent_grade(frames, 1024)

    def test_smaller_actual_eye_extent_cannot_impersonate_large_recommendation(self):
        with self.assertRaisesRegex(ValueError, "Actual engine viewport"):
            recommended_extent_grade(self.extent_frames(1024), 2448)

    def test_redundant_atlas_or_incomplete_gameplay_cannot_pass_extent_gate(self):
        frames = self.extent_frames(1024)
        frames[-1]["direct_xr_submissions"] = 0
        with self.assertRaisesRegex(ValueError, "redundant stereo rescaling"):
            recommended_extent_grade(frames, 1024)
        with self.assertRaisesRegex(ValueError, "redundant stereo rescaling"):
            recommended_extent_grade(self.extent_frames(1024)[:-1], 1024)


if __name__ == "__main__":
    unittest.main()
