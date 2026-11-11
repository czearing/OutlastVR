import copy
import math
import unittest

from verify_native_camera_gait import analyze_records


def fixture():
    manifest = dict(scenario="Continue", gait=True, head_turn=False)
    frames, cameras, poses = [], [], []
    for serial in range(1, 901):
        progress = max(0, min(120, serial - 60)) / 120
        root = [360 * progress, 0, 10 * progress]
        height = 60 if 300 <= serial <= 420 else 165
        origin = [root[0], root[1], root[2] + height]
        frames.append(dict(native_frame=serial, world_panel=False, gait_probe=True,
                           head_turn_probe=False))
        cameras.append(dict(native_frame=serial, world_panel=False, camera_pose_serial=serial,
                            tracking_up=[0, 0, 1], tracking_backward=[1, 0, 0], origin=origin[:]))
        poses.append(dict(serial=serial, basic_state=0, special_state=0, camera_mode=0,
                          root=root, base_pose=origin + [180, .6 * math.sin(serial / 20),
                                                        .8 * math.sin(serial / 10)]))
    events = [dict(gameplay_frame=frame, key=key, down=down) for frame, key, down in
              ((60, 87, True), (180, 87, False), (300, 162, True), (420, 162, False))]
    return manifest, frames, cameras, poses, events


class NativeCameraGaitTests(unittest.TestCase):
    def setUp(self):
        self.data = fixture()

    def reject(self, message):
        with self.assertRaisesRegex(ValueError, message):
            analyze_records(*self.data)

    def test_real_gait_scope(self):
        report = analyze_records(*self.data)
        self.assertEqual(report["native_horizontal_pawn_displacement_units"], 360)
        self.assertEqual(report["native_terrain_height_change_units"], 10)
        self.assertEqual(report["native_stance_heights_units"]["crouched"], 60)
        self.assertTrue(report["native_crouch_preserved"])
        self.assertIsNone(report["positional_bob_removed"])
        self.assertFalse(report["comfortable_play_certified"])

    def test_wrong_scenario(self):
        self.data[0]["head_turn"] = True
        self.reject("isolated native gait")

    def test_versioned_clear_corridor_gait(self):
        self.data[0]["gait_schedule_version"] = 2
        self.data[4][1]["gameplay_frame"] = 120
        for frame, camera, pose in zip(self.data[1], self.data[2], self.data[3]):
            frame["gait_schedule_version"] = 2
            progress = max(0, min(60, pose["serial"] - 60)) / 120
            root = [360 * progress, 0, 10 * progress]
            delta = [root[axis] - pose["root"][axis] for axis in range(3)]
            pose["root"] = root
            for axis in range(3):
                pose["base_pose"][axis] += delta[axis]
                camera["origin"][axis] += delta[axis]
        self.assertEqual(analyze_records(*self.data)["native_horizontal_pawn_displacement_units"], 180)

    def test_capture_schedule_metadata_cannot_be_relabeled(self):
        self.data[1][100]["gait_schedule_version"] = 2
        self.reject("schedule metadata differs")

    def test_unsupported_gait_schedule_version(self):
        self.data[0]["gait_schedule_version"] = 4
        self.reject("Unsupported native gait")

    def test_lean_capture_requires_separate_acceptance(self):
        self.data[0]["lean"] = True
        self.reject("isolated native gait")

    def test_posted_events_without_walking(self):
        for pose in self.data[3]:
            pose["root"][0] = 0
        self.reject("native pawn travel")

    def test_posted_events_without_crouching(self):
        for pose in self.data[3]:
            pose["base_pose"][2] = pose["root"][2] + 165
        for camera, pose in zip(self.data[2], self.data[3]):
            camera["origin"][2] = pose["base_pose"][2]
        self.reject("actual native camera stance")

    def test_fixed_standing_height_breaks_crouching(self):
        for camera, pose in zip(self.data[2], self.data[3]):
            camera["origin"][2] = pose["root"][2] + 165
        self.reject("native crouched height")

    def test_stance_does_not_recover(self):
        for camera, pose in zip(self.data[2][420:], self.data[3][420:]):
            pose["base_pose"][2] = pose["root"][2] + 60
            camera["origin"][2] = pose["base_pose"][2]
        self.reject("did not recover")

    def test_animated_roll_reaches_tracking(self):
        self.data[2][100]["tracking_up"] = [0, .01, .99995]
        self.reject("tracked gravity basis")

    def test_paused_camera_with_continued_rendering(self):
        for camera in self.data[2][599:850]:
            camera["camera_pose_serial"] = 599
        self.reject("updates stopped")

    def test_missing_camera_link(self):
        self.data[2][100]["camera_pose_serial"] = 901
        self.reject("Missing linked native camera pose")

    def test_completed_source_is_not_replaced_with_latest_animation(self):
        for camera in self.data[2]:
            camera["camera_source_serial"] = camera["camera_pose_serial"]
            camera["camera_completed_serial"] = camera["camera_source_serial"]
            camera["camera_pose_serial"] = 900
        self.assertTrue(analyze_records(*self.data)["native_crouch_preserved"])

    def test_bit_equivalent_crouch_poses_do_not_fake_paused_native_updates(self):
        for camera in self.data[2][349:410]:
            camera["camera_completed_serial"] = camera["camera_pose_serial"]
            camera["camera_source_serial"] = 350
        self.assertTrue(analyze_records(*self.data)["native_crouch_preserved"])

    def test_duplicate_camera_evidence(self):
        self.data[2].append(copy.deepcopy(self.data[2][-1]))
        self.reject("duplicate native_frame")

    def test_boot_camera_updates_are_not_gameplay_frames(self):
        boot = dict(native_frame=0, world_panel=True, camera_pose_serial=0)
        self.data[2].extend([boot, boot.copy()])
        self.assertEqual(analyze_records(*self.data)["gameplay_projections"], 900)

    def test_native_crouch_transition_states_are_not_steady_stances(self):
        for begin, end, state in ((300, 318, 1), (420, 436, 2)):
            for pose in self.data[3][begin:end]:
                pose["special_state"] = state
                pose["camera_mode"] = -1
        self.assertTrue(analyze_records(*self.data)["native_crouch_preserved"])

    def test_unsupported_steady_camera_state(self):
        self.data[3][100]["special_state"] = 3
        self.reject("unsupported native camera state")

    def test_native_pose_is_frozen(self):
        for pose in self.data[3]:
            pose["base_pose"][4:] = [0, 0]
        self.reject("exercise animated camera")

    def test_unreleased_owned_key(self):
        self.data[4].pop()
        self.reject("input schedule differs")

    def test_nonfinite_camera(self):
        self.data[2][100]["origin"][2] = float("nan")
        self.reject("Invalid native camera vector")

    def test_incomplete_projection_coverage(self):
        self.data[1].pop()
        self.reject("Fewer than 900")


if __name__ == "__main__":
    unittest.main()
