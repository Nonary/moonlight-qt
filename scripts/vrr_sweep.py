#!/usr/bin/env python3
"""Concurrent, resumable VRR timing experiments using the native virtual-time lab.

Only configurations and summary results are written, never per-frame output.
Each job is identified by its binary, inputs, full arguments and configuration.
"""

import argparse
import concurrent.futures
import hashlib
import itertools
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time


def digest(path):
    result = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def atomic_json(path, value):
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(value, stream, sort_keys=True, indent=2, allow_nan=False)
        stream.write("\n")
    os.replace(temporary, path)


def frame_count(path):
    data = path.read_bytes()
    if len(data) < 32 or data[:8] != b"MLVRR14\0":
        raise ValueError("invalid capture header")
    version, count, _ = struct.unpack_from("<QQQ", data, 8)
    if version != 1 or count > 32768 or len(data) != 32 + 128 * count:
        raise ValueError("invalid capture length or version")
    frames = set()
    for offset in range(32, len(data), 128):
        _, event, identifier, _, *words = struct.unpack_from("<QQQq12q", data, offset)
        if event == 2 or (event == 9 and words[1] > 0):
            frames.add(identifier)
    return len(frames)


def run_job(job):
    path, manifest, command, resume = job
    if resume and path.exists():
        cached = json.loads(path.read_text())
        if cached.get("manifest") == manifest and cached.get("success"):
            return cached
    start = time.perf_counter()
    process = subprocess.run(command, text=True, capture_output=True, timeout=600, check=False)
    result = {"manifest": manifest, "command": command, "success": process.returncode == 0,
              "wall_seconds": time.perf_counter() - start}
    if process.returncode:
        result.update(returncode=process.returncode, error=process.stderr[-4096:])
    else:
        result["metrics"] = json.loads(process.stdout)
    atomic_json(path, result)
    return result


def aggregate(results):
    groups = {}
    for result in results:
        groups.setdefault(result["manifest"]["config_id"], []).append(result)
    output = []
    for identifier, runs in groups.items():
        entry = {"config_id": identifier, "config": runs[0]["manifest"]["config"],
                 "runs": len(runs), "valid": False}
        if all(run["success"] for run in runs):
            metrics = [run["metrics"] for run in runs]
            if all(m["smoothness_percent"] is not None and m["feedback_coverage_percent"] >= 90 for m in metrics):
                entry.update(valid=True,
                             smoothness_worst=min(m["smoothness_percent"] for m in metrics),
                             latency_p95_worst_ms=max(m["latency_p95_ms"] for m in metrics),
                             latency_p99_worst_ms=max(m["latency_p99_ms"] for m in metrics),
                             drop_worst_percent=max(100 * m["dropped"] / m["input_frames"] for m in metrics),
                             coverage_worst=min(m["feedback_coverage_percent"] for m in metrics))
        output.append(entry)
    return sorted(output, key=lambda item: item["config_id"])


def frontier(entries):
    valid = [entry for entry in entries if entry["valid"]]
    keys = (("smoothness_worst", -1), ("latency_p95_worst_ms", 1), ("drop_worst_percent", 1))
    def dominates(a, b):
        comparisons = [(a[key] * sign, b[key] * sign) for key, sign in keys]
        return all(x <= y for x, y in comparisons) and any(x < y for x, y in comparisons)
    return [entry for entry in valid if not any(dominates(other, entry) for other in valid)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, required=True)
    parser.add_argument("--grid", type=Path, required=True, help="JSON object mapping configuration keys to candidate lists")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--validation-input", type=Path)
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--seeds", default="1,2,3")
    parser.add_argument("--validation-seeds", default="101,102")
    parser.add_argument("--frames", type=int, default=10000)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--environment", type=Path, help="JSON object of simulator options, e.g. {\"display-hz\": 144}")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs must be between 1 and 32")
    grid = json.loads(args.grid.read_text())
    if not isinstance(grid, dict) or not grid or any(not isinstance(v, list) or not v for v in grid.values()):
        parser.error("grid must map keys to nonempty lists")
    keys = sorted(grid)
    candidates = [dict(zip(keys, values)) for values in itertools.product(*(grid[key] for key in keys))]
    if len(candidates) > 4096:
        parser.error("grid exceeds 4096 configurations")
    train_seeds = [int(seed) for seed in args.seeds.split(",")]
    validation_seeds = [int(seed) for seed in args.validation_seeds.split(",")]
    if set(train_seeds) & set(validation_seeds):
        parser.error("training and validation seeds must be disjoint")
    args.lab = args.lab.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    args.output = args.output.resolve()
    environment = json.loads(args.environment.read_text()) if args.environment else {}
    allowed = {"source-hz", "display-hz", "floor-hz", "jitter-us", "render-us", "compositor-us", "feedback-us",
               "wake-us", "feedback-loss-percent", "fixed-display", "host-variance-us", "render-scale", "swapchain-images"}
    if set(environment) - allowed:
        parser.error("unknown environment option")
    provenance = {"schema": 1, "lab_sha256": digest(args.lab), "environment": environment,
                  "input_sha256": digest(args.input) if args.input else None,
                  "validation_input_sha256": digest(args.validation_input) if args.validation_input else None}
    config_paths = {}
    for config in candidates:
        text = "".join(f"{key}={value}\n" for key, value in sorted(config.items()))
        identifier = hashlib.sha256(text.encode()).hexdigest()[:16]
        path = args.output / f"{identifier}.conf"
        path.write_text(text)
        config_paths[identifier] = (config, path)

    split = None
    if args.input and not args.validation_input:
        total = frame_count(args.input)
        split = int(total * 0.7)
        if min(split, total - split) < 128:
            parser.error("capture needs at least 128 frames in each split, or provide --validation-input")

    def jobs_for(identifiers, validation):
        for identifier in identifiers:
            config, path = config_paths[identifier]
            for seed in validation_seeds if validation else train_seeds:
                command = [str(args.lab), "simulate", "--seed", str(seed), "--config", str(path), "--frames", str(args.frames)]
                source = args.validation_input if validation and args.validation_input else args.input
                if source:
                    command.extend(["--input", str(source.resolve())])
                    if split is not None:
                        command.extend(["--skip", str(split)] if validation else ["--limit", str(split)])
                for key, value in sorted(environment.items()):
                    command.extend(["--" + key, str(value)])
                manifest = dict(provenance, config_id=identifier, config=config, seed=seed,
                                validation=validation, frames=args.frames, split=split)
                job_id = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
                yield args.output / f"{job_id}.json", manifest, command, args.resume

    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        # Native subprocesses do the CPU work; threads only schedule/reap jobs.
        training = list(executor.map(run_job, jobs_for(config_paths, False)))
        aggregate_training = aggregate(training)
        finalists = frontier(aggregate_training)
        validation = list(executor.map(run_job, jobs_for([f["config_id"] for f in finalists], True)))
    summary = dict(provenance, jobs=args.jobs, wall_seconds=time.perf_counter() - started,
                   training=aggregate_training, training_frontier=finalists,
                   validation=aggregate(validation),
                   failed_jobs=sum(not run["success"] for run in training + validation))
    atomic_json(args.output / "summary.json", summary)
    print(json.dumps({"configurations": len(config_paths), "training_jobs": len(training),
                      "validation_jobs": len(validation), "frontier": len(finalists),
                      "failed_jobs": summary["failed_jobs"], "wall_seconds": summary["wall_seconds"],
                      "summary": str(args.output / "summary.json")}, sort_keys=True))
    return 1 if summary["failed_jobs"] or not finalists else 0


if __name__ == "__main__":
    raise SystemExit(main())
