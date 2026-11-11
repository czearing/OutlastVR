import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import cv2
import numpy as np

from verify_native_camcorder import finite_baseline_grade, fixture_grade, independent_world_image, screen_mask
from verify_native_camcorder_mesh import CALIBRATION_SHA256, calibration_contact, fixed_grip_mount
from verify_native_camcorder_postprocess import raw_program_grade


class NativeRawOpticsProgramTests(unittest.TestCase):
    def test_actual_bound_program_bytes_have_strict_same_frame_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            payload = bytes(range(32))
            path = directory / "native-raw-optics-program-400.bin"
            path.write_bytes(payload)
            row = dict(raw_optics_schema_version=2, frame=400, first_hdr_program=path.name,
                       first_hdr_program_bytes=len(payload),
                       first_hdr_postprocess_shader=hashlib.sha256(payload).hexdigest())
            self.assertEqual(raw_program_grade(directory, row), path)
            for field, wrong in (
                    ("frame", 401), ("frame", 400.0), ("first_hdr_program", "..\\outside.bin"),
                    ("first_hdr_program_bytes", 28), ("first_hdr_program_bytes", 32.0),
                    ("first_hdr_postprocess_shader", "0" * 64),
                    ("raw_optics_schema_version", True), ("raw_optics_schema_version", 3)):
                bad = dict(row)
                bad[field] = wrong
                with self.subTest(field=field, wrong=wrong), self.assertRaises(ValueError):
                    raw_program_grade(directory, bad)
            with self.assertRaises(ValueError):
                raw_program_grade(directory, row, "wrong-first-hdr-program")
            path.write_bytes(payload[:-1] + b"\xff")
            with self.assertRaises(ValueError):
                raw_program_grade(directory, row)
            path.unlink()
            with self.assertRaises(ValueError):
                raw_program_grade(directory, row)

    def test_historical_raw_provenance_is_not_relabelled_as_new_program_evidence(self):
        self.assertIsNone(raw_program_grade(Path("."), {"frame": 400}))
        with self.assertRaises(ValueError):
            raw_program_grade(Path("."), {"frame": 400, "raw_optics_schema_version": 2})


class NativeCameraGripCalibrationTests(unittest.TestCase):
    def test_native_raised_pose_is_not_the_reference_socket_contact(self):
        path = Path(__file__).with_name("native_camera_grip_calibration.json")
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), CALIBRATION_SHA256)
        contact = calibration_contact(json.loads(path.read_bytes()))
        np.testing.assert_allclose(contact, [-.812240687, 4.82349342, -5.27160731], atol=1e-8)
        self.assertGreater(np.linalg.norm(contact - [5.13967896, .155340061, -.216287166]), 8)

    def test_both_fixed_mounts_keep_correct_contact_forward_and_up(self):
        contact = calibration_contact(json.loads(Path(__file__).with_name("native_camera_grip_calibration.json").read_bytes()))
        grips = [(np.array([3., .2, -.1]), np.eye(3)), (np.array([4., -.2, .1]), np.eye(3))]
        identity = np.eye(4).reshape(16).tolist()
        for hand in (0, 1):
            point = contact * [1, 1 if hand else -1, 1]
            row = dict(hand=hand, bone=36 if hand else 12, stock_original_local=identity,
                       right_wrist_world=identity, left_wrist_world=identity, model_grip_point=point.tolist())
            world = fixed_grip_mount(row, grips, contact)
            np.testing.assert_allclose(world[0, :3], [0, 0, -1], atol=1e-12)
            np.testing.assert_allclose(world[2, :3], [0, -1, 0], atol=1e-12)
            np.testing.assert_allclose((np.append(point, 1) @ world)[:3], grips[hand][0], atol=1e-12)
            for wrong in ([5.13967896, .155340061, -.216287166], (point + [.1, 0, 0]).tolist()):
                bad = copy.deepcopy(row)
                bad["model_grip_point"] = wrong
                with self.assertRaises(ValueError):
                    fixed_grip_mount(bad, grips, contact)


