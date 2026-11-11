import unittest

import numpy as np

from verify_native_body import segment_reference


class NativeBodyReferenceTests(unittest.TestCase):
    def setUp(self):
        self.before = np.zeros((73, 8))
        self.standing = np.ones((73, 8))
        self.target = {"physical": False, "reference_upper_body": True,
                       "hands": [{"tracked": True}, {"tracked": False}]}

    def test_tracked_reference_arm_uses_calibration_not_camera_animation(self):
        for driver in (10, 11):
            self.assertIs(segment_reference(self.before, self.standing, self.target, driver),
                          self.standing)
        for driver in (34, 35, 61, 62, 67, 68):
            self.assertIs(segment_reference(self.before, self.standing, self.target, driver),
                          self.before)

    def test_physical_body_uses_calibration_for_all_limbs(self):
        self.target["physical"] = True
        for driver in (10, 11, 34, 35, 61, 62, 67, 68):
            self.assertIs(segment_reference(self.before, self.standing, self.target, driver),
                          self.standing)

    def test_legacy_arms_retain_original_animation_reference(self):
        del self.target["reference_upper_body"]
        self.assertIs(segment_reference(self.before, self.standing, self.target, 10), self.before)

    def test_reference_selection_does_not_hide_changed_segment_lengths(self):
        self.standing[11, 4] = 30
        self.before[11, 4] = 20
        source = segment_reference(self.before, self.standing, self.target, 10)
        self.assertGreater(abs(np.linalg.norm(source[11, 4:7] - source[10, 4:7]) -
                               np.linalg.norm(self.before[11, 4:7] - self.before[10, 4:7])), .02)


if __name__ == "__main__":
    unittest.main()
