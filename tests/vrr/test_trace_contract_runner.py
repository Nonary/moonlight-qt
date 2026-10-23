import importlib.util
import hashlib
import json
import os
import re
import struct
from pathlib import Path
import sys
import tempfile
import unittest
import zlib
from unittest import mock

spec = importlib.util.spec_from_file_location("trace_contract_runner", Path(__file__).with_name("run_trace_contracts.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def exact_result():
    return {"fidelity": {"baseline_exact": True}, "replayed_frames": 160,
            "capture": {"recorded_sequence_integrity_valid": True,
                        "delivered_frames": 160, "scheduled_frames": 160,
                        "presented_frames": 158,
                        "timestamp_integrity": {"valid": True},
                        "row_semantic_integrity": {"valid": True},
                        "session_config_integrity": {"valid": True}}}


class TraceContractRunnerTests(unittest.TestCase):
    def test_assertion_suites_cannot_silently_disable_release_checks(self):
        for source in Path(__file__).parent.glob("tst_*.cpp"):
            text = source.read_text(encoding="utf-8-sig")
            if re.search(r"\bassert\s*\(", text):
                with self.subTest(source=source.name):
                    self.assertIn('#include "assertions.h"', text,
                                  "assert-based VRR suites require the release assertion guard")

    def test_all_required_targets_must_exist(self):
        runner.require_tests(dict.fromkeys(runner.REQUIRED_TESTS))
        for missing in runner.REQUIRED_TESTS:
            with self.subTest(missing=missing), self.assertRaises(RuntimeError):
                runner.require_tests(dict.fromkeys(runner.REQUIRED_TESTS - {missing}))

    def test_exact_requires_every_integrity_gate(self):
        runner.require_exact(exact_result())
        for path in [("fidelity", "baseline_exact"), ("capture", "recorded_sequence_integrity_valid"),
                     ("capture", "timestamp_integrity", "valid"), ("capture", "row_semantic_integrity", "valid"),
                     ("capture", "session_config_integrity", "valid")]:
            for bad in (False, None, "true", 1):
                result = exact_result()
                field = result
                for component in path[:-1]:
                    field = field[component]
                field[path[-1]] = bad
                with self.subTest(path=path, value=bad), self.assertRaises(RuntimeError):
                    runner.require_exact(result)
        for empty in (0, -1, None, True):
            result = exact_result(); result["replayed_frames"] = empty
            with self.subTest(frames=empty), self.assertRaises(RuntimeError):
                runner.require_exact(result)
        with self.assertRaises(RuntimeError):
            runner.require_exact({})

    def test_exact_uses_scheduled_population_and_allows_prior_terminal_rows(self):
        result = exact_result()
        result["replayed_frames"] = 175996
        result["capture"].update(delivered_frames=176008, scheduled_frames=175996,
                                 presented_frames=175994)
        runner.require_exact(result)

    def test_exact_rejects_malformed_sections_and_impossible_populations(self):
        for result in ([], {"fidelity": [], "capture": {}},
                       {**exact_result(), "capture": []}):
            with self.subTest(result=result), self.assertRaises(RuntimeError):
                runner.require_exact(result)
        for key, value in (("delivered_frames", 159), ("delivered_frames", True),
                           ("scheduled_frames", 159), ("scheduled_frames", True),
                           ("scheduled_frames", 161), ("presented_frames", 161),
                           ("presented_frames", -1), ("presented_frames", True)):
            result = exact_result()
            result["capture"][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(RuntimeError):
                runner.require_exact(result)
        # Scheduled and presented may be lower than delivered/scheduled.
        result = exact_result()
        result["capture"].update(delivered_frames=161, presented_frames=0)
        runner.require_exact(result)

    def _fake_bin(self, root):
        bin_dir = root / "bin"
        bin_dir.mkdir()
        suffix = ".exe" if os.name == "nt" else ""
        for name in runner.REQUIRED_TESTS | {"vrrreplay"}:
            (bin_dir / (name + suffix)).touch()
        return bin_dir

    def _successful_fake_run(self, command, log, env, timeout, *, expected_exit=0, result="valid",
                             omit_fixture=None, fail_after=None, seen_env=None):
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text("mock output\n", encoding="utf-8")
        name = Path(command[0]).stem
        if seen_env is not None:
            seen_env.append(env)
        if fail_after == name:
            raise __import__("subprocess").TimeoutExpired(command, timeout)
        if name.startswith("tst_"):
            fixture_root = Path(env["MOONLIGHT_VRR_TEST_EXPORT_FIXED_RESERVE_TRACE"])
            fixture_root.parent.mkdir(parents=True, exist_ok=True)
            for kind in runner.READINESS_FIXTURES:
                if kind != omit_fixture:
                    Path(str(fixture_root) + f"-{kind}.vrrtrace").write_bytes(b"fresh trace")
        elif "--help" in command:
            log.write_text("usage: vrrreplay\n", encoding="utf-8")
        else:
            output = Path(command[command.index("--output") + 1])
            if result == "valid":
                result = exact_result()
                result["capture"]["trace"] = Path(command[1]).name
                if expected_exit == 3:
                    result["fidelity"].update(baseline_exact=False, invalid_controller_lifecycle_rows=1)
                    result["capture"]["clean_close_footer"] = {"decoded_sha256_matches": True}
            output.write_text(json.dumps(result), encoding="utf-8")

    def _main(self, root, run):
        bin_dir = self._fake_bin(root)
        output = root / "artifacts"
        def fake_counterexamples(trace, destination):
            paths = [destination / (name + ".csv") for name in
                     ("wrong-successor", "wrong-proof", "missing-successor")]
            for path in paths:
                path.write_bytes(b"counterexample supplied by the fixture-generator unit test")
            return paths
        with mock.patch.object(sys, "argv", ["run_trace_contracts.py", "--bin-dir", str(bin_dir),
                                              "--output-dir", str(output)]), \
             mock.patch.object(runner, "successor_counterexamples", side_effect=fake_counterexamples), \
             mock.patch.object(runner, "run_program", side_effect=run):
            code = runner.main()
        summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
        artifact_dir = Path(summary["artifact_directory"])
        return code, summary, artifact_dir

    def test_main_requires_fresh_fixture_exports_and_ignores_stale_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            old = root / "artifacts" / "readiness-ready.vrrtrace"
            old.parent.mkdir(parents=True)
            old.write_bytes(b"stale but plausible")
            code, summary, _ = self._main(
                root, lambda *args, **kwargs: self._successful_fake_run(
                    *args, **kwargs, omit_fixture="ready"))
            self.assertEqual(code, 1)
            self.assertFalse(summary["passed"])
            self.assertIn("export the ready readiness fixture", summary["error"])

    def test_main_rejects_missing_malformed_and_contradictory_zero_exit_results(self):
        cases = ("missing", "malformed", "contradictory")
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                def run(command, log, env, timeout):
                    if Path(command[0]).stem == "vrrreplay" and "--help" not in command:
                        log.parent.mkdir(parents=True, exist_ok=True)
                        log.write_text("ok", encoding="utf-8")
                        if case == "missing":
                            return 0
                        bad = exact_result()
                        if case == "malformed":
                            bad["capture"] = []
                        else:
                            bad["capture"]["delivered_frames"] = 159
                        Path(command[command.index("--output") + 1]).write_text(
                            json.dumps(bad), encoding="utf-8")
                        return 0
                    return self._successful_fake_run(command, log, env, timeout)
                code, summary, _ = self._main(root, run)
                self.assertEqual(code, 1)
                self.assertFalse(summary["passed"])
                self.assertFalse(summary["completed"])
                self.assertEqual(summary["exact_replay"], [])

    def test_main_process_failure_wins_over_plausible_result_file(self):
        with tempfile.TemporaryDirectory() as directory:
            def fail_with_plausible_output(command, log, env, timeout):
                if Path(command[0]).stem == "vrrreplay" and "--help" not in command:
                    log.parent.mkdir(parents=True, exist_ok=True)
                    log.write_text("failed after writing output", encoding="utf-8")
                    Path(command[command.index("--output") + 1]).write_text(
                        json.dumps(exact_result()), encoding="utf-8")
                    raise RuntimeError("vrrreplay exited 7; see log")
                return self._successful_fake_run(command, log, env, timeout)
            code, summary, _ = self._main(Path(directory), fail_with_plausible_output)
            self.assertEqual(code, 1)
            self.assertFalse(summary["passed"])
            self.assertFalse(summary["completed"])
            self.assertEqual(summary["exact_replay"], [])

    def test_main_rejects_exact_result_for_a_different_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            def wrong_fixture(command, log, env, timeout):
                if Path(command[0]).stem == "vrrreplay" and "--help" not in command:
                    result = exact_result()
                    result["capture"]["trace"] = "a-different-clean-capture.vrrtrace"
                    Path(command[command.index("--output") + 1]).write_text(
                        json.dumps(result), encoding="utf-8")
                    log.write_text("zero-exit exact result for wrong input", encoding="utf-8")
                    return 0
                return self._successful_fake_run(command, log, env, timeout)
            code, summary, _ = self._main(Path(directory), wrong_fixture)
            self.assertEqual(code, 1)
            self.assertFalse(summary["passed"])
            self.assertFalse(summary["completed"])
            self.assertIn("another trace", summary["error"])
            self.assertEqual(summary["exact_replay"], [])

    def test_main_clears_inherited_subset_flags_and_timeout_leaves_failure_summary(self):
        inherited = ("MOONLIGHT_VRR_TEST_FIXED_RESERVE_ONLY",
                     "MOONLIGHT_VRR_TEST_TRAJECTORY_RESERVATION_ONLY",
                     "MOONLIGHT_VRR_TEST_SKIP_RESERVATION")
        seen = []
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.dict(os.environ, {key: "1" for key in inherited}):
                code, summary, _ = self._main(
                    Path(directory), lambda *args, **kwargs: self._successful_fake_run(
                        *args, **kwargs, seen_env=seen))
            self.assertEqual(code, 0)
            self.assertTrue(summary["passed"])
            self.assertEqual(len(summary["rejected_replay"]), 3)
            self.assertTrue(seen)
            self.assertTrue(all(not any(key in env for key in inherited) for env in seen))
        with tempfile.TemporaryDirectory() as directory:
            def timeout_run(command, log, env, timeout):
                if Path(command[0]).stem == "vrrreplay" and "--help" not in command:
                    raise __import__("subprocess").TimeoutExpired(command, timeout)
                return self._successful_fake_run(command, log, env, timeout)
            code, summary, _ = self._main(Path(directory), timeout_run)
            self.assertEqual(code, 1)
            self.assertFalse(summary["passed"])
            self.assertFalse(summary["completed"])
            self.assertEqual(len(summary["tests"]), len(runner.REQUIRED_TESTS))
            self.assertIn("timed out", summary["error"].lower())

    def test_result_file_cannot_override_failed_process(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = root / "plausible-result.json"
            command = [sys.executable, "-c",
                       "import pathlib,sys;pathlib.Path(sys.argv[1]).write_text('{}');sys.exit(3)", str(result)]
            with self.assertRaisesRegex(RuntimeError, "exited 3"):
                runner.run_program(command, root / "process.log", os.environ.copy(), 10)
            self.assertTrue(result.exists())
            self.assertGreaterEqual(runner.run_program(
                command, root / "expected-rejection.log", os.environ.copy(), 10, expected_exit=3), 0)
            with self.assertRaisesRegex(RuntimeError, "exited 0"):
                runner.run_program([sys.executable, "-c", "pass"], root / "unexpected-success.log",
                                   os.environ.copy(), 10, expected_exit=3)

    def test_successor_counterexamples_preserve_rows_and_rehash_the_changed_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = "frame,trajectory_resync_dropped,trajectory_successor_frame,trajectory_successor_proof"
            body = header + "\n37,1,38,2\n38,0,-1,0\n39,0,-1,0\n"
            footer = ("#vrr_trace_footer,format_version=2,rows_enqueued=3,decoded_sha256=" +
                      hashlib.sha256(body.encode()).hexdigest())
            raw = (body + footer + "\n").encode()
            payload = struct.pack(">I", len(raw)) + zlib.compress(raw)
            trace = root / "ready.vrrtrace"
            trace.write_bytes(b"MLVRR1\n" + struct.pack("<I", len(payload)) + payload)
            paths = runner.successor_counterexamples(trace, root)
            self.assertEqual([p.stem for p in paths],
                             ["wrong-successor", "wrong-proof", "missing-successor"])
            for path, changed_row in zip(paths, ("37,1,39,2", "37,1,38,1", "37,1,40,2")):
                lines = path.read_text().splitlines()
                self.assertEqual(lines[:4], [header, changed_row, "38,0,-1,0", "39,0,-1,0"])
                expected = hashlib.sha256(("\n".join(lines[:-1]) + "\n").encode()).hexdigest()
                self.assertEqual(lines[-1],
                                 "#vrr_trace_footer,format_version=2,rows_enqueued=3,decoded_sha256=" + expected)

    def test_main_cannot_pass_when_corrupt_proof_rejection_lacks_causal_evidence(self):
        for bad in ("exact", "no-lifecycle-failure", "bad-hash"):
            with self.subTest(bad=bad), tempfile.TemporaryDirectory() as directory:
                def run(command, log, env, timeout, **kwargs):
                    self._successful_fake_run(command, log, env, timeout, **kwargs)
                    if kwargs.get("expected_exit") != 3:
                        return 0
                    result_path = Path(command[command.index("--output") + 1])
                    result = json.loads(result_path.read_text())
                    if bad == "exact":
                        result["fidelity"]["baseline_exact"] = True
                    elif bad == "no-lifecycle-failure":
                        result["fidelity"]["invalid_controller_lifecycle_rows"] = 0
                    else:
                        result["capture"]["clean_close_footer"]["decoded_sha256_matches"] = False
                    result_path.write_text(json.dumps(result))
                code, summary, _ = self._main(Path(directory), run)
                self.assertEqual(code, 1)
                self.assertFalse(summary["completed"])
                self.assertFalse(summary["passed"])
                self.assertEqual(summary["rejected_replay"], [])


if __name__ == "__main__":
    unittest.main()
