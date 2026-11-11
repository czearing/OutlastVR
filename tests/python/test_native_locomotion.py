import math
import unittest

from verify_native_locomotion import analyze_records


def fixture(lean=False, version=1, delayed_stance=False, right_tail_frames=16):
    manifest = dict(scenario="Continue", gait=True, head_turn=False, lean=lean)
    if version >= 2:
        manifest["gait_schedule_version"] = version
    release = {1: 180, 2: 120, 3: 80}[version]
    frames, cameras, poses, updates = [], [], [], []
    initial = dict(serial=1, camera=1, hero=2, root=[-7500, -600, 557],
                   root_rotator=[0, 32768, 0], basic_state=0, special_state=0,
                   camera_mode=0, camera_flags=2, stance_region_0x2f80=[0, 0, 0],
                   lean_intent=0, lean_weight=0, lean_weight_observed=False,
                   base_pose=[-7520, -602, 725, 180, 0, 0])
    poses.append(initial)
    updates.append(dict(camera_pose_serial=1, camera=1, final_pose=initial["base_pose"][:]))
    for frame in range(1, 902):
        progress = max(0, min(release - 60, frame - 60))
        root = [-7500 - progress * (8 if version == 3 else 4),
                -600 + progress * (2 if version == 3 else 1), 548 + progress / 12]
        crouched = 300 <= frame <= 420
        stance_flag = crouched and (not delayed_stance or frame >= 380)
        special = 1 if 300 <= frame <= 318 else 2 if 421 <= frame <= 436 else 0
        weight = 0
        if lean:
            for start, stop, sign in ((500, 560, -1), (650, 710, 1)):
                if start <= frame <= stop:
                    weight = sign
                elif stop < frame <= stop + (right_tail_frames if sign > 0 else 16):
                    decay = 16 / right_tail_frames if sign > 0 else 1
                    weight = sign * 2 ** (-(frame - stop) * decay)
        intent = (-1 if 500 <= frame < 560 else 1 if 650 <= frame < 710 else 0) if lean else 0
        supported = not special and weight == 0 and intent == 0
        height = 60 if crouched else 168 + (.6 if 60 <= frame <= release + 10 else .2) * math.sin(frame * .13)
        if crouched and delayed_stance:
            height += (.03125 if frame >= 380 else 0) + .01 * math.sin(frame * .13)
        if frame <= 3:
            height = [175, 173, 169][frame - 1]
        bone = [root[0] - 20, root[1] - 2 - 45 * weight,
                root[2] + height - 9.3 * abs(weight)]
        pose = dict(serial=frame + 1, camera=1, hero=2, root=root, root_rotator=[0, 32768, 0],
                    basic_state=0, special_state=special, camera_mode=-1 if special else 0,
                    camera_flags=2, stance_region_0x2f80=[0, 0, int(stance_flag)],
                    lean_intent=intent, lean_weight=weight, lean_weight_observed=True,
                    base_pose=bone + [180, .6 * math.sin(frame / 20), .8 * math.sin(frame / 10)])
        poses.append(pose)
        final = [bone[0] + .25, bone[1] - .125, bone[2] + .5]
        updates.append(dict(camera_pose_serial=frame + 1, camera=1,
                            final_pose=final + pose["base_pose"][3:]))
        if frame > 900:
            continue
        reference = 60 if crouched else 168
        if crouched and delayed_stance:
            reference += ((.03125 + .01 * math.sin(380 * .13)) if frame >= 380
                          else .01 * math.sin(319 * .13))
        if frame >= 437:
            reference = 168 + .2 * math.sin(437 * .13)
        if lean:
            for reset in (577, 711 + right_tail_frames):
                if frame >= reset:
                    reference = 168 + .2 * math.sin(reset * .13)
        tracked = [root[0] - 20 + .25, root[1] - 2 - .125, root[2] + reference + .5]
        frames.append(dict(native_frame=1000 + frame, world_panel=False, gait_probe=True,
                           head_turn_probe=False, lean_probe=lean))
        if version >= 2:
            frames[-1]["gait_schedule_version"] = version
        cameras.append(dict(native_frame=1000 + frame, world_panel=False,
                            camera_pose_serial=frame + 2, camera_source_serial=frame + 1,
                            camera_completed_serial=frame + 1, camera_stabilized=supported,
                            origin=final, tracking_origin=tracked if supported else final[:],
                            tracking_up=[0, 0, 1], tracking_backward=[1, 0, 0]))
    schedule = [(60, 87, True), (release, 87, False), (300, 162, True), (420, 162, False)]
    if lean:
        schedule += [(500, 81, True), (560, 81, False), (650, 69, True), (710, 69, False)]
    events = [dict(gameplay_frame=f, key=k, down=d) for f, k, d in schedule]
    return manifest, frames, cameras, poses, updates, events


