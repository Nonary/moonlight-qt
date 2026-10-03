# VRR17 smoothness and buffer accounting review

Reviewed release tags `v6.1.0-vrr14` and `v6.1.0-vrr17` (`704a89f9`),
and checkout `1ccefb6e` (`v6.1.0-vrr17.1`). This records the initial,
observation-only review and its experiments before the subsequent policy edits.
See [calibration follow-up](vrr17-calibration.md) for the selected 2/2/4
allowances, vrr14-style slot protection and initial qualification change.
No affected-user vrr14/vrr17 gameplay comparison was available.

## Where smoothness can differ

| Change since vrr14 | Observable consequence and comparison |
| --- | --- |
| Interval-quality reserve replaced hitch-driven reserve | Revision 7 needs a second of contiguous error coverage, both current and historical quality pressure, and fresh readiness-attributed error. It requests at most 250 us per 250 ms, applied at most 125 us per frame. It can take several seconds to acquire a few milliseconds of protection. In vrr14 a fresh hitch could request protection directly, with 500 us application steps. Test transient bursts, not just steady averages. |
| Long quality history retains reserve | Below-target history renews the hold even after current errors disappear. Holds are 6/8/10 seconds and release is 125/100/50 us per second. One millisecond alone takes 8/10/20 seconds to drain after release becomes eligible. This is retention, not evidence of current GPU work or oscillation. |
| Per-frame latch revision 2 adds 225/400 us entry/exit margins | At 116 FPS/120 Hz with the base 100 us guard, vrr14's 8,433 us threshold fits an 8,621 us source interval; vrr17's 8,658 us entry threshold does not. Windows can request protected presentation much more often. Immediate Vulkan enforces the floor in software, which can reduce sustainable throughput. Check transitions and actual native behavior. |
| Reduce judder now actively retimes frames | Vrr14's production smoothing gain was zero even with the saved setting enabled. Current enabled sessions blend predicted and raw source slots 50/50, with a positive 2 ms retiming cap. This can improve short/long pairs, but is a separate variable from buffering and preserves neither exact RTP intervals nor a guarantee of uniform image motion. |
| Readiness and native mechanics changed | Fence-value validation, explicit Windows decode waits, Vulkan completion polling, persistent Vulkan presentation modes, source-offset recovery, and trace integrity fixes are independent changes. Reverting the controller wholesale would discard useful fixes and alter readiness accounting. |

The D3D11 native-call correction predates the vrr14 release: both compared tags
forward the selected interval through `DxgiPresentParameters::present()`. Do not
mistake the older hardcoded-zero bug described in historical notes for this
vrr14-to-vrr17 regression. Vrr17.1's 4K texture-binding change also postdates the
vrr17 release and must be tested as a separate variable.

The hybrid worth testing keeps current decoder, lifecycle, source-clock,
presentation-evidence and trace fixes while evaluating vrr14-style hitch-driven
protection and the earlier latch margin. Cadence smoothing must be tested
independently. Restoring misleading DXGI refresh references as display events
is not part of this proposal. Nor is increasing every preset's buffer cap.

`tests/vrr/configs/vrr17-review-variants.json` defines cadence smoothing, latch margin,
and hitch-driven reserve, then combines them as `hybrid-candidate`. These are
explicit replay experiments, not an exact vrr14 emulator or a new production
default. Generate the runnable config with `scripts/make-vrr-review-config.py`
from a fresh current-policy replay summary; this supplies the complete policy
snapshot, including its caps and verified-feedback policy. Partial replay
controller objects by themselves start from historical generic defaults and
cannot isolate these changes. The variants file deliberately lacks
`config_schema` so it cannot silently run as a standalone replay config.
Fixed replay cannot model changes in native blocking, decoder backpressure or
live admission; saturated scenarios are unusable for latency conclusions.

Use a newly selected completed affected-user capture, pass exact replay first,
then run this config in one batch. Compare presented jerk first, sender-spacing
error separately, client latency tails, drops, and native mode transitions.
Only a matched live comparison can select a shipping hybrid.

