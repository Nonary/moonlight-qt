# Post-fix VRR telemetry assessment

Date: 2026-07-19  
Branch reviewed: `vrr-lite`  
Source HEAD reviewed: `faa166b7` (`feat(vrr): split pacing telemetry by outcome`)  
Primary completed post-fix session: `moonlight-20260719-204945.log`

## Executive summary

The reporting repair is a substantial improvement. The old VRR statistics
handoff had a data race, mixed unrelated meanings into one readiness-miss
counter, omitted the final partial statistics window, and printed controller
state without enough context to tell whether it was current. The new design
gives the pacer ownership of cumulative telemetry, publishes coherent
snapshots under a mutex, lets the decoder merge snapshot deltas, finalizes the
partial window at shutdown, and splits timing observations from actual output
failures.

The first completed session with the new report looks healthy. Across 42,019
VRR-eligible frames it reports no network or jitter drops, no presentation
failures or cancellations, no pacing drops, and no spacing corrections.
Incoming, decoded, and rendered rates all match at 97.45 FPS. Preparation was
late for 6.70% of eligible frames, but those frames were still presented and
their recent conditional lateness was modest: 1.02/2.25/3.67 ms at
p50/p95/p99. There is no evidence in this session of a failed recovery loop,
controller poisoning, or persistent degradation carried over from earlier
sessions.

One new statistic is misleading: `submit late` reports 100.00%. This is not
evidence that every frame missed display timing. The worker deliberately
waits until the presentation target is reached before calling Present, then
classifies the submission as late when the measured submission boundary is
strictly greater than that same target. Normal call overhead makes a value
slightly greater than the target nearly inevitable. The counter is behaving
as implemented, but the implemented predicate does not distinguish healthy
on-target submission from materially late submission.

## What changed in the reporting repair

### Pacer telemetry now has a single synchronized owner

Previously, the VRR pacing worker wrote directly into
`m_ActiveWndVideoStats` while the decoder thread concurrently copied, summed,
and zeroed that same structure. Those plain unsynchronized reads and writes
were a C++ data race. They could lose increments, count an old value again
after a rollover, or combine controller fields published by different frames.

The repair introduces `PacerTelemetry`, which owns cumulative pacer counters
and current controller state. Producers update it while holding a mutex, and
readers receive a complete `PacerTelemetrySnapshot` under the same mutex. The
decoder retains its last snapshot and merges only the delta into its own
windowed statistics.

This is better than making individual fields atomic because a report needs a
coherent multi-field observation. Per-field atomics could prevent torn values
while still combining counters, sequence information, and controller state
from different moments.

Relevant implementation:

- `app/streaming/video/ffmpeg-renderers/pacer/pacertelemetry.h`
- `FFmpegVideoDecoder::syncPacerTelemetry()` in
  `app/streaming/video/ffmpeg.cpp`

### The ambiguous readiness counter was split by cause and outcome

The old `VRR readiness misses` value combined preparation lateness with late
entry into the target waiter. It was easy to interpret it as dropped frames or
failed recovery even though the frames could still be presented normally.

The new report separates:

- preparation finishing after the target;
- entry into the target wait after its deadline;
- submission occurring after the target;
- failed and cancelled presentations;
- pacing drops and display-spacing corrections.

It also supplies the eligible-frame denominator and percentages. That makes
it possible to distinguish an internal timing observation from a real output
failure. In the completed session, 2,815 preparations were late but failures,
cancellations, and drops were all zero. The earlier single counter could not
express that distinction.

### Preparation lateness now includes magnitude, not just frequency

The report includes recent p50/p95/p99 preparation-lateness values. This
matters because frequency alone does not describe severity. A frame that
finishes preparation tens of microseconds after a target is very different
from one that arrives an entire source period late.

The observed 1.02/2.25/3.67 ms percentiles are conditional on late
preparations: the sample buffer contains lateness values only when
`prepareLate` is true. They should therefore be read as “how late the recent
late frames were,” not as percentiles across all frames.

