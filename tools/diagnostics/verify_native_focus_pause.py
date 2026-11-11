"""Grade timed native focus-pause execution, not all scripted-animation freezes."""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from verify_native_probe import require


def rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines() if line]


def load(directory):
    require((directory / "capture-complete.txt").is_file(), "Native focus capture is incomplete")
    frames = rows(directory / "frames.jsonl")
    projected = [r for r in frames if not r.get("canceled")]
    canceled = [r for r in frames if r.get("canceled")]
    require(projected and all(r["frame"] < projected[0]["frame"] for r in canceled),
            "Native focus fixture canceled a projected gameplay frame")
    return {
        "manifest": json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig")),
        "events": rows(directory / "native-focus-events.jsonl"),
        "focus": [r for r in rows(directory / "native-input-events.jsonl")
                  if r["kind"] == "focus_pause_policy"],
        "camera": rows(directory / "camera-updates.jsonl"),
        "frames": projected,
        "skeleton": [{"clock_us": r["clock_us"], "hands":
                      [h["tracked"] for h in r["targets"]["hands"]] if r.get("targets") else None}
                     for r in rows(directory / "native-skeleton.jsonl")],
    }


def grade(tracked, control, module, probe):
    digest = hashlib.sha256(module.read_bytes()).hexdigest().upper()
    probe_digest = hashlib.sha256(probe.read_bytes()).hexdigest().upper()
    reports = {}
    for label, data in (("Tracked", tracked), ("Control", control)):
        manifest, events = data["manifest"], data["events"]
        require(manifest["mod_sha256"] == digest and manifest["probe_sha256"] == probe_digest and
                manifest["body"] and manifest["scenario"] == "Continue" and
                manifest["focus_policy"] == label and manifest["focus_fixture_version"] == 3,
                "Focus capture does not match its exact module, probe and explicit policy")
        require([e["phase"] for e in events] == [1, 2, 3, 4],
                "Native focus fixture lacks its ordered timed phases")
        require([e["xr_focused"] for e in events] == [True, False, True, True] and
                [e["desktop_active"] for e in events] == [False, False, False, True] and
                [e["posted_activation"] for e in events] == [True, False, False, True] and
                all(e["activation_message"] == 6 for e in events),
                "Fixture did not exercise the REA-proven native activation path")
        durations = [(b["clock_us"] - a["clock_us"]) / 1e6
                     for a, b in zip(events, events[1:])]
        require(durations[0] >= 24 and all(d >= 1.5 for d in durations[1:]),
                "Native background or XR transition dwell was shortened")
        require(all(abs((b["tick_ms"] - a["tick_ms"]) / 1000 - d) < .05
                    for a, b, d in zip(events, events[1:], durations)),
                "Independent native monotonic timers disagree")
        require(all(a["clock_us"] <= b["clock_us"] for a, b in zip(data["focus"], data["focus"][1:])),
                "Native focus requests are out of execution order")
        require(data["frames"][-1]["gameplay_frame"] >= 899 and
                data["frames"][-1]["focus_fixture_phase"] == 4 and
                sum(f["focus_fixture_phase"] == 1 for f in data["frames"]) >= 60 and
                all(f["focus_fixture_version"] == 3 and not f["consumed_replays"]
                    for f in data["frames"]),
                "Actual native stereo rendering did not survive the complete focus fixture")
        t1, t2, t3, t4 = [e["clock_us"] for e in events]
        changes = [r for r in data["focus"] if t1 <= r["clock_us"] <= t4 + 1000000]
        native = [r for r in changes if r["source"] == "native_request"]
        require(native and all(r["native_pause_enabled"] for r in native) and
                len({r["engine"] for r in changes}) == 1,
                "Actual focus requests lack one pause-enabled native engine")
        loss = [r for r in native if r["requested"] and r["clock_us"] < t2]
        gain = [r for r in native if not r["requested"] and r["clock_us"] >= t4]
        require(loss and gain and all(0 <= r["clock_us"] - t1 < 1000000 for r in loss) and
                all(r["called"] and not r["applied"] for r in gain),
                "Owned activation messages did not reach native engine loss and regain requests")
        xr = [r for r in changes if r["source"] == "xr_focus"]
        if label == "Tracked":
            require(all(r["xr_focused"] and not r["applied"] and not r["called"] for r in loss),
                    "Desktop loss paused native gameplay or issued an unsolicited unpause")
            require(len(xr) == 2 and [r["applied"] for r in xr] == [1, 0] and
                    [r["xr_focused"] for r in xr] == [False, True] and
                    all(r["called"] and r["requested"] for r in xr) and
                    0 <= xr[0]["clock_us"] - t2 < 1000000 and
                    0 <= xr[1]["clock_us"] - t3 < 1000000,
                    "XR loss/regain did not apply exactly the retained native background request")
        else:
            require(not xr and all(r["called"] and r["applied"] == r["requested"] for r in loss),
                    "Counterexample did not forward the original native pause request unchanged")

        camera = data["camera"]
        require(camera and all(r["elapsed"] >= 0 for r in camera),
                "Invalid native camera timing")
        before = [r for r in camera if t1 - 1000000 < r["clock_us"] < t1 - 100000 and r["elapsed"] > 0]
        require(before, "Native scene was not advancing immediately before deactivation")
        hero_camera = max({r["camera"] for r in before}, key=lambda c: sum(r["camera"] == c for r in before))
        camera = [r for r in camera if r["camera"] == hero_camera]
        background = [r for r in camera if t1 + 500000 < r["clock_us"] < t2 - 250000]
        resumed = [r for r in camera if t3 + 500000 < r["clock_us"] < t4 - 100000]
        after = [r for r in camera if r["clock_us"] > t4 + 100000 and r["elapsed"] > 0]
        require(after, "Native scene did not recover after the original activation request")
        positive = [r for r in background if r["elapsed"] > 1e-6]
        skeleton = [r for r in data["skeleton"] if t1 + 500000 < r["clock_us"] < t2 - 250000]
        if label == "Tracked":
            require(len(positive) >= 60 and sum(r["elapsed"] for r in positive) >= .25 * durations[0],
                    "Native scene froze during the 24-second XR-focused background interval")
            times = [t1 + 500000] + [r["clock_us"] for r in positive] + [t2 - 250000]
            require(max(b - a for a, b in zip(times, times[1:])) < 1000000,
                    "Native scene had a one-second timing gap while XR remained focused")
            require(sum(r["elapsed"] for r in resumed) > .2 and
                    len([r for r in resumed if r["elapsed"] > 0]) >= 5,
                    "Native gameplay did not resume on XR focus without desktop reactivation")
            paused = [r for r in camera if t2 + 500000 < r["clock_us"] < t3 - 100000]
            require(not any(r["elapsed"] > 1e-6 for r in paused),
                    "Native background pause was not restored when XR focus was lost")
            require(len(skeleton) >= 60 and all(r["hands"] and r["hands"][1] for r in skeleton),
                    "Actual native skeletal evaluation or right-controller tracking stopped in the background")
        else:
            require(not positive and not any(r["elapsed"] > 1e-6 for r in resumed),
                    "Native counterexample did not reproduce a background pause")
            require(not skeleton, "Counterexample did not stop actual native skeletal evaluation")
        reports[label.lower()] = {
            "background_seconds": durations[0], "native_background_updates": len(background),
            "positive_native_background_updates": len(positive),
            "native_background_elapsed": sum(r["elapsed"] for r in background),
            "native_focus_requests": len(native), "xr_policy_transitions": len(xr),
            "native_background_skeleton_evaluations": len(skeleton),
            "tracked_right_hand_evaluations": sum(bool(r["hands"] and r["hands"][1]) for r in skeleton),
        }
    return {"native_background_focus_acceptance": True, "full_animation_freeze_acceptance": False,
            "blocked_corner_recovery_acceptance": False,
            "module_sha256": digest, "probe_sha256": probe_digest, **reports}


