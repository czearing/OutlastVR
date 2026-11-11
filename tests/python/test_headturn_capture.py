import json
import math
import tempfile
import unittest
from pathlib import Path

from verify_headturn_capture import analyze, rotation


class HeadTurnCaptureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name)
        (self.path / "capture-complete.txt").write_text("synthetic verifier fixture\n")
        self.manifest = dict(head_turn=True, scenario="Continue", mod_sha256="synthetic-test-only")
        (self.path / "native-run-manifest.json").write_text(json.dumps(self.manifest))
        self.frames = []
        self.cameras = []
        for index in range(900):
            yaw = 0 if index < 300 or index >= 750 else math.pi/2 if index < 450 else (
                -math.pi/2 if index < 600 else math.pi)
            q = [0, math.sin(yaw/2), 0, math.cos(yaw/2)]
            views = [dict(orientation=q, position=(rotation(q) @
                          [.032 if eye else -.032, 0, 0]).tolist()) for eye in range(2)]
            self.frames.append(dict(
                frame=index, native_frame=index, world_panel=False, head_turn_probe=True,
                capture_serial=index+30, captured=index % 30 == 0,
                views=views, sphere_inside_culls=5, crosshair_updates=1,
                driver_draws=[dict(indexed=True, instances=2)], logical_geometry_draws=1,
                eye_width=512, eye_height=512))
            matrix = [0]*16
            matrix[11] = 1
            self.cameras.append(dict(native_frame=index, world_panel=False, matrix=matrix))
        self.presentation = dict(status="pass", gameplay_pairs=[dict(frame=i) for i in range(0, 900, 30)],
                                 ungraded_gameplay_pairs=[])

    def write_capture(self):
        (self.path / "native-run-manifest.json").write_text(json.dumps(self.manifest))
        for name, values in (("frames.jsonl", self.frames), ("native-cameras.jsonl", self.cameras)):
            (self.path / name).write_text("\n".join(json.dumps(value) for value in values))
        (self.path / "native-presentation-proof.json").write_text(json.dumps(self.presentation))

    def test_complete_coverage(self):
        self.write_capture()
        result = analyze(self.path)
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["phases"]["90"]["projections"], 150)
        self.assertEqual(result["phases"]["-90"]["native_containment_calls"], 750)
        self.assertEqual(result["maximum_native_body_rotation_degrees"], 0)

    def pitch_fixture(self):
        self.manifest["head_turn_pitch_degrees"] = -15
        for index, frame in enumerate(self.frames):
            frame["head_turn_pitch_degrees"] = -15
            if not 300 <= index < 750:
                continue
            y, w = frame["views"][0]["orientation"][1::2]
            sp, cp = math.sin(-math.pi / 24), math.cos(-math.pi / 24)
            q = [w * sp, y * cp, -y * sp, w * cp]
            for eye, view in enumerate(frame["views"]):
                view["orientation"] = q
                view["position"] = (rotation(q) @ [.032 if eye else -.032, 0, 0]).tolist()

    def test_recorded_downward_gaze_fixture_preserves_strict_head_turns(self):
        self.pitch_fixture()
        self.write_capture()
        self.assertTrue(analyze(self.path)["visual_accepted"])

    def test_pitch_fixture_does_not_hide_missing_or_wrong_actual_pose(self):
        self.pitch_fixture()
        self.frames[300]["views"][0]["orientation"] = [0, math.sqrt(.5), 0, math.sqrt(.5)]
        self.frames[300]["views"][1]["orientation"] = self.frames[300]["views"][0]["orientation"]
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "outside its recorded fixture"):
            analyze(self.path)

    def test_pitch_fixture_metadata_must_match_every_projection(self):
        self.pitch_fixture()
        self.frames[300]["head_turn_pitch_degrees"] = 0
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "fixture metadata differs"):
            analyze(self.path)

    def test_image_failures_remain_unaccepted(self):
        self.presentation["gameplay_pairs"] = [pair for pair in self.presentation["gameplay_pairs"]
                                               if pair["frame"] != 330]
        self.presentation["ungraded_gameplay_pairs"] = [dict(frame=330, reason="vertical alignment")]
        self.write_capture()
        result = analyze(self.path)
        self.assertFalse(result["visual_accepted"])
        self.assertEqual(result["status"], "head_turn_exercised_visual_acceptance_failed")
        self.assertEqual(result["ungraded_gameplay_pairs"], self.presentation["ungraded_gameplay_pairs"])

    def test_missing_containment_is_rejected(self):
        self.frames[450]["sphere_inside_culls"] = 0
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "containment hook was not exercised"):
            analyze(self.path)

    def test_unsupported_eye_extent_is_rejected(self):
        self.frames[450]["eye_width"] = self.frames[450]["eye_height"] = 513
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "incorrect eye extents"):
            analyze(self.path)

    def test_body_turn_is_rejected(self):
        self.cameras[600]["matrix"][11] = 0
        self.cameras[600]["matrix"][3] = 1
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "body camera turned"):
            analyze(self.path)

    def test_unrotated_ipd_is_rejected(self):
        self.frames[300]["views"][0]["position"] = [-.032, 0, 0]
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "position does not rotate"):
            analyze(self.path)

    def test_duplicate_draw_is_rejected(self):
        self.frames[300]["driver_draws"] *= 2
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "one submission per logical"):
            analyze(self.path)

    def test_missing_image_verdict_is_rejected(self):
        self.presentation["gameplay_pairs"].pop()
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "cover exactly the scheduled"):
            analyze(self.path)

    def test_skipped_capture_is_rejected(self):
        self.frames[330]["captured"] = False
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "Scheduled gameplay image capture"):
            analyze(self.path)

    def test_conflicting_image_verdict_is_rejected(self):
        self.presentation["ungraded_gameplay_pairs"] = [dict(frame=330, reason="vertical alignment")]
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "conflicting verdicts"):
            analyze(self.path)

    def test_duplicate_image_verdict_is_rejected(self):
        self.presentation["gameplay_pairs"].append(self.presentation["gameplay_pairs"][0])
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "Duplicate gameplay image verdict"):
            analyze(self.path)

    def test_missing_reticle_phase_is_rejected(self):
        for frame in self.frames[300:450]:
            frame["crosshair_updates"] = 0
        for frame in self.frames[:150]:
            frame["crosshair_updates"] = 2
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "reticle path.*90-degree"):
            analyze(self.path)


if __name__ == "__main__":
    unittest.main()