### Controller state is timestamped and internally consistent

The new state contains a monotonically increasing sequence and sample time.
The final report identifies the latest state as sequence 42,020 and only
48.72 ms old. The one-count difference from 42,019 eligible frames is expected
because beginning the VRR session also advances the telemetry sequence.

The state fields now describe one decision-time sample:

```text
readiness budget: 2.13 ms
timing reserve:   9.22 ms
display guard:    0.13 ms
render lead:      5.30 ms
render wake lead: 0.01 ms
target wake lead: 0.00 ms
source period:   10.18 ms
```

This is much safer than copying nonzero fields independently and treating zero
as an implicit validity flag. Zero is valid during startup or a rebase and
should not preserve stale state from an older decision.

### Shutdown now merges the final partial window

The decoder now shuts down the pacer, takes a final cumulative snapshot, and
merges the remaining active window before logging global statistics. Short
sessions and the final fraction of a second should no longer silently lose
telemetry during a normal shutdown.

## Evidence from the completed post-fix session

The 20:49 session ran for approximately 7 minutes 20 seconds and exited through
the gamepad quit combination. Its final report is:

```text
Incoming frame rate from network: 97.45 FPS
Decoding frame rate: 97.45 FPS
Rendering frame rate: 97.45 FPS
Host processing latency min/max/average: 1.8/516.5/2.3 ms
Frames dropped by your network connection: 0.00%
Frames dropped due to network jitter: 0.00%
Average network latency: 1 ms (variance: 0 ms)
Average decoding time: 0.58 ms
Average frame queue delay: 2.95 ms
Average rendering time: 3.19 ms
VRR eligible: 42019; prepare late 6.70% (2815,
    recent p50/p95/p99 1.02/2.25/3.67 ms)
VRR wait-entry/submit late: 0.01%/100.00%;
    failed/cancelled: 0/0; drops/spacing: 0/0
```

The 97.45 FPS average is not evidence of Moonlight dropping frames from the
116 FPS configured stream. The incoming, decoding, and rendering rates are
identical, and every eligible VRR frame was rendered. The lower average means
the host supplied about 97.45 distinct frames per second over this interval,
likely because the game did not continuously render at the configured stream
ceiling.

The matching pipeline rates and zero output-loss counters are the strongest
evidence that the pacing path was stable. Earlier July 19 sessions contained
periods with pacing drops, high jitter-drop percentages, or inflated ambiguous
readiness counts. None of those conditions persisted into this completed
post-fix run. That is consistent with proper session initialization and
recovery rather than poisoned state carrying forward.

## What still looks wrong

### 1. `Submit late: 100%` is structurally uninformative

The normal VRR worker path does the following:

1. Wait for `decision.targetUs`.
2. Recheck that the clock has reached the presentation floor.
3. Record another clock sample at the Present call.
4. Obtain the backend's submission timestamp from inside that call.
5. Set `submitLate` when `submissionBoundaryUs > decision.targetUs`.

Because steps 3 and 4 occur after the code has enforced a floor at the target,
the timestamp will normally be at least slightly later than the target. A
strict greater-than comparison at microsecond resolution turns harmless call
overhead into a “late” frame. It reports 100% even though there were zero
failed presentations, zero pacing drops, and zero spacing corrections.

This metric should not currently be used to tune the controller or judge
whether pacing is healing.

#### Recommended fix

Replace the boolean count with a signed submission-error distribution:

```text
submission error = submission boundary - presentation target
```

Report p50/p95/p99 and maximum error. If a rate is still desired, define
“materially late” using an explicit tolerance, for example:

- later than `target + guard`;
- later than `target + learned scheduler tolerance`; or
- late enough to cross a display/source timing boundary.

