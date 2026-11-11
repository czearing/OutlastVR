"""Grade the controlled physical-camera run, including real lens images.

Every native lens RGB pixel must equal its same-frame unmodified-retail-shader
mono reference. Contrast-normalized RootSIFT/ray checks support optical direction
for lit captures, not exact geometry. Authored-mount captures account for the
physical lens-to-eye baseline using the actual rendered eye transform, require
positive triangulated depths, and bound independent reprojection error.
With --native-stock-control, the same optical limits apply to genuine HDR
scene inputs before nonlinear stock camera distortion, while every delivered
processed RGB pixel must still match its original-shader reference.
Unlit captures instead require the actual engine view to agree with independently
identified, inverse-reference-skinned retail lens geometry; their different
lighting cannot be used as a reliable lit-world feature-matching oracle.
This does not certify physical-headset comfort or performance.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path

import cv2
import numpy as np

from verify_native_probe import require, rotation
from verify_native_camcorder_mesh import grade as mesh_grade, optical_sample


def rows(path):
    with path.open(encoding="utf-8-sig") as stream:
        return [json.loads(line) for line in stream]


def fixture_grade(manifest, frames):
    version = manifest.get("physical_camcorder_fixture_version")
    if version is not None:
        require(type(version) is int and version in (1, 2, 3, 4, 5, 6, 7),
                "Unknown physical-camera fixture version")
    active = [row for row in frames if not row.get("canceled")]
    if version in (5, 6, 7) or any(row.get("physical_camcorder_fixture_version") in (5, 6, 7) for row in active):
        require(version in (5, 6, 7) and active and
                all(type(row.get("physical_camcorder_fixture_version")) is int and
                    row["physical_camcorder_fixture_version"] == version for row in active),
                "Controlled optical grip poses differ from their actual submitted fixture version")


def load(directory):
    frames = {}
    with (directory / "frames.jsonl").open() as stream:
        for line in stream:
            item = json.loads(line)
            if not item.get("canceled"):
                frames[item["frame"]] = {key: item[key] for key in
                                        ("frame", "gameplay_frame", "native_frame", "views", "world_panel")}
                if "lens_sample" in item:
                    frames[item["frame"]]["lens_sample"] = item["lens_sample"]
    manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    return dict(directory=directory, frames=frames, manifest=manifest,
                native_frames={row["native_frame"]: row for row in frames.values()},
                events=rows(directory / "physical-camcorder-events.jsonl"),
                views=rows(directory / "native-lens-views.jsonl"),
                composition=rows(directory / "native-camcorder-composition.jsonl"),
                lifecycle=rows(directory / "native-camcorder-lifecycle.jsonl"),
                cameras={item["native_frame"]: item for item in rows(directory / "native-cameras.jsonl")},
                samples=rows(directory / "native-lens-samples.jsonl"),
                references=rows(directory / "native-mono-resolve.jsonl"))


def freshness_grade(events):
    require(events, "No actual camera updates for body freshness attribution")
    fresh = expired = future = absent = mismatched = 0
    for row in events:
        require({"body_request_schema_version", "clock", "body_request_clock_us", "body_request_age_us",
                 "body_request_matches_hero", "body_request_current", "held"} <= row.keys(),
                "Missing native camera freshness evidence fields")
        require(type(row["body_request_schema_version"]) is int and row["body_request_schema_version"] == 1,
                "Unknown native camera freshness evidence version")
        now, sampled, age = row["clock"], row["body_request_clock_us"], row["body_request_age_us"]
        matches, current = row["body_request_matches_hero"], row["body_request_current"]
        require(type(now) is int and now >= 0 and
                (sampled is None or type(sampled) is int and sampled >= 0) and
                type(matches) is bool and type(current) is bool and type(row["held"]) is bool,
                "Invalid native camera freshness evidence types")
        expected_age = now - sampled if sampled is not None and now >= sampled else None
        require(age is None if expected_age is None else type(age) is int and age == expected_age,
                "Body request age is not bound to its actual camera observation clock")
        require(not matches or sampled is not None, "An absent body request cannot own the native hero")
        expected_current = matches and age is not None and age <= 250_000
        require(current == expected_current and (current or not row["held"]),
                "Native held camera bypassed body identity or the unchanged 250ms freshness gate")
        fresh += current
        expired += age is not None and age > 250_000
        future += sampled is not None and sampled > now
        absent += sampled is None
        mismatched += sampled is not None and not matches
    return dict(body_freshness_gate_checked=True, schema_version=1, updates=len(events),
                current_updates=fresh, expired_updates=expired, future_clock_updates=future,
                absent_request_updates=absent, mismatched_hero_updates=mismatched)


def state_grade(data):
    events, frames = data["events"], data["native_frames"]
    require(len(events) >= 1000, "Missing native camera updates")
    require(all(b["clock"] >= a["clock"] for a, b in zip(events, events[1:])), "Camera clock regressed")
    held = [row for row in events if row["held"]]
    require(all(sum(row["hand"] == hand for row in held) >= 70 for hand in (0, 1)),
            "Both hands did not hold the native camera")
    require(all(row["visible"] and row["hand"] in (0, 1) for row in held), "Invalid camera ownership")
    for start, end, label in ((60, 67, "remote grip"), (655, 674, "tracking loss"),
                              (682, 687, "tracking regain"), (705, 724, "focus loss"),
                              (880, 895, "release")):
        selected = [row for row in events if row["frame"] in frames and
                    start <= frames[row["frame"]]["gameplay_frame"] <= end]
        require(len(selected) >= 3, "Missing " + label + " observations")
        require(not any(row["held"] for row in selected), "Camera retained/acquired during " + label)
    toggles = [row for row in events if row["toggle"]]
    require(len(toggles) >= 4, "Missing native raise/lower requests")
    for row in toggles:
        require(any(later["native_state"] == (1 if row["held"] else 0) for later in events
                    if row["clock"] <= later["clock"] <= row["clock"] + 2_000_000),
                "Native camera toggle was not acknowledged within its bounded deadline")
    require(sum(bool(row["rhi_texture"]) for row in held) > len(held) * .9,
            "Held camera did not publish its actual native render texture")
    require(sum(row["night_vision"] for row in held) >= 40, "Native night vision was not exercised")
    battery = [row["battery"] for row in held]
    require(all(math.isfinite(value) and 0 <= value <= 1 for value in battery) and
            max(battery) - min(battery) > .003, "Native battery did not consume real energy")
    require(max(row["isolated_effects"] for row in events) >= 2, "Fullscreen native FX were not isolated")
    require(max(row["isolated_hud"] for row in events) >= 2, "Stock fullscreen camera HUD was not isolated")
    collections = [row for row in data["lifecycle"] if row["phase"] == "before_gc" and
                   row["held"] and row["rhi_texture"] and row["requested"]]
    require(collections, "No real garbage collection of an active held lens")
    generation = collections[0]["generation"]
    require(any(row["phase"] == "after_gc" and row["generation"] > generation and not row["rhi_texture"]
                for row in data["lifecycle"]), "Collected camera resource remained published")
    require(any(row["generation"] > generation and row["held"] and row["rhi_texture"] for row in events),
            "Native capture was not recreated/reacquired after collection")
    errors = []
    for view in data["views"]:
        candidates = [row for row in held if row["rhi_texture"] and abs(row["frame"] - view["frame"]) <= 4]
        if not candidates:
            continue
        row = min(candidates, key=lambda item: np.linalg.norm(np.array(item["lens_position"]) - view["origin"]))
        distance = float(np.linalg.norm(np.array(row["lens_position"]) - view["origin"]))
        pitch, yaw, roll = np.array(row["rotation"]) * (2 * math.pi / 65536)
        forward = np.array([math.cos(pitch)*math.cos(yaw), math.cos(pitch)*math.sin(yaw), math.sin(pitch)])
        up = np.array([-math.sin(pitch)*math.cos(yaw), -math.sin(pitch)*math.sin(yaw), math.cos(pitch)])
        up = up*math.cos(roll) + np.array([-math.sin(yaw), math.cos(yaw), 0])*math.sin(roll)
        angular = max(np.linalg.norm(np.array(view["backward"]) + forward), np.linalg.norm(np.array(view["up"]) - up))
        errors.append((distance, float(angular)))
    require(len(errors) >= 100, "Insufficient independently observed native lens poses")
    require(float(np.percentile(np.array(errors)[:, 0], 90)) < .02 and
            float(np.percentile(np.array(errors)[:, 1], 90)) < .003,
            "Actual native lens pose/orientation does not match the tracked optical pose")
    result = dict(updates=len(events), held=len(held), held_by_hand=[sum(row["hand"] == hand for row in held) for hand in (0, 1)],
                native_toggles=len(toggles), lens_pose_observations=len(errors),
                lens_pose_error90_native_units=float(np.percentile(np.array(errors)[:, 0], 90)),
                lens_axes_error90=float(np.percentile(np.array(errors)[:, 1], 90)),
                battery_consumed=max(battery)-min(battery), held_gc_generation=generation)
    if any("body_request_schema_version" in row for row in events):
        result["tracking_freshness"] = freshness_grade(events)
    return result


def finite_baseline_grade(camera, view, rays, target, width, height):
    eyes = np.asarray(camera.get("rendered_eyes", []), dtype=float)
    require(eyes.shape == (2, 32) and np.isfinite(eyes).all(),
            "Missing exact rendered native eye transforms")
    eye = eyes[0]
    projection = eye[:16].reshape(4, 4)
    intrinsic_basis = projection[[0, 1, 3], :3]
    require(abs(float(np.linalg.det(intrinsic_basis))) > 1e-6,
            "Rendered native eye projection is singular")
    require(np.allclose(eye[20:23], camera["pretranslation"], atol=.001, rtol=0) and
            np.max(np.abs(projection[[0, 1, 3], 3] +
                          intrinsic_basis @ eye[16:19])) < .003,
            "Rendered native eye origin and uploaded projection disagree")
    clip = np.column_stack((target[:, 0]*2/width-1, 1-target[:, 1]*2/height,
                            np.ones(len(target))))
    eye_rays = clip @ np.linalg.inv(intrinsic_basis).T
    eye_rays /= np.linalg.norm(eye_rays, axis=1, keepdims=True)
    lens_rays = rays / np.linalg.norm(rays, axis=1, keepdims=True)
    origin = np.asarray(view["origin"])
    baseline = eye[16:19] - eye[20:23] - origin
    cosine = np.einsum("ij,ij->i", lens_rays, eye_rays)
    denominator = 1-cosine*cosine
    require(np.all(denominator > 1e-10), "Native optical correspondences cannot resolve the camera baseline")
    lens_distance = (lens_rays @ baseline - cosine*(eye_rays @ baseline))/denominator
    eye_distance = cosine*lens_distance - eye_rays @ baseline
    require(np.all(lens_distance > 0) and np.all(eye_distance > 0),
            "Native optical correspondences triangulate behind the physical lens or headset")
    points = origin + lens_distance[:, None]*lens_rays
    projected = np.column_stack((points + eye[20:23], np.ones(len(points)))) @ projection.T
    require(np.all(projected[:, 3] > 0), "Triangulated native scene is behind the rendered eye")
    pixels = np.column_stack(((projected[:, 0]/projected[:, 3]+1)*width/2,
                              (1-projected[:, 1]/projected[:, 3])*height/2))
    median, p90 = map(float, np.percentile(np.linalg.norm(pixels-target, axis=1), [50, 90]))
    require(median < 2 and p90 < 4,
            "Lens pixels disagree with the actual finite-baseline native eye projection")
    return dict(direction_method="finite_baseline_positive_depth_triangulation",
                lens_to_eye_baseline_native_units=float(np.linalg.norm(baseline)),
                positive_depth_correspondences=len(target),
                direction_error_median_px=median, direction_error90_px=p90)


def lens_grade(data, sample, replacement=None, *, raw_direction=None):
    directory, frames = data["directory"], data["frames"]
    if "settled_ms" in sample:
        require(sample["settled_ms"] >= 500, "Lens calibration sampled an unsettled native raise animation")
    available = [frame for frame in frames if (directory / f"frame_{frame}_eye_0.ppm").exists()]
    if any("lens_sample" in frame for frame in frames.values()):
        simultaneous = [frames[value] for value in available
                        if frames[value].get("lens_sample") == sample["sample"]]
        require(len(simultaneous) == 1, "Missing unique same-submission lens/world image pair")
        frame = simultaneous[0]
    else:
        frame = frames[min(available, key=lambda value: abs(frames[value]["native_frame"] - sample["frame"]))]
    view = min(data["views"], key=lambda value: abs(value["frame"] - sample["frame"]))
    lens_rgb = cv2.imread(str(directory / f"native-lens-{sample['sample']}.ppm"))
    if replacement is not None:
        lens_rgb = cv2.cvtColor(replacement, cv2.COLOR_GRAY2BGR) if replacement.ndim == 2 else replacement
    require(lens_rgb is not None and lens_rgb.shape == (288, 512, 3) and float(lens_rgb.std()) > 5,
            "Lens source is absent, black or incorrectly sized")
    reference = next((row for row in data["references"] if row["frame"] == sample["frame"] and
                      row["sample"] == sample["sample"]), None)
    require(reference is not None and reference["checked_pixels"] == 512*288 and
            reference["different_pixels"] == 0 and reference["original_retail_shader"] and
            reference["atlas_samplers"], "Missing whole-image original-retail-shader reference")
    expected = cv2.imread(str(directory / reference["reference_image"]))
    require(expected is not None and np.array_equal(lens_rgb, expected),
            "Whole native lens image differs from its independently rendered mono reference")
    if raw_direction is not None:
        require(raw_direction["sample"] == sample["sample"] and
                raw_direction["sample_frame"] == sample["frame"] and
                raw_direction["world_frame"] == frame["frame"],
                "Native pre-distortion optics belong to a different final lens sample")
        return dict(complete_rgb_reference_pixels=512*288,
                    optical_pixels="checksummed_actual_native_pre_stock_distortion_HDR",
                    processed_pixels="whole_image_unmodified_retail_shader_reference",
                    **raw_direction)
    if sample.get("capture_mode") == 1:
        geometry = optical_sample(directory, sample, data["views"])
        return dict(sample=sample["sample"], sample_frame=sample["frame"], world_frame=frame["frame"],
                    complete_rgb_reference_pixels=512*288, capture_mode="unlit_night_vision",
                    photometric_matching="not_applicable_to_differently_lit_world", **geometry)
    return dict(complete_rgb_reference_pixels=512*288,
                **direction_grade(data, sample, lens_rgb, frame, view))


def screen_mask(row, eye, width, height, horizontal_offset=0):
    matrix = np.array(row["screen_matrices"][eye]).reshape(4, 4)
    corners = np.array([[-1, -1, 0, 1], [1, -1, 0, 1], [1, 1, 0, 1], [-1, 1, 0, 1]]) @ matrix.T
    require(np.isfinite(corners).all() and (corners[:, 3] > 0).all(), "Screen projection is invalid")
    ndc = corners[:, :2] / corners[:, 3:4]
    polygon = np.column_stack(((ndc[:, 0] + 1) * width / 2,
                               (1 - ndc[:, 1]) * height / 2)).astype(np.int32)
    polygon[:, 0] += horizontal_offset
    mask = np.zeros((height, width), np.uint8)
    cv2.fillConvexPoly(mask, polygon, 1)
    return cv2.dilate(mask, np.ones((7, 7), np.uint8)) != 0


def independent_world_image(data, frame):
    directory = data["directory"]
    composition = [row for row in data["composition"] if row["frame"] == frame["native_frame"]]
    require(len(composition) == 1, "Missing unique same-frame native LCD projection")
    before_path = directory / f"native-screen-before-{frame['native_frame']}-eye-0.ppm"
    require(before_path.is_file(), "Missing actual same-frame world image before LCD composition")
    before = cv2.imread(str(before_path))
    composed = cv2.imread(str(directory / f"frame_{frame['frame']}_eye_0.ppm"))
    require(before is not None and composed is not None and before.shape == composed.shape,
            "Independent pre-LCD world image changed extent")
    height, width = before.shape[:2]
    excluded = screen_mask(composition[0], 0, width, height)
    require(np.any(~excluded) and np.array_equal(before[~excluded], composed[~excluded]),
            "Pre-LCD world reference differs outside its actual native LCD footprint")
    return cv2.cvtColor(before, cv2.COLOR_BGR2GRAY), (~excluded).astype(np.uint8) * 255


def direction_grade(data, sample, lens_rgb, frame, view):
    """Apply the unchanged optical thresholds to an identified rectilinear image."""
    directory = data["directory"]
    lens = cv2.cvtColor(lens_rgb, cv2.COLOR_BGR2GRAY)
    world, independent_mask = independent_world_image(data, frame)
    contrast = cv2.createCLAHE(clipLimit=2., tileGridSize=(8, 8))
    sift = cv2.SIFT_create(contrastThreshold=.005)
    key_l, desc_l = sift.detectAndCompute(contrast.apply(lens), None)
    key_w, desc_w = sift.detectAndCompute(contrast.apply(world), independent_mask)
    require(desc_l is not None and desc_w is not None, "Lens lacks independent world texture")
    desc_l = np.sqrt(desc_l / (desc_l.sum(axis=1, keepdims=True) + 1e-7))
    desc_w = np.sqrt(desc_w / (desc_w.sum(axis=1, keepdims=True) + 1e-7))
    matcher = cv2.BFMatcher()
    forward = matcher.knnMatch(desc_l, desc_w, k=2)
    reverse = matcher.knnMatch(desc_w, desc_l, k=2)
    mutual = {pair[0].queryIdx: pair[0].trainIdx for pair in reverse
              if len(pair) == 2 and pair[0].distance < .72*pair[1].distance}
    matches = [pair[0] for pair in forward
               if len(pair) == 2 and pair[0].distance < .72*pair[1].distance and
               mutual.get(pair[0].trainIdx) == pair[0].queryIdx]
    require(len(matches) >= 12, "Lens has insufficient independent-direction correspondences")
    source = np.array([key_l[match.queryIdx].pt for match in matches], np.float32)
    target = np.array([key_w[match.trainIdx].pt for match in matches], np.float32)
    homography, valid = cv2.findHomography(source, target, cv2.RANSAC, 5)
    require(homography is not None and valid is not None, "Lens source is geometrically inconsistent")
    source, target = source[valid[:, 0] != 0], target[valid[:, 0] != 0]
    halves = [int(np.sum(source[:, 0] < 256)), int(np.sum(source[:, 0] >= 256))]
    require(len(source) >= 12 and min(halves) >= 4,
            "Lens lacks independent optical-direction correspondences in both halves")
    projection = np.array(view["projection"]).reshape(4, 4).T
    sx, sy = np.linalg.norm(projection[0, :3]), np.linalg.norm(projection[1, :3])
    basis_lens = np.column_stack((view["right"], view["up"], view["backward"]))
    rays = np.column_stack(((source[:, 0]/256-1)/sx, (1-source[:, 1]/144)/sy,
                            -np.ones(len(source)))) @ basis_lens.T
    camera = data["cameras"][frame["native_frame"]]
    mount_version = data["manifest"].get("camera_mount_version", 1)
    require(type(mount_version) is int and mount_version in (1, 2, 3, 4, 5),
            "Unknown native camera optical mount version")
    if mount_version in (2, 3, 4, 5):
        geometry = optical_sample(directory, sample, data["views"])
        finite = finite_baseline_grade(camera, view, rays, target, world.shape[1], world.shape[0])
        return dict(sample=sample["sample"], sample_frame=sample["frame"], world_frame=frame["frame"],
                    inliers=len(source), inliers_by_half=halves,
                    world_reference="same_frame_native_before_lcd",
                    excluded_lcd_pixels=int(np.sum(independent_mask == 0)),
                    **geometry, **finite)
    up, back = np.array(camera["tracking_up"]), np.array(camera["tracking_backward"])
    basis = np.column_stack((np.cross(back, up), up, back))
    eye = frame["views"][0]
    local = rays @ basis @ rotation(eye["orientation"])
    left, right, top, bottom = np.tan(eye["fov"])
    predicted = np.column_stack(((-local[:, 0]/local[:, 2]-left)/(right-left)*world.shape[1],
                                 (top+local[:, 1]/local[:, 2])/(top-bottom)*world.shape[0]))
    median, p90 = map(float, np.percentile(np.linalg.norm(predicted-target, axis=1), [50, 90]))
    require(median < world.shape[1]*.035 and p90 < world.shape[1]*.06,
            "Lens pixels do not agree with the independent native optical direction")
    return dict(sample=sample["sample"], sample_frame=sample["frame"], world_frame=frame["frame"],
                inliers=len(source), inliers_by_half=halves, direction_error_median_px=median,
                direction_error90_px=p90, world_reference="same_frame_native_before_lcd",
                excluded_lcd_pixels=int(np.sum(independent_mask == 0)))


def green(image):
    values = image.astype(np.float32)
    return (values[:, :, 1] > 50) & (values[:, :, 1] > values[:, :, 2]*1.6) & (values[:, :, 1] > values[:, :, 0]*1.4)


def screen_grade(data, fault=None, native_postprocess=False):
    directory = data["directory"]
    captured = [dict(row, image_frame=data["native_frames"][row["frame"]]["frame"])
                for row in data["composition"] if row["frame"] in data["native_frames"] and
                (directory / f"frame_{data['native_frames'][row['frame']]['frame']}_eye_0.ppm").exists()]
    require(any(row["hand"] == 0 for row in captured) and any(row["hand"] == 1 for row in captured),
            "No actual screen images from both hands")
    normal = next((row for row in captured if not row["night_vision"]), None)
    night = [row for row in captured if row["night_vision"]]
    require(normal is not None and len(night) >= 2, "Missing real normal/NV screen images")
    require(all(row["depth_matched"] and row["rhi_texture"] for row in data["composition"]),
            "Screen lost its matched depth or native source")
    require(all(row["native_world_camera_effect"] == 0 for row in data["composition"]),
            "Native fullscreen camera postprocessing remained active")
    results = []
    for row in night:
        for eye in (0, 1):
            image = cv2.imread(str(directory / f"frame_{row['image_frame']}_eye_{eye}.ppm"))
            baseline = cv2.imread(str(directory / f"native-screen-before-{row['frame']}-eye-{eye}.ppm"))
            require(image is not None and baseline is not None and image.shape == baseline.shape,
                    "Missing actual physical-screen stereo image")
            height, width = image.shape[:2]
            mask = screen_mask(row, eye, width, height, width//3 if fault == "screen-pose" else 0)
            if fault == "fullscreen-nv":
                image = image.copy()
                image[:, :, 0] //= 5
                image[:, :, 2] //= 5
            if fault == "missing-screen":
                image = image.copy()
                image[mask] = baseline[mask]
            changed_green = green(image) & ~cv2.dilate(green(baseline).astype(np.uint8),
                                                       np.ones((7, 7), np.uint8)).astype(bool)
            inside, outside = int(np.sum(changed_green & mask)), int(np.sum(changed_green & ~mask))
            visible = np.any(mask)
            changed_screen = int(np.sum(np.any(image != baseline, axis=2) & mask))
            if native_postprocess:
                require(changed_screen > 0 if visible else changed_screen == 0,
                        "Proven native NV source was not delivered to its projected LCD")
            else:
                require((inside >= 12 if visible else inside == 0) and
                        outside <= max(8, int(width*height*.00002)),
                        "Night vision is absent from the screen or leaks into the surrounding world")
            world_pixels = ~mask
            require(np.any(world_pixels), "Screen projection left no independent surrounding-world pixels")
            world_delta = int(np.max(np.abs(image[world_pixels].astype(int)-baseline[world_pixels].astype(int))))
            require(world_delta == 0, "Night vision changed same-frame surrounding-world pixels")
            results.append(dict(frame=row["image_frame"], native_frame=row["frame"], eye=eye,
                                screen_in_view=bool(visible), hand=row["hand"], new_green_screen_pixels=inside,
                                new_green_world_pixels=outside, surrounding_world_rgb_max_delta=world_delta,
                                changed_screen_pixels=changed_screen,
                                unchanged_world_pixels=int(np.sum(world_pixels))))
    require(all(any(row["hand"] == hand and row["screen_in_view"] for row in results) for hand in (0, 1)),
            "Missing visible native night-vision screens from both hands")
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--module", required=True, type=Path)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--split-control", type=Path)
    parser.add_argument("--native-stock-control", type=Path,
                        help="Verify actual stock camera FX and rectilinear pre-distortion HDR optics")
    args = parser.parse_args()
    require((args.directory / "capture-complete.txt").is_file(), "Native capture did not finish")
    manifest = json.loads((args.directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    digest = hashlib.sha256(args.module.read_bytes()).hexdigest().upper()
    require(manifest["physical_camcorder"] and manifest["body"] and manifest["floor"] and
            manifest["mod_sha256"] == digest, "Camera proof does not match the exact physical-camera module")
    fixture_grade(manifest, rows(args.directory / "frames.jsonl"))
    data = load(args.directory)
    native_mesh = mesh_grade(args.directory) if (args.directory / "native-camera-mesh.jsonl").exists() else None
    state = state_grade(data)
    require(len(data["samples"]) == 3, "Missing scheduled complete native lens images")
    stock_fx = None
    raw_lenses = None
    if args.native_stock_control is not None:
        from verify_native_camcorder_postprocess import (
            FX_FAULTS, RAW_OPTICS_FAULTS, grade as stock_fx_grade, raw_optics_grade)
        stock_fx = stock_fx_grade(args.directory, args.module, args.native_stock_control)
        raw_lenses = {row["sample"]: row for row in raw_optics_grade(args.directory)}
        require(set(raw_lenses) == {row["sample"] for row in data["samples"]},
                "Stock native optics do not cover every actual processed lens sample")
    lenses = [lens_grade(data, sample, raw_direction=raw_lenses[sample["sample"]] if raw_lenses is not None else None)
              for sample in data["samples"]]
    screens = screen_grade(data, native_postprocess=stock_fx is not None)
    controls = []

    def reject(call, name):
        try:
            call()
        except ValueError:
            controls.append(name)
            return
        raise ValueError("Camera grader accepted negative control: " + name)

    if args.selftest:
        require(args.split_control is not None, "Selftest requires the retained actual split-feed control")
        if stock_fx is not None:
            for fault in FX_FAULTS:
                reject(lambda fault=fault: stock_fx_grade(
                    args.directory, args.module, args.native_stock_control, fault), "native-fx-" + fault)
            for fault in RAW_OPTICS_FAULTS:
                reject(lambda fault=fault: raw_optics_grade(args.directory, fault), "raw-optics-" + fault)
        for name in ("missing-left", "missing-gc", "missing-hud", "frozen-battery", "wrong-lens-origin"):
            altered = copy.deepcopy(data)
            if name == "missing-left":
                for row in altered["events"]:
                    if row["hand"] == 0:
                        row["held"] = False
            elif name == "missing-gc":
                altered["lifecycle"] = []
            elif name == "missing-hud":
                for row in altered["events"]:
                    row["isolated_hud"] = 0
            elif name == "frozen-battery":
                for row in altered["events"]:
                    row["battery"] = .5
            else:
                for row in altered["views"]:
                    row["origin"][0] += 10000
            reject(lambda: state_grade(altered), name)
        sample = data["samples"][0]
        split = cv2.imread(str(args.split_control / "native-lens-0.ppm"), cv2.IMREAD_GRAYSCALE)
        require(split is not None, "Missing retained split-feed negative image")
        reject(lambda: lens_grade(data, sample, split), "actual-retained-split-feed")
        reject(lambda: lens_grade(data, sample, np.zeros((288, 512), np.uint8)), "black-lens")
        nearest = lenses[0]["world_frame"]
        headset = cv2.imread(str(args.directory / f"frame_{nearest}_eye_0.ppm"), cv2.IMREAD_GRAYSCALE)
        reject(lambda: lens_grade(data, sample, cv2.resize(headset, (512, 288))), "headset-substitution")
        stale = cv2.imread(str(args.directory / "native-lens-1.ppm"))
        reject(lambda: lens_grade(data, sample, stale), "stale-lens-frame")
        damaged = cv2.imread(str(args.directory / "native-lens-0.ppm"))
        damaged[20, 20, 0] ^= 1
        reject(lambda: lens_grade(data, sample, damaged), "single-rgb-code-error")
        altered = copy.deepcopy(data)
        altered["references"][0]["frame"] += 1
        reject(lambda: lens_grade(altered, sample), "wrong-reference-frame")
        if any("lens_sample" in frame for frame in data["frames"].values()):
            altered = copy.deepcopy(data)
            for frame in altered["frames"].values():
                if frame.get("lens_sample") == sample["sample"]:
                    frame["lens_sample"] = -1
            reject(lambda: lens_grade(altered, sample), "missing-same-submission-world")
        altered = copy.deepcopy(data)
        for view in altered["views"]:
            view["right"] = [-value for value in view["right"]]
            view["backward"] = [-value for value in view["backward"]]
        reject(lambda: lens_grade(altered, sample), "reversed-native-optical-direction")
        if manifest.get("camera_mount_version", 1) in (2, 3, 4, 5):
            altered = copy.deepcopy(data)
            for camera in altered["cameras"].values():
                camera.pop("rendered_eyes", None)
            reject(lambda: lens_grade(altered, sample), "missing-rendered-eye-baseline")
            altered = copy.deepcopy(data)
            for camera in altered["cameras"].values():
                camera["rendered_eyes"][0][16] += 100
            reject(lambda: lens_grade(altered, sample), "inconsistent-rendered-eye-origin")
        for unlit in [row for row in data["samples"] if row.get("capture_mode") == 1]:
            altered = copy.deepcopy(data)
            for view in altered["views"]:
                view["origin"][0] += 100
            reject(lambda: lens_grade(altered, unlit), "unlit-wrong-retail-lens-origin")
            altered = copy.deepcopy(data)
            for view in altered["views"]:
                view["backward"] = [-value for value in view["backward"]]
            reject(lambda: lens_grade(altered, unlit), "unlit-reversed-retail-lens-axis")
            damaged_unlit = cv2.imread(str(args.directory / f"native-lens-{unlit['sample']}.ppm"))
            damaged_unlit[20, 20, 0] ^= 1
            reject(lambda: lens_grade(data, unlit, damaged_unlit), "unlit-single-rgb-code-error")
        altered = copy.deepcopy(data)
        for row in altered["composition"]:
            row["native_world_camera_effect"] = 1
        reject(lambda: screen_grade(altered), "native-fullscreen-camera-effect")
        for name in ("fullscreen-nv", "missing-screen", "screen-pose"):
            reject(lambda: screen_grade(data, name), name)
    inputs = {args.directory / name for name in (
        "frames.jsonl", "physical-camcorder-events.jsonl", "native-lens-views.jsonl",
        "native-camcorder-composition.jsonl", "native-camcorder-lifecycle.jsonl",
        "native-cameras.jsonl", "native-lens-samples.jsonl", "native-mono-resolve.jsonl",
        "native-run-manifest.json", "capture-complete.txt")}
    for pattern in ("native-lens-*.ppm", "native-mono-reference-*.ppm",
                    "native-screen-before-*.ppm", "frame_*_eye_*.ppm"):
        inputs.update(args.directory.glob(pattern))
    if native_mesh is not None:
        inputs.update(args.directory / name for name in
                      ("native-camera-mesh.jsonl", "native-retail-camera-observation.jsonl"))
        if native_mesh["stock_mount_version"] in (2, 4, 5):
            inputs.add(args.directory / "native-skeleton.jsonl")
        if native_mesh["stock_mount_version"] == 5:
            inputs.add(args.directory / "native-camera-grip-calibration.json")
    if stock_fx is not None:
        for pattern in ("native-postprocess-*", "native-raw-optics*", "native-effect-visibility.jsonl"):
            inputs.update(path for path in args.directory.glob(pattern) if path.is_file())
    report = dict(status="accepted", module_sha256=digest, native_state=state, native_mesh=native_mesh,
                  native_lens_images=lenses, native_stock_fx=stock_fx,
                  stock_control_input_sha256=(
                      {name: hashlib.sha256((args.native_stock_control / name).read_bytes()).hexdigest()
                       for name in ("native-run-manifest.json", "native-retail-camera-observation.jsonl",
                                    "native-postprocess-factory.jsonl")} if stock_fx is not None else None),
                  actual_screen_containment=screens, rejected_controls=controls,
                  input_sha256={path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in sorted(inputs)},
                  verifier_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  mesh_verifier_sha256=hashlib.sha256(
                      Path(__file__).with_name("verify_native_camcorder_mesh.py").read_bytes()).hexdigest(),
                  limitations=__doc__, native_run_manifest_sha256=hashlib.sha256(
                      (args.directory / "native-run-manifest.json").read_bytes()).hexdigest())
    (args.directory / "native-camcorder-proof.json").write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
