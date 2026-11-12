"""Headset-free DLL/GPU acceptance; never starts the game or changes XR registration."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET


RETAIL_SHA256 = "06c1c37c7f1d0bd0296328ff977ff847bee82f878fb4d79f4282a026aef68a96"
REQUIRED_TESTS = {
    "test_outlast_config", "test_outlast_camcorder", "test_outlast_native_use",
    "test_outlast_motion_controls", "test_native_camcorder_lifecycle",
    "test_outlast_hand_collision", "test_outlast_desktop_cursor", "test_outlast_vr_hints",
    "test_outlast_body_lifecycle", "test_outlast_locomotion_camera",
    "test_outlast_native_view", "test_outlast_focus_pause",
    "test_atlas_pixel_routing",
    "test_input_analog_recovery",
    "test_native_camera_resources", "test_native_image", "test_native_visibility",
    "test_openxr_session", "test_openxr_lifecycle", "test_converted_index_cache",
    "test_material_world_position_native", "test_material_full_samplers",
    "test_integrated_mod", "test_integrated_reset_retry", "test_integrated_flat_wrap",
    "test_integrated_thread_overlap", "test_integrated_ui_scaleform_handoff",
    "test_integrated_camera_bob", "test_integrated_floor_startup",
    "python_regressions",
}
DLL_MODES = (
    "", "--thread-handoff", "--thread-overlap", "--ui", "--ui-in-renderer",
    "--ui-scaleform", "--ui-scaleform-handoff", "--camera-tilt", "--camera-bob",
    "--floor-startup", "--comfort", "--snap", "--snap-recovery", "--ipd-cycle",
    "--input-chords", "--input-recovery", "--reset-retry",
    "--flat-transitions", "--flat-upload", "--flat-wrap",
)
DLL_REJECTIONS = {
    "--fault-dual": "Compositor physical draw count failed: indexed=2; plain=0; instances=4",
    "--fault-mono": "Compositor disparity failed:",
    "--fault-ui-mono": "Compositor projected ray alignment failed:",
    "--fault-flat-mono": "Compositor projected ray alignment failed:",
    "--fault-camera-bob": "Compositor upright camera alignment failed:",
    "--fault-camera-tilt": "Compositor upright camera alignment failed:",
    "--comfort-fault-scale": "Compositor disparity failed:",
    "--fault-snap": "Compositor projected ray alignment failed:",
}
LIMITATIONS = [
    "No game launch, headset, Steam progression, or whole-campaign acceptance.",
    "DLL cases are fresh processes; repeated XR lifecycles are component code in one process.",
    "Mock runtime plus real GPU readback is not consumer-runtime compatibility certification.",
    "Physical camcorder optics/comfort, arbitrary scripted pickups and headset performance require playtesting.",
]


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def grade_result(returncode, text, markers, expected_code=0):
    if returncode != expected_code:
        raise ValueError(f"Unexpected exit {returncode}; expected {expected_code}")
    missing = [marker for marker in markers if marker not in text]
    if missing:
        raise ValueError(f"Missing independent oracle diagnostics: {missing}")
    if expected_code == 0 and any(marker in text for marker in ("FAIL:", "FATAL:", "REJECTED:")):
        raise ValueError("A successful exit concealed an explicit failure")


def grade_junit(path, expected):
    root = ET.parse(path).getroot()
    for suite in root.iter():
        if suite.tag in ("testsuite", "testsuites") and any(
            int(suite.attrib.get(key, 0)) for key in ("failures", "errors", "disabled", "skipped")
        ):
            raise ValueError("JUnit suite reports unsuccessful or skipped tests")
    cases = list(root.iter("testcase"))
    names = [case.attrib["name"] for case in cases]
    if len(names) != len(set(names)) or set(names) != set(expected):
        raise ValueError("JUnit roster differs from the discovered CTest roster")
    for case in cases:
        if any(case.find(tag) is not None for tag in ("failure", "error", "skipped")):
            raise ValueError(f"CTest did not pass {case.attrib['name']}")
        if case.attrib.get("status", "run") != "run" or re.search(
            r"^OK \(skipped=[1-9][0-9]*\)\s*$", case.findtext("system-out", ""), re.MULTILINE
        ):
            raise ValueError(f"CTest or nested Python cases were skipped: {case.attrib['name']}")
    return len(cases)


def memory_headroom():
    class MemoryStatus(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [
            (name, ctypes.c_ulonglong) for name in (
                "total_physical", "available_physical", "total_commit", "available_commit",
                "total_virtual", "available_virtual", "extended_virtual")
        ]
    status = MemoryStatus()
    status.length = ctypes.sizeof(status)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        raise OSError("Cannot query memory headroom")
    if status.available_physical < 3 * 1024**3 or status.available_commit < 4 * 1024**3:
        raise RuntimeError("Insufficient shared-machine headroom: need 3 GiB free RAM and 4 GiB commit")
    return dict(available_physical=status.available_physical, available_commit=status.available_commit)


def bounded_count(value):
    count = int(value)
    if not 1 <= count <= 4096:
        raise argparse.ArgumentTypeError("Count must be 1..4096")
    return count


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--module", required=True, type=Path)
    parser.add_argument("--retail", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="New evidence directory; never overwritten")
    parser.add_argument("--cycles", type=bounded_count, default=2, help="Fresh-process DLL matrix repetitions")
    parser.add_argument("--session-cycles", type=bounded_count, default=64, help="In-process component XR lifecycles")
    args = parser.parse_args(argv)
    if os.name != "nt":
        parser.error("The acceptance fixtures require Windows")
    build, module, retail, output = (path.resolve() for path in (
        args.build, args.module, args.retail, args.output))
    output.mkdir(parents=True, exist_ok=False)
    report = dict(schema=1, automated_passed=False, production_ready=False,
                  started_unix=time.time(), limitations=LIMITATIONS, cases=[])
    report_path = output / "acceptance.json"

    def save():
        temporary = report_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        temporary.replace(report_path)

    environment = os.environ.copy()

    def run(name, command, markers=(), expected_code=0, timeout=120):
        memory_headroom()
        log = output / f"{name}.log"
        row = dict(name=name, command=[str(part) for part in command], log=log.name, passed=False,
                   expected_returncode=expected_code, required_diagnostics=list(markers))
        report["cases"].append(row)
        save()
        started = time.monotonic()
        try:
            with log.open("wb") as stream:
                result = subprocess.run(row["command"], stdout=stream, stderr=subprocess.STDOUT,
                                        cwd=output, env=environment, timeout=timeout, check=False)
            row["returncode"] = result.returncode
            text = log.read_text(encoding="utf-8", errors="replace")
            grade_result(result.returncode, text, markers, expected_code)
            row["passed"] = True
            return text
        finally:
            row["seconds"] = round(time.monotonic() - started, 3)
            save()

    save()
    try:
        report["memory_start"] = memory_headroom()
        if shutil.disk_usage(output).free < 2 * 1024**3:
            raise RuntimeError("Need at least 2 GiB free evidence/build drive space")
        if module.name != "OutlastVRSinglePass.dll":
            raise ValueError("Select the actual OutlastVRSinglePass.dll")
        if sha256(retail) != RETAIL_SHA256:
            raise ValueError("Unsupported retail executable; no target will be executed")
        ctest = shutil.which("ctest")
        if not ctest:
            raise FileNotFoundError("ctest is required")
        binaries = build / "bin"
        paths = {
            "module": module, "retail": retail,
            "integrated": binaries / "test_integrated_mod.exe",
            "lifecycle": binaries / "test_openxr_lifecycle.exe",
            "uniforms": binaries / "test_tracked_uniforms.exe",
            "camcorder": binaries / "test_outlast_camcorder.exe",
            "hand_collision": binaries / "test_outlast_hand_collision.exe",
            "body_lifecycle": binaries / "test_outlast_body_lifecycle.exe",
        }
        inputs = {name: dict(path=str(path), sha256=sha256(path)) for name, path in paths.items()}
        report["inputs"] = inputs
        root = Path(__file__).resolve().parents[1]
        revision = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                                  capture_output=True, text=True, check=True).stdout.strip()
        status = subprocess.run(["git", "-C", str(root), "status", "--porcelain"],
                                capture_output=True, text=True, check=True).stdout
        report["source"] = dict(commit=revision, dirty=bool(status))
        # CTest must exercise the selected production module, not a second candidate.
        roster = json.loads(run("roster", [ctest, "--test-dir", build, "--show-only=json-v1"]))
        tests = roster["tests"]
        names = [test["name"] for test in tests]
        if not REQUIRED_TESTS.issubset(names) or len(names) != len(set(names)):
            raise ValueError(f"Missing or duplicate required test roster: {sorted(REQUIRED_TESTS-set(names))}")
        integrated = next(test for test in tests if test["name"] == "test_integrated_mod")
        if len(integrated["command"]) < 3 or (
            Path(integrated["command"][1]).resolve() != module or
            Path(integrated["command"][2]).resolve() != retail
        ):
            raise ValueError("CTest production DLL/retail inputs differ from the selected inputs")
        for test in tests:
            path = Path(test["command"][0]).resolve()
            name = "test_executable:" + str(path)
            if name not in inputs:
                paths[name] = path
                inputs[name] = dict(path=str(path), sha256=sha256(path))
        environment["PATH"] = str(module.parent) + os.pathsep + environment["PATH"]
        run("ctest", [ctest, "--test-dir", build, "--output-on-failure", "--parallel", "2",
                      "--output-junit", output / "ctest.xml"], timeout=1800)
        report["ctest_passed"] = grade_junit(output / "ctest.xml", names)
        run("xr-lifecycles", [paths["lifecycle"], "--cycles", str(args.session_cycles)],
            [f"PASS: repeated_in_process_xr_lifecycles={args.session_cycles};"],
            timeout=max(120, args.session_cycles * 5))
        for cycle in range(args.cycles):
            for mode in DLL_MODES:
                name = mode.removeprefix("--") or "default"
                run(f"dll-{cycle:04}-{name}", [paths["integrated"], module, retail] + ([mode] if mode else []),
                    ["PASS: integrated production DLL; native_frames=20;", "XR_cleanup_handles=4"])
        for mode, diagnostic in DLL_REJECTIONS.items():
            run("reject-" + mode.removeprefix("--"), [paths["integrated"], module, retail, mode],
                ["REJECTED: compositor stereo content or single-draw contract failed", diagnostic,
                 "FATAL: xrEndFrame XrResult=-1"], expected_code=0xC0000602)
        run("reject-index-cache", [paths["uniforms"], "--fault-index-cache-stale"],
            ["FAIL: Tracked inverse matrices, fog camera or view rotation content mismatch; frame=3"],
            expected_code=1)
        run("reject-fixed-hand-anchor", [paths["hand_collision"], "--fault-root-anchor"],
            ["FAIL: Room-scale head/body movement left the tracked hand anchored across a wall"],
            expected_code=1)
        run("reject-native-crouch", [paths["body_lifecycle"], "--fault-native-crouch"],
            ["FAIL: Tracked hand detached from its moving body/controller frame"],
            expected_code=1)
        run("reject-nonblocking-volume", [paths["hand_collision"], "--fault-nonblocking-volume"],
            ["FAIL: Non-blocking room volume stopped controller hand tracking"],
            expected_code=1)
        for name, original in inputs.items():
            if sha256(paths[name]) != original["sha256"]:
                raise ValueError(f"Input changed during acceptance: {name}")
        report["memory_end"] = memory_headroom()
        report["automated_passed"] = True
        print(f"PASS: {report['ctest_passed']} CTest cases; "
              f"{args.cycles * len(DLL_MODES)} production DLL runs; "
              f"{args.session_cycles} in-process XR lifecycles; "
              f"{len(DLL_REJECTIONS)+4} exact-diagnostic negative controls")
        print(f"Hardware/campaign gates remain open. Evidence: {report_path}")
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, ET.ParseError, KeyError) as error:
        report["error"] = str(error)
        print(f"FAIL: {error}; evidence retained at {report_path}", file=sys.stderr)
        return 1
    finally:
        report["finished_unix"] = time.time()
        save()


if __name__ == "__main__":
    sys.exit(main())