```sh
vrrreplay CAPTURE --require-exact-baseline --output exact-baseline.json
vrrreplay CAPTURE --output current-policy.json
python3 scripts/make-vrr-review-config.py current-policy.json --output review-config.json
vrrreplay CAPTURE --config review-config.json --jobs 0 --output review-results.json
```

## What the diagnostics now explain

Existing **Average decoding time**, **Average frame queue delay**, and
**Average rendering time** keep their definitions. **GPU decode synchronization
wait** is a separate line. It exposes previously unreported asynchronous GPU
waiting; it is not merged into the existing decoding or queue number. No new
aggregate client-latency headline is added to the overlay.

The overlay adds applied reserve versus its effective cap, the next request,
growth/hold/release reason, the last granted and rejected growth steps with their
ages, and the clean-time hold. Queue residence is separated from pacing/other
time. Preparation and Present are separated, with GPU-ready waiting explicitly
shown as part of preparation. The GPU head start is a scheduling budget, not an
extra latency component. GPU-ready details show measured coverage and remain
unavailable when the backend supplies no valid measurement. Protected-submission share and submission jerk expose
timing changes that the old quality percentage hid. That percentage is now
labelled **Client timing quality**, because it is not measured visual smoothness.

| Boundary or mechanism | Cost and interpretation |
| --- | --- |
| Host capture/encode and transport | Existing host processing and RTT remain separate. RTT is not one-way frame transport time; absent host/client clock correlation prevents inventing that cost. |
| Frame assembly, decoder queue, packet submission to decoder output | Per-stage mean/p95/p99 in the latency report. Asynchronous GPU decode waiting follows separately. |
| Decoder handoff and pacer queue | Output-to-admission and admission-to-dequeue residence, separately measured. Queue capacity remains three waiting frames plus active work. |
| Worker GPU decode synchronization | Blocking wait before scheduling. Its separate overlay line preserves comparison with older releases. |
| Controller, render wait, target wait and spacing correction | Non-overlapping measured stage costs. Wake overshoot is a detail inside its wait, not an additional summand. Remaining worker overhead is explicit. |
| Preparation, acquisition, GPU readiness, flush, Present | Preparation and Present partition rendering. Native details can overlap preparation; unavailable details remain unavailable. Present return does not measure subsequent display latency. |
| Adaptive reserve | Applied budget, requested change, attributed frame, interval error, readiness lateness, capped step, cooldown and hold. A catch-up interval charges the preceding late frame. Rejected growth is never retained as future demand. |
| Smoothing, source mapping and recovery | Signed cadence adjustment, source-offset change, readiness clamp and presentation-floor push are separately reported. They can move a target without any reserve increase. |
| Limits and preparation budgets | Effective cap, source-relative preset cap, queue-capacity cap, render lead and GPU head start are independent budgets. Never sum them with measured residence. |

`report-vrr-latency.py` accepts a `.vrrtrace` or decoded CSV and writes JSON
with `--output` and a readable cost table with `--markdown`. All additive client
stage means use the same valid presented-frame denominator and sum to the
matching partitioned decoder-output-to-Present-return mean. That denominator
may be smaller than the headline total when stage timestamps are incomplete.
Percentiles do not add. Missing or
inconsistent timestamps invalidate the partition rather than becoming zeros.
The JSON retains every request change/capped attempt and links its attributed
frame's measured costs when available. Those observations establish the
controller's readiness gate, not a unique hardware or network root cause.

The schema-5 extension is optional for historical files. New fields are appended;
replay audits their values when present. Producer-side terminal rows do not read
worker-owned state, and invalid/stale updates cannot claim a growth cause.
No launcher environment, trace schema version, retention or CLI contract changes.

One existing reporting defect is now visible without changing historical fields:
revision 7 clips demand inside `IntervalBuffer` before the old
`playout_capacity_limited` comparison. That bit can stay false while growth is
being rejected. Use `buffer_clipped_increase_us` and its action/limits to identify
these attempts. Its magnitude is the rejected bounded step, not the entire
unabsorbed disturbance.