The tolerance should represent a consequence, not merely the passage of one
microsecond. A histogram or percentiles preserve much more diagnostic value
than a saturated boolean. The existing `submitErrorUs` trace field already
computes the signed error and can provide the basis for this aggregate.

Tests should cover the real worker ordering: wait until the target, perform a
thin Present call slightly afterward, and verify that the result is considered
healthy rather than materially late. The current telemetry unit test verifies
that a supplied boolean is counted, but does not verify that the worker's
classification has useful semantics.

### 2. The word `recent` needs clearer scope

Preparation percentiles use the most recent 128 *late-preparation samples*,
not the most recent 128 eligible frames and not a time-bounded window. During
a very healthy interval, those 128 late samples may span a long period.

#### Recommended fix

Rename the field to make the conditioning explicit, such as:

```text
late-preparation magnitude p50/p95/p99 (last 128 late frames)
```

Alternatively, retain a bounded sample of all eligible frames, storing zero or
a signed preparation margin for on-time frames. That would permit both the
late rate and severity distribution to describe the same recent population.

### 3. The maximum host-processing latency is suspicious but isolated

The session reports a 516.5 ms maximum host-processing latency against a
2.3 ms average. Similar high maxima occurred in prior sessions. With zero
network loss and otherwise low averages, this is more consistent with a rare
host/startup stall or a coarse maximum that persists for the whole session
than a continuous pacing problem.

#### Recommended investigation

Add a small count of host-latency outliers above useful thresholds, such as
16.7, 33, 100, and 250 ms, or record recent p95/p99 values. A lifetime maximum
cannot reveal whether there was one startup event or recurring half-second
stalls. Correlate those events with host logs and visible stalls before
changing client pacing.

### 4. Build provenance is not exact

The exercised binary has a modification time of 20:47, while source commit
`faa166b7` was created at 21:02. The binary clearly contains the new report, so
it was likely built from the reporting changes before those changes were
committed. Nevertheless, the run cannot prove that the exact committed tree
was exercised.

#### Recommended fix

Rebuild from a cleanly identified HEAD before the next validation run and log
the Git commit ID, dirty-worktree state, and binary build timestamp in the
launcher header. This removes ambiguity when comparing behavioral results to
source changes.

## Assessment of controller healing

The completed post-fix report does not show a healing failure:

- every VRR-eligible frame was successfully presented;
- incoming, decoded, and rendered rates match;
- failures, cancellations, pacing drops, and spacing corrections are zero;
- network and jitter loss are zero;
- the controller state sample is fresh;
- preparation-lateness magnitude is bounded well below one 10.18 ms source
  period at p99;
- degraded counters from previous sessions did not carry into this session.

The 6.70% preparation-late rate alone is not evidence of a defect. The
controller deliberately balances preparation reserve against latency, and a
tail of successful late preparations can be expected. It becomes actionable
if lateness magnitudes grow toward or beyond a source period, if actual output
drops increase, if queue/render time trends upward, or if the controller state
oscillates in a time-series trace.

The final log contains only cumulative outcomes and one latest controller
sample. It can show that the session ended healthy, but it cannot fully prove
how quickly the controller recovered from a disturbance. If recovery dynamics
are the next concern, capture a bounded time series around deliberate load or
cadence changes with decision budget, preparation margin, submission error,
queue depth, drops, and rebase/recovery events. That would measure healing
directly instead of inferring it from an end-of-session snapshot.

## Recommended priority

1. Redefine or replace `submit late`; its current 100% result is misleading by
   construction.
2. Rebuild exact HEAD and collect another completed session to confirm the
   healthy result and the new counters across more than one run.
3. Clarify that preparation percentiles are conditional on late frames.
4. Add host-latency percentiles or threshold counts if the 500+ ms maxima
   correlate with visible stalls.

No controller tuning is justified by the current `submit late` value. The
output outcomes are healthy, and the remaining high percentage is primarily a
measurement-definition problem.
