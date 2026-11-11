"""Verify scripted head turns, native containment execution and retained image failures."""
import argparse
import json
import math
from pathlib import Path

import numpy as np

from verify_native_probe import draw_grade, require, rotation


def analyze(evidence):
    require((evidence / "capture-complete.txt").is_file(), "Head-turn capture did not complete")
    manifest = json.loads((evidence / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    require(manifest["head_turn"] and manifest["scenario"] == "Continue",
            "Capture manifest does not select native Continue head turns")
    pitch_degrees = manifest.get("head_turn_pitch_degrees", 0)
    require(type(pitch_degrees) is int and pitch_degrees in (0, -15),
            "Unsupported native head-turn pitch fixture")
    frames = [json.loads(line) for line in (evidence / "frames.jsonl").read_text().splitlines()]
    world = [frame for frame in frames if not frame.get("canceled") and not frame["world_panel"]]
    require(len(world) >= 900, "Head-turn capture has fewer than 900 gameplay projections")
    cameras = {camera["native_frame"]: camera for camera in
               map(json.loads, (evidence / "native-cameras.jsonl").read_text().splitlines())}
    presentation = json.loads((evidence / "native-presentation-proof.json").read_text())
    require(presentation["status"] == "pass", "Missing independently graded native presentation")
    groups = {angle: [] for angle in (0, 90, -90, 180)}
    for frame in world:
        require(frame["head_turn_probe"], "A gameplay frame did not use the head-turn runtime")
        require(frame.get("head_turn_pitch_degrees", 0) == pitch_degrees,
                "Head-turn pitch fixture metadata differs")
        serial = frame["capture_serial"]
        require(frame["captured"] == (serial <= 5 or serial % 30 == 0),
                "Scheduled gameplay image capture was skipped or mislabeled")
        quaternions = [np.array(view["orientation"]) for view in frame["views"]]
        for quaternion in quaternions:
            rotation(quaternion)
        require(abs(float(quaternions[0] @ quaternions[1])) > .99999,
                "Scripted eye orientations disagree")
        for eye, view in enumerate(frame["views"]):
            expected = rotation(quaternions[eye]) @ np.array([.032 if eye else -.032, 0, 0])
            require(np.max(np.abs(np.array(view["position"]) - expected)) < 1e-5,
                    "Scripted eye position does not rotate with the headset")
        x, y, z, w = quaternions[0]
        yaw = math.degrees(2 * math.atan2(y, w))
        angle = round(yaw / 90) * 90 if abs(yaw) > 80 else 0
        require(angle in groups and (abs(yaw - angle) < .01 if angle else abs(yaw) < 5),
                "Scripted headset yaw differs from its selected head-turn phase")
        half_yaw = math.radians(yaw) / 2
        half_pitch = math.radians(pitch_degrees if angle else 0) / 2
        cy, sy, cp, sp = math.cos(half_yaw), math.sin(half_yaw), math.cos(half_pitch), math.sin(half_pitch)
        expected_orientation = np.array([cy * sp, sy * cp, -sy * sp, cy * cp])
        require(np.max(np.abs(quaternions[0] - expected_orientation)) < 1e-6,
                "Head-turn probe unexpectedly pitched or rolled outside its recorded fixture")
        require(frame["sphere_inside_culls"] > 0, "Recovered native containment hook was not exercised")
        draw_grade(frame)
        groups[angle].append(frame)
    require(all(len(groups[angle]) == 150 for angle in (90, -90, 180)),
            "Head-turn capture lacks 150 real projections at each large yaw")
    turns = groups[90] + groups[-90] + groups[180]
    forwards = []
    for frame in turns:
        camera = cameras[frame["native_frame"]]
        require(not camera["world_panel"], "Head-turn camera evidence belongs to a menu")
        matrix = np.array(camera["matrix"]).reshape(4, 4).T
        require(np.isfinite(matrix).all(), "Native body camera is nonfinite")
        forward = matrix[3, :3]
        require(np.linalg.norm(forward) > 1e-6, "Native body camera is degenerate")
        forwards.append(forward / np.linalg.norm(forward))
    maximum_body_turn = max(math.degrees(math.acos(float(np.clip(forwards[0] @ f, -1, 1))))
                            for f in forwards)
    require(maximum_body_turn < 5, "Native body camera turned with the scripted headset")
    require(sum(frame["crosshair_updates"] for frame in world) >= len(world),
            "Native reticle suppression/update path was not exercised throughout gameplay")
    graded_frames = [pair["frame"] for pair in presentation["gameplay_pairs"]]
    graded = set(graded_frames)
    ungraded = presentation["ungraded_gameplay_pairs"]
    ungraded_frames = [pair["frame"] for pair in ungraded]
    rejected = set(ungraded_frames)
    require(len(graded) == len(graded_frames) and len(rejected) == len(ungraded_frames),
            "Duplicate gameplay image verdict")
    require(not graded.intersection(rejected), "Gameplay image has conflicting verdicts")
    sampled = {frame["frame"] for frame in world if frame["captured"]}
    require(graded.union(rejected) == sampled,
            "Presentation verdicts do not cover exactly the scheduled gameplay images")
    coverage = {}
    for angle, group in groups.items():
        reticle_updates = sum(frame["crosshair_updates"] for frame in group)
        require(reticle_updates >= len(group),
                f"Native reticle path was not exercised throughout the {angle}-degree phase")
        coverage[str(angle)] = dict(
            projections=len(group),
            executed_paired_draws=sum(draw_grade(frame) for frame in group),
            native_containment_calls=sum(frame["sphere_inside_culls"] for frame in group),
            native_crosshair_updates=reticle_updates,
            graded_sample_pairs=sum(frame["frame"] in graded for frame in group))
    visual_accepted = not ungraded and all(coverage[str(angle)]["graded_sample_pairs"]
                                          for angle in (90, -90, 180))
    return dict(
        status="pass" if visual_accepted else "head_turn_exercised_visual_acceptance_failed",
        binary_identity=manifest, gameplay_projections=len(world),
        head_turn_pitch_degrees=pitch_degrees,
        maximum_native_body_rotation_degrees=maximum_body_turn, phases=coverage,
        ungraded_gameplay_pairs=ungraded, visual_accepted=bool(visual_accepted),
        deployed=False, head_bob_fixed=False, comfortable_play_certified=False,
        limitations="Exercises a recovered visibility gate with a stationary native body camera, not every "
                    "visibility path or effect. Image failures remain unaccepted. Native reticle-hook execution "
                    "and its controlled alpha/state regression do not independently isolate reticle pixels. "
                    "No physical headset output or cadence acceptance.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    report = analyze(args.evidence)
    (args.evidence / "headturn-proof.json").write_text(json.dumps(report, indent=2) + "\n")
    require(report["visual_accepted"], "Native head turns completed but sampled stereo image coverage failed; "
            "see headturn-proof.json; do not deploy as visually accepted")
    print("PASS: native ±90/180-degree head turns, unchanged body camera, recovered containment hook, "
          "single paired draws and all sampled stereo pairs")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError) as error:
        raise SystemExit(f"FAIL: {error}") from error
