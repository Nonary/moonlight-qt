# Comparing measured latency presets

Use actual captures from each preset to compare Linux latency. A counterfactual
replay of one setting retains assumptions about decoder readiness and native
service costs that can conceal the live differences between settings.

The development launcher `moonlight-dev` already enables asynchronous compressed
tracing into `~/moonlight-logs`. The updated development binary records the active
preset and starting calibration on every row, including a dropped first frame.
Reconnect after selecting each preset. Close Moonlight normally after a run so
the trace receives its completeness footer. Reconnected sessions are archived
separately; do not combine them into one result.

Play the same repeatable scene for each of Lowest latency, Balanced, and
Smoothest, keeping resolution, FPS, refresh, HDR, frame smoothing, and Gamescope
test settings constant. Include a few minutes of steady gameplay and a recovery
period after any deliberate disturbance. Keep calibration in place for the first
comparison of the behavior experienced in normal use; each preset's separate
starting profile is recorded. Do not silently clear it to manufacture equal
startup conditions. Repeat in a different order if workload drift is material.

The temporary automatic oscillation option was removed on 2026-09-11.
Select presets manually and reconnect for each capture. The report still splits
older oscillating captures into phases for historical analysis.

Historical oscillation captures kept the source clock and live history across switches, while
bypassing normal calibration loading/saving. It tests live transitions, not fresh
independent preset sessions. Idle/suspended time counts; a recreated worker starts
over. Parameters and phase IDs are recorded per row, and reports do not calculate
cadence pairs across phase boundaries. Completeness refers to the entire original
capture, not a fabricated standalone trace for each phase. Fixed-policy exact
replay does not support parameter transitions within these oscillating captures.
Use non-oscillating sessions when strict controller replay is required.

After closing the sessions, pass the explicitly selected capture paths:

```sh
python3 scripts/report-vrr-latency.py \
  /path/to/lowest.vrrtrace /path/to/balanced.vrrtrace /path/to/smoothest.vrrtrace \
  --output build/latency-captures.json --markdown build/latency-captures.md
```

The report identifies presets from metadata, verifies footer accounting and
decoded-content hashes, and keeps captures separate. Missing presets, mixed
configuration, incomplete captures, and old captures without decoder-output
timing are explicit. JSON includes the full path, size, modification time,
SHA-256, policy fingerprint, initial-profile hash, and metric sample counts.
Capture completeness is not the strict replay gate and does not establish matched
workloads or physical display validation.

The main table gives mean client processing, queue/pacing, rendering, padding,
and submission jerk. Client processing ends at the presentation call's return;
queue/pacing, rendering and explicit GPU decode waiting partition it with the same valid-frame denominator.
Padding is a controller allowance, not an additive stage. JSON additionally
reports median/p95/p99, decoder output versus GPU readiness boundaries,
decode-sync waiting, queue residence, acquisition/render costs, drop outcomes,
and buffer-history release eligibility.

Submission jerk counts adjacent interval changes over 2 ms with its denominator.
Host source intervals over 25 ms are counted separately across all delivered
frames, including locally dropped ones. Sender-spacing residual excludes those
host stalls. Neither metric proves visible smoothness or click-to-photon latency.
Native presentation feedback remains separate and retains its existing validation.

For old traces, `ready_to_submission_us` remains available, but missing
`decoder_output_us` makes overlay-equivalent latency unavailable. Never relabel
the former as the latter. Run strict replay separately when testing controller
fidelity; the diagnostic extension does not relax that gate for Vulkan.

## Queue statistic correction (2026-09-11)

The current queue/pacing report excludes `decode_sync_wait_us`, matching the
overlay. Full client processing still includes this wait for diagnostics. No
new decode-wait overlay row is added and the existing decoding-time statistic
is unchanged. Older saved reports used queue as the entire non-render remainder;
regenerate them before comparing their queue column with current reports.
Linux now selects the same prediction-based buffer policy as Windows; historical
Linux captures retain their recorded event-gated policy for exact replay.
