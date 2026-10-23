"""Run trace-related regressions and require complete, exact replay evidence."""
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


# Required names make a missing target fail before any reassuring pass count.
# Additional built VRR suites are run too, preserving the broader CI sweep.
REQUIRED_TESTS = {
    "tst_vrrbufferregressions",
    "tst_vrrtracemetrics", "tst_sourcegapsmoothing", "tst_vrrtimingcontroller",
    "tst_vrrpacingworker", "tst_vrrreplayconfig", "tst_vrrratepolicy",
    "tst_inputpercentileintegration", "tst_vrrinputcontracts", "tst_inputaccounting", "tst_inputtracechunks",
    "tst_phasetrajectory", "tst_receivedriftclock", "tst_rollingquantile",
    "tst_vrrcacheidentity", "tst_overlay", "tst_performancestats",
    "tst_incomingframetiming", "tst_presentationclock", "tst_dxgipresent",
    "tst_vrrrenderpolicy", "tst_d3d11bindpolicy", "tst_vrrdiagnostics",
}
QT_TESTS = {"tst_vrrratepolicy", "tst_vrrreplayconfig", "tst_vrrdiagnostics"}
READINESS_FIXTURES = ("ready", "unverified", "waited", "retried")


def require_tests(executables):
    missing = REQUIRED_TESTS - executables.keys()
    if missing:
        raise RuntimeError("Missing trace-contract suites: " + ", ".join(sorted(missing)))


def require_exact(result):
    """A JSON file or zero timestamp drift alone is insufficient evidence."""
    if not isinstance(result, dict):
        raise RuntimeError("Exact replay result must be a JSON object")
    for section in ("fidelity", "capture"):
        if not isinstance(result.get(section), dict):
            raise RuntimeError(f"Exact replay result missing object: {section}")
    required = {
        "fidelity": ("baseline_exact",),
        "capture": ("recorded_sequence_integrity_valid",),
    }
    for section, fields in required.items():
        for field in fields:
            if result.get(section, {}).get(field) is not True:
                raise RuntimeError(f"Exact replay missing or false: {section}.{field}")
    for section in ("timestamp_integrity", "row_semantic_integrity", "session_config_integrity"):
        integrity = result["capture"].get(section)
        if not isinstance(integrity, dict) or integrity.get("valid") is not True:
            raise RuntimeError(f"Exact replay integrity failed: capture.{section}.valid")
    frames = result.get("replayed_frames")
    if type(frames) is not int or frames <= 0:
        raise RuntimeError("Exact replay did not verify any frames")
    capture = result["capture"]
    delivered = capture.get("delivered_frames")
    scheduled = capture.get("scheduled_frames")
    presented = capture.get("presented_frames")
    # The replay loop skips terminal rows that never reached scheduling.
    # Delivered therefore bounds scheduled; replayed_frames is the scheduled
    # population. Scheduled rows may in turn be dropped before presentation.
    if type(delivered) is not int or delivered < 0:
        raise RuntimeError("Exact replay has an invalid capture.delivered_frames")
    if type(scheduled) is not int or scheduled != frames:
        raise RuntimeError("Exact replay frame population contradicts capture.scheduled_frames")
    if delivered < scheduled:
        raise RuntimeError("Exact replay scheduled frame population exceeds delivered frames")
    if type(presented) is not int or presented < 0 or presented > scheduled:
        raise RuntimeError("Exact replay presented frame population is inconsistent")


def run_program(command, log, env, timeout, expected_exit=0):
    start = time.monotonic()
    with log.open("w", encoding="utf-8") as output:
        completed = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                   env=env, timeout=timeout, check=False)
    if completed.returncode != expected_exit:
        raise RuntimeError(f"{Path(command[0]).name} exited {completed.returncode}; see {log}")
    return round(time.monotonic() - start, 3)