def fixture():
    width, height = 2688, 2880
    translation = np.array([5., -3., 2.])
    projection = np.array([[1.5, 0, .12, 0], [0, 1.6, -.07, 0],
                           [0, 0, 1., 0], [0, 0, -1., 0]])
    projection[:, 3] = -projection[:, :3] @ translation
    eyes = np.zeros((2, 32))
    eyes[:, :16] = projection.reshape(16)
    eyes[:, 16:19] = translation
    eyes[:, 20:23] = translation
    camera = dict(rendered_eyes=eyes.tolist(), pretranslation=translation.tolist())
    origin = np.array([.35, -.18, -.3])
    points = np.array([[x, y, z] for x in (-.4, -.1, .2, .5)
                       for y in (-.2, .25) for z in (-1.2, -2.5)])
    rays = points - origin
    rays /= np.linalg.norm(rays, axis=1, keepdims=True)
    target = np.column_stack((
        ((1.5 * points[:, 0] + .12 * points[:, 2]) / -points[:, 2] + 1) * width / 2,
        (1 - (1.6 * points[:, 1] - .07 * points[:, 2]) / -points[:, 2]) * height / 2,
    ))
    return camera, dict(origin=origin.tolist()), rays, target, width, height


class NativeCameraFixtureTests(unittest.TestCase):
    def test_controlled_roll_requires_actual_matching_submissions(self):
        fixture_grade({"physical_camcorder_fixture_version": 5},
                      [{"physical_camcorder_fixture_version": 5}])

    def test_missing_mixed_and_mistyped_submissions_are_rejected(self):
        for frames in ([], [{}], [{"physical_camcorder_fixture_version": 3}],
                       [{"physical_camcorder_fixture_version": 5.0}],
                       [{"physical_camcorder_fixture_version": 5},
                        {"physical_camcorder_fixture_version": 4}]):
            with self.subTest(frames=frames), self.assertRaises(ValueError):
                fixture_grade({"physical_camcorder_fixture_version": 5}, frames)

    def test_a_controlled_roll_cannot_be_relabelled_as_a_historical_fixture(self):
        for manifest in ({}, {"physical_camcorder_fixture_version": 3},
                         {"physical_camcorder_fixture_version": 4}):
            with self.subTest(manifest=manifest), self.assertRaises(ValueError):
                fixture_grade(manifest, [{"physical_camcorder_fixture_version": 5}])

    def test_cancelled_frames_do_not_supply_a_pose_witness(self):
        fixture_grade({"physical_camcorder_fixture_version": 5},
                      [{"physical_camcorder_fixture_version": 5}, {"canceled": True}])
        with self.assertRaises(ValueError):
            fixture_grade({"physical_camcorder_fixture_version": 5},
                          [{"physical_camcorder_fixture_version": 5, "canceled": True}])

    def test_legacy_fixtures_remain_compatible(self):
        for manifest in ({}, {"physical_camcorder_fixture_version": 3},
                         {"physical_camcorder_fixture_version": 4}):
            fixture_grade(manifest, [{}])

    def test_unknown_or_mistyped_manifest_versions_fail_closed(self):
        for version in (0, 8, -1, True, "5", 5.0):
            with self.subTest(version=version), self.assertRaises(ValueError):
                fixture_grade({"physical_camcorder_fixture_version": version}, [{}])

    def test_canonical_grip_fixture_cannot_be_confused_with_rolled_or_combined_poses(self):
        fixture_grade({"physical_camcorder_fixture_version": 6},
                      [{"physical_camcorder_fixture_version": 6}])
        fixture_grade({"physical_camcorder_fixture_version": 7},
                      [{"physical_camcorder_fixture_version": 7}])
        for manifest_version, submitted_version in (
                (6, 5), (5, 6), (4, 6), (6, 4), (6, 6.0), (6, 7), (7, 6), (4, 7), (7, 7.0)):
            with self.subTest(manifest=manifest_version, submitted=submitted_version), self.assertRaises(ValueError):
                fixture_grade({"physical_camcorder_fixture_version": manifest_version},
                              [{"physical_camcorder_fixture_version": submitted_version}])


