#!/usr/bin/env python3
"""Join passive D3D11 frame identities to PresentMon v1 QPC events.

No nearest-frame guess: require one unused event on the same PID/swapchain
inside the recorded QPC bracket. Keep unmatched/ambiguous rows explicitly.
OS display timing is evidence, not an optical scanout or tear measurement.
"""
import argparse
import bisect
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path


def rows(path):
    with Path(path).open(encoding="utf-8-sig", newline="") as stream:
        yield from csv.DictReader(line for line in stream if not line.startswith("#"))


def correlate(gpu_rows, present_rows):
    groups = defaultdict(list)
    for index, row in enumerate(present_rows):
        key = (int(row["ProcessID"]), int(row["SwapChainAddress"], 16))
        groups[key].append((int(row["QPCTime"]), index, row))
    for group in groups.values():
        group.sort(key=lambda item: item[0])
    ticks = {key: [item[0] for item in group] for key, group in groups.items()}
    matches, claimed, count = [], set(), Counter()
    for row in gpu_rows:
        if row["event"] != "d3d11_present":
            continue
        key = (int(row["a"]), int(row["object_id"]))
        begin, end = int(row["c"]), int(row["d"])
        group, times = groups.get(key, []), ticks.get(key, [])
        candidates = group[bisect.bisect_left(times, begin):bisect.bisect_right(times, end)]
        status = "missing" if not candidates else "ambiguous"
        output = dict(row)
        if begin <= 0 or end < begin or int(row["e"]) <= 0:
            status = "invalid_clock"
        elif len(candidates) == 1:
            qpc, index, event = candidates[0]
            if index in claimed:
                status = "duplicate_claim"
            else:
                claimed.add(index)
                status = "matched"
                output.update({"pm_" + name: value for name, value in event.items()})
                output["event_offset_us"] = (qpc - begin) * 1e6 / int(row["e"])
        output["match_status"] = status
        count[status] += 1
        matches.append(output)
    return matches, dict(count)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="Full diagnostic capture directory")
    args = parser.parse_args()
    sidecars = sorted(args.capture.glob("Moonlight.vrrtrace.gpu-*.csv"))
    gpu, sidecar_integrity = [], []
    for path in sidecars:
        gpu.extend(rows(path))
        with path.open("rb") as stream:
            stream.seek(max(0, path.stat().st_size - 256))
            footer = stream.read().decode("utf-8", errors="replace").splitlines()[-1:]
        sidecar_integrity.append({"name": path.name, "complete": footer == ["# closed=1; truncated=0; dropped_rows=0"]})
    matched, counts = correlate(gpu, list(rows(args.capture / "PresentMon.csv")))
    fields = list(dict.fromkeys(key for row in matched for key in row))
    with (args.capture / "windows-frame-correlation.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(matched)
    summary = {"schema": 1, "native_present_rows": len(matched), "matches": counts,
               "sidecars": sidecar_integrity,
               "all_native_presents_matched": bool(matched) and counts.get("matched", 0) == len(matched),
               "etw_loss_verified": False,
               "scope": "Exact QPC bracket + PID + swapchain matching. ETW loss and coverage require separate inspection. AllowsTearing is permission, not observed tearing. Native timing changes under a counterfactual remain modeled."}
    (args.capture / "windows-frame-correlation.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if summary["all_native_presents_matched"] and all(x["complete"] for x in sidecar_integrity) else 2


if __name__ == "__main__":
    raise SystemExit(main())
