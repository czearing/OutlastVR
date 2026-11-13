"""Grade a normalized, complete headset timing CSV; never collect or launch a game."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path


COLUMNS = ("frame_id", "elapsed_ms", "app_cpu_ms", "app_gpu_ms", "dropped", "reprojected")


def require(value, message):
    if not value:
        raise ValueError(message)


def percentile(values, percent):
    ordered = sorted(values)
    return ordered[math.ceil(len(ordered)*percent/100)-1]


def grade(rows, refresh_hz):
    require(math.isfinite(refresh_hz) and 30 <= refresh_hz <= 240, "Invalid headset refresh rate")
    require(len(rows) >= 2, "Missing headset timing samples")
    previous = None
    for row in rows:
        require(set(row) == set(COLUMNS), "Unexpected timing CSV columns")
        require(type(row["frame_id"]) is int and row["frame_id"] >= 0, "Invalid frame identity")
        for column in ("elapsed_ms", "app_cpu_ms", "app_gpu_ms"):
            require(math.isfinite(row[column]) and row[column] >= 0, f"Invalid {column}")
        require(all(type(row[c]) is int and row[c] in (0, 1) for c in ("dropped", "reprojected")),
                "Invalid dropped/reprojected flag")
        if previous is not None:
            require(row["frame_id"] == previous["frame_id"]+1, "Missing, duplicate or reordered frame")
            require(row["elapsed_ms"] > previous["elapsed_ms"], "Non-increasing timing clock")
        previous = row
    duration = rows[-1]["elapsed_ms"]-rows[0]["elapsed_ms"]
    require(duration >= 120000, "Capture must cover at least 120 seconds after warmup")
    budget = 1000/refresh_hz
    # The input contains one row per display interval, including misses/reprojection.
    require(abs(duration/(len(rows)-1)-budget) <= budget*.02,
            "Capture cadence differs from the declared refresh rate")
    cpu = [r["app_cpu_ms"] for r in rows]
    gpu = [r["app_gpu_ms"] for r in rows]
    require(any(cpu) and any(gpu), "Capture lacks measured application CPU/GPU work")
    cadence = [rows[n]["elapsed_ms"]-rows[n-1]["elapsed_ms"] for n in range(1, len(rows))]
    missed = sum(bool(r["dropped"] or r["reprojected"]) for r in rows)/len(rows)
    metrics = dict(cpu_p95_ms=percentile(cpu, 95), cpu_p99_ms=percentile(cpu, 99),
                   gpu_p95_ms=percentile(gpu, 95), gpu_p99_ms=percentile(gpu, 99),
                   cpu_max_ms=max(cpu), gpu_max_ms=max(gpu),
                   display_interval_p99_ms=percentile(cadence, 99),
                   display_interval_max_ms=max(cadence), missed_fraction=missed)
    failures = []
    for stage in ("cpu", "gpu"):
        if metrics[f"{stage}_p95_ms"] > budget*.85:
            failures.append(f"{stage} p95 exceeds 15% headroom target")
        if metrics[f"{stage}_p99_ms"] > budget:
            failures.append(f"{stage} p99 exceeds refresh budget")
    if metrics["display_interval_p99_ms"] > budget*1.10:
        failures.append("display interval p99 exceeds pacing tolerance")
    if missed > .01:
        failures.append("dropped/reprojected intervals exceed 1%")
    if max(metrics["cpu_max_ms"], metrics["gpu_max_ms"], metrics["display_interval_max_ms"]) > budget*2:
        failures.append("steady-route stall exceeds two refresh intervals")
    return dict(status="passed" if not failures else "failed", failures=failures,
                samples=len(rows), duration_ms=duration, refresh_hz=refresh_hz,
                budget_ms=budget, p95_target_ms=budget*.85, **metrics)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--refresh-hz", required=True, type=float)
    args = parser.parse_args()
    with args.capture.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        require(reader.fieldnames == list(COLUMNS), "Unexpected timing CSV header")
        rows = []
        for row in reader:
            require(set(row) == set(COLUMNS) and None not in row.values(), "Incomplete or oversized CSV row")
            rows.append(dict(frame_id=int(row["frame_id"]), elapsed_ms=float(row["elapsed_ms"]),
                             app_cpu_ms=float(row["app_cpu_ms"]), app_gpu_ms=float(row["app_gpu_ms"]),
                             dropped=int(row["dropped"]), reprojected=int(row["reprojected"])))
    result = grade(rows, args.refresh_hz)
    result["capture_sha256"] = hashlib.sha256(args.capture.read_bytes()).hexdigest()
    result["scope"] = "Normalized headset trace only; provenance and visual acceptance are separate."
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