class NativeCameraOpticalBaselineTests(unittest.TestCase):
    def test_finite_authored_lens_baseline_uses_actual_eye_projection(self):
        result = finite_baseline_grade(*fixture())
        self.assertEqual(result["positive_depth_correspondences"], 16)
        self.assertEqual(result["direction_method"], "finite_baseline_positive_depth_triangulation")
        self.assertLess(result["direction_error90_px"], 1e-6)

    def test_the_infinite_distance_model_is_not_a_valid_near_camera_oracle(self):
        _, _, rays, target, width, height = fixture()
        infinite = np.column_stack((
            ((1.5 * rays[:, 0] + .12 * rays[:, 2]) / -rays[:, 2] + 1) * width / 2,
            (1 - (1.6 * rays[:, 1] - .07 * rays[:, 2]) / -rays[:, 2]) * height / 2,
        ))
        self.assertGreater(float(np.median(np.linalg.norm(infinite - target, axis=1))), width * .035)

    def test_backwards_lens_and_incorrect_baseline_direction_are_rejected(self):
        camera, view, rays, target, width, height = fixture()
        with self.assertRaises(ValueError):
            finite_baseline_grade(camera, view, -rays, target, width, height)
        wrong_view = copy.deepcopy(view)
        wrong_view["origin"][1] += .4
        with self.assertRaises(ValueError):
            finite_baseline_grade(camera, wrong_view, rays, target, width, height)

    def test_missing_or_inconsistent_actual_native_eye_transform_is_rejected(self):
        camera, view, rays, target, width, height = fixture()
        for mutation in (
            lambda value: value.pop("rendered_eyes"),
            lambda value: value["rendered_eyes"][0].__setitem__(3, 0),
            lambda value: value["pretranslation"].__setitem__(1, 0),
        ):
            bad = copy.deepcopy(camera)
            mutation(bad)
            with self.assertRaises(ValueError):
                finite_baseline_grade(bad, view, rays, target, width, height)


class IndependentWorldReferenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        transform = np.diag([.35, .35, 1., 1.]).reshape(16).tolist()
        self.row = dict(frame=5, screen_matrices=[transform, transform])
        self.data = dict(directory=self.root, composition=[self.row])
        self.frame = dict(frame=6, native_frame=5)
        self.before = np.full((64, 96, 3), [10, 20, 30], dtype=np.uint8)
        self.excluded = screen_mask(self.row, 0, 96, 64)
        self.composed = self.before.copy()
        self.composed[self.excluded] = [120, 130, 140]
        cv2.imwrite(str(self.root / "native-screen-before-5-eye-0.ppm"), self.before)
        cv2.imwrite(str(self.root / "frame_6_eye_0.ppm"), self.composed)

    def test_the_camera_cannot_match_its_own_display_as_independent_world_evidence(self):
        world, mask = independent_world_image(self.data, self.frame)
        np.testing.assert_array_equal(world, cv2.cvtColor(self.before, cv2.COLOR_BGR2GRAY))
        self.assertTrue(np.all(mask[self.excluded] == 0))
        self.assertTrue(np.all(mask[~self.excluded] == 255))

    def test_world_pixels_outside_the_native_screen_cannot_change(self):
        self.composed[0, 0, 0] += 1
        cv2.imwrite(str(self.root / "frame_6_eye_0.ppm"), self.composed)
        with self.assertRaises(ValueError):
            independent_world_image(self.data, self.frame)

    def test_missing_duplicate_and_wrong_frame_lcd_metadata_are_rejected(self):
        for records in ([], [self.row, self.row], [dict(self.row, frame=4)]):
            with self.assertRaises(ValueError):
                independent_world_image(dict(self.data, composition=records), self.frame)

    def test_misidentified_screen_and_missing_before_image_are_rejected(self):
        bad = copy.deepcopy(self.data)
        bad["composition"][0]["screen_matrices"][0][3] = 1
        with self.assertRaises(ValueError):
            independent_world_image(bad, self.frame)
        (self.root / "native-screen-before-5-eye-0.ppm").unlink()
        with self.assertRaises(ValueError):
            independent_world_image(self.data, self.frame)


if __name__ == "__main__":
    unittest.main()
