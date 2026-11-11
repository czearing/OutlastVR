"""Verify stock material-effect visibility without changing authored parameters.

This establishes native lobby proxy selection, not arbitrary authored glitch
events, temporal window stability, full HUD layout or physical-headset acceptance.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct

from verify_native_camcorder import rows
from verify_native_probe import require


def proxy_counts(observed, fault=None):
    require(observed and observed[-1]["kind"] == "complete" and observed[-1]["samples"] >= 100,
            "Independent native observer did not complete")
    counts = {"normal": 0, "night_vision": 0}
    incomplete = 0
    for record in observed:
        capture = record.get("native_capture", {})
        if not capture.get("owner") or not capture.get("proxy_count"):
            continue
        mode = ("normal" if capture["chain"] == capture["normal_camera_chain"] else
                "night_vision" if capture["chain"] == capture["night_vision_chain"] else None)
        require(mode is not None, "Private capture no longer uses the actual native camera chain")
        expected_members = 4 if mode == "normal" else 3
        require(len(capture["effects"]) == expected_members and fault != "removed-source-effect",
                "Source chain was edited instead of respecting native effect visibility")
        # The passive observer does not suspend the game and can see the first
        # proxy while the native constructor is still appending the second.
        if capture["proxy_count"] == len(capture["private_proxy_addresses"]) == 1:
            require(capture["private_proxy_depth_priority_groups"] == [1 if mode == "night_vision" else 4],
                    "Partial native construction has an unexpected first proxy")
            incomplete += 1
            continue
        require(capture["proxy_count"] == len(capture["private_proxy_addresses"]) == 2 and
                fault != "extra-proxy",
                "Private native proxy list contains disabled effects or lost an enabled effect")
        counts[mode] += 1
    require(min(counts.values()) >= 100, "Insufficient independent normal/NV proxy observations")
    return counts, incomplete


def grade(directory, module, fault=None):
    manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    digest = hashlib.sha256(module.read_bytes()).hexdigest().upper()
    require((directory / "capture-complete.txt").is_file() and
            manifest["physical_camcorder"] and manifest["native_camera_hud"] and
            manifest["mod_sha256"] == digest and fault != "module",
            "Visibility evidence does not identify the completed exact-module native capture")
    observed = rows(directory / "native-retail-camera-observation.jsonl")
    counts, incomplete = proxy_counts(observed, fault)
    dispatches = [row for row in rows(directory / "native-postprocess-dispatch.jsonl")
                  if row["kind"] == "dispatch" and not row["main"] and
                  row["dpg"] == 4 and not row["lighting_only"]]
    require(len(dispatches) >= 2 and all(len(view["proxies"]) == 2
                for row in dispatches for view in row["views"]),
            "Completed native render dispatch contains disabled proxies or lost an enabled effect")
    effects = {row["object"]: row for row in observed if row["kind"] == "native_effect_identity"}
    materials = {row["effect"]: row for row in observed if row["kind"] == "native_effect_material"}
    visibility = rows(directory / "native-effect-visibility.jsonl")
    require(visibility, "Missing actual material proxy factory evidence")
    if fault in ("wrong-source-flags", "forward-disabled", "drop-enabled", "wrong-material", "change-world"):
        visibility = copy.deepcopy(visibility)
        for row in visibility:
            if fault == "change-world" and not row["private_capture"]:
                row["omitted_disabled_effect"] = True
                break
            if row["private_capture"]:
                if fault == "wrong-source-flags":
                    row["source_flags"] ^= 2
                elif fault == "wrong-material":
                    row["material"] += 8
                elif fault == "forward-disabled" and row["omitted_disabled_effect"]:
                    row["proxy"] = 1
                elif fault == "drop-enabled" and not row["omitted_disabled_effect"]:
                    row["proxy"] = 0
                else:
                    continue
                break
    omitted = {}
    enabled = 0
    world = 0
    for row in visibility:
        if not row["private_capture"]:
            require(not row["omitted_disabled_effect"] and row["proxy"],
                    "Visibility gate changed an unowned world material proxy")
            world += 1
            continue
        identity = effects.get(row["effect"])
        material = materials.get(row["effect"])
        require(identity is not None and identity["class"] == "MaterialEffect" and
                identity["create_proxy_rva"] == 0x544c70 and material is not None,
                "Factory source lacks an independent native material-effect identity")
        flags = struct.unpack_from("<I", bytes.fromhex(identity["header_hex"]), 0x60)[0]
        require(row["source_flags"] == flags and row["material"] == material["material"],
                "Factory flags/material disagree with independently read native source bytes")
        visible = bool(flags & 2)
        require(row["native_gameplay_visible"] == visible and
                row["omitted_disabled_effect"] == (not visible) and bool(row["proxy"]) == visible,
                "Private material proxy ignored the native show-in-game bit")
        if visible:
            enabled += 1
        else:
            omitted[material["name"]] = omitted.get(material["name"], 0) + 1
    require(enabled >= 100 and world >= 50 and set(omitted) == {
                "Deform_INST", "CameraGlitch_INST", "CameraGlitch-02_INST"} and
            min(omitted.values()) >= 100,
            "Missing actual enabled camera color, untouched world or disabled normal/NV material coverage")
    scalar_observations = 0
    for record in observed:
        for material in record.get("native_capture", {}).get("dynamic_materials", []):
            for layer in material["parameters"]["layers"]:
                if layer["name"] != "Asylum_post_process.Camera.CameraGlitch-02_INST":
                    continue
                values = {item["name"]: item["value_bits"] for item in layer["scalars"]}
                require(values.get("GlitchAmount") == 0x3f800000 and fault != "zero-glitch-strength",
                        "Authored NV glitch strength was altered rather than respecting visibility")
                scalar_observations += 1
    require(scalar_observations >= 100, "Authored native NV glitch strength was not independently observed")
    return dict(native_lobby_material_visibility_acceptance=True, module_sha256=digest,
                independent_private_proxy_samples=counts, omitted_native_disabled_materials=omitted,
                incomplete_non_atomic_constructor_snapshots_not_accepted=incomplete,
                completed_native_render_dispatches=len(dispatches),
                enabled_private_materials=enabled, unchanged_other_view_materials=world,
                independently_observed_unmodified_nv_strength=scalar_observations,
                arbitrary_authored_glitch_event_acceptance=False, physical_headset_acceptance=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--old-native-control", type=Path, required=True)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    result = grade(args.directory, args.module)
    old = rows(args.old_native_control / "native-retail-camera-observation.jsonl")
    try:
        proxy_counts(old)
    except ValueError as error:
        require("Private native proxy list" in str(error),
                "Old native control failed for a reason other than the actual disabled-proxy defect")
        result["actual_old_native_disabled_proxy_control_rejected"] = True
    else:
        raise ValueError("Actual old native capture did not reproduce the disabled-proxy defect")
    rejected = []
    if args.selftest:
        for fault in ("module", "removed-source-effect", "extra-proxy", "wrong-source-flags",
                      "forward-disabled", "drop-enabled", "wrong-material", "change-world",
                      "zero-glitch-strength"):
            try:
                grade(args.directory, args.module, fault)
            except ValueError:
                rejected.append(fault)
            else:
                raise ValueError("Visibility oracle accepted corruption: " + fault)
    result["corruptions_rejected"] = rejected
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