## Available evidence

This Linux workspace has no mounted canonical Windows trace roots. The newest
local supplied capture is
`/home/chasep/Downloads/VRR-Logs/20260915-154338-342-e938fa9f/Moonlight.vrrtrace`,
435,389 bytes, last write 2026-09-15 12:44:15.888758 UTC, SHA-256
`b6d140744e5cd0c278fc02bdd6b77af01bc7eca0118b1ccfa6321398255293ca`.
No matching launcher replay sidecar exists in that supplied directory.
Fresh unmodified replay passes with complete sequence integrity and exact
baseline. It is a 60 FPS/120 Hz Balanced capture, not an affected-user vrr17
comparison. It is useful for historical replay compatibility and accounting.

Its mean output-to-Present-return time is 10.679 ms: queue/pacing 4.203 ms,
GPU decode wait 4.266 ms, and rendering 2.209 ms. Reserve is a separate budget
averaging 10.621 ms. Submission jerk exceeds 2 ms for 15.11% of adjacent pairs,
and nine source intervals exceed 25 ms. These figures do not demonstrate that
vrr17 caused the reported regression or that a candidate fixes visible motion.

The isolated current-policy batch on this capture produced the following
exploratory results. These are simulations, not the original observations above
and not an affected-user A/B. All variants retained the complete current policy
except their listed changes; none saturated or violated the modeled interval
floor. Native driver blocking and live admission remain fixed by the recording.

| Scenario | Presented jerk >2 ms | Jerk p99 ms | Sender error p99 ms | Decode-to-submission p50 / p99 ms |
| --- | ---: | ---: | ---: | ---: |
| Current session policy | 7.8% | 6.461 | 4.335 | 11.762 / 18.190 |
| Raw RTP cadence only | 7.8% | 6.461 | 4.335 | 11.730 / 18.190 |
| Earlier latch margin only | 7.8% | 6.461 | 4.335 | 11.762 / 18.190 |
| Hitch-driven reserve only | 1.4% | 2.375 | 1.959 | 18.165 / 25.709 |
| Combined hybrid experiment | 1.4% | 4.542 | 1.013 | 18.173 / 25.520 |

The reserve experiment improves modeled cadence at a roughly 6.4 ms median
latency cost. Removing smoothing improves sender-spacing error but worsens the
jerk tail relative to hitch-driven reserve alone. This is not a free improvement
and does not justify a blanket rollback. The 60 FPS recording cannot test the
near-120 Hz latch-margin concern; a matched high-rate capture is needed.

## Validation and handoff

The Linux application and diagnostic tools build successfully. Eight C++ suites
pass: timing controller, rate policy, pacing worker, replay config, render policy,
D3D11 binding policy, DXGI present contract, and calibration profile. All 34
Python tests pass, including the latency-report and isolated-config contracts.
`vrrreplay --help` also passes.

Fresh exact replay passes for the supplied historical capture and newly
exported cold/warm worker fixtures with the appended diagnostics. All eight
semantic diagnostic-tampering checks fail exact replay as expected after their
footer hashes are repaired. A nominal plus four-fault stress batch passes the
16 ms reserve cap, 30 ms p99 latency bound, and zero modeled interval-violation
assertions; p99 latency is 18.190-18.218 ms on this recording. These are bounded
software tests, not runtime GPU, optical, or end-to-end latency validation.

Local artifacts and logs are under `build/vrr17-review-uokTLQ` (ignored build
output); the review comparison is `final-review-scenarios.json`, the exact
historical gate is `final-exact-baseline.json`, and measured costs are in
`final-observed-costs.json` / `.md`. The Windows application has not been built
or deployed to ChaseShare. This review's diagnostic changes did not change
production timing; the subsequent [calibration follow-up](vrr17-calibration.md)
does. A visual-smoothness conclusion still requires matched captures and a
comparison on an affected client.
