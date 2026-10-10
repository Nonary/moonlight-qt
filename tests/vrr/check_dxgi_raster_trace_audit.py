#!/usr/bin/env python3
"""Check exact DXGI protection-raster replay and reject unsupported samples.

Usage: check_dxgi_raster_trace_audit.py /path/to/vrrreplay /path/to/fixture.csv
Use the worker's exported ALIGN=0 protection fixture. Mutations repair the footer
hash so rejection must come from the native evidence audit.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("replay", type=Path)
    parser.add_argument("fixture", type=Path)
    args = parser.parse_args()
    original = args.fixture.read_bytes().splitlines(keepends=True)
    columns = original[0].decode().strip().split(",")
    indices = {name: index for index, name in enumerate(columns)}

    def value(fields, name):
        return int(fields[indices[name]])

    rows = [(index, line.decode().strip().split(","))
            for index, line in enumerate(original[1:], 1)
            if not line.startswith(b"#")]
    guard_rows = [(index, fields) for index, fields in rows
                  if value(fields, "native_raster_sampling_requested") == 0
                  and value(fields, "native_raster_before_query_result_valid") == 1]
    assert guard_rows, "fixture must exercise before-only protection without ALIGN"
    assert all(value(fields, "native_raster_after_query_result_valid") == 0
               for _, fields in guard_rows)
    selected, fields = guard_rows[0]
    semantic = "row_semantic_integrity"
    relationship = "native_raster_relationship_mismatch_rows"
    timestamp = "timestamp_integrity"
    ordering = "native_raster_timing_order_mismatch_rows"
    cases = [
        ({"flip_protection_checked": 0}, semantic, relationship),
        ({"flip_protection_query_result": -1}, semantic, relationship),
        ({"flip_protection_pending": 1}, semantic, relationship),
        ({"native_vblank_virtualization_disabled": 0}, semantic, relationship),
        ({"native_same_gpu_output": 0}, semantic, relationship),
        ({"native_display_path_valid": 0}, semantic, relationship),
        ({"native_display_target_available": 0}, semantic, relationship),
        ({"native_display_signal_valid": 0}, semantic, relationship),
        ({"native_render_adapter_luid_valid": 0}, semantic, relationship),
        ({"native_display_source_adapter_luid":
          value(fields, "native_render_adapter_luid") + 1}, semantic, relationship),
        ({"native_raster_vidpn_source_id":
          value(fields, "native_display_source_id") + 1}, semantic, relationship),
        ({"native_raster_after_query_result_valid": 1,
          "native_raster_after_query_result": 0,
          "native_raster_after_query_start_us": value(fields, "native_present_end_us"),
          "native_raster_after_query_end_us": value(fields, "native_present_end_us"),
          "native_raster_after_in_vertical_blank": 1}, semantic, relationship),
        ({"flip_protection_query_start_us":
          value(fields, "native_raster_before_query_start_us") + 1}, timestamp, ordering),
        ({"flip_protection_query_end_us":
          value(fields, "native_raster_before_query_end_us") - 1}, timestamp, ordering),
    ]
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        output = directory / "result.json"

        def replay(path):
            output.unlink(missing_ok=True)
            result = subprocess.run([str(args.replay.resolve()), str(path),
                                     "--require-exact-baseline", "--output", str(output)],
                                    capture_output=True)
            assert output.exists(), result.stderr.decode()
            return result, json.loads(output.read_text())

        def write_mutation(changes):
            lines = list(original)
            changed = list(fields)
            for key, replacement in changes.items():
                changed[indices[key]] = str(replacement)
            lines[selected] = (",".join(changed) + "\n").encode()
            footer_at = next(index for index, line in enumerate(lines)
                             if line.startswith(b"#vrr_trace_footer,"))
            body = b"".join(lines[:footer_at])
            footer = lines[footer_at].decode().strip().split(",")
            footer = [f"decoded_sha256={hashlib.sha256(body).hexdigest()}"
                      if part.startswith("decoded_sha256=") else part for part in footer]
            modified = directory / "modified.csv"
            modified.write_bytes(body + (",".join(footer) + "\n").encode())
            return modified

        result, baseline = replay(args.fixture.resolve())
        assert result.returncode == 0, result.stderr.decode()
        assert baseline["fidelity"]["baseline_exact"]
        assert baseline["capture"]["recorded_sequence_integrity_valid"]
        assert baseline["fidelity"]["exact_raster_envelope_classifications"] == \
            baseline["capture"]["presented_frames"]
        # A guard can select synchronization before querying raster. An opened
        # source and no sample must remain legal, including historical captures.
        absent = {name: 0 for name in columns if name.startswith("native_raster_before_")}
        result, summary = replay(write_mutation(absent))
        assert result.returncode == 0 and summary["fidelity"]["baseline_exact"], (
            "optional guard sample", result.returncode, result.stderr.decode())
        assert summary["capture"]["recorded_sequence_integrity_valid"]

        for changes, group, counter in cases:
            result, summary = replay(write_mutation(changes))
            assert result.returncode == 3 and not summary["fidelity"]["baseline_exact"], (
                changes, result.returncode, result.stderr.decode())
            assert summary["capture"][group][counter] > 0, (changes, group, counter)
    print(f"Exact DXGI protection baseline, optional sample, and all {len(cases)} rejection checks passed")


if __name__ == "__main__":
    main()
