"""Verify real native corner sweeps, controller reacquisition and three GPU pose witnesses."""
import argparse
import bisect
import copy
import hashlib
import json
from pathlib import Path
import struct

import numpy as np

from verify_native_body import load_rows, match_gpu_pose, matrix, require
from verify_native_probe import rotation


def load(directory):
    require((directory / "capture-complete.txt").is_file(), "Native corner capture is incomplete")
    gpu = {}
    for name in ("draw-probe", "contact-draw-probe", "recovery-draw-probe"):
        gpu[name] = [
            {"draw": r["draw"], "native_frame": r["native_frame"],
             "vertex_constants": [c for c in r["vertex_constants"] if c["name"] == "BoneMatrices"]}
            for r in load_rows(directory / name / "draws.jsonl")
            if "draw" in r and any(c["name"] == "BoneMatrices" for c in r["vertex_constants"])]
    return {
        "manifest": json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig")),
        "recovery": load_rows(directory / "native-hand-recovery.jsonl"),
        "queries": load_rows(directory / "native-hand-contacts.jsonl"),
        "skeletons": load_rows(directory / "native-skeleton.jsonl"),
        "cameras": load_rows(directory / "native-cameras.jsonl"),
        "poses": {r["serial"]: r for r in load_rows(directory / "camera-poses.jsonl")},
        "frames": [r for r in load_rows(directory / "frames.jsonl") if not r.get("canceled")],
        "gpu": gpu,
    }


def close(a, b, tolerance=.005):
    return np.linalg.norm(np.array(a) - b) < tolerance