def selftest(tracked, control, module, probe):
    controls = ("module", "probe", "phase", "short-dwell", "wrong-message", "pause-disabled",
                "unsolicited-unpause", "wrong-application", "missing-xr", "wrong-owner",
                "background-freeze", "control-not-paused", "resume-freeze", "repeated-unpause",
                "skeleton-freeze", "right-untracked")
    for fault in controls:
        a, b = copy.deepcopy(tracked), copy.deepcopy(control)
        t1, t2, t3, t4 = [e["clock_us"] for e in a["events"]]
        loss = next(r for r in a["focus"] if r["source"] == "native_request" and
                    r["requested"] and t1 <= r["clock_us"] < t2)
        xr = [r for r in a["focus"] if r["source"] == "xr_focus"]
        if fault == "module":
            a["manifest"]["mod_sha256"] = "0" * 64
        elif fault == "probe":
            a["manifest"]["probe_sha256"] = "0" * 64
        elif fault == "phase":
            a["events"].pop()
        elif fault == "short-dwell":
            a["events"][1]["clock_us"] = t1 + 23000000
        elif fault == "wrong-message":
            a["events"][0]["activation_message"] = 0x1c
        elif fault == "pause-disabled":
            loss["native_pause_enabled"] = False
        elif fault == "unsolicited-unpause":
            loss["called"] = True
        elif fault == "wrong-application":
            loss["applied"] = 1
        elif fault == "missing-xr":
            a["focus"].remove(xr[0])
        elif fault == "wrong-owner":
            xr[0]["engine"] ^= 1
        elif fault == "background-freeze":
            for r in a["camera"]:
                if t1 < r["clock_us"] < t2:
                    r["elapsed"] = 0
        elif fault == "control-not-paused":
            start = b["events"][0]["clock_us"]
            b["camera"].append({"camera": b["camera"][-1]["camera"],
                                "clock_us": start + 1000000, "elapsed": .02})
        elif fault == "resume-freeze":
            for r in a["camera"]:
                if t3 < r["clock_us"] < t4:
                    r["elapsed"] = 0
        elif fault == "repeated-unpause":
            a["focus"].append(copy.deepcopy(xr[1]))
            a["focus"].sort(key=lambda r: r["clock_us"])
        elif fault == "skeleton-freeze":
            a["skeleton"] = [r for r in a["skeleton"] if not t1 < r["clock_us"] < t2]
        elif fault == "right-untracked":
            next(r for r in a["skeleton"] if t1 + 500000 < r["clock_us"] < t2 - 250000)["hands"][1] = False
        try:
            grade(a, b, module, probe)
        except ValueError:
            continue
        raise ValueError(f"Focus oracle accepted corruption control: {fault}")
    return len(controls)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--control", required=True, type=Path)
    parser.add_argument("--module", required=True, type=Path)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    tracked, control = load(args.evidence), load(args.control)
    result = grade(tracked, control, args.module, args.probe)
    if args.selftest:
        result["rejected_controls"] = selftest(tracked, control, args.module, args.probe)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
