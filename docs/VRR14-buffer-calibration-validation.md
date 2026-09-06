# Bufferable-delay calibration

Implemented September 6, 2026, following the controller 15 build documented in
`VRR14-hybrid-validation.md`. The current controller is version 16 and the cache
payload is version 11; older learned histories are rejected.

The reserve now targets `max(0, p99.95(bufferable error) - 3 ms)`, with no fixed
2 ms cushion. A miss is strictly more than 3 ms beyond the complete applied
buffer. Eligible successes, including errors between 0 and 3 ms, remain in the
rolling five-minute history. The existing 0.5 ms minimum buffer and 50 us timing
guard remain. Cached values exclude the separately applied guard to prevent
repeated restarts from adding it again.

Calibration compares observed receiver work against normal work on the same
source cadence. Intentional smoothing, swapchain image waits, prior GPU work,
and backlog caused by the source cadence cannot inflate the learned reserve.
Temporary busy episodes qualify when they drain; sustained overload and skipped
work do not qualify. A later miss holds release without restoring the full
cold-start allowance. Repeated successful sessions can qualify a zero reserve.

The overlay reports pipeline buffer coverage separately from native presentation
timing and identifies processing overload. Full elapsed preparation and queue
latency remain visible in telemetry.

## Validation

- Linux application build and installed-binary version smoke passed in
  `moonlight-dev`.
- Timing tests passed under ASan/UBSan, including exact 3 ms boundaries, all-sample
  percentile accounting, variable game cadence, receiver jitter at 30–120 FPS,
  temporary work bursts, sustained overload, dropped work and recovery.
- Cache tests passed, including zero-reserve repeated starts, non-compounding
  guards, native-presentation qualification and rejection of old histories.
- Worker ownership, cancellation and exact checkpoint replay tests passed.
- Lab integration and deadline-audit tests passed. Capturing and resimulating
  the same fixed-display workload reproduces its results without charging image
  acquisition or prior GPU waits twice.

Controlled 310-second simulations used a 144 Hz variable-refresh display model,
approximately 1–1.25 ms preparation work, and no network jitter, compositor delay,
feedback delay, injected stall or scheduling error. Each rate retained five
minutes of eligible evidence, with zero drops and 100% modeled buffer coverage.

| Source FPS | Final reserve target | Final 30 s applied buffer | Final 30 s decode-to-presentation p95 |
| --- | --- | --- | --- |
| 30 | 0 ms | 0.500 ms | 1.634 ms |
| 60 | 0 ms | 0.500 ms | 1.631 ms |
| 75 | 0 ms | 0.500 ms | 1.636 ms |
| 100 | 0 ms | 0.500 ms | 1.640 ms |
| 120 | 0 ms | 0.500 ms | 1.634 ms |

Cold startup still begins with one negotiated frame interval. The unchanged
20 us per selected frame release limit means a cold 30 FPS stream takes about
59 seconds to reach the minimum, including its initial evidence gate. Its full
310-second p95 therefore includes startup delay (27.809 ms); this is distinct
from settled latency. Proven cache starts can avoid that initial allowance.

Results and bounded captures are in `build-tests/vrr14/bufferable/`; deployment
hashes and the rollback binary path are recorded in `deployed.json` there.
These are controlled simulations, not a fresh live stream. Live smoothness,
queue delay, and superiority over VRR13 remain to be measured.
