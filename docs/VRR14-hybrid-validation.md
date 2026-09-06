# VRR13 cadence with VRR14 timing observation

Implemented September 6, 2026. This follows the failure analysis in
`VRR14-architecture-audit.md`; that document describes the preceding binary.

The new path restores the damped VRR13 source clock and migrates its smoothing
preference. Vulkan image completion runs on an observer thread, preserving native
dependencies and the five-surface limit. Each delayed measurement retains its
original frame's calibration inputs. Overlapping GPU work is not charged twice,
and compositor latency excludes the wait for image completion.
Completion brackets wider than 500 us are excluded from calibration, so a
descheduled observer cannot inflate the learned GPU tail.

Five-minute history retention is separate from the release gate (128 samples,
two clean seconds, then gradual release). The tolerance-adjusted learned tail
keeps its 2 ms cushion. Cache histories use model 15/profile 10. Repeated proven
starts additionally require a full window of passing native presentation evidence;
model readiness alone cannot qualify them.

The first feasible presentation plan is immutable. Runtime and lab telemetry
compare actual presentation against that plan, preserve later execution targets
separately, and count errors strictly greater than 3 ms. Missing feedback is not
success. Queue delay still includes actual waits, and preparation still includes
the full span through observed image completion.

## Validation

- Linux application build passed in `moonlight-dev`.
- Timing/recovery/percentile tests and ASan/UBSan passed, including 30, 60, 75,
  100 and 120 FPS workloads, delayed measurement association, either feedback
  arrival order, unchanged deadlines, and the strict 3 ms boundary.
- Worker tests passed with two frames submitted before either GPU completion,
  retained decoder surfaces, cancellation, complete cleanup and exact replay.
- Native Vulkan dependency tests passed on **AMD Custom GPU 0932 (RADV VANGOGH)**,
  including asynchronous submission behind an unsignaled imported dependency,
  HDR-capable image storage, readback and the synchronous overlay path.
- Cache tests cover missing or failed presentation evidence, repeatable starts,
  duplicate saves, expiry, bounded storage, lock contention and corrupt data.
- Lab integration covers deterministic replay after recorder overwrite,
  feedback loss, fixed-refresh behavior, concurrent sweeps and resume.

Simulation artifacts are in `build-tests/vrr14/hybrid/`. Controlled 310-second
workloads at 30, 60, 75, 100, 116 and 120 FPS retained a 2 ms learned reserve and
had no drops. The simulated measured deadline rate was 100%. At the exact display
ceiling, decode-to-presentation latency remained higher; passing a deadline metric
alone is not evidence of minimum latency.

The recorded 23:29 workload is more demanding. Under the lab's assumed display
and GPU model, mean/p95 decode-to-presentation changed from 22.09/27.11 ms for the
previous smoothed path to 20.96/24.84 ms for the hybrid. Both were simulated, not
new live measurements. The hybrid's original-deadline coverage was only 80.35%
in this scenario. This does **not** establish the 99.95% live target or a large
smoothness improvement. In particular, elapsed GPU/decode dependency waits from
an old capture do not identify the GPU's service demand under a different pipeline.

A fresh logged stream is required to assess actual queue delay, presentation
misses, pacing drops, cadence and feedback coverage together. The implementation
and synchronization checks are complete; live superiority over VRR13 is unproven.
