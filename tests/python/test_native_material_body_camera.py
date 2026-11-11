import copy
import json
import math
from pathlib import Path
import tempfile
import unittest

from verify_native_reflective_glass import combined_camera_observations, material_body_camera_policy
from verify_native_camcorder_mesh import CALIBRATION_SHA256


def fixture():
    identity = dict(material_body_camera_fixture_version=1, body=True, floor=True,
                    physical_camcorder=True, native_camera_hud=True, camera_mount_version=4,
                    physical_camcorder_fixture_version=3,
                    reference_eye_ownership_version=1, no_projection_images_fixture_version=2,
                    headset_extent_2688x2880=True)
    frames = []
    for world in range(320, 421):
        phase = world - 300
        center = [.45 * math.sin(phase * .07), 1.7 + .25 * math.sin(phase * .11),
                  .45 * math.sin(phase * .05)]
        frames.append(dict(gameplay_frame=world, native_frame=world + 280,
                           material_body_camera_fixture_version=1, floor_reference_ready=True,
                           floor_reference_acquisitions=1, standing_reference_m=1.7,
                           camera_mount_version=4, physical_camcorder_fixture_version=3,
                           views=[dict(orientation=[0, 0, 0, 1],
                                       position=[center[0] + d, center[1], center[2]])
                                  for d in (-.032, .032)]))
    return identity, frames


