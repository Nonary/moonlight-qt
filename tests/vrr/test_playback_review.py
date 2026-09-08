"""Evidence-gate tests for the platform-independent playback review runner."""

import copy
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / "scripts/review-vrr-playback.py"
SPEC = importlib.util.spec_from_file_location("playback_review", SCRIPT)
review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(review)


class PlaybackReviewTests(unittest.TestCase):
    def setUp(self):
        self.summary = {
            "scenario": "candidate", "exit_code": 0,
            "capture": {"recorded_sequence_integrity_valid": True},
            "fidelity": {"baseline_exact": True,
                         "reference_decision_state_exact": True,
                         "reference_controller_diagnostics_exact": True},
            "controller_replay_ready": True,
            "replay_presented_jerk_over_2ms_per_mille": 5,
            "assertions": {"configured": True, "passed": True, "results": [
                {"metric": "simulation.tear.modelled_interval_violations",
                 "operator": "<=", "expected": 0, "actual": 0, "passed": True},
                {"metric": "simulation.latency_us.decode_to_submission.p99",
                 "operator": "<=", "expected": 30000, "actual": 10000, "passed": True},
            ]},
            "simulation": {
                "worker_saturated": False,
                "native_presentation_contract_changed": False,
                "sender_cadence": {
                    "presented_jerk_us": {"count": 20, "p95": 50, "p99": 100},
                    "absolute_spacing_error_us": {"count": 20, "p95": 50, "p99": 100},
                    "source_stall_pairs": 0,
                },
                "latency_us": {"decode_to_submission": {
                    "count": 22, "mean": 9000, "p50": 9000, "p95": 10000,
                    "p99": 10000, "max": 12000,
                }},
                "playout_delay_us": {"count": 22, "max": 8000},
                "submission_drift_us": {"count": 22, "p99": 100},
                "occupancy_decision_shift_us": {"count": 22, "p50": 0},
                "tear": {"modelled_interval_violations": 0},
            },
        }

    def test_exactness_requires_exit_integrity_and_fidelity(self):
        self.assertTrue(review.exact_baseline({"exit_code": 0}, self.summary))
        for code, missing, exact in [(3, True, True), (0, False, True),
                                     (0, True, False), (0, None, True)]:
            summary = copy.deepcopy(self.summary)
            summary["capture"]["recorded_sequence_integrity_valid"] = missing
            summary["fidelity"]["baseline_exact"] = exact
            self.assertFalse(review.exact_baseline({"exit_code": code}, summary))
        self.assertFalse(review.exact_baseline({"exit_code": 0}, {}))
        self.assertFalse(review.exact_baseline(
            {"exit_code": 0, "output_error": "invalid JSON"}, self.summary))

    def test_changed_candidate_needs_exact_reference_not_identical_output(self):
        self.summary["fidelity"]["baseline_exact"] = False
        result = review.scenario_result(self.summary, True)
        self.assertTrue(result["qualified_controller_comparison"])
        self.summary["fidelity"]["reference_decision_state_exact"] = False
        self.assertFalse(review.scenario_result(
            self.summary, True)["qualified_controller_comparison"])

    def test_saturation_failed_assertions_and_unknowns_cannot_qualify(self):
        for section, key, value in [
                ("simulation", "worker_saturated", True),
                ("simulation", "worker_saturated", None),
                ("assertions", "passed", False),
                ("assertions", "passed", None),
                ("fidelity", "reference_controller_diagnostics_exact", False)]:
            summary = copy.deepcopy(self.summary)
            summary[section][key] = value
            self.assertFalse(review.scenario_result(
                summary, True)["qualified_controller_comparison"])
        self.summary["exit_code"] = 4
        self.assertFalse(review.scenario_result(
            self.summary, True)["qualified_controller_comparison"])
        self.assertFalse(review.scenario_result(
            self.summary, False)["qualified_controller_comparison"])

    def test_missing_metrics_are_unavailable_not_zero(self):
        del self.summary["simulation"]["sender_cadence"]["presented_jerk_us"]
        result = review.scenario_result(self.summary, True)
        self.assertIsNone(result["native_window_samples"])
        self.assertIn("not lifetime", result["native_window_scope"])
        self.assertIsNone(result["jerk_p99_us"])
        self.assertFalse(result["qualified_controller_comparison"])
        for value in (float("nan"), float("inf"), True, "0", 10 ** 1000):
            self.assertIsNone(review.number(value))
        self.assertEqual(review.number(0), 0)

    def test_empty_assertions_and_missing_bounds_cannot_qualify(self):
        for assertions in (
                {}, {"passed": True}, {"configured": True, "passed": True, "results": []},
                {"configured": True, "passed": True,
                 "results": self.summary["assertions"]["results"][:1]},
                {"configured": True, "passed": True,
                 "results": self.summary["assertions"]["results"][1:]},
                {"configured": True, "passed": True, "results": [None]}):
            summary = copy.deepcopy(self.summary)
            summary["assertions"] = assertions
            self.assertFalse(review.scenario_result(
                summary, True)["qualified_controller_comparison"])

    def test_assertion_records_must_match_current_metrics_and_bounds(self):
        for key, value in (("actual", 15000), ("expected", 5000), ("passed", False),
                           ("metric", "unknown.metric"), ("operator", "invalid")):
            summary = copy.deepcopy(self.summary)
            summary["assertions"]["results"][1][key] = value
            self.assertFalse(review.scenario_result(
                summary, True)["qualified_controller_comparison"])

    def test_unknown_timing_or_zero_samples_cannot_qualify(self):
        for ready in (None, False):
            summary = copy.deepcopy(self.summary)
            summary["controller_replay_ready"] = ready
            self.assertFalse(review.scenario_result(
                summary, True)["qualified_controller_comparison"])
        for count in (None, 0, True):
            summary = copy.deepcopy(self.summary)
            summary["simulation"]["occupancy_decision_shift_us"]["count"] = count
            self.assertFalse(review.scenario_result(
                summary, True)["qualified_controller_comparison"])
        self.summary["simulation"] = None
        self.assertFalse(review.scenario_result(
            self.summary, True)["qualified_controller_comparison"])

    def test_changed_or_unknown_native_contract_cannot_qualify(self):
        for changed in (True, None):
            summary = copy.deepcopy(self.summary)
            summary["simulation"]["native_presentation_contract_changed"] = changed
            result = review.scenario_result(summary, True)
            self.assertFalse(result["qualified_controller_comparison"])
            self.assertTrue(any("native presentation contract" in reason
                                for reason in result["qualification_exclusions"]))
            if changed:
                self.assertEqual(result["classification"], "native contract changed; nonpredictive")

    def test_complete_assertion_failing_batch_preserves_independent_results(self):
        failed = copy.deepcopy(self.summary)
        failed.update(scenario="counterexample", exit_code=4)
        failed["assertions"]["passed"] = False
        batch = {"passed": False, "scenarios": [self.summary, failed]}
        scenarios, valid = review.batch_summaries(
            {"exit_code": 4}, batch, ["candidate", "counterexample"])
        self.assertTrue(valid)
        self.assertTrue(review.scenario_result(scenarios[0], True, valid)[
            "qualified_controller_comparison"])
        self.assertFalse(review.scenario_result(scenarios[1], True, valid)[
            "qualified_controller_comparison"])
        batch["passed"] = True
        self.assertFalse(review.batch_summaries(
            {"exit_code": 4}, batch, ["candidate", "counterexample"])[1])
        batch["passed"] = False
        failed["exit_code"] = -11
        self.assertFalse(review.batch_summaries(
            {"exit_code": -11}, batch, ["candidate", "counterexample"])[1])

    def test_partial_duplicate_or_failed_batch_cannot_qualify(self):
        batch = {"passed": True, "scenarios": [self.summary]}
        for code, names in ((7, ["candidate"]), (0, ["candidate", "missing"])):
            scenarios, valid = review.batch_summaries({"exit_code": code}, batch, names)
            self.assertFalse(valid)
            self.assertFalse(review.scenario_result(scenarios[0], True, valid)[
                "qualified_controller_comparison"])
        batch["scenarios"].append(self.summary)
        self.assertFalse(review.batch_summaries(
            {"exit_code": 0}, batch, ["candidate", "counterexample"])[1])
        for malformed in ({"scenarios": [None]}, {"scenarios": {}}, {}):
            self.assertEqual(review.batch_summaries(
                {"exit_code": 0}, malformed, ["candidate"]), ([], False))

    def test_single_scenario_process_retains_exit_and_name(self):
        summary = copy.deepcopy(self.summary)
        summary["simulation"]["scenario"] = summary.pop("scenario")
        del summary["exit_code"]
        scenarios, valid = review.batch_summaries(
            {"exit_code": 0}, summary, ["candidate"])
        self.assertTrue(valid)
        self.assertEqual(scenarios[0]["exit_code"], 0)
        self.assertEqual(review.scenario_result(scenarios[0], True, valid)["name"],
                         "candidate")

    def test_failed_process_cannot_reuse_output_or_hide_exit(self):
        with tempfile.TemporaryDirectory(prefix="vrr review ") as temporary:
            directory = Path(temporary)
            # This child tests the CLI protocol, not the timing simulator.
            program = directory / "child script.py"
            program.write_text("import sys\nsys.exit(7)\n", encoding="utf-8")
            run, summary = review.run_replay(
                Path(sys.executable), program, directory, "failed", [])
            self.assertEqual(run["exit_code"], 7)
            self.assertEqual(summary, {})
            self.assertFalse(review.exact_baseline(run, summary))
            self.assertIn("output_error", run)
            self.assertEqual(len(run["artifacts"]), 2)
            old_summary = directory / "failed.json"
            old_summary.write_text(json.dumps(self.summary), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "refusing to reuse"):
                review.run_replay(Path(sys.executable), program, directory, "failed", [])
            self.assertEqual(json.loads(old_summary.read_text()), self.summary)

    def test_launch_error_and_nonfinite_output_are_recorded(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            run, summary = review.run_replay(directory / "missing executable",
                                             directory / "trace", directory, "launch", [])
            self.assertIsNone(run["exit_code"])
            self.assertIn("unable to launch", run["output_error"])
            self.assertEqual(summary, {})
            program = directory / "child.py"
            program.write_text(
                "import pathlib, sys\n"
                "pathlib.Path(sys.argv[-1]).write_text('{\"metric\": NaN}')\n",
                encoding="utf-8")
            run, summary = review.run_replay(
                Path(sys.executable), program, directory, "nonfinite", [])
            self.assertEqual(run["exit_code"], 0)
            self.assertIn("non-finite", run["output_error"])
            self.assertEqual(summary, {})

    def test_missing_optical_and_per_mode_coverage_remains_explicit(self):
        coverage = review.capture_coverage(self.summary)
        self.assertFalse(coverage["raster_simulation_ready"])
        self.assertIsNone(coverage["native_outcome_coverage_by_present_mode"])
        self.assertIsNone(coverage["raster_before_valid_rows"])
        self.assertTrue(any("adaptive-mode" in value for value in coverage["missing_coverage"]))

    def test_final_report_is_invalidated_if_config_changes_after_replay(self):
        for change_configuration in (False, True):
            with self.subTest(change_configuration=change_configuration):
                with tempfile.TemporaryDirectory() as temporary:
                    directory = Path(temporary)
                    trace = directory / "capture.vrrtrace"
                    trace.write_bytes(b"test CLI orchestration only")
                    configuration = directory / "config.json"
                    configuration.write_text(json.dumps({
                        "config_schema": 1, "scenarios": [{"name": "candidate"}],
                    }), encoding="utf-8")
                    output = directory / "review"
                    arguments = [str(SCRIPT), str(trace), "--replay", sys.executable,
                                 "--config", str(configuration), "--output", str(output)]

                    def run_replay(binary, source, run_directory, name, extra):
                        if name == "batch-1" and change_configuration:
                            with configuration.open("a", encoding="utf-8") as config:
                                config.write("\n")
                        return {"exit_code": 0}, copy.deepcopy(self.summary)

                    with mock.patch.object(sys, "argv", arguments), \
                            mock.patch.object(review, "run_replay", side_effect=run_replay), \
                            mock.patch.object(sys, "stdout", io.StringIO()), \
                            mock.patch.object(sys, "stderr", io.StringIO()):
                        result = review.main()
                    report = json.loads((output / "review.json").read_text())
                    self.assertEqual(result, 2 if change_configuration else 0)
                    self.assertIs(report["completed"], not change_configuration)
                    self.assertIs(report["provenance_valid"], not change_configuration)
                    row = report["captures"][0]["scenarios"][0]
                    self.assertIs(row["qualified_controller_comparison"],
                                  not change_configuration)
                    self.assertFalse(report["physical_fix_proven"])
                    if change_configuration:
                        self.assertIn("input changed", report["error"])

    def test_failure_status_priority_is_independent_of_capture_order(self):
        for failures in ((3, 2), (2, 3), (4, 3, 2), (2, 4, 3)):
            status = 0
            for failure in failures:
                status = review.review_status(status, failure)
            self.assertEqual(status, 2)
        self.assertEqual(review.review_status(4, 3), 3)
        self.assertEqual(review.review_status(3, 4), 3)

    def test_identity_detects_content_change(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "capture.vrrtrace"
            path.write_bytes(b"first")
            before = review.identity(path)
            path.write_bytes(b"later")
            after = review.identity(path)
            self.assertNotEqual(before["sha256"], after["sha256"])
            self.assertEqual(before["bytes"], after["bytes"])


if __name__ == "__main__":
    unittest.main()
