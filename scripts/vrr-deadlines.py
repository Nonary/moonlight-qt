#!/usr/bin/env python3
"""Audit VRR13 original deadlines; native feedback is observation only."""
import argparse
import csv
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile


def audit(lines, tolerance_us=2000):
    header = next(lines).rstrip("\r\n")
    names = header.split(",")
    if "original_target_us" not in names:
        raise ValueError("capture predates original-deadline diagnostics")
    digest = hashlib.sha256((header + "\n").encode())
    submissions, anchors, pending = {}, {}, {}
    rows = decisions = readiness_misses = submission_misses = drops = 0
    native_seen = native_misses = host_stalls = 0
    footer = None
    first_at = last_at = 0
    windows = []
    for line in lines:
        line = line.rstrip("\r\n")
        if line.startswith("#vrr_trace_footer,"):
            if footer is not None:
                raise ValueError("duplicate footer")
            footer = dict(part.split("=", 1) for part in line.split(",")[1:])
            continue
        if footer is not None:
            raise ValueError("data after footer")
        digest.update((line + "\n").encode())
        values = next(csv.reader([line]))
        if len(values) != len(names):
            raise ValueError("malformed trace row")
        row = dict(zip(names, values))
        def n(key):
            return int(row.get(key, "0"))
        rows += 1
        drops += n("dropped")
        at = n("pacer_arrival_us")
        first_at = first_at or at
        last_at = max(last_at, at)
        if n("external_rebase_applied") or n("phase_discontinuity"):
            # IDs and native sequence numbers can restart after display changes.
            submissions.clear()
            anchors.clear()
            pending.clear()
        if not n("decision_valid"):
            continue
        deadline = n("original_target_us")
        if not deadline:
            raise ValueError("scheduled frame has no original deadline")
        decisions += 1
        ready_miss = n("prepare_end_us") > deadline + tolerance_us
        submit_miss = bool(n("presented") and n("submission_boundary_us") > deadline + tolerance_us)
        readiness_misses += ready_miss
        submission_misses += submit_miss
        host_stalls += n("sender_interval_us") > max(25000, n("source_period_us") * 3 // 2)
        windows.append((at, ready_miss, submit_miss))
        if n("presented") and n("submission_id_valid"):
            submissions[n("submission_id")] = deadline
        if n("latch_valid") and n("latch_qpc_correlation_valid"):
            # SyncQPCTime is the timestamp of SyncRefreshCount, NOT of the
            # independently numbered PresentRefreshCount. Never extrapolate.
            anchors[n("latch_sync_refresh_seq")] = n("latch_time_us")
            identity = n("latch_submission_id")
            if identity in submissions:
                pending[identity] = n("latch_present_refresh_seq")
        for identity, sequence in list(pending.items()):
            if sequence in anchors and identity in submissions:
                native_seen += 1
                native_misses += anchors[sequence] > submissions.pop(identity) + tolerance_us
                del pending[identity]
        # Old unavailable samples stay missing in the coverage denominator.
        for mapping in (submissions, anchors, pending):
            while len(mapping) > 1024:
                del mapping[next(iter(mapping))]
    complete = bool(footer and footer.get("clean_shutdown") == "1" and
                    footer.get("rows_dropped") == "0" and
                    footer.get("write_failed") == "0" and
                    footer.get("size_capped") == "0" and
                    footer.get("decoded_sha256") == digest.hexdigest())
    def window(selected):
        return {"frames": len(selected), "readiness_misses": sum(r for _, r, _ in selected),
                "submission_misses": sum(s for _, _, s in selected)}
    return {"complete_capture": complete, "tolerance_us": tolerance_us,
            "rows": rows, "scheduled_frames": decisions, "drops": drops,
            "host_stalls": host_stalls, "readiness_misses": readiness_misses,
            "submission_misses": submission_misses, "native_observed": native_seen,
            "native_misses": native_misses, "native_missing": decisions - native_seen,
            "native_coverage_percent": 100 * native_seen / decisions if decisions else 0,
            "startup_60s": window([v for v in windows if v[0] - first_at <= 60000000]),
            "tail_60s": window([v for v in windows if last_at - v[0] <= 60000000])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("decoder", Path(__file__).with_name("decode-vrr-trace.py"))
    decoder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(decoder)
    with args.trace.open("rb") as source, tempfile.TemporaryFile() as expanded:
        if source.read(7) == decoder.MAGIC:
            source.seek(0)
            decoder.decode(source, expanded)
            expanded.seek(0)
            result = audit(iter(io.TextIOWrapper(expanded, encoding="utf-8")))
        else:
            source.seek(0)
            result = audit(iter(io.TextIOWrapper(source, encoding="utf-8")))
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0 if result["complete_capture"] and result["scheduled_frames"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
