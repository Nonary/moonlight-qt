#!/usr/bin/env python3
"""Run the production C++ VRR simulator with an independent capture baseline.

Requires Python 3.9+ and vrrreplay; does not require Windows or a display.
All artifacts go into a new directory. A successful simulation is not proof
of a physical display fix: this model retains recorded renderer service.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import operator
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def identity(path: Path) -> dict:
    path = path.resolve(strict=True)
    before = path.stat()
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError(f"input changed while hashing: {path}")
    return {
        "path": str(path), "bytes": after.st_size,
        "mtime_ns": after.st_mtime_ns,
        "modified_utc": dt.datetime.fromtimestamp(
            after.st_mtime, dt.timezone.utc).isoformat(),
        "sha256": digest.hexdigest(),
    }


def write_json(path: Path, value: object) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def at(value: dict, dotted: str):
    for name in dotted.split("."):
        if not isinstance(value, dict) or name not in value:
            return None
        value = value[name]
    return value


def number(value):
    try:
        return value if type(value) in (int, float) and math.isfinite(value) else None
    except OverflowError:
        return None


def read_json(path: Path):
    def reject_constant(value):
        raise ValueError(f"non-finite JSON number: {value}")
    return json.loads(path.read_text(encoding="utf-8"), parse_constant=reject_constant)


def exact_baseline(run: dict, summary: dict) -> bool:
    return (run.get("exit_code") == 0
            and "output_error" not in run
            and at(summary, "capture.recorded_sequence_integrity_valid") is True
            and at(summary, "fidelity.baseline_exact") is True)


def review_status(current: int, new: int) -> int:
    # Execution/provenance failure outranks an inexact capture, which outranks
    # an individual scenario assertion or evidence-gate failure.
    priority = {0: 0, 4: 1, 3: 2, 2: 3}
    return new if priority[new] > priority[current] else current


def checked_assertions(summary: dict) -> tuple[bool, bool, bool]:
    assertions = summary.get("assertions")
    results = at(summary, "assertions.results")
    if (not isinstance(assertions, dict) or assertions.get("configured") is not True
            or assertions.get("passed") is not True
            or not isinstance(results, list) or not results):
        return False, False, False
    operations = {"<": operator.lt, "<=": operator.le, "==": operator.eq,
                  ">=": operator.ge, ">": operator.gt}
    interval_bound = latency_bound = False
    for assertion in results:
        if not isinstance(assertion, dict):
            return False, False, False
        metric = assertion.get("metric")
        operation = assertion.get("operator")
        actual = number(assertion.get("actual"))
        expected = number(assertion.get("expected"))
        if (not isinstance(metric, str) or not isinstance(operation, str)
                or operation not in operations or actual is None or expected is None
                or assertion.get("passed") is not True
                or actual != number(at(summary, metric))
                or not operations[operation](actual, expected)):
            return False, False, False
        if (metric == "simulation.tear.modelled_interval_violations"
                and operation in ("<=", "==") and expected == 0):
            interval_bound = True
        if (metric in ("simulation.latency_us.decode_to_submission.p99",
                       "simulation.latency_us.decode_to_submission.max")
                and operation in ("<", "<=") and expected > 0):
            latency_bound = True
    return True, interval_bound, latency_bound


def scenario_result(summary: dict, baseline_valid: bool,
                    run_valid: bool = True) -> dict:
    simulation = summary.get("simulation")
    if not isinstance(simulation, dict):
        simulation = {}
    # A changed candidate should not match the recorded submissions. Its
    # separate reference controller must still reproduce the captured state.
    reference_valid = (
        at(summary, "fidelity.reference_decision_state_exact") is True
        and at(summary, "fidelity.reference_controller_diagnostics_exact") is True)
    saturated = simulation.get("worker_saturated")
    assertions_passed = at(summary, "assertions.passed")
    assertions_valid, interval_bound, latency_bound = checked_assertions(summary)
    controller_ready = summary.get("controller_replay_ready") is True
    native_contract_changed = simulation.get("native_presentation_contract_changed")
    exclusions = []
    for valid, reason in (
            (run_valid, "incomplete or inconsistent replay process output"),
            (baseline_valid, "strict capture baseline unavailable"),
            (reference_valid, "reference controller does not reproduce capture"),
            (controller_ready, "controller timing or lifecycle readiness unavailable"),
            (native_contract_changed is False,
             "native presentation contract changed or compatibility unknown; recorded service is nonpredictive"),
            (saturated is False, "worker saturated or saturation unknown"),
            (summary.get("exit_code") == 0, "scenario process failed or exit unknown"),
            (assertions_valid, "nonempty, consistent passing assertions unavailable"),
            (interval_bound, "zero interval-violation assertion unavailable"),
            (latency_bound, "explicit latency p99 or maximum bound unavailable")):
        if not valid:
            exclusions.append(reason)
    row = {
        "name": summary.get("scenario", simulation.get("scenario", "unknown")),
        "exit_code": summary.get("exit_code"),
        "baseline_exact": baseline_valid, "reference_exact": reference_valid,
        "worker_saturated": saturated, "assertions_passed": assertions_passed,
        "controller_replay_ready": controller_ready,
        "native_presentation_contract_changed": native_contract_changed,
        "assertions_valid": assertions_valid,
        "interval_safety_bound_checked": interval_bound,
        "latency_bound_checked": latency_bound,
    }
    paths = {
        "jerk_p95_us": "simulation.sender_cadence.presented_jerk_us.p95",
        "jerk_p99_us": "simulation.sender_cadence.presented_jerk_us.p99",
        "jerk_over_2ms_per_mille": "replay_presented_jerk_over_2ms_per_mille",
        "sender_error_p95_us": "simulation.sender_cadence.absolute_spacing_error_us.p95",
        "sender_error_p99_us": "simulation.sender_cadence.absolute_spacing_error_us.p99",
        "client_spacing_errors_over_3ms": "simulation.sender_cadence.client_spacing_errors_over_3ms",
        "client_spacing_pairs": "simulation.sender_cadence.client_spacing_pairs",
        "source_stalls": "simulation.sender_cadence.source_stall_pairs",
        "latency_mean_us": "simulation.latency_us.decode_to_submission.mean",
        "latency_p50_us": "simulation.latency_us.decode_to_submission.p50",
        "latency_p95_us": "simulation.latency_us.decode_to_submission.p95",
        "latency_p99_us": "simulation.latency_us.decode_to_submission.p99",
        "latency_max_us": "simulation.latency_us.decode_to_submission.max",
        "padding_max_us": "simulation.playout_delay_us.max",
        "submission_drift_p99_us": "simulation.submission_drift_us.p99",
        "interval_violations": "simulation.tear.modelled_interval_violations",
        "raster_exposure_lower": "replay_raster_exposure_lower_bound",
        "raster_exposure_upper": "replay_raster_exposure_upper_bound",
        "native_window_samples": "simulation.smoothness_feedback.native_window_samples",
        "native_window_misses": "simulation.smoothness_feedback.native_window_misses",
    }
    row.update({key: number(at(summary, path)) for key, path in paths.items()})
    row["native_window_scope"] = "rolling controller feedback window; not lifetime outcome coverage"
    # A zero-valued distribution with no samples is unavailable evidence.
    distributions = {
        "simulation.sender_cadence.presented_jerk_us": ("p95", "p99"),
        "simulation.sender_cadence.absolute_spacing_error_us": ("p95", "p99"),
        "simulation.latency_us.decode_to_submission": ("mean", "p50", "p95", "p99", "max"),
        "simulation.playout_delay_us": ("max",),
        "simulation.submission_drift_us": ("p99",),
        "simulation.occupancy_decision_shift_us": ("p50",),
    }
    missing_metrics = []
    for path, fields in distributions.items():
        count = at(summary, path + ".count")
        if (type(count) is not int or count <= 0
                or any(number(at(summary, path + "." + field)) is None
                       for field in fields)):
            missing_metrics.append(path)
    for key in ("jerk_over_2ms_per_mille", "source_stalls", "interval_violations"):
        if row[key] is None or row[key] < 0:
            missing_metrics.append(paths[key])
    if missing_metrics:
        exclusions.append("required sampled metrics unavailable")
    row["missing_required_metrics"] = missing_metrics
    row["qualification_exclusions"] = exclusions
    row["qualified_controller_comparison"] = not exclusions
    row["classification"] = "controller comparison" if not exclusions else (
        "native contract changed; nonpredictive" if native_contract_changed is True else
        "saturated; prediction invalid" if saturated is True else "exploratory")
    return row


def run_replay(binary: Path, trace: Path, directory: Path, name: str,
               extra: list[str]) -> tuple[dict, dict]:
    output = directory / f"{name}.json"
    stdout = directory / f"{name}.stdout.txt"
    stderr = directory / f"{name}.stderr.txt"
    for artifact in (output, stdout, stderr):
        if artifact.exists():
            raise ValueError(f"refusing to reuse replay artifact: {artifact}")
    arguments = [str(binary), str(trace), *extra, "--output", str(output)]
    run = {"arguments": arguments, "exit_code": None}
    try:
        result = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                check=False)
        stdout.write_bytes(result.stdout)
        stderr.write_bytes(result.stderr)
        run["exit_code"] = result.returncode
    except OSError as error:
        stdout.write_bytes(b"")
        stderr.write_text(str(error) + "\n", encoding="utf-8")
        run["output_error"] = f"unable to launch replay: {error}"
    summary = {}
    if output.exists():
        try:
            summary = read_json(output)
            if not isinstance(summary, dict):
                raise ValueError("replay output is not a JSON object")
        except (ValueError, UnicodeError) as error:
            run["output_error"] = str(error)
            summary = {}
    else:
        run.setdefault("output_error", "replay did not write a summary")
    run["artifacts"] = [identity(path) for path in (output, stdout, stderr) if path.exists()]
    return run, summary


def scenario_names(configuration: Path) -> list[str]:
    value = read_json(configuration)
    scenarios = at(value, "scenarios")
    if not isinstance(scenarios, list) or not scenarios:
        raise ValueError(f"configuration has no scenarios: {configuration}")
    names = [at(item, "name") for item in scenarios]
    if (any(not isinstance(name, str) or not name for name in names)
            or len(set(names)) != len(names)):
        raise ValueError(f"configuration needs unique, nonempty scenario names: {configuration}")
    return names


def batch_summaries(run: dict, batch: dict, names: list[str]) -> tuple[list[dict], bool]:
    scenarios = batch.get("scenarios")
    single = scenarios is None and isinstance(batch.get("simulation"), dict)
    if single:
        scenarios = [dict(batch, exit_code=run.get("exit_code"))]
    if (not isinstance(scenarios, list) or not scenarios
            or any(not isinstance(item, dict) for item in scenarios)):
        return [], False
    actual_names = [item.get("scenario", at(item, "simulation.scenario"))
                    for item in scenarios]
    codes = [item.get("exit_code") for item in scenarios]
    if (any(not isinstance(name, str) for name in actual_names)
            or sorted(actual_names) != sorted(names)
            or any(type(code) is not int or code not in (0, 4) for code in codes)
            or any(at(item, "assertions.passed") is not (code == 0)
                   for item, code in zip(scenarios, codes))
            or "output_error" in run):
        return scenarios, False
    # An assertion-failing child returns 4 without invalidating another completed
    # scenario. A partial batch, crash, or inconsistent aggregate cannot qualify.
    all_passed = all(code == 0 for code in codes)
    valid = (run.get("exit_code") == 0 if all_passed else
             run.get("exit_code") in [code for code in codes if code != 0])
    if not single:
        valid = valid and batch.get("passed") is all_passed
    return scenarios, valid


def capture_coverage(summary: dict) -> dict:
    gates = at(summary, "diagnostic_readiness.gates")
    raster = "capture.telemetry_coverage.native_outcome_and_qpc_integrity.raster_sampling"
    missing = ["physical panel output is not observed",
               "native presentation outcomes by actual Present mode are not reported; "
               "aggregate feedback cannot establish adaptive-mode coverage"]
    if at(summary, raster + ".capture_valid") is not True:
        missing.append("valid native raster observations are unavailable")
    if summary.get("raster_simulation_ready") is not True:
        missing.append("raster simulation is not ready; exposure bounds are exploratory")
    return {
        "display_hz": number(at(summary, "capture.display_hz")),
        "stream_fps": number(at(summary, "capture.stream_fps")),
        "duration_seconds": number(at(summary, "capture.duration_seconds")),
        "presented_frames": number(at(summary, "capture.presented_frames")),
        "raster_simulation_ready": summary.get("raster_simulation_ready") is True,
        "native_outcome_coverage_by_present_mode": None,
        "raster_before_valid_rows": number(at(summary, raster + ".before_query_success.valid")),
        "raster_after_valid_rows": number(at(summary, raster + ".after_query_success.valid")),
        "unmet_or_unknown_diagnostic_gates": (
            sorted(key for key, value in gates.items() if value is not True)
            if isinstance(gates, dict) else None),
        "missing_coverage": missing,
    }


def verify_inputs(report: dict) -> None:
    inputs = [report["runner"], report["replay"], *report["configurations"],
              *(capture["trace"] for capture in report["captures"])]
    for before in inputs:
        if before != identity(Path(before["path"])):
            raise ValueError(f"input changed during analysis; rerun: {before['path']}")


def invalidate_report(report: dict, error: str) -> None:
    report.update(completed=False, provenance_valid=False, error=error, exit_code=2)
    for capture in report["captures"]:
        for row in capture["scenarios"]:
            row["qualified_controller_comparison"] = False
            row["classification"] = "invalid; review incomplete"
            row["qualification_exclusions"].append("review incomplete or provenance invalid")


def markdown(report: dict) -> str:
    lines = ["# VRR playback review", "",
             "The simulator runs the production C++ controller with recorded arrivals, "
             "admission and renderer service costs. Jerk uses CPU submissions as a "
             "presentation proxy. Changing native Present behavior is not simulated.", "",
             "**A physical display fix is not proven by this report.** "
             "Missing native intervals are unavailable evidence; they are not successes.", "",
             f"Review completed: **{report.get('completed', False)}**; "
             f"input provenance verified: **{report.get('provenance_valid', False)}**; "
             f"exit code: {report.get('exit_code')}.", ""]
    if report.get("error"):
        lines += ["Review failed: " + report["error"], ""]
    for capture in report["captures"]:
        trace = capture["trace"]
        coverage = capture["coverage"]
        lines += [f"## {Path(trace['path']).name}", "",
                  f"Input: `{trace['path']}`  ",
                  f"SHA-256: `{trace['sha256']}`  ",
                  f"Bytes: {trace['bytes']}; modified UTC: {trace['modified_utc']}  ",
                  f"Strict baseline exit: {capture['baseline_run']['exit_code']}; "
                  f"exact: **{capture['baseline_exact']}**.  ",
                  f"Captured display: {coverage['display_hz']} Hz; requested stream: "
                  f"{coverage['stream_fps']} FPS; duration: {coverage['duration_seconds']} seconds.", "",
                  "Missing coverage: " + "; ".join(coverage["missing_coverage"]) + ".", "",
                  "| Scenario | Status | Jerk p99 ms | Jerk >2ms % | Source residual p99 ms | Latency mean / p99 ms |",
                  "| --- | --- | ---: | ---: | ---: | ---: |"]
        def scaled(value, divisor):
            return "N/A" if value is None else f"{value / divisor:.2f}"
        for row in capture["scenarios"]:
            latency = ("invalid / unknown" if (row["worker_saturated"] is not False or
                       row["native_presentation_contract_changed"] is not False) else
                       f"{scaled(row['latency_mean_us'], 1000)} / {scaled(row['latency_p99_us'], 1000)}")
            name = str(row["name"]).replace("|", "\\|").replace("\n", " ").replace("\r", " ")
            lines.append(f"| {name} | {row['classification']} | "
                         f"{scaled(row['jerk_p99_us'], 1000)} | "
                         f"{scaled(row['jerk_over_2ms_per_mille'], 10)} | "
                         f"{scaled(row['sender_error_p99_us'], 1000)} | {latency} |")
        lines += ["", "Saturated rows retain raw diagnostics in JSON, but their apparent "
                  "smoothness and latency cannot select a candidate. Qualification exclusions "
                  "are recorded per scenario in review.json. A controller comparison is limited "
                  "to the captured workload and recorded renderer service; it does not qualify "
                  "raster predictions or demonstrate a user-visible fix.", ""]
    return "\n".join(lines)


def write_report(output: Path, report: dict) -> None:
    write_json(output / "review.json", report)
    (output / "review.md").write_text(markdown(report), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", nargs="+", type=Path)
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path,
                        help="new output directory; existing directories are rejected")
    parser.add_argument("--config", type=Path,
                        default=ROOT / "tests/vrr/configs/hybrid-scheduling-review.json")
    parser.add_argument("--stress-config", type=Path)
    parser.add_argument("--jobs", type=int, default=0)
    args = parser.parse_args()
    if not 0 <= args.jobs <= 64:
        parser.error("--jobs must be between 0 and 64")
    report = None
    output = None
    try:
        binary = args.replay.resolve(strict=True)
        configurations = [args.config.resolve(strict=True)]
        if args.stress_config:
            configurations.append(args.stress_config.resolve(strict=True))
        traces = [path.resolve(strict=True) for path in args.traces]
        args.output.mkdir(parents=True, exist_ok=False)
        output = args.output.resolve()
        report = {
            "review_schema": 1, "runner": identity(Path(__file__)),
            "replay": identity(binary),
            "configurations": [identity(path) for path in configurations],
            "native_policy_counterfactual_available": False,
            "physical_fix_proven": False, "captures": [],
            "completed": False, "provenance_valid": False, "exit_code": None,
        }
        # Leave a clearly incomplete manifest if interrupted. Publish qualified
        # rows only after every input has been rehashed against its original copy.
        write_report(output, report)
        configured_names = [scenario_names(path) for path in configurations]
        status = 0
        for index, trace in enumerate(traces, 1):
            directory = output / f"capture-{index}"
            directory.mkdir()
            before = identity(trace)
            print(f"Reviewing {trace}", flush=True)
            baseline_run, baseline = run_replay(
                binary, trace, directory, "baseline", ["--require-exact-baseline"])
            valid = exact_baseline(baseline_run, baseline)
            capture = {"trace": before, "baseline_run": baseline_run,
                       "baseline_exact": valid, "coverage": capture_coverage(baseline),
                       "scenario_runs": [], "scenarios": []}
            report["captures"].append(capture)
            if not valid:
                status = review_status(status, 3)
            if baseline_run.get("exit_code") not in (0, 3) or "output_error" in baseline_run:
                status = review_status(status, 2)
            for config_index, configuration in enumerate(configurations, 1):
                run, batch = run_replay(binary, trace, directory, f"batch-{config_index}",
                                        ["--config", str(configuration), "--jobs", str(args.jobs)])
                run["configuration_index"] = config_index
                capture["scenario_runs"].append(run)
                scenarios, run_valid = batch_summaries(
                    run, batch, configured_names[config_index - 1])
                run["complete_scenario_accounting"] = run_valid
                if not run_valid:
                    status = review_status(status, 2)
                for summary in scenarios:
                    row = scenario_result(summary, valid, run_valid)
                    row["configuration_index"] = config_index
                    capture["scenarios"].append(row)
                    if not row["qualified_controller_comparison"]:
                        status = review_status(status, 4)
                if run["exit_code"] != 0:
                    status = review_status(status, 4)
            if before != identity(trace):
                raise ValueError(f"capture changed during replay: {trace}")
        verify_inputs(report)
        report.update(completed=True, provenance_valid=True, exit_code=status)
        write_report(output, report)
        print(f"Report: {output / 'review.md'}", flush=True)
        return status
    except (OSError, ValueError) as error:
        if report is not None and output is not None:
            invalidate_report(report, str(error))
            try:
                write_report(output, report)
            except OSError as write_error:
                print(f"unable to record review failure: {write_error}", file=sys.stderr)
        print(f"review-vrr-playback: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
