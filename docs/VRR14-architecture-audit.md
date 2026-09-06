# VRR14 architecture audit against VRR13

The current controller's readiness success rate does not establish smooth,
low-latency presentation. The September 5 23:29 logged run reports 99.9809%
model coverage, 18.07 ms average queue delay, 6.09 ms preparation, and 1.68%
client pacing drops. That contradicts treating model coverage as the objective.
No application code, user settings, or desktop binary was changed by this audit.

## Confirmed differences and failures

1. **The smoothing preference was not carried over.** The user's stored VRR13
   choice is `smoothvrrframetiming=true`; VRR14 uses a separate key defaulting to
   false and currently has `vrr14smooth=false`. Older logs explicitly identify
   smoothed frame timing; the new log identifies host cadence. These modes have
   different objectives. Comparing their apparent smoothness as the same policy
   was a mistake. Enabling VRR14's existing smoother alone would not reproduce
   VRR13's different smoothing algorithm or solve the latency failures.

2. **The worker waits for GPU completion before submission.** The Vulkan path
   issues a timeline signal and blocks the pacing worker in `vkWaitSemaphores`.
   The retained capture averages 5.33 ms in that wait, then 3.01 ms between GPU
   preparation completion and submission. VRR13 queued rendering asynchronously.
   This establishes serialization, not that all 8.34 ms can be removed: GPU work,
   image ownership and presentation dependencies still have to be respected.

3. **A late frame gets a later target.** `Controller::prepared()` calls
   `constrain()`, which raises the target to at least current time plus compositor
   lead. The final target is then used by the simulator's prediction-error metric.
   The buffer coverage metric instead counts a virtual readiness model; it does
   not count native presentation misses. Both are inadequate replacements for
   comparing native presentation against an immutable intended deadline.

4. **Five minutes of retention became five minutes of mandatory cold-start
   protection.** Reserve release requires a complete live window, except after
   three separately proven sessions and 60 seconds of new validation. Histogram
   preload alone does not permit fast adaptation. Retention, confidence, and the
   time needed to react are different design decisions and should be separate.

5. **The controller uses late evidence as a reason to add queue delay without
   identifying which action can fix it.** Transport variation can require a
   jitter reserve. Slow GPU work can require earlier preparation. Presentation
   variation can require a different submission lead. Treating those as one
   interchangeable pressure signal risks spending latency without fixing cadence.

## Measured audit

Capture: `moonlight-pacing-20260905-232929-299157.vrr14`. The bounded recorder
retains 31.71 seconds, not the complete 62-second calibration history. After
requiring complete frame records and valid correlated presentation timestamps:

| Measurement | Retained capture |
| --- | ---: |
| Paired frames | 3,273 |
| Presented more than 3 ms after first plan | 266 |
| Presented more than 3 ms after final revised target | 6 |
| Targets shifted later by more than 3 ms | 273 |
| Mean decode to presentation | 26.63 ms |
| p95 decode to presentation | 34.78 ms |
| Mean waiting outside preparation | 18.44 ms |

These are observed timestamps, not a counterfactual claim that a different
scheduler would present on time. First plans already contain scheduling floors;
they do not reconstruct a lost pre-planning ideal deadline. Missing or invalid
feedback is not scored as success. The preliminary 276-frame count included
feedback with timestamps beyond callback observation; the audited 266 excludes
those invalid samples. The different session/capture windows must not be combined
into one percentage.

## Replacement design and acceptance conditions

Use VRR13's established source-cadence policy as the behavioral reference, with
the same smoothing mode. Keep one intended presentation deadline per frame and
record any later execution deadline separately. Measure actual presentation
error and drops independently of calibration readiness and feedback availability.

Preserve the five-minute rolling history and cache, but separate it from the
response time of calibration. Borrow historical confidence explicitly; never
label cached observations as fresh measured successes.

Make timing observation asynchronous where the backend permits it. Preserve
GPU semaphore/fence dependencies and bounded images in flight; do not treat
removing a CPU wait as permission to display unfinished work. Attribute deadline
failures to readiness, scheduling, and presentation before choosing whether to
start earlier, submit earlier, or increase the jitter reserve.

Lower FPS supplies more time to finish work between frames. It does not make
arbitrary network jitter disappear: alternating 0/6 ms arrival delays still
require phase protection to preserve source cadence at both 30 and 116 FPS.
The prior pipeline study already demonstrated this; an unconditional frame-time
subtraction is not a valid general solution.

A candidate must be compared with VRR13 using the same source cadence, rendering
work, mode and frame identities. Report latency, real initial-deadline errors,
drops, output interval variation, and feedback coverage together. The earlier
idealized simulator does not model the measured compositor, GPU scheduling and
synchronization behavior well enough to establish that a candidate is better
in live playback. Successful self-replay establishes determinism, not improvement.

## Reproduction

```sh
python3 scripts/vrr_deadlines.py /home/deck/moonlight-logs/moonlight-pacing-20260905-232929-299157.vrr14 --output build-tests/vrr14/architecture-audit/latest.json
python3 tests/vrr14/test_deadline_audit.py
```

The audit regression proves that moving a target cannot erase a late frame,
exactly 3 ms passes, and unavailable feedback cannot manufacture a success.
VRR13 source snapshots and prior comparative work remain under
`build-tests/vrr/architecture-20260905/` and `tests/vrr/`.
