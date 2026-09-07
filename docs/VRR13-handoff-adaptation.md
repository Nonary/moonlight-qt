# VRR14 integration with measured smoothness feedback

Production keeps the restored VRR14 predictor, source-clock smoothing, native
presentation prediction, and integration features. It additionally enables the
versioned `playout_smoothness_feedback_enabled` policy. Older captures default
that parameter to zero and retain their original behavior.

## Buffer accounting

The five-minute readiness histogram learns receiver jitter, temporary FIFO
backlog, excess preparation work, and render scheduling delay. Typical work is
part of the expected schedule. Intentional pacing and swapchain acquisition
waits are excluded; sustained processing overload is not cached as jitter.

The required protection is the greater of the readiness p99.95 estimate and
measured smoothness correction. Subtract usable headroom exactly once, then
apply the existing separate 300 us guard and minimum buffer. Thus an 8 ms
protection estimate with 2 ms headroom requests 6 ms of buffer before the guard.
The 3 ms violation threshold is not additional free latency credit.

Headroom is `max(0, source period - max(display period, typical rendering))`.
Readiness profile version 15 distinguishes the new accounting from version 14
(which subtracted the tolerance) and version 13. Incompatible histories are
rejected, including in replay; cached readiness still survives compatible
sessions. Measured interval correction is learned anew each session.

## Actual smoothness feedback

Two independent histories track consecutive submission intervals and matched
native presentation intervals. Each compares the actual interval with the
original intended smoothed interval. Constant presentation offset does not
count as a smoothness failure. Interval errors **>= 3000 us** are violations.
Native samples whose timestamp uncertainty straddles that boundary remain
unavailable evidence. Gaps, cancellation, source discontinuity, host stalls,
and unmatched feedback cannot manufacture successful intervals.

A violation requests the affected frame's original buffer plus its headroom,
plus the error beyond tolerance (including one microsecond at the boundary).
A catch-up interval is attributed to its preceding late frame. This prevents
delayed feedback from repeatedly adding to a newer, larger buffer. Successful
intervals enter the correction histogram as zero demand, preserving the
99.95% denominator. The two histories combine by maximum, not addition.

The controller raises applied delay by at most 500 us per frame, releases it
gradually after clean evidence, and retains five-minute tail history. The old
8 ms/cadence-based ceiling is removed. The physical queue capacity remains a
hard bound: a requirement beyond it is explicitly reported as capacity limited.
Buffering cannot guarantee the target under unbounded downstream timing faults.

## Native correlation and diagnostics

Typical preparation cost determines the submission offset; its tail determines
when preparation begins. Matched native feedback learns median compositor lead
and the observed scanout floor. DXGI timestamps are joined by refresh identity,
not by treating SyncQPCTime as the timestamp of its accompanying PresentCount.
Samples before submission or outside validity bounds are rejected. Sequence
continuity resets across timing epochs and presentation-mode changes.

Trace rows include original deadlines, predicted scanout, compositor lead,
headroom, requested buffer, correction protection, capacity-limit status, and
both smoothness sample/miss counts. Replay verifies all recorded controller
fields. Its `simulation.smoothness_feedback` object exposes candidate demand
and window counts. Candidate native feedback uses recorded service latency
shifted with submission; it is a model, not observed candidate scanout.

`scripts/vrr-deadlines.py` retains the absolute-deadline audit and reports the
online interval-feedback counters separately. Missing native evidence remains
unavailable, and success percentages apply only to observed eligible intervals.
A sparse native sample set does not establish 99.95% whole-session smoothness.

## Preserved features and deployment

RTP mapping, cadence fitting, 20% source-clock phase smoothing, per-frame D3D11
protection, asynchronous overlays, native-refresh FPS choices, renderer lifetime
fixes, decoder completion fences, and local replay-grade capture remain enabled.
Both launchers retain the stable `vrr-lite` directory and exact replay gate.
Use the fast Windows build and complete staging/ZIP/share verification workflow.

## Validation

Deterministic tests cover 2999/3000/3001 us boundaries, constant lateness,
headroom accounting, delayed/catch-up feedback, missing/out-of-order samples,
uncertainty, saturated demand, versioned profiles, and native-only correction.
A controller loop with repeated 15 ms readiness-unmodeled delay raises its
buffer and has no additional interval misses in its final 1999 intervals.
Worker trace fixtures exercise cached startup and warm-up with a controlled
preparation stall, followed by exact serialization/replay checks.

The latest completed gameplay capture selected for this change is
`C:\Users\Chase\vrr-traces\Moonlight-vrr-20260906-235259-313.vrrtrace`,
4,796,150 bytes, last written 2026-09-07 04:56:20.7172745 UTC, SHA-256
`9DE0D289147E6613DDC3C71F4CD44547C42D317A8B7D128E27F29DD37E0C3C53`.
Both trace roots were re-enumerated and this identity rechecked before comparison.
The fresh unchanged-policy baseline exits 3 and the matching launcher sidecar
reports non-exact replay, despite valid recorded sequence integrity. All
comparisons below are exploratory, not strict live A/B proof.

Modeled presented jerk over 2 ms changes from 19.9% to 2.9%; jerk p95 from
5.764 to 1.479 ms and p99 from 9.454 to 3.542 ms. Sender-spacing error p99 changes
from 5.381 to 3.821 ms. Mean decode-to-submission delay changes from 6.652 to
13.715 ms; p99 from 11.535 to 17.710 ms. The candidate ends with 33 submission
violations in 16,629 eligible intervals and 222 modeled native violations in
5,015 observed intervals. It reaches the queue bound on 16,592 decisions, with
25.819 ms requested versus 14.746 ms applied at the end. This does not establish
the 99.95% goal; the limit and incomplete native coverage are explicit.

Nominal, periodic decision/preparation/submission faults, and scheduler bursts
are evaluated together by `tests/vrr/configs/smoothness-feedback-stress.json`.
Each asserts zero modeled interval violations and p99 decode-to-submission
latency <= 30 ms. These injected-fault bounds are not normal operating latency.