class CombinedMaterialFixtureTests(unittest.TestCase):
    def test_canonical_mount_preserves_combined_inputs_and_requires_exact_identity(self):
        identity, frames = fixture()
        identity.update(camera_mount_version=5, camera_grip_calibration_sha256=CALIBRATION_SHA256)
        for frame in frames:
            frame["camera_mount_version"] = 5
        self.assertTrue(material_body_camera_policy(identity, frames)["combined_floor_pose_policy_validated"])
        for mutation in (
                lambda value: value.pop("camera_grip_calibration_sha256"),
                lambda value: value.__setitem__("camera_grip_calibration_sha256", "0" * 64)):
            damaged = dict(identity)
            mutation(damaged)
            with self.assertRaises(ValueError):
                material_body_camera_policy(damaged, frames)
        frames[40]["camera_mount_version"] = 4
        with self.assertRaises(ValueError):
            material_body_camera_policy(identity, frames)

    def test_canonical_combined_observations_reject_wrong_policy_and_calibration(self):
        _, frames = fixture()
        for frame in frames:
            frame["camera_mount_version"] = 5
        matrix = [1 if i % 5 == 0 else 0 for i in range(16)]
        records = [dict(frame=600 + i, source="body_post_ik", hand=i % 2,
                        controller_tracked=True, stock_mount_version=5,
                        optical_tracking="immutable_anatomical_grip_minus_Z_plus_Y", native_body_serial=i + 1,
                        asset_name="handycam", world=matrix, expected_world=matrix) for i in range(20)]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            calibration = root / "native-camera-grip-calibration.json"
            calibration.write_bytes(Path(__file__).with_name("native_camera_grip_calibration.json").read_bytes())
            path = root / "native-camera-mesh.jsonl"
            path.write_text("\n".join(json.dumps(item) for item in records))
            self.assertTrue(combined_camera_observations(root, frames)["actual_native_camera_states_validated"])
            records[0]["optical_tracking"] = "immutable_authored_wrist_mount"
            path.write_text("\n".join(json.dumps(item) for item in records))
            with self.assertRaises(ValueError):
                combined_camera_observations(root, frames)
            calibration.write_bytes(b"{}")
            with self.assertRaises(ValueError):
                combined_camera_observations(root, frames)

    def test_combined_floor_is_in_every_actual_submitted_eye(self):
        identity, frames = fixture()
        result = material_body_camera_policy(identity, frames)
        self.assertTrue(result["combined_floor_pose_policy_validated"])
        self.assertEqual(result["submitted_roomscale_frames"], 101)

    def test_cadenced_combined_fixture_cannot_relabel_full_continuous_coverage(self):
        identity, frames = fixture()
        identity.update(material_body_camera_fixture_version=2, material_readback_stride=20)
        for frame in frames:
            frame.update(material_body_camera_fixture_version=2, material_readback_stride=20)
        self.assertEqual(material_body_camera_policy(identity, frames)["material_readback_stride"], 20)
        for value in (1, True, 10):
            bad = copy.deepcopy(identity)
            bad["material_readback_stride"] = value
            with self.assertRaises(ValueError):
                material_body_camera_policy(bad, frames)
        frames[40]["material_readback_stride"] = 1
        with self.assertRaises(ValueError):
            material_body_camera_policy(identity, frames)

    def test_regrip_fixture_requires_its_actual_new_controller_schedule(self):
        identity, frames = fixture()
        identity.update(material_body_camera_fixture_version=3, material_readback_stride=20,
                        physical_camcorder_fixture_version=4)
        for frame in frames:
            frame.update(material_body_camera_fixture_version=3, material_readback_stride=20,
                         physical_camcorder_fixture_version=4)
        self.assertEqual(material_body_camera_policy(identity, frames)["fixture_version"], 3)
        frames[40]["physical_camcorder_fixture_version"] = 3
        with self.assertRaises(ValueError):
            material_body_camera_policy(identity, frames)

    def test_incomplete_or_relabelled_declarations_are_rejected(self):
        identity, frames = fixture()
        for key, value in (("body", False), ("floor", False), ("physical_camcorder", False),
                           ("native_camera_hud", False), ("camera_mount_version", 3),
                           ("reference_eye_ownership_version", 0),
                           ("material_body_camera_fixture_version", True)):
            damaged = copy.deepcopy(identity)
            damaged[key] = value
            with self.assertRaises(ValueError):
                material_body_camera_policy(damaged, frames)

    def test_missing_height_or_floor_acquisition_is_rejected(self):
        identity, frames = fixture()
        for mutation in (
            lambda f: f[40]["views"][1]["position"].__setitem__(1, f[40]["views"][1]["position"][1] - 1.7),
            lambda f: f[40].__setitem__("floor_reference_ready", False),
            lambda f: f[40].__setitem__("floor_reference_acquisitions", 2),
            lambda f: f[40].__setitem__("material_body_camera_fixture_version", 0),
            lambda f: f.pop(),
        ):
            damaged = copy.deepcopy(frames)
            mutation(damaged)
            with self.assertRaises(ValueError):
                material_body_camera_policy(identity, damaged)

    def test_legacy_isolated_fixture_cannot_claim_combined_state(self):
        self.assertFalse(material_body_camera_policy(
            dict(body=False, physical_camcorder=False), [{}])["combined_floor_pose_policy_validated"])
        with self.assertRaises(ValueError):
            material_body_camera_policy(dict(body=True, physical_camcorder=False), [{}])

    def test_held_camera_must_be_observed_in_both_hands_during_material_frames(self):
        _, frames = fixture()
        identity = [1 if i % 5 == 0 else 0 for i in range(16)]
        records = [dict(frame=600 + i, source="body_post_ik", hand=i % 2,
                        controller_tracked=True, stock_mount_version=4,
                        optical_tracking="immutable_authored_wrist_mount", native_body_serial=i + 1,
                        asset_name="handycam", world=identity, expected_world=identity)
                   for i in range(20)]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "native-camera-mesh.jsonl"
            path.write_text("\n".join(json.dumps(item) for item in records), encoding="utf-8")
            result = combined_camera_observations(root, frames)
            self.assertEqual(result["actual_left_held_camera_observations"], 10)
            self.assertEqual(result["actual_right_held_camera_observations"], 10)
            records[0]["controller_tracked"] = False
            path.write_text("\n".join(json.dumps(item) for item in records), encoding="utf-8")
            with self.assertRaises(ValueError):
                combined_camera_observations(root, frames)

    def test_each_material_witness_needs_its_actual_declared_native_mount(self):
        _, frames = fixture()
        for frame in frames:
            frame["material_body_camera_fixture_version"] = 3
        matrix = [1 if i % 5 == 0 else 0 for i in range(16)]
        records = [dict(frame=world + 279, source="body_post_ik",
                        hand=1 if world < 350 else -1 if world < 398 else 0,
                        controller_tracked=world < 350 or world >= 398,
                        stock_mount_version=4, optical_tracking="immutable_authored_wrist_mount",
                        native_body_serial=world, asset_name="handycam",
                        world=matrix, expected_world=matrix) for world in range(320, 421)]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "native-camera-mesh.jsonl"
            path.write_text("\n".join(json.dumps(item) for item in records), encoding="utf-8")
            result = combined_camera_observations(root, frames)
            self.assertEqual(result["actual_material_witness_camera_hands"],
                             {"320": 1, "340": 1, "360": -1, "380": -1, "400": 0, "420": 0})
            for mutation in (
                lambda row: row.__setitem__("hand", -1),
                lambda row: row.__setitem__("frame", row["frame"] + 1),
                lambda row: row.__setitem__("native_body_serial", 0),
            ):
                damaged = copy.deepcopy(records)
                mutation(damaged[80])
                path.write_text("\n".join(json.dumps(item) for item in damaged), encoding="utf-8")
                with self.assertRaises(ValueError):
                    combined_camera_observations(root, frames)


if __name__ == "__main__":
    unittest.main()
