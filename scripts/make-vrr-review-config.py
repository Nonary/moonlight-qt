#!/usr/bin/env python3
"""Build an isolated VRR14/17 comparison from a current-policy replay summary.

First run vrrreplay CAPTURE --output current-policy.json (without --set,
--config or --require-exact-baseline). Exact historical replay is a separate gate.
Partial controller overrides otherwise start from generic replay defaults.
"""
import argparse
import copy
import json
from pathlib import Path


def make_config(summary, variants):
    if "scenarios" in summary:
        summary = next(s for s in summary["scenarios"] if s["scenario"] == "session-policy")
    base = summary["simulation"]["resolved_parameters"]["controller"]
    if not base.get("timestamp_playout_enabled") or base.get("playout_responsive_buffer") != 7:
        raise ValueError("Expected a complete current interval-policy controller snapshot")
    if variants.get("review_schema") != 1:
        raise ValueError("Unsupported review variant schema")
    # Keep one full base for every variant, including the control. The replay
    # parser merges the individual parameter objects over this complete base.
    return {"config_schema": 1, "parameters": {"controller": copy.deepcopy(base)},
            "scenarios": copy.deepcopy(variants["scenarios"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--variants", type=Path,
                        default=Path(__file__).resolve().parents[1] / "tests/vrr/configs/vrr17-review-variants.json")
    args = parser.parse_args()
    try:
        config = make_config(json.loads(args.summary.read_text()), json.loads(args.variants.read_text()))
        args.output.write_text(json.dumps(config, indent=2) + "\n")
    except (OSError, ValueError, KeyError, StopIteration) as error:
        parser.exit(1, f"make-vrr-review-config: {error}\n")


if __name__ == "__main__":
    main()
