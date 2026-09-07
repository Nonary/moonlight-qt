import hashlib
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("deadlines", Path(__file__).parents[2] / "scripts/vrr-deadlines.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class DeadlineAuditTest(unittest.TestCase):
    def capture(self, rows):
        names = list(dict.fromkeys(key for row in rows for key in row))
        text = ",".join(names) + "\n"
        for row in rows:
            text += ",".join(str(row.get(key, 0)) for key in names) + "\n"
        digest = hashlib.sha256(text.encode()).hexdigest()
        return iter((text + "#vrr_trace_footer,clean_shutdown=1,rows_dropped=0,write_failed=0,size_capped=0,decoded_sha256=" + digest + "\n").splitlines(True))

    def test_original_deadline_and_missing_feedback(self):
        row = dict(original_target_us=10000, target_us=30000, pacer_arrival_us=1000,
                   decision_valid=1, prepare_end_us=25000, presented=1,
                   submission_boundary_us=30000, dropped=0)
        result = module.audit(self.capture([row]))
        self.assertTrue(result["complete_capture"])
        self.assertEqual(result["readiness_misses"], 1)
        self.assertEqual(result["submission_misses"], 1)
        self.assertEqual(result["native_missing"], 1)
        self.assertEqual(result["native_coverage_percent"], 0)

    def test_dxgi_sync_time_is_not_present_time(self):
        row = dict(original_target_us=10000, pacer_arrival_us=1000, decision_valid=1,
                   presented=1, submission_id_valid=1, submission_id=10,
                   latch_valid=1, latch_qpc_correlation_valid=1,
                   latch_submission_id=10, latch_present_refresh_seq=5,
                   latch_sync_refresh_seq=6, latch_time_us=100000)
        result = module.audit(self.capture([row]))
        self.assertEqual(result["native_observed"], 0)
        self.assertEqual(result["native_misses"], 0)
        row["latch_present_refresh_seq"] = 6
        result = module.audit(self.capture([row]))
        self.assertEqual(result["native_observed"], 1)
        self.assertEqual(result["native_misses"], 1)

    def test_changed_deadline_breaks_integrity(self):
        lines = list(self.capture([dict(original_target_us=10000, decision_valid=1)]))
        lines[1] = lines[1].replace("10000", "20000")
        self.assertFalse(module.audit(iter(lines))["complete_capture"])


if __name__ == "__main__":
    unittest.main()
