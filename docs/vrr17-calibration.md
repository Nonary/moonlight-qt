# VRR17 follow-up: stable learning and vrr14-style protection

Source: `1ccefb6e` (vrr17.1), plus the buffer-accounting review, the
2026-09-18 follow-up, and the 2026-09-19 ownership/buffer-attribution correction.
This implements the requested direction; it is not a
claim of matched vrr14 gameplay smoothness. The initial comparison and separate
decode/queue/render accounting are documented in [the review](vrr17-review.md).

## Selected policy

| Control | Current production behavior |
| --- | --- |
| High-rate presentation protection | vrr14's per-slot rule, `playout_per_frame_latch=1`: protect slots closer than a display period plus guard; do not add vrr17's 225/400 us entry/exit margins. |
| Low Latency / Balanced Target / Smooth | Maximum allowances of 2 / 2 / 4 fitted source frames, not fixed added delay. Existing absolute ceilings of 16 / 16 / 24 ms and the physical queue-capacity bound still apply. |
| Initial calibration | Require both 500 ms of contiguous interval coverage and 32 consecutive valid intervals. The historical gate required one second and two intervals. Low FPS can still take longer than one second to collect 32 intervals. |
| Ongoing growth | Require below-target long-window quality, current pressure, fresh readiness-attributed error, and serial service plus decoder-queue pressure that each fit the qualified one-second window's intended time; request at most 250 us per 250 ms and apply at most 125 us per frame. |
| Retention and release | 6 / 8 / 10-second holds and 125 / 250 / 50 us per second release. Only current pressure renews the live release hold; long quality history still qualifies future growth. |
| FPS changes and gaps | Do not clear completed initial calibration or earned protection. A broken interval sequence uses the historical one-second requalification gate afterward, not another fast startup. |

At 116 FPS / 120 Hz, a nominal 8,621 us source interval clears vrr14's
8,433 us display-plus-guard threshold. It does not clear vrr17's 8,658 us
entry threshold. Restoring the earlier rule avoids systematically requesting
native synchronized presents for that otherwise eligible cadence; backends
without native protection likewise stop adding the extra 225 us software
spacing margin. Native-rate/tighter slots still receive the existing protection.
Revision 2 remains explicitly available for historical replay.

The preset change does not allocate more queue slots or set a two-/four-frame
startup delay. Capacity remains three waiting frames plus an active worker;
the controller's delay-capacity calculation also reserves preparation and
smoothing room. Smooth's four-frame allowance can therefore be clipped before
four frames. The user-facing settings and diagnostics state the independent
limits. The retained absolute ceilings prevent low-FPS scenes from permitting
arbitrarily large time buffers. Low Latency and Balanced retain distinct quality
targets, history lengths, holds and release speeds despite their equal caps.

Faster calibration is a shorter initial evidence gate, not a faster attack law
or a restarted learner for every rate change. The rolling error estimator still
uses up to one second of recent intervals, and compares actual submission
spacing with intended spacing. Clean source-rate changes alone do not justify
growth. The old hitch-driven fast attack was not restored. Cadence smoothing,
source-offset recovery, verified GPU readiness and native-feedback fixes are
retained.

## Accounting and compatibility

The overlay separately shows calibration collecting, ready, or requalifying,
with consecutive evidence duration/count. It retains the independent buffer
action, growth/clipped step, hold, cooldown and cap breakdown. GPU decode
synchronization remains its own statistic: it is not added to the existing
decoding, frame-queue or rendering headline values.

New trace parameters record the initial duration and sample threshold. Their
schema defaults remain one second/two intervals, so old traces keep their
behavior. Optional `buffer_calibration_complete`, `buffer_calibration_samples`
and `buffer_calibration_coverage_us` columns are audited when present. The cost
report includes these as evidence attached to buffer events, not as additive
latency. No trace schema version, launcher environment or retention changes.

## Validation

The results below are the historical calibration record from before the
2026-09-19 renderer-ownership and serial-service changes. They validate the
2/2/4 allowance and initial-calibration policy on Linux, but they do not
validate the newer D3D11 split fence, Vulkan source-mapping retirement, or the
current serial-service/release rules. Current deterministic and replay results
belong with the change that introduced those behaviors.

