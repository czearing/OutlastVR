import math
import unittest
import struct

from verify_native_glass import sweep_poses
from verify_native_reflective_glass import viewport_witness


def frames(fixture, center):
    result = []
    for world in range(320, 421):
        phase = world - 300
        yaw = center + math.radians(8) * math.sin(phase * .05)
        pitch = math.radians(-15) + math.radians(3) * math.sin(phase * .07)
        sy, cy = math.sin(yaw / 2), math.cos(yaw / 2)
        sp, cp = math.sin(pitch / 2), math.cos(pitch / 2)
        orientation = [cy * sp, sy * cp, -sy * sp, cy * cp]
        result.append(dict(native_frame=world + 280, gameplay_frame=world,
                           window_sweep_fixture_version=fixture,
                           views=[dict(orientation=orientation.copy()) for _ in range(2)]))
    return result


class ParticleSweepTests(unittest.TestCase):
    def test_original_side_facing_contract_remains_exact(self):
        sample = frames(2, math.pi / 2)
        self.assertEqual(len(sweep_poses(sample, set(range(600, 701)), 2)), 101)

    def test_emitter_facing_contract_has_its_own_exact_orientation(self):
        sample = frames(3, 0)
        self.assertEqual(len(sweep_poses(sample, set(range(600, 701)), 3)), 101)

    def test_side_facing_cannot_be_relabeled_emitter_facing(self):
        with self.assertRaises(ValueError):
            sweep_poses(frames(3, math.pi / 2), set(range(600, 701)), 3)

    def test_emitter_facing_cannot_be_relabeled_side_facing(self):
        with self.assertRaises(ValueError):
            sweep_poses(frames(2, 0), set(range(600, 701)), 2)

    def test_both_eyes_and_complete_frame_sequence_are_required(self):
        sample = frames(3, 0)
        sample[50]["views"][1]["orientation"][0] += .01
        with self.assertRaises(ValueError):
            sweep_poses(sample, set(range(600, 701)), 3)
        with self.assertRaises(ValueError):
            sweep_poses(frames(3, 0)[:-1], set(range(600, 701)), 3)

    def test_unknown_fixture_is_rejected(self):
        with self.assertRaises(ValueError):
            sweep_poses(frames(4, 0), set(range(600, 701)), 4)

    def test_native_to_native_viewport_difference_is_reported_not_hidden(self):
        actual = struct.pack("<4H", 100, 200, 300, 400)
        half = struct.pack("<4H", 101, 200, 300, 400)
        result = viewport_witness(actual, actual, half, 320)
        self.assertEqual(result["native_to_native_different_rgb_pixels"], 1)
        self.assertEqual(result["native_to_native_different_depth_alpha_pixels"], 0)
        self.assertTrue(result["original_same_raster_equals_actual"])

    def test_same_raster_disagreement_still_has_zero_tolerance(self):
        actual = struct.pack("<4H", 100, 200, 300, 400)
        changed = struct.pack("<4H", 101, 200, 300, 400)
        with self.assertRaises(ValueError):
            viewport_witness(actual, changed, actual, 320)


if __name__ == "__main__":
    unittest.main()
