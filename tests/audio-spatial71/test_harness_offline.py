#!/usr/bin/env python3
"""Exercise the native harness without activating an audio stream.

The caller supplies Qt/SDL runtime DLLs beside the executable or on PATH.
Endpoint enumeration reads metadata only; identifiers are not printed here.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


def execute(binary, label, arguments, expected, event=None, allow_unavailable_discovery=False):
    result = subprocess.run(
        [str(binary), *arguments],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
        check=False,
    )
    if allow_unavailable_discovery and event == "endpoint_inventory" and result.returncode == 1:
        # Hosted Windows runners may lack running audio services. Only these
        # documented service failures may skip optional inventory; an arbitrary
        # renderer/API failure must still fail the suite. No service is started.
        service_errors = {"0x80070422", "0x80070424", "0x800706ba", "0x88890010"}
        matches = re.findall(
            r"Spatial 7\.1 endpoint enumeration: [^\r\n]+ failed: (0x[0-9a-fA-F]{8})", result.stderr
        )
        records = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        inventory = [record for record in records if record.get("event") == event]
        if (len(matches) == 1 and matches[0].lower() in service_errors and len(inventory) == 1
                and inventory[0].get("result") == "FAIL"
                and inventory[0].get("active_endpoint_opened") is False):
            print(f"SKIP {label}: Windows audio service unavailable ({matches[0]}); discovery unverified")
            return False
    if result.returncode != expected:
        raise AssertionError(f"{label}: exit {result.returncode}, expected {expected}")
    if expected == 2 and "no audio device opened" not in result.stderr:
        raise AssertionError(f"{label}: missing argument-rejection marker")
    if event:
        records = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        matching = [record for record in records if record.get("event") == event]
        if len(matching) != 1 or matching[0].get("result") != "PASS":
            raise AssertionError(f"{label}: expected one successful {event}")
        record = matching[0]
        if record.get("active_endpoint_opened") is not False:
            raise AssertionError(f"{label}: no-stream contract missing")
        if event == "invalid_endpoint_test" and record.get("cases") != 7:
            raise AssertionError(f"{label}: incomplete rejected-configuration coverage")
        if event == "endpoint_inventory":
            for endpoint in record["endpoints"]:
                if not isinstance(endpoint.get("id"), str) or not endpoint["id"]:
                    raise AssertionError(f"{label}: invalid endpoint ID")
                if not isinstance(endpoint.get("name"), str) or not isinstance(endpoint.get("is_default"), bool):
                    raise AssertionError(f"{label}: invalid endpoint metadata")
    elif expected == 0 and "actual optional Moonlight renderer" not in result.stdout:
        raise AssertionError(f"{label}: missing help text")
    print(f"PASS {label}")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("Spatial71RendererTest.exe"))
    parser.add_argument("--allow-unavailable-discovery", action="store_true",
                        help="Report optional inventory as SKIP for known unavailable audio-service HRESULTs")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    execute(binary, "help", ["--help"], 0)
    execute(binary, "no-arguments", [], 0)
    execute(binary, "rejected-configurations", ["--invalid-endpoint-test"], 0, "invalid_endpoint_test")
    discovered = execute(binary, "read-only-enumeration", ["--list-endpoints"], 0, "endpoint_inventory",
                         allow_unavailable_discovery=args.allow_unavailable_discovery)
    prefix = ["--endpoint-id", "invalid", "--silence"]
    rejected = [
        ("mode-only", ["--tones"]),
        ("endpoint-only", ["--endpoint-id", "invalid"]),
        ("default-only", ["--default-endpoint"]),
        ("conflicting-modes", [*prefix, "--tones"]),
        ("unknown", [*prefix, "--unknown", "1"]),
        ("zero-duration", [*prefix, "--seconds", "0"]),
        ("excess-duration", [*prefix, "--seconds", "3601"]),
        ("noninteger-duration", [*prefix, "--seconds", "1.5"]),
        ("zero-cycles", [*prefix, "--cycles", "0"]),
        ("excess-cycles", [*prefix, "--cycles", "21"]),
        ("gap-missing-start", [*prefix, "--gap-ms", "50"]),
        ("gap-missing-duration", [*prefix, "--gap-at-ms", "1000"]),
        ("gap-beyond-test", [*prefix, "--seconds", "1", "--gap-at-ms", "900", "--gap-ms", "500"]),
        ("duplicate-endpoint", [*prefix, "--endpoint-id", "invalid"]),
        ("duplicate-default", ["--default-endpoint", "--default-endpoint", "--silence"]),
        ("default-and-explicit", ["--default-endpoint", *prefix]),
        ("explicit-and-default", [*prefix, "--default-endpoint"]),
        ("empty-explicit-id", ["--endpoint-id", "", "--silence"]),
        ("missing-value", [*prefix, "--seconds"]),
        ("help-extra", ["--help", "--tones"]),
        ("enumeration-extra", ["--list-endpoints", "--silence"]),
    ]
    for label, arguments in rejected:
        execute(binary, label, arguments, 2)
    passed = len(rejected) + 3 + int(discovered)
    skipped = " SKIP 1 optional discovery check;" if not discovered else ""
    print(f"PASS {passed} native harness checks;{skipped} no audio stream activated")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"FAIL {error}", file=sys.stderr)
        sys.exit(1)