def successor_counterexamples(trace, output):
    """Keep capture accounting/hash valid while corrupting reservation evidence."""
    decoder_path = Path(__file__).resolve().parents[2] / "scripts" / "decode-vrr-trace.py"
    spec = importlib.util.spec_from_file_location("trace_contract_decoder", decoder_path)
    decoder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(decoder)
    decoded = io.BytesIO()
    decoder.decode(io.BytesIO(trace.read_bytes()), decoded)
    lines = decoded.getvalue().decode("utf-8").splitlines()
    header = lines[0].split(",")
    rows = [line.split(",") for line in lines[1:-1]]
    if not lines[-1].startswith("#vrr_trace_footer,"):
        raise RuntimeError("Readiness fixture has no clean-close footer")
    column = {name: header.index(name) for name in (
        "frame", "trajectory_resync_dropped", "trajectory_successor_frame", "trajectory_successor_proof")}
    index = next((i for i, row in enumerate(rows)
                  if row[column["trajectory_resync_dropped"]] == "1"), None)
    if index is None:
        raise RuntimeError("Ready fixture did not exercise a successor reservation")
    actual = int(rows[index][column["trajectory_successor_frame"]])
    later = next((int(row[column["frame"]]) for row in rows[index + 1:]
                  if int(row[column["frame"]]) > actual), None)
    if later is None:
        raise RuntimeError("Ready fixture has no later image for the wrong-successor control")
    missing = max(int(row[column["frame"]]) for row in rows) + 1
    paths = []
    for name in ("wrong-successor", "wrong-proof", "missing-successor"):
        changed = [list(row) for row in rows]
        row = changed[index]
        if name == "wrong-proof":
            row[column["trajectory_successor_proof"]] = (
                "1" if row[column["trajectory_successor_proof"]] == "2" else "2")
        else:
            row[column["trajectory_successor_frame"]] = str(later if name == "wrong-successor" else missing)
        body = ",".join(header) + "\n" + "".join(",".join(row) + "\n" for row in changed)
        footer = lines[-1].split(",")
        hashes = [i for i, value in enumerate(footer) if value.startswith("decoded_sha256=")]
        if len(hashes) != 1:
            raise RuntimeError("Readiness fixture is missing its unique decoded hash")
        footer[hashes[0]] = "decoded_sha256=" + hashlib.sha256(body.encode("utf-8")).hexdigest()
        path = output / (name + ".csv")
        path.write_text(body + ",".join(footer) + "\n", encoding="utf-8", newline="\n")
        paths.append(path)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", required=True, type=Path)
    parser.add_argument("--app-dir", type=Path, default=Path(__file__).resolve().parents[2] / "app")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    args = parser.parse_args()
    output_root = args.output_dir.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    # A prior trace/JSON must never satisfy a missing export in the new run.
    output = Path(tempfile.mkdtemp(prefix="run-", dir=output_root))
    summary = {"passed": False, "completed": False, "artifact_directory": str(output),
               "tests": [], "exact_replay": [], "rejected_replay": []}
    (output_root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    try:
        suffix = ".exe" if os.name == "nt" else ""
        binaries = args.bin_dir.resolve()
        executables = {p.stem if suffix else p.name: p for p in binaries.glob("tst_*" + suffix)
                       if p.is_file() and (suffix or os.access(p, os.X_OK))}
        require_tests(executables)
        replay = binaries / ("vrrreplay" + suffix)
        if not replay.is_file():
            raise RuntimeError("Missing vrrreplay executable")
        env = os.environ.copy()
        for key in ("MOONLIGHT_VRR_TEST_FIXED_RESERVE_ONLY", "MOONLIGHT_VRR_TEST_TRAJECTORY_RESERVATION_ONLY",
                    "MOONLIGHT_VRR_TEST_SKIP_RESERVATION"):
            env.pop(key, None)
        fixture_root = output / "readiness"
        env["MOONLIGHT_VRR_TEST_EXPORT_FIXED_RESERVE_TRACE"] = str(fixture_root)
        for name, program in sorted(executables.items()):
            command = [str(program)]
            if name == "tst_overlay":
                command.append(str(args.app_dir.resolve()))
            elif name in QT_TESTS:
                command += ["-o", "-,txt"]
            print(f"Running {name}", flush=True)
            elapsed = run_program(command, output / (name + ".log"), env, args.timeout_seconds)
            summary["tests"].append({"name": name, "exit_code": 0, "elapsed_seconds": elapsed})
        run_program([str(replay), "--help"], output / "replay-help.txt", env, args.timeout_seconds)
        if not (output / "replay-help.txt").stat().st_size:
            raise RuntimeError("Replay help output is empty")
        for kind in READINESS_FIXTURES:
            trace = Path(str(fixture_root) + f"-{kind}.vrrtrace")
            if not trace.is_file() or not trace.stat().st_size:
                raise RuntimeError(f"Worker did not export the {kind} readiness fixture")
            result_path = output / f"replay-{kind}.json"
            run_program([str(replay), str(trace), "--require-exact-baseline", "--output", str(result_path)],
                        output / f"replay-{kind}.log", env, args.timeout_seconds)
            result = json.loads(result_path.read_text(encoding="utf-8-sig"))
            require_exact(result)
            if result["capture"].get("trace") != trace.name:
                raise RuntimeError(f"Exact replay result belongs to another trace: expected {trace.name}")
            summary["exact_replay"].append({"fixture": kind, "passed": True,
                                             "replayed_frames": result["replayed_frames"]})
        for trace in successor_counterexamples(Path(str(fixture_root) + "-ready.vrrtrace"), output):
            result_path = trace.with_suffix(".json")
            run_program([str(replay), str(trace), "--require-exact-baseline", "--output", str(result_path)],
                        trace.with_suffix(".log"), env, args.timeout_seconds, expected_exit=3)
            result = json.loads(result_path.read_text(encoding="utf-8-sig"))
            fidelity = result.get("fidelity", {})
            if (fidelity.get("baseline_exact") is not False or
                    type(fidelity.get("invalid_controller_lifecycle_rows")) is not int or
                    fidelity["invalid_controller_lifecycle_rows"] <= 0):
                raise RuntimeError("Corrupt successor fixture did not fail its lifecycle audit")
            capture = result.get("capture", {})
            if (capture.get("trace") != trace.name or
                    capture.get("clean_close_footer", {}).get("decoded_sha256_matches") is not True):
                raise RuntimeError("Corrupt successor fixture failed input identity or content accounting")
            summary["rejected_replay"].append({"fixture": trace.stem, "rejected": True, "exit_code": 3})
        summary["passed"] = True
    except (RuntimeError, OSError, ValueError, TypeError, AttributeError,
            subprocess.TimeoutExpired) as error:
        summary["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        # `completed` means the full gate, including all exact fixture replays,
        # reached its successful end. A handled failure leaves a durable failed
        # summary rather than looking like a completed gate with a late error.
        summary["completed"] = summary["passed"]
        for directory in (output, output_root):
            (directory / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    if summary["passed"]:
        print(f"Passed {len(summary['tests'])} suites, four exact readiness replays and three corrupt-proof rejections")
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
