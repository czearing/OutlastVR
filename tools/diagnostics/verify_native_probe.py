"""Grade the isolated native brightness-menu capture, not arbitrary gameplay.

Executed draw counts detect duplicated converted submissions, not shader coverage
or GPU cost. Feature parallax rejects mono/flat shifts, not all rendering errors.
Menu rays test the two-meter plane; triangulated world rays test the animated
native camera, world scale, tracking and FOV in this scene, not headset comfort.
Descriptor mismatches can be outliers; percentile metrics cannot exclude isolated
bad pixels. Missing texture or correspondence is a failure, never a zero disparity.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path

import cv2
import numpy as np


def require(value, message):
    if not value:
        raise ValueError(message)


def rotation(orientation):
    x, y, z, w = orientation
    norm = math.sqrt(x*x + y*y + z*z + w*w)
    require(abs(norm - 1) < 1e-4, "Nonunit submitted orientation")
    return np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
        [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)],
    ])


def correspondences(left, right, contrast=.006):
    sift = cv2.SIFT_create(contrastThreshold=contrast)
    keys_l, desc_l = sift.detectAndCompute(left, None)
    keys_r, desc_r = sift.detectAndCompute(right, None)
    require(desc_l is not None and desc_r is not None, "Missing textured eye features")
    matcher = cv2.BFMatcher()
    forward = matcher.knnMatch(desc_l, desc_r, k=2)
    reverse = matcher.knnMatch(desc_r, desc_l, k=2)
    valid_reverse = {a.queryIdx: a.trainIdx for pair in reverse if len(pair) == 2
                     for a, b in [pair] if a.distance < .7*b.distance}
    points = []
    for pair in forward:
        if len(pair) != 2:
            continue
        a, b = pair
        if a.distance < .7*b.distance and valid_reverse.get(a.trainIdx) == a.queryIdx:
            points.append((*keys_l[a.queryIdx].pt, *keys_r[a.trainIdx].pt))
    require(len(points) >= 20, "Insufficient reciprocal eye correspondences")
    return np.array(points)


def refine_correspondences(left, right, points):
    start = points[:, :2].astype(np.float32).reshape(-1, 1, 2)
    initial = points[:, 2:].astype(np.float32).reshape(-1, 1, 2)
    settings = dict(winSize=(15, 15), maxLevel=2,
                    criteria=(cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 30, .01),
                    flags=cv2.OPTFLOW_USE_INITIAL_FLOW)
    later, valid, residual = cv2.calcOpticalFlowPyrLK(left, right, start, initial.copy(), **settings)
    require(later is not None and valid is not None and residual is not None,
            "Optical-flow correspondence localization failed")
    earlier, reverse_valid, _ = cv2.calcOpticalFlowPyrLK(right, left, later, start.copy(), **settings)
    require(earlier is not None and reverse_valid is not None,
            "Reverse correspondence localization failed")
    # Photometric and round-trip gates must not select for horizontal epipolar motion.
    reliable = ((valid[:, 0] != 0) & (reverse_valid[:, 0] != 0) &
                (np.linalg.norm(earlier[:, 0]-start[:, 0], axis=1) < .5) &
                (residual[:, 0] < 8))
    return np.column_stack((start[reliable, 0], later[reliable, 0]))


def menu_projection(points, views, width, height, anchor_views=None):
    matrices = [rotation(v["orientation"]) for v in views]
    positions = [np.array(v["position"]) for v in views]
    require(np.max(np.abs(matrices[0]-matrices[1])) < 1e-5,
            "This native probe grader requires parallel scripted eyes")
    anchor_views = views if anchor_views is None else anchor_views
    head = rotation(anchor_views[0]["orientation"])
    center = (np.array(anchor_views[0]["position"])+anchor_views[1]["position"])*.5 + head @ np.array([0., 0., -2.])
    normal = head @ np.array([0., 0., 1.])
    l, r, up, down = np.tan(views[0]["fov"])
    x = l + (points[:, 0]+.5)/width*(r-l)
    y = up - (points[:, 1]+.5)/height*(up-down)
    rays = np.column_stack((x, y, -np.ones(len(points)))) @ matrices[0].T
    times = np.dot(center-positions[0], normal)/(rays @ normal)
    require(np.all(times > 0), "Menu ray misses its forward plane")
    world = positions[0] + rays*times[:, None]
    local = (world-positions[1]) @ matrices[1]
    l, r, up, down = np.tan(views[1]["fov"])
    predicted = np.column_stack((
        (-local[:, 0]/local[:, 2]-l)/(r-l)*width-.5,
        (up+local[:, 1]/local[:, 2])/(up-down)*height-.5,
    ))
    return np.linalg.norm(predicted-points[:, 2:4], axis=1)


def panel_grade(left, right, views, anchor_views):
    require(left is not None and right is not None and left.shape == right.shape and
            left.shape in ((512, 512), (1024, 1024), (2448, 2448), (2880, 2688)),
            "Missing or incorrect native panel eye extent")
    require(min(float(im.std()) for im in (left, right)) >= 5,
            "Native menu panel is black or unmeasurably flat")
    points = refine_correspondences(left, right, correspondences(left, right))
    require(len(points) >= 100, "Insufficient native full-menu panel correspondences")
    errors = menu_projection(points, views, left.shape[1], left.shape[0], anchor_views)
    median, p90 = map(float, np.percentile(errors, [50, 90]))
    require(median < .25 and p90 < 1.5,
            "Native panel rays disagree with the initial fixed two-meter world plane")
    return dict(matches=len(points), fixed_panel_ray_median_error_px=median,
                fixed_panel_ray_error90_px=p90)


def gameplay_grade(left, right, views):
    require(len(views) == 2, "Missing submitted stereo eye poses")
    matrices = [rotation(view["orientation"]) for view in views]
    baseline = (np.array(views[1]["position"])-views[0]["position"]) @ matrices[0]
    require(np.max(np.abs(matrices[0]-matrices[1])) < 1e-5 and
            baseline[0] > 0 and np.max(np.abs(baseline[1:])) < 1e-5 and
            np.max(np.abs(np.array(views[0]["fov"])-views[1]["fov"])) < 1e-5,
            "Horizontal stereo grader requires a rectified submitted eye rig")
    require(left is not None and right is not None and left.shape == right.shape and
            left.shape in ((512, 512), (1024, 1024), (2448, 2448), (2880, 2688)),
            "Missing or incorrect native gameplay eye extent")
    scale = left.shape[0]/512
    horizontal_scale = left.shape[1]/512
    texture = [float(im.std()) for im in (left, right)]
    require(min(texture) >= 1, "Gameplay capture is black or in an unmeasurable loading fade")
    for contrast in (.006, .002):
        raw = correspondences(left, right, contrast)
        points = refine_correspondences(left, right, raw)
        disparity = points[:, 0]-points[:, 2]
        vertical = np.abs(points[:, 1]-points[:, 3])
        # Retry dark-region detection, not the geometry or alignment criteria.
        world = (disparity > .5*horizontal_scale) & (points[:, 1] > 150*scale) & (points[:, 1] < 450*scale)
        if int(world.sum()) >= 20:
            break
    raw_world = ((raw[:, 0]-raw[:, 2] > .5*horizontal_scale) &
                 (raw[:, 1] > 150*scale) & (raw[:, 1] < 450*scale))
    raw_vertical90 = (float(np.percentile(np.abs(raw[raw_world, 1]-raw[raw_world, 3]), 90))
                      if raw_world.any() else None)
    # Foreground UI and the mirrored rear-view display are not depth correspondences.
    require(int(world.sum()) >= 20, "Insufficient textured gameplay world correspondences")
    disparity, vertical = disparity[world], vertical[world]
    spread = float(np.percentile(disparity, 90)-np.percentile(disparity, 10))
    require(spread > horizontal_scale, "Gameplay lacks measurable depth-varying parallax")
    require(float(np.percentile(vertical, 90)) < 2,
            "Gameplay world has incorrect vertical stereo alignment")
    return dict(world_matches=int(world.sum()), texture_std=texture, feature_contrast_threshold=contrast,
                eye_extent=left.shape[0], eye_width=left.shape[1], eye_height=left.shape[0], vertical90_limit_px=2,
                raw_correspondences=len(raw), refined_correspondences=len(points),
                raw_world_matches=int(raw_world.sum()), raw_world_vertical90_px=raw_vertical90,
                world_disparity_median_px=float(np.median(disparity)),
                world_parallax_spread_px=spread,
                world_vertical90_px=float(np.percentile(vertical, 90)))


def recommended_extent_grade(projected, eye_size, headset_extent=False):
    require(type(headset_extent) is bool and
            ((headset_extent and eye_size is None) or
             (not headset_extent and type(eye_size) is int and eye_size in (1024, 2448))),
            "Unsupported declared native diagnostic eye extent")
    width, height = (2688, 2880) if headset_extent else (eye_size, eye_size)
    require(projected and all(
        (frame["eye_width"], frame["eye_height"]) == (width, height) and
        (frame["source_width"], frame["source_height"]) == (width, height)
        for frame in projected), "Actual engine viewport did not follow the recommended XR extent")
    gameplay = [frame for frame in projected if not frame["world_panel"]]
    require(not gameplay or (len(gameplay) >= 900 and
            projected[-1]["direct_xr_submissions"] >= len(gameplay)-1),
            "Actual exact-extent gameplay retained redundant stereo rescaling")
    return dict(source_width=width, source_height=height,
                native_gameplay_projections=len(gameplay),
                direct_atlas_submissions_before_final_frame=projected[-1]["direct_xr_submissions"],
                actual_paired_draws=sum(draw_grade(frame) for frame in projected),
                physical_headset_fps_measured=False)


def presentation_grade(evidence, projected, scenario, selftest, require_all_gameplay=False):
    manifest = json.loads((evidence/"native-run-manifest.json").read_text(encoding="utf-8-sig"))
    render_size = (recommended_extent_grade(projected, manifest.get("diagnostic_eye_size", 1024),
                                            manifest.get("headset_extent_2688x2880", False))
                   if manifest.get("recommended_resolution") else None)
    draws = sum(draw_grade(frame) for frame in projected)
    anchor = projected[0]["views"]
    menu = [f for f in projected if f["world_panel"] and f["captured"] and f["capture_serial"] % 30 == 0]
    require(len(menu) >= 4, "Missing scheduled native full-menu captures")
    def images(frame):
        result = [cv2.imread(str(evidence/f"frame_{frame['frame']}_eye_{eye}.ppm"),
                             cv2.IMREAD_GRAYSCALE) for eye in range(2)]
        require(all(image is not None and image.shape == (frame["eye_height"], frame["eye_width"])
                    for image in result), "Captured images disagree with the submitted native eye extents")
        return result
    panel_results = [dict(frame=f["frame"], capture_serial=f["capture_serial"],
                          **panel_grade(*images(f), f["views"], anchor)) for f in menu[:10]]
    gameplay = [f for f in projected if not f["world_panel"]]
    gameplay_results, unmeasurable = [], []
    if scenario == "continue":
        require(len(gameplay) >= 300, "Native Continue did not complete 300 gameplay projections")
        if require_all_gameplay:
            require(all(frame["captured"] for frame in gameplay if frame["capture_serial"] % 30 == 0),
                    "Missing scheduled strict gameplay image capture")
        for frame in gameplay:
            if not frame["captured"]:
                continue
            try:
                result = gameplay_grade(*images(frame), frame["views"])
            except ValueError as error:
                unmeasurable.append(dict(frame=frame["frame"], reason=str(error)))
                continue
            gameplay_results.append(dict(frame=frame["frame"], **result))
        require(len(gameplay_results) >= 4, "Fewer than four measurable stereo gameplay pairs")
    else:
        require(not gameplay, "Menu-only capture unexpectedly entered gameplay")
    controls = []
    if selftest:
        frame = menu[-1]
        left, right = images(frame)
        controls.append(reject(lambda: panel_grade(left, left, frame["views"], anchor), "mono-panel"))
        wrong = copy.deepcopy(anchor)
        for view in wrong:
            view["position"][2] += 1
        controls.append(reject(lambda: panel_grade(left, right, frame["views"], wrong), "incorrect-panel-depth"))
        duplicate = copy.deepcopy(frame)
        duplicate["driver_draws"].append(next(d for d in duplicate["driver_draws"]
                                             if d["indexed"] and d["instances"] == 2))
        controls.append(reject(lambda: draw_grade(duplicate), "duplicate-executed-draw"))
        if gameplay_results:
            frame = next(f for f in gameplay if f["frame"] == gameplay_results[-1]["frame"])
            left, right = images(frame)
            controls.append(reject(lambda: gameplay_grade(left, left, frame["views"]), "mono-gameplay"))
            shifted = cv2.warpAffine(left, np.float32([[1, 0, -8*left.shape[1]/512], [0, 1, 0]]),
                                     left.shape[::-1])
            controls.append(reject(lambda: gameplay_grade(left, shifted, frame["views"]), "flat-shift-gameplay"))
            vertical = cv2.warpAffine(right, np.float32([[1, 0, 0], [0, 1, 5]]), right.shape[::-1])
            controls.append(reject(lambda: gameplay_grade(left, vertical, frame["views"]), "vertical-eye-error"))
            for sample in gameplay_results:
                frame = next(f for f in gameplay if f["frame"] == sample["frame"])
                left, right = images(frame)
                for offset in (-5, 5):
                    vertical = cv2.warpAffine(right, np.float32([[1, 0, 0], [0, 1, offset]]), right.shape[::-1])
                    controls.append(reject(lambda: gameplay_grade(left, vertical, frame["views"]),
                                           f"vertical-eye-error-frame-{frame['frame']}-{offset:+d}"))
    report = dict(status="fail" if require_all_gameplay and unmeasurable else "pass",
                  scope=f"native {scenario}; no headset output",
                  require_all_gameplay=require_all_gameplay,
                  projected_frames=len(projected), gameplay_frames=len(gameplay),
                  executed_converted_draws=draws, panel_frames=panel_results,
                  native_recommended_render_size=render_size,
                  gameplay_pairs=gameplay_results, ungraded_gameplay_pairs=unmeasurable,
                  rejected_controls=controls,
                  correspondence_method="reciprocal SIFT; bidirectional subpixel LK; photometric/round-trip gates only",
                  grader_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  render_profile=render_profile(projected),
                  limitations="Correspondences cannot exclude isolated pixel/effect errors, unvisited scenes, "
                  "or headset discomfort. Every ungraded pair and its reason is retained; a sampled-pair pass "
                  "does not certify those pairs. Subpixel refinement does not filter vertical motion, "
                  "but ambiguous or photometrically inconsistent features can still be excluded. "
                  "Draw counts cannot certify GPU cost or frame pacing.")
    (evidence/"native-presentation-proof.json").write_text(json.dumps(report, indent=2)+"\n")
    require(not require_all_gameplay or not unmeasurable,
            f"Strict gameplay stereo acceptance failed for {len(unmeasurable)} captured pairs; see retained report")
    print(f"PASS: {len(projected)} projections; {len(gameplay)} gameplay; {draws} executed paired draws; "
          f"{len(panel_results)} panel and {len(gameplay_results)} gameplay pairs; {len(controls)} rejected controls")


def render_profile(projected):
    if not all("cpu_render_us" in frame for frame in projected):
        return dict(status="unavailable", reason="This historical capture has no rendering timers")
    summarize = lambda values: dict(zip(("p50_ms", "p90_ms", "p99_ms"),
                                       map(float, np.percentile(values, [50, 90, 99])/1000)))
    result = dict(status="measured_not_cadence_certified",
                  source_extents=sorted({(f["source_width"], f["source_height"]) for f in projected}),
                  scope="CPU render work starts after XR wait and ends before XR transport. "
                  "Asynchronous GPU timestamps cover that rendering interval, not compositor or XR copy cost. "
                  "The instrumented no-headset runtime is not a headset FPS benchmark.")
    for panel, label in ((True, "menu"), (False, "gameplay")):
        frames = [f for f in projected if f["world_panel"] == panel][30:]
        if not frames:
            continue
        gpu = {f["gpu_timing_frame"]: f["gpu_work_us"] for f in frames if f["gpu_timing_status"] == 2}
        result[label] = dict(frames=len(frames), cpu_render=summarize([f["cpu_render_us"] for f in frames]),
                             xr_wait=summarize([f["xr_wait_us"] for f in frames]),
                             gpu_timing_states={str(state): sum(f["gpu_timing_status"] == state for f in frames)
                                                for state in sorted({f["gpu_timing_status"] for f in frames})},
                             gpu_samples=len(gpu),
                             gpu_render=summarize(list(gpu.values())) if gpu else None,
                             shader_variants_max=max(f["shader_variants"] for f in frames))
    return result


def image_grade(left, right, views):
    require(left is not None and right is not None and left.shape == right.shape == (512, 512),
            "Missing or incorrect native eye extent")
    texture = [float(np.std(im[190:350, 150:350])) for im in (left, right)]
    require(min(texture) >= 1., "Native world region is black or unmeasurably flat")
    points = correspondences(left, right)
    world = points[(points[:, 1] > 190) & (points[:, 1] < 350)]
    menu = points[(points[:, 0] > 110) & (points[:, 0] < 375) &
                  (points[:, 1] > 162) & (points[:, 1] < 174)]
    require(len(world) >= 30 and len(menu) >= 8, "Native world/menu correspondence is inconclusive")
    disparity = world[:, 0]-world[:, 2]
    vertical = np.abs(world[:, 1]-world[:, 3])
    spread = float(np.percentile(disparity, 90)-np.percentile(disparity, 10))
    require(float(np.median(disparity)) > .3, "Native world is mono or eye order is wrong")
    require(spread > 1., "Native world lacks measurable depth-varying parallax")
    vertical90 = float(np.percentile(vertical, 90))
    require(vertical90 < 2., "Native world has incorrect vertical stereo alignment")
    errors = menu_projection(menu, views, 512, 512)
    menu_error = float(np.median(errors))
    require(menu_error < .75, "Native menu disagrees with the two-meter plane or submitted FOV")
    return dict(world_matches=len(world), menu_matches=len(menu), texture_std=texture,
                world_disparity_median_px=float(np.median(disparity)),
                world_parallax_spread_px=spread, world_vertical90_px=vertical90,
                menu_ray_median_error_px=menu_error)


def world_motion_grade(images, start, end, camera_start, camera_end, tracking=True):
    def native_camera(camera, view):
        matrix = np.array(camera["matrix"]).reshape(4, 4).T
        rows = [matrix[row, :3]/np.linalg.norm(matrix[row, :3]) for row in (0, 1, 3)]
        basis = np.column_stack((rows[0], rows[1], -rows[2]))
        require(np.max(np.abs(basis.T @ basis-np.eye(3))) < 1e-4,
                "Native camera evidence is not an orthogonal view basis")
        if tracking:
            initial_yaw = .08*math.sin(.005)
            initial = rotation([0, math.sin(initial_yaw/2), 0, math.cos(initial_yaw/2)])
            basis = basis @ initial.T @ rotation(view["orientation"])
        return basis, np.array(camera["origin"])

    for frame in (start, end):
        require(all(abs(v["fov"][0]+v["fov"][1]) < 1e-6 and
                    abs(v["fov"][2]+v["fov"][3]) < 1e-6 for v in frame["views"]),
                "Triangulated native probe requires its symmetric scripted FOV")
    basis_start, origin_start = native_camera(camera_start, start["views"][0])
    basis_end, origin_end = native_camera(camera_end, end["views"][0])
    stereo = correspondences(*images[:2])
    temporal = correspondences(images[0], images[2])
    key = lambda point: tuple(np.round(point[:2], 3))
    lookup = {key(point): point for point in stereo}
    fx = 512/(2*math.tan(start["views"][0]["fov"][1]))
    fy = 512/(2*math.tan(start["views"][0]["fov"][2]))
    end_fx = 512/(2*math.tan(end["views"][0]["fov"][1]))
    end_fy = 512/(2*math.tan(end["views"][0]["fov"][2]))
    errors = []
    for point in temporal:
        if not 190 < point[1] < 350 or key(point) not in lookup:
            continue
        eye_pair = lookup[key(point)]
        disparity = eye_pair[0]-eye_pair[2]
        if disparity < .5 or abs(eye_pair[1]-eye_pair[3]) > 1:
            continue
        depth = fx*.064/disparity
        local = np.array([(eye_pair[0]-255.5)/fx*depth-.032,
                          (255.5-eye_pair[1])/fy*depth, -depth])
        world = origin_start + basis_start @ local*100
        later = basis_end.T @ (world-origin_end)/100
        later[0] += .032
        require(later[2] < 0, "Triangulated native geometry moved behind the camera")
        predicted = [255.5-end_fx*later[0]/later[2], 255.5+end_fy*later[1]/later[2]]
        errors.append(float(np.linalg.norm(np.array(predicted)-point[2:])))
    require(len(errors) >= 8, "Native temporal world-ray correspondence is inconclusive")
    median = float(np.median(errors))
    require(median < 2., "Native tracked world rays disagree with actual rendered pixels")
    return dict(start_frame=start["frame"], end_frame=end["frame"],
                triangulated_points=len(errors), median_world_ray_error_px=median,
                world_ray_error90_px=float(np.percentile(errors, 90)))


def draw_grade(frame):
    views = frame["views"]
    require(len(views) == 2, "Native submission does not contain two views")
    ipd = float(np.linalg.norm(np.array(views[0]["position"])-views[1]["position"]))
    require(abs(ipd-.064) < 1e-5, "Native submitted IPD is incorrect")
    converted = [d for d in frame["driver_draws"]
                 if d["indexed"] and d["instances"] >= 2 and not d["instances"] % 2]
    require(len(converted) == frame["logical_geometry_draws"] and len(converted) > 0,
            "Executed converted draws disagree with one submission per logical geometry draw")
    require((frame["eye_width"], frame["eye_height"]) in
            ((512, 512), (1024, 1024), (2448, 2448), (2688, 2880)),
            "Native submission has incorrect eye extents")
    return len(converted)


def reject(action, name):
    try:
        action()
    except ValueError as error:
        return dict(control=name, rejected=str(error))
    raise ValueError(f"Native grader incorrectly accepted its {name} control")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--scenario", choices=("brightness", "menu", "continue"), default="brightness")
    parser.add_argument("--require-all-gameplay", action="store_true")
    args = parser.parse_args()
    require(not args.require_all_gameplay or args.scenario == "continue",
            "Strict gameplay acceptance requires the Continue scenario")
    require((args.evidence/"capture-complete.txt").exists(), "Native capture did not finish")
    with (args.evidence/"frames.jsonl").open() as stream:
        frames = [json.loads(line) for line in stream]
    projected = [f for f in frames if not f.get("canceled")]
    require(len(projected) >= 299, "Native capture is truncated")
    if args.scenario != "brightness":
        presentation_grade(args.evidence, projected, args.scenario, args.selftest, args.require_all_gameplay)
        return
    draws = sum(draw_grade(f) for f in projected)
    selected = {f["frame"]: f for f in projected if f["frame"] % 30 == 0}
    require(set(range(30, 301, 30)) <= selected.keys(), "Missing scheduled native eye captures")
    results = []
    for serial in range(30, 301, 30):
        frame = selected[serial]
        images = [cv2.imread(str(args.evidence/f"frame_{serial}_eye_{eye}.ppm"),
                             cv2.IMREAD_GRAYSCALE) for eye in range(2)]
        try:
            result = image_grade(*images, frame["views"])
        except ValueError as error:
            raise ValueError(f"Native frame {serial}: {error}") from error
        results.append(dict(frame=serial, **result))
    with (args.evidence/"native-cameras.jsonl").open() as stream:
        cameras = {c["native_frame"]: c for c in map(json.loads, stream)}
    motion = []
    for first, last in ((30, 120), (60, 240), (120, 300)):
        images = [cv2.imread(str(args.evidence/f"frame_{f}_eye_{eye}.ppm"), cv2.IMREAD_GRAYSCALE)
                  for f, eye in ((first, 0), (first, 1), (last, 0))]
        motion.append(world_motion_grade(images, selected[first], selected[last],
                                         cameras[selected[first]["native_frame"]],
                                         cameras[selected[last]["native_frame"]]))
    controls = []
    if args.selftest:
        frame = selected[120]
        images = [cv2.imread(str(args.evidence/f"frame_120_eye_{eye}.ppm"),
                             cv2.IMREAD_GRAYSCALE) for eye in range(2)]
        controls.append(reject(lambda: image_grade(images[0], images[0], frame["views"]), "mono"))
        shifted = cv2.warpAffine(images[0], np.float32([[1, 0, -8], [0, 1, 0]]), (512, 512))
        controls.append(reject(lambda: image_grade(images[0], shifted, frame["views"]), "flat-shift"))
        controls.append(reject(lambda: image_grade(images[0], np.zeros_like(images[0]), frame["views"]),
                               "black-right-world"))
        duplicate = copy.deepcopy(frame)
        duplicate["driver_draws"].append(next(d for d in duplicate["driver_draws"]
                                             if d["indexed"] and d["instances"] == 2))
        controls.append(reject(lambda: draw_grade(duplicate), "duplicate-executed-draw"))
        changed = copy.deepcopy(frame["views"])
        for view in changed:
            view["fov"] = [f*.85 for f in view["fov"]]
        controls.append(reject(lambda: image_grade(*images, changed), "dishonest-submitted-FOV"))
        first, last = selected[30], selected[120]
        images = [cv2.imread(str(args.evidence/f"frame_{f}_eye_{eye}.ppm"), cv2.IMREAD_GRAYSCALE)
                  for f, eye in ((30, 0), (30, 1), (120, 0))]
        controls.append(reject(lambda: world_motion_grade(
            images, first, last, cameras[first["native_frame"]], cameras[last["native_frame"]],
            tracking=False), "absent-head-tracking"))
    report = dict(status="pass", scope="native startup brightness-menu scene; no headset output",
                  projected_frames=len(projected), executed_converted_draws=draws,
                  sampled_frames=results, rejected_controls=controls,
                  triangulated_world_motion=motion,
                  limitations=__doc__)
    (args.evidence/"native-stereo-proof.json").write_text(json.dumps(report, indent=2)+"\n")
    print(f"PASS: {len(projected)} native projections, {draws} executed converted draws;"
          f" 10 measured stereo pairs; {len(controls)} rejected controls")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError) as error:
        raise SystemExit(f"FAIL: {error}") from error