def grade(data, module, probe):
    manifest = data["manifest"]
    require(manifest["mod_sha256"] == hashlib.sha256(module.read_bytes()).hexdigest().upper() and
            manifest["probe_sha256"] == hashlib.sha256(probe.read_bytes()).hexdigest().upper() and
            manifest["body"] and manifest["floor"] and manifest["contacts"] and
            manifest["corner_recovery"] and manifest["draw_contact"] and
            manifest["contact_fixture_version"] == 6 and manifest["body_schedule_version"] == 4,
            "Native recovery does not match its module, probe and explicit versioned corner fixture")
    require(data["frames"][-1]["gameplay_frame"] >= 899 and
            all(r["contact_fixture_version"] == 6 and r["body_schedule_version"] == 4 and
                not r["consumed_replays"] for r in data["frames"]),
            "Native corner fixture did not finish its original single-pass path")
    queries = {}
    for ray in data["queries"]:
        require(ray["thread"] == ray["input_thread"] and close(ray["extent"], [8, 8, 8]),
                "Recovery query lost actual native input-thread/volume ownership")
        queries.setdefault((ray["clock_us"], ray["hand"]), []).append(ray)
    previous, occlusions, normals = {}, [], []
    releases = []
    rows = data["recovery"]
    require(len(rows) >= 1600 and len({r["hero"] for r in rows}) == 1,
            "Insufficient coherent native hero recovery observations")
    for row in rows:
        hand, clock = row["hand"], row["clock_us"]
        anchor, desired = np.array(row["anchor"]), np.array(row["desired"])
        require(hand in (0, 1) and np.isfinite([anchor, desired, row["position"]]).all() and
                close(anchor[:2], row["root"][:2]), "Invalid native corner request")
        old = previous.get(hand)
        if old and old["generation"] == row["generation"] and clock - old["clock_us"] < 250000:
            require(close(row["previous_position"], old["position"]) and
                    close(row["previous_anchor"], old["anchor"]) and
                    row["previous_blocked"] == old["blocked"],
                    "Recovery did not retain its actual immediately preceding hand/contact")
        rays = list(queries.pop((clock, hand), []))
        expected_reset, revalidated = 0, False
        valid = row["previous_valid"]
        old_position = np.array(row["previous_position"])
        if valid and row["previous_blocked"]:
            tether, reach = old_position - anchor, desired - anchor
            if tether @ tether > max(reach @ reach, 10000):
                expected_reset, valid = 1, False
            elif np.linalg.norm(anchor - row["previous_anchor"]) > .1:
                require(rays, "Native old-contact validation sweep is missing")
                validation = rays.pop(0)
                require(close(validation["start"], anchor) and close(validation["end"], old_position),
                        "Native revalidation did not test current body anchor to the old hand")
                revalidated = True
                if not validation["clear"]:
                    valid, expected_reset = False, 2
                    require(np.isfinite(validation["normal"]).all() and
                            abs(np.linalg.norm(validation["normal"]) - 1) < .01,
                            "Native occluding corner has an invalid surface normal")
                    occlusions.append(row)
                    normals.append(np.array(validation["normal"]))
        require(row["reset_reason"] == expected_reset and row["revalidated"] == revalidated,
                "Published recovery reason disagrees with independently reconstructed native sweeps")
        start, blocked = old_position.copy() if valid else anchor.copy(), False
        for _ in range(3):
            delta = desired - start
            length = np.linalg.norm(delta)
            if length < .1:
                break
            require(rays, "Native current-controller sweep is missing")
            ray = rays.pop(0)
            require(ray["thread"] == row["thread"] and close(ray["start"], start) and
                    close(ray["end"], desired), "Recovery sweep bypassed its current safe start/controller")
            if ray["clear"]:
                start = desired
                break
            normal = np.array(ray["normal"])
            require(np.isfinite(normal).all() and abs(normal @ normal - 1) < .01 and
                    0 <= ray["time"] <= 1, "Invalid real native blocking contact")
            blocked = True
            start = start + delta * max(0, ray["time"] - .1 / length)
            remaining = desired - start
            desired = desired - normal * min(0, remaining @ normal)
        require(not rays and row["blocked"] == blocked and close(row["position"], start),
                "Actual hand did not match the native safe sweep/slide result")
        if old and old["blocked"] and not blocked:
            require(close(row["position"], row["desired"]), "Released hand retained its obsolete tether")
            releases.append(row)
        previous[hand] = row
    require(not queries, "Recovery records omitted actual native world queries")
    require({r["hand"] for r in occlusions} == {0, 1} and
            any(np.linalg.norm(np.cross(a, b)) > .7 for a in normals for b in normals),
            "Native path did not revalidate both hands around distinct actual corner faces")
    latencies = []
    for row in occlusions:
        release = next((r for r in releases if r["hand"] == row["hand"] and
                        r["clock_us"] > row["clock_us"] and r["generation"] == row["generation"]), None)
        require(release is not None and release["clock_us"] - row["clock_us"] < 1000000,
                "Hand did not reacquire its unobstructed controller after the real corner")
        latencies.append((release["clock_us"] - row["clock_us"]) / 1e6)
    root = np.array([r["root"] for r in rows])
    require(np.ptp(root[:, 0]) > 100 and np.ptp(root[:, 1]) > 300,
            "Actual native pawn did not traverse the two-axis corner path")

    skeletons = data["skeletons"]
    first = next(r for r in skeletons if r["reference_records"])
    records = np.array([struct.unpack("<20f", struct.pack("<20I", *r))
                        for r in first["reference_records"]])
    reference = []
    for i, record in enumerate(records):
        local = matrix(record[4:11])
        reference.append(reference[first["parents"][i]] @ local if i else local)
    inverse_reference = np.linalg.inv(reference)
    centers = [records[f, 8:11].mean(axis=0) * .5 for f in ([16, 19, 22, 25], [40, 43, 46, 49])]
    clocks = [r["clock_us"] for r in skeletons]
    for row in occlusions:
        index = bisect.bisect_left(clocks, row["clock_us"])
        nearby = skeletons[max(0, index - 1):index + 2]
        require(any(r["hero"] == row["hero"] and abs(r["clock_us"] - row["clock_us"]) < 50000 and
                    close(r["root"], row["root"], .02) for r in nearby),
                "Native corner diagnostics do not match the actual evaluated pawn root")
    errors, followed = [], [[], []]
    for row in skeletons:
        targets = row["targets"]
        if not targets:
            continue
        basis = np.column_stack((targets["right"], targets["up"], targets["backward"]))
        for side, hand in enumerate(targets["hands"]):
            if not hand["tracked"] or hand["world_contact"]:
                continue
            wrist = next(b for b in row["bindings"] if b["index"] == (12 if side == 0 else 36))
            require(wrist["name"] == ("Hero-L-Hand" if side == 0 else "Hero-R-Hand") and
                    wrist["matrix_error"] == 0, "Recovery lost actual native wrist-matrix ownership")
            palm = (np.append(centers[side], 1) @ np.array(wrist["native_matrix"]).reshape(4, 4))[:3]
            raw = np.array(hand["controller_position"]) + rotation(hand["controller_orientation"]) @ [
                0, targets["hand_wristward_m"], 0]
            require(close(raw, hand["raw_position"], 1e-6), "Released hand changed its actual controller goal")
            goal = np.array(targets["origin"]) + basis @ raw * targets["units_per_meter"]
            errors.append(float(np.linalg.norm(palm - goal)))
            followed[side].append(row["clock_us"])
    require(len(errors) >= 1000 and max(errors) < .003,
            "Actual native palms did not follow their independently reconstructed live controller goals")
    for row in occlusions:
        release = next(r for r in releases if r["hand"] == row["hand"] and r["clock_us"] > row["clock_us"])
        require(any(0 <= clock - release["clock_us"] < 50000 for clock in followed[row["hand"]]),
                "Actual rendered native hand did not promptly follow its released controller")

    required = {3, 12, 36, 63, 69, 29, 30, 31, 32, 53, 54, 55, 56, 57, 58, 59, 60, 65, 66, 71, 72}
    arms = {12, 36, 29, 30, 31, 32, 53, 54, 55, 56}
    witnesses = {}
    for name in ("draw-probe", "contact-draw-probe", "recovery-draw-probe"):
        pose, draws, coverage, changed, _, source_clock = match_gpu_pose(
            skeletons, data["cameras"], data["poses"], inverse_reference, data["gpu"][name],
            required, required if name == "draw-probe" else arms, contact=name != "draw-probe")
        require(all(h["tracked"] for h in pose["targets"]["hands"]),
                "Native GPU witness did not contain both actually tracked hands")
        if name == "draw-probe":
            require(pose["targets"]["physical"] and pose["pawn_crouched"],
                    "Primary GPU witness omitted its settled physical crouch")
        constrained = sum(h["world_contact"] for h in pose["targets"]["hands"])
        if name == "contact-draw-probe":
            require(constrained, "Contact GPU witness is not a real constrained hand pose")
        elif name == "recovery-draw-probe":
            require(not constrained and all(any(0 <= source_clock - r["clock_us"] < 500000 and
                        r["hand"] == hand for r in releases) for hand in (0, 1)),
                    "Recovered GPU witness is stale, constrained or precedes actual hand release")
        witnesses[name] = {"native_frame": data["gpu"][name][0]["native_frame"],
                           "native_pose_serial": pose["serial"], "matched_draws": draws,
                           "matched_bones": sorted(coverage), "changed_bones": sorted(changed),
                           "world_constrained_hands": constrained}
    require(len({r["native_frame"] for r in witnesses.values()}) == 3,
            "Crouch, contact and recovered hand witnesses reused a single GPU frame")
    return {"native_corner_recovery_acceptance": True, "physical_headset_acceptance": False,
            "recovery_evaluations": len(rows), "native_world_queries": len(data["queries"]),
            "occluded_old_contacts": len(occlusions), "release_transitions": len(releases),
            "max_release_seconds_after_occlusion": max(latencies),
            "independently_reconstructed_free_palms": len(errors), "max_palm_error_units": max(errors),
            "gpu_witnesses": witnesses}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--module", required=True, type=Path)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    data = load(args.evidence)
    result = grade(data, args.module, args.probe)
    if args.selftest:
        faults = ("module", "probe", "fixture", "old-position", "anchor", "reset-reason", "validation",
                  "normal", "early-release", "retained-tether", "thread", "hero", "recovered-gpu",
                  "constrained-gpu", "native-palm", "controller-goal")
        for fault in faults:
            broken = dict(data)
            broken["recovery"] = copy.deepcopy(data["recovery"])
            occluded = next(r for r in broken["recovery"] if r["reset_reason"] == 2)
            if fault in ("module", "probe", "fixture"):
                broken["manifest"] = dict(data["manifest"])
                key = {"module": "mod_sha256", "probe": "probe_sha256", "fixture": "contact_fixture_version"}[fault]
                broken["manifest"][key] = 0
            elif fault == "old-position":
                occluded["previous_position"][0] += 1
            elif fault == "anchor":
                occluded["anchor"][0] += 1
            elif fault == "reset-reason":
                occluded["reset_reason"] = 0
            elif fault in ("validation", "normal", "thread"):
                broken["queries"] = copy.deepcopy(data["queries"])
                ray = next(r for r in broken["queries"] if r["clock_us"] == occluded["clock_us"] and
                           r["hand"] == occluded["hand"])
                if fault == "validation":
                    ray["clear"] = True
                elif fault == "normal":
                    ray["normal"] = [0, 0, 0]
                else:
                    ray["thread"] ^= 1
            elif fault == "early-release":
                occluded["blocked"] = False
                occluded["position"] = occluded["desired"]
            elif fault == "retained-tether":
                free = next(r for r in broken["recovery"] if r["clock_us"] > occluded["clock_us"] and
                            r["hand"] == occluded["hand"] and not r["blocked"])
                free["position"] = occluded["previous_position"]
            elif fault == "hero":
                occluded["hero"] ^= 1
            elif fault in ("recovered-gpu", "constrained-gpu"):
                broken["gpu"] = copy.deepcopy(data["gpu"])
                if fault == "constrained-gpu":
                    broken["gpu"]["recovery-draw-probe"] = broken["gpu"]["contact-draw-probe"]
                else:
                    for draw in broken["gpu"]["recovery-draw-probe"]:
                        for c in draw["vertex_constants"]:
                            c["values"] = [0] * len(c["values"])
            else:
                broken["skeletons"] = copy.deepcopy(data["skeletons"])
                for row in broken["skeletons"]:
                    if not row["targets"]:
                        continue
                    if fault == "native-palm":
                        next(b for b in row["bindings"] if b["index"] == 36)["native_matrix"][12] += 1
                    else:
                        row["targets"]["hands"][1]["controller_position"][0] += .1
            try:
                grade(broken, args.module, args.probe)
            except ValueError:
                continue
            raise ValueError(f"Native corner oracle accepted corruption: {fault}")
        result["rejected_controls"] = len(faults)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
