"""Grade actual retail document collection, not input delivery or frame completion."""
import argparse
import bisect
import copy
import hashlib
import json
import math
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def rotate(q, vector):
    require(len(q) == 4 and all(math.isfinite(x) for x in q) and
            abs(sum(x*x for x in q)-1) < .0001, "Invalid recorded controller quaternion")
    x, y, z, w = q
    vx, vy, vz = vector
    tx, ty, tz = 2*(y*vz-z*vy), 2*(z*vx-x*vz), 2*(x*vy-y*vx)
    return [vx+w*tx+y*tz-z*ty, vy+w*ty+z*tx-x*tz, vz+w*tz+x*ty-y*tx]


def grade(data):
    observer, uses, fixtures, pads, frames, bodies = (
        data[name] for name in ("observer", "uses", "fixtures", "pads", "frames", "bodies"))
    headers = [r for r in observer if r["kind"] == "observer"]
    require(len(headers) == 1 and not headers[0]["writes_native_state"] and
            headers[0]["original_calls_per_hook"] == 1, "Missing passive native observer identity")
    base = headers[0]["module_base"]
    states = [r for r in observer if r["kind"] == "state"]
    require(states and all(r["steam_disabled"] for r in states), "Steam was not disabled throughout")
    collections = [r for r in observer if r["kind"] == "collect"]
    require(len(collections) == 1, "Expected exactly one actual native document collection")
    collected = collections[0]
    hero, item, tag = (collected[k] for k in ("hero", "item", "tag"))
    require(hero and item and tag, "Missing native document/profile identity")
    require(not collected["had_tag_before"] and not collected["unread_had_tag_before"] and
            collected["has_tag_after"] and collected["unread_has_tag_after"],
            "Document tag was not genuinely added to collected and unread native arrays")
    require(collected["collected_after"] == collected["collected_before"]+1 and
            collected["unread_after"] == collected["unread_before"]+1,
            "Native inventory changed by something other than one document")
    require(not collected["flags_before"] & 1 and
            collected["flags_after"] == collected["flags_before"] | 1 and collected["hud_tag"] == tag,
            "Native collected flag or HUD tag did not commit")
    require(uses and all(r["view_restored"] for r in uses), "Native use scope leaked its view pose")
    require(all(r["thread"] == r["input_thread"] and r["input_thread"] for r in uses if r["controller_aim"]),
            "Controller targeting escaped the observed native input thread")
    starts = [r for r in uses if r["kind"] == "pickup" and r["action"] and r["result"]]
    require(len(starts) == 1, "Expected one successful native pickup start")
    start = starts[0]
    require(start["hero"] == hero and start["selected_pickup"] == item and
            start["controller_aim"] and start["special"] == 49,
            "Actual document was not selected through scoped controller aim")
    candidates = [r for r in start["pickup_candidates"] if r["actor"] == item]
    require(len(candidates) == 1 and candidates[0]["vtable"]-base == 0x1AAE460,
            "Selected actor does not match the recovered retail document vtable")
    require(start["native_horizontal_range"] == [15, 100], "Native authored pickup range changed")
    distance = math.dist(start["body_origin"][:2], candidates[0]["origin"][:2])
    require(15 <= distance <= 100, "Pickup bypassed native body range")
    to_item = [a-b for a, b in zip(candidates[0]["origin"], start["origin"])]
    ray_dot = sum(a*b for a, b in zip(to_item, start["direction"]))/math.sqrt(sum(x*x for x in to_item))
    require(ray_dot > .98, "Controller ray did not point at the native document")
    notify = [r for r in observer if r["kind"] == "notify" and r["hero"] == hero and
              r["selected_before"] == item and r["selected_after"] == item and
              r["special_before"] == 49 and r["special_after"] == 49]
    require(notify and max(r["world"] for r in notify) < collected["world"],
            "Missing native pickup animation notify before collection")
    relevant = [r for r in states if r["hero"] == hero and r["item"] == item]
    require(any(r["basic"] == 2 and r["special"] == 49 and r["selected"] == item for r in relevant),
            "Actual native pickup animation never acquired the document")
    require(relevant[-1]["world"] >= 899 and relevant[-1]["basic"] == 0 and
            relevant[-1]["special"] == 0 and relevant[-1]["selected"] == 0 and relevant[-1]["flags"] & 1,
            "Document animation did not finish and return to ordinary native locomotion")
    require(max(math.dist(a["root"], b["root"]) for a, b in zip(relevant, relevant[1:])) < 60,
            "Document approach contains an unexplained native actor discontinuity")
    pad_times = [r["clock_us"] for r in pads]
    index = bisect.bisect_right(pad_times, start["clock_us"])-1
    require(index >= 0, "No native input precedes the pickup")
    pad = pads[index]
    require(0 <= start["clock_us"]-pad["clock_us"] <= 250000 and pad["buttons"] & 16384 and
            pad["focused"] and pad["right_aim_tracked"] and pad["use_ready"],
            "Pickup was not driven by fresh focused controller input")
    frame_map = {r["frame"]: r for r in frames}
    fixture_map = {r["world"]: r for r in fixtures}

    def world_for_input(record):
        require(record["xr_time"] % 11111111 == 0, "Unexpected diagnostic XR clock")
        frame = frame_map.get(record["xr_time"]//11111111)
        return frame["gameplay_frame"] if frame else None

    world = world_for_input(pad)
    require(world in fixture_map and fixture_map[world]["trigger"] == 1,
            "Native pickup does not correspond to the recorded controller trigger")
    q = fixture_map[world]["aim"]
    local_direction = rotate(q, [0, 0, -1])
    sources = [r for r in bodies if r["hero"] == hero and r.get("targets") and
               0 <= start["clock_us"]-r["clock_us"] <= 250000]
    errors = []
    for source in sources:
        target = source["targets"]
        basis = [target[k] for k in ("right", "up", "backward")]
        position = target["hands"][1]["controller_position"]
        origin = [target["origin"][j]+target["units_per_meter"] *
                  sum(basis[i][j]*position[i] for i in range(3)) +
                  start["body_origin"][j]-source["root"][j] for j in range(3)]
        direction = [sum(basis[i][j]*local_direction[i] for i in range(3)) for j in range(3)]
        errors.append((math.dist(origin, start["origin"]), math.dist(direction, start["direction"])))
    require(any(position < .02 and direction < .0001 for position, direction in errors),
            "Native use ray disagrees with independent recorded controller/tracking reconstruction")
    initial = next(r for r in relevant if r["basic"] == 2 and r["special"] == 49)
    head_dot = sum(a*b for a, b in zip(initial["native_forward"], start["direction"]))
    require(head_dot < -.8, "Fixture did not distinguish controller-directed pickup from head gaze")
    last_notify = max(r["world"] for r in notify)
    continued = [r for r in pads if (w := world_for_input(r)) is not None and
                 last_notify+100 <= w < last_notify+108 and r["buttons"] & 16384 and
                 r["use_ready"] and r["focused"] and r["right_aim_tracked"]]
    require(continued, "Fresh controller continuation was blocked during native document animation")
    require({r["route_stage"] for r in fixtures} == {0, 1, 2, 3},
            "Controller-only fixture did not traverse the around-counter route")
    return dict(status="pass", hero=hero, item=item, tag=tag, native_document_collections=1,
                native_notify_calls=len(notify), native_horizontal_distance=distance,
                controller_head_direction_dot=head_dot, continuation_input_samples=len(continued),
                native_view_scopes_restored=len(uses), native_profile_arrays_updated=True,
                native_hud_tag_updated=True, steam_disabled=True, returned_to_locomotion=True,
                best_ray_errors=min(errors),
                limitations="Owned headset-free document fixture only; not arbitrary items/maps, "
                "journal reading, physical hand ergonomics or headset FPS.")


def reject_controls(data):
    controls = [
        ("missing-collection", "observer", lambda r: r["kind"] != "collect", None),
        ("duplicate-collection", "observer", None, "duplicate"),
        ("missing-notify", "observer", lambda r: r["kind"] != "notify", None),
        ("leaked-view", "uses", None, ("view_restored", False)),
        ("wrong-thread", "uses", None, ("thread", 0)),
        ("head-gaze-pickup", "uses", None, ("controller_aim", False)),
        ("missing-continuation", "pads", None, ("use_ready", False)),
        ("wrong-ray", "uses", None, ("direction", [1, 0, 0])),
        ("online-steam", "observer", None, ("steam_disabled", False)),
        ("wrong-hud-tag", "observer", None, ("hud_tag", -1)),
        ("missing-profile-tag", "observer", None, ("has_tag_after", False)),
        ("already-collected", "observer", None, ("had_tag_before", True)),
        ("missing-native-flag", "observer", None, ("flags_after", 0)),
        ("expanded-range", "uses", None, ("native_horizontal_range", [0, 10000])),
    ]
    result = []
    for name, key, keep, change in controls:
        corrupted = dict(data)
        corrupted[key] = copy.deepcopy(data[key])
        if keep:
            corrupted[key] = list(filter(keep, corrupted[key]))
        elif change == "duplicate":
            corrupted[key].append(copy.deepcopy(next(r for r in corrupted[key] if r["kind"] == "collect")))
        else:
            field, value = change
            for record in corrupted[key]:
                if field in record:
                    if name == "missing-continuation":
                        frame = next((f for f in data["frames"] if f["frame"] == record["xr_time"]//11111111), None)
                        notified = max(r["world"] for r in data["observer"] if r["kind"] == "notify")
                        if frame is None or not notified+100 <= frame["gameplay_frame"] < notified+108:
                            continue
                    record[field] = value
        try:
            grade(corrupted)
        except ValueError as error:
            result.append(dict(control=name, rejected=str(error)))
        else:
            raise ValueError(f"Collectible grader accepted its {name} corruption control")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--preservation", type=Path, required=True)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    capture = args.capture
    require((capture/"capture-complete.txt").is_file(), "Native capture did not finish")
    identity = json.loads((capture/"native-run-manifest.json").read_text(encoding="utf-8-sig"))
    digest = hashlib.sha256(args.module.read_bytes()).hexdigest().upper()
    require(digest == identity["mod_sha256"] and identity["collectible_fixture_version"] == 1 and
            identity["collectible_path_fixture_version"] == 5, "Wrong native module or collectible fixture")
    data = {key: rows(capture/name) for key, name in (
        ("observer", "native-collectible-observer.jsonl"), ("uses", "native-use-events.jsonl"),
        ("fixtures", "native-collectible-fixture.jsonl"), ("pads", "native-input-events.jsonl"),
        ("frames", "frames.jsonl"), ("bodies", "native-skeleton.jsonl"))}
    data["pads"] = [r for r in data["pads"] if r["kind"] == "motion_pad"]
    data["frames"] = [r for r in data["frames"] if not r.get("canceled")]
    report = grade(data)
    if args.selftest:
        report["rejected_controls"] = reject_controls(data)
    preserved = json.loads((args.preservation/"manifest.json").read_text(encoding="utf-8-sig"))
    for record in preserved:
        for path in (record["Path"], record["Backup"]):
            require(hashlib.sha256(Path(path).read_bytes()).hexdigest().upper() == record["Hash"],
                    "Original save/config or preservation copy changed")
    profiles = [r for r in preserved if Path(r["Path"]).name == "profile.bin"]
    require(len(profiles) == 1 and profiles[0]["Hash"] == identity["offline_profile_sha256"] and
            identity["offline_save_copies"] > 0, "Offline fixture did not use the preserved current save")
    cloud = next(Path(r["Path"]).parent for r in preserved if Path(r["Path"]).name == "CloudStorage.ini")
    offline = cloud.parent/"SaveData"
    original_offline = {Path(r["Path"]) for r in preserved if Path(r["Path"]).is_relative_to(offline)}
    require({p for p in offline.rglob("*") if p.is_file()} == original_offline,
            "Disposable offline save/profile files were not removed")
    report.update(module_sha256=digest, original_files_byte_verified=len(preserved),
                  offline_clones_removed=True,
                  grader_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (capture/"native-collectible-proof.json").write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