class NativeLocomotionTests(unittest.TestCase):
    def setUp(self):
        self.data = fixture()

    def reject(self, message):
        with self.assertRaisesRegex(ValueError, message):
            analyze_records(*self.data)

    def test_real_source_bound_root_motion_and_stances(self):
        report = analyze_records(*self.data)
        self.assertTrue(report["positional_bob_removed"])
        self.assertTrue(report["native_root_terrain_stance_preserved"])
        self.assertEqual(report["gameplay_projections"], 900)
        self.assertFalse(report["comfortable_play_certified"])

    def test_both_native_lean_directions_and_release_tails(self):
        report = analyze_records(*fixture(lean=True))
        self.assertTrue(report["observed_left_lean"])
        self.assertTrue(report["observed_right_lean"])
        self.assertGreaterEqual(report["preserved_lean_release_tail_views"], 3)

    def test_versioned_corridor_fixture_still_requires_real_motion_and_stances(self):
        report = analyze_records(*fixture(lean=True, version=2))
        self.assertGreater(report["native_gait"]["native_horizontal_pawn_displacement_units"], 100)
        self.assertTrue(report["observed_right_lean"])
        self.assertTrue(report["native_root_terrain_stance_preserved"])

    def test_measured_short_walk_fixture_retains_the_same_motion_gates(self):
        report = analyze_records(*fixture(lean=True, version=3))
        self.assertGreater(report["native_gait"]["native_horizontal_pawn_displacement_units"], 100)
        self.assertTrue(report["positional_bob_removed"])

    def test_delayed_native_stance_flag_keeps_distinct_native_anchors(self):
        report = analyze_records(*fixture(lean=True, version=3, delayed_stance=True))
        phase = report["phases"]["crouched"]
        self.assertEqual(phase["native_stance_transitions"], 1)
        self.assertGreater(phase["tracked_bone_height_range"], .02)
        self.assertEqual(len(phase["steady_stances"]), 2)
        self.assertTrue(all(stance["tracked_bone_height_range"] < .001
                            for stance in phase["steady_stances"]))

    def test_delayed_stance_does_not_hide_animation_bob(self):
        self.data = fixture(lean=True, version=3, delayed_stance=True)
        for camera in self.data[2][350:410]:
            camera["tracking_origin"] = camera["origin"][:]
        self.reject("independent native root/bone correction")

    def test_alternating_stance_flags_do_not_establish_steady_motion(self):
        for pose in self.data[3][350:411]:
            pose["stance_region_0x2f80"][2] = pose["serial"] % 2
        self.reject("No sustained native stance")

    def test_native_release_tail_finishes_before_recovered_stance_is_measured(self):
        report = analyze_records(*fixture(lean=True, version=3, delayed_stance=True, right_tail_frames=47))
        phase = report["phases"]["standing_recovered"]
        self.assertEqual(phase["native_release_tail_prefix_views"], 8)
        self.assertGreater(report["preserved_lean_release_tail_views"], 3)

    def test_later_native_release_tail_cannot_be_clipped(self):
        self.data = fixture(lean=True, version=3, right_tail_frames=47)
        self.data[2][750]["tracking_origin"][2] += .02
        self.reject("independent native root/bone correction")

    def test_recovery_requires_twenty_actually_settled_views(self):
        self.data = fixture(lean=True, version=3, right_tail_frames=150)
        self.reject("Insufficient settled native standing_recovered coverage")

    def test_retained_translation_bob(self):
        for camera in self.data[2][70:175]:
            camera["tracking_origin"] = camera["origin"][:]
        self.reject("independent native root/bone correction")

    def test_wrong_standing_height_through_crouch(self):
        self.data[2][350]["tracking_origin"][2] += 108
        self.reject("independent native root/bone correction")

    def test_frozen_root_during_real_native_travel(self):
        for camera in self.data[2]:
            camera["tracking_origin"][0] = -7519.75
        self.reject("independent native root/bone correction")

    def test_latest_animation_pose_is_not_renderer_source(self):
        self.data[2][100]["camera_source_serial"] += 1
        self.reject("Future native pose")

    def test_future_completion_history_is_rejected(self):
        self.data[2][100]["camera_completed_serial"] -= 1
        self.reject("Future native pose")

    def test_completed_source_is_mandatory(self):
        self.data[4].pop(101)
        self.reject("Missing completed native source")

    def test_native_cross_axis_inverse_view_roundoff(self):
        camera = self.data[2][100]
        camera["origin"][2] += .0006
        camera["tracking_origin"][2] += .0006
        self.assertTrue(analyze_records(*self.data)["completed_pose_association_verified"])

    def test_non_roundoff_camera_displacement_is_rejected(self):
        self.data[2][100]["origin"][2] += .1
        self.reject("closest causally completed native camera")

    def test_closest_source_is_not_latest_completed_pose(self):
        camera = self.data[2][100]
        camera["camera_completed_serial"] += 1
        self.assertTrue(analyze_records(*self.data)["completed_pose_association_verified"])

    def test_expired_history_cannot_supply_a_source(self):
        camera = self.data[2][100]
        camera["camera_completed_serial"] += 70
        camera["camera_pose_serial"] = camera["camera_completed_serial"]
        self.reject("closest causally completed native camera")

    def test_false_bypass_is_rejected(self):
        self.data[2][100]["camera_stabilized"] = False
        self.reject("bypass/eligibility")

    def test_special_state_must_remain_native(self):
        self.data[2][310]["tracking_origin"][2] += 2
        self.reject("independent native root/bone correction")

    def test_clipped_actual_lean_tail_is_rejected(self):
        self.data = fixture(lean=True)
        self.data[2][570]["tracking_origin"][1] += 2
        self.reject("independent native root/bone correction")

    def test_lean_events_without_real_camera_lean_are_rejected(self):
        self.data = fixture(lean=True)
        for sample in self.data[3]:
            sample["lean_weight"] = 0
        self.reject("bypass/eligibility|independent native root/bone correction")

    def test_coherent_but_transient_startup_anchor_is_rejected(self):
        self.data[3][0]["base_pose"][2] += 7
        self.data[4][0]["final_pose"][2] += 7
        for camera in self.data[2][:299]:
            camera["tracking_origin"][2] += 7
        self.reject("transient or wrong stance height")

    def test_nonfinite_tracking_is_rejected(self):
        self.data[2][100]["tracking_origin"][2] = float("nan")
        self.reject("Invalid native camera vector")

    def test_wrong_capture_scope_is_rejected(self):
        self.data[1][100]["lean_probe"] = True
        self.reject("lean capture scope")

    def test_incomplete_actual_projection_coverage_is_rejected(self):
        self.data[1].pop()
        self.reject("Fewer than 900")


if __name__ == "__main__":
    unittest.main()