The Linux application and diagnostics build successfully. All eight C++ suites,
35 Python tests and `vrrreplay --help` pass. New controller tests cover initial
qualification at 20/30/60/116/240 FPS, unchanged growth limits, and repeated
120 -> 19 -> 120 -> 30 -> 99 -> 116 -> 60 -> 120 FPS transitions for all presets
with smoothing both on and off. Clean transitions do not inflate startup
padding or restart completed calibration. The near-ceiling fixture distinguishes
adaptive production slots from explicit historical revision-2 protection at
116 FPS / 120 Hz; existing native-rate and software-floor tests remain covered.

Immediately before final replay, the only completed supplied local capture was:

```text
/home/chasep/Downloads/VRR-Logs/20260915-154338-342-e938fa9f/Moonlight.vrrtrace
435389 bytes; modified 2026-09-15 12:44:15.888758 UTC
SHA-256 b6d140744e5cd0c278fc02bdd6b77af01bc7eca0118b1ccfa6321398255293ca
```

There are no mounted canonical Windows trace roots or matching launcher replay
sidecar here. The supplied capture nevertheless passes a fresh exact baseline
and complete recorded sequence integrity; comparison with the untouched
pre-change baseline is unchanged. New cold/warm worker fixtures also pass exact
replay. All eleven diagnostic-tampering checks fail the semantic gate as
expected after their footer hashes are repaired.

This is a 60 FPS / 120 Hz Balanced capture, not a high-rate affected-user A/B.
Current-policy replay and isolated variants with one-second initial calibration
or revision-2 headroom have the same headline results: presented jerk over 2 ms
for 7.8% of eligible pairs, jerk p99 6.461 ms, sender-spacing error p99 4.335 ms,
and decode-to-submission p50/p99 11.762/18.190 ms. Mean latency changes by less
than one microsecond versus the prior production policy. It does not exercise
the enlarged allowance or near-ceiling protection and supplies no evidence of
improved visible motion.

The nominal, periodic-decision, preparation, submission and scheduler-burst
stress scenarios all pass their 16 ms reserve ceiling, 30 ms p99 latency bound,
and zero modeled interval-violation assertions. Their p99 latencies are
18.190-18.361 ms. These are synthetic bounds, not normal operating latency or
native-GPU/optical validation.

Final artifacts are under `build/vrr17-calibration-Wu1szT`: the untouched
`before-exact.json` and `before-policy.json`, `final-historical-exact.json`,
`final-current-policy.json`, `final-comparison.json`, `final-stress.json`,
worker fixtures, audit/report results and build/test logs. Diagnostic binaries
were incrementally rebuilt in `build/vrr17-review-uokTLQ/vrr`.

The isolated batch is reproducible from a freshly selected exact-compatible
capture and current-policy summary:

```sh
vrrreplay CAPTURE --require-exact-baseline --output exact.json
vrrreplay CAPTURE --output current-policy.json
python3 scripts/make-vrr-review-config.py current-policy.json \
    --variants tests/vrr/configs/vrr17-calibration-variants.json \
    --output calibration-config.json
vrrreplay CAPTURE --config calibration-config.json --jobs 0 --output comparison.json
```

Only local Linux builds were produced. No Windows build or ChaseShare
deployment was performed. This is a validation/deployment boundary, not a Linux-only
source change: the controller and statistics apply to Windows too. The subsequent
[in-app tracing checkbox](vrr-diagnostics.md) records frames and session logs on
the Desktop without changing timing policies.
A matched high-rate gameplay comparison on an
affected client remains necessary before claiming the smoothness regression
is fixed.

The current serial-service revision 2 and deferred GPU accounting correction
are described in [service-gate correction](vrr-service-gate-correction.md).

## Current Low Latency correction (2026-09-20)

Low Latency now allows one fitted source frame, Balanced Target two, and
Smooth four. The earlier 2/2/4 results above describe the preceding policy.
The absolute 16/16/24 ms ceilings and capacity safety bound still apply.
The settings description and production resolver both use the corrected
1/2/4 allowances; captured explicit policies retain their recorded limits.
