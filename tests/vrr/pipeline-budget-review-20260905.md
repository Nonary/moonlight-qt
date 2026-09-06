# Measured pipeline budget: September 5

The new trial removes automatic source-period scaling and calibrates one
presentation budget from observed availability plus stable preparation cost.
The previous calibration used producer completion alone. After releasing its
startup buffer, it could leave insufficient protection for GPU readiness even
when the game cadence was perfectly regular. A long source period also created
an unnecessarily large startup buffer that released slowly at low FPS.

The rendering lead remains 3 ms, including learned tail protection when needed.
It starts preparation inside the buffered window. The budget includes stable
CPU preparation once; an independent render reserve is not appended to every
presentation target. This is a CPU submission model: asynchronous Vulkan render
completion and visible scanout are not measured by these replays.

## Math and headroom

For a known source schedule `S_i`, client availability `A_i`, and preparation
cost `C_i`, a constant phase `D` must satisfy:

```
target_i = S_i + D
D >= max_i(A_i + C_i - S_i)
```

That maximum is a future-knowledge lower bound for perfectly preserving all
source intervals, assuming feasible display spacing and renderer capacity.
A causal controller estimates the required distribution from past samples.
Its actual target must also respect current readiness and the display floor.

With identical pipeline variation, this requirement does not grow when FPS
drops. Spare display spacing helps service work and recover after delays, but
subtracting it directly from the protective jitter buffer exposes that jitter.
The initial display-period-cap/headroom-credit sweeps did exactly that and were
rejected. An alternating 0/6 ms arrival disturbance requires the same protective
phase at 30 and 116 FPS, despite their very different display headroom.

## Selected policy

`pipeline-q980-cap12000` uses joint p98 plus 300 us, a 10 ms startup and 12 ms
ceiling, a 1 ms minimum, 64 admitted samples before estimation and 128 before
voluntary release. Neither startup nor ceiling scales with the source period.
Release is at least 1 ms per second of fitted source time, with the historical
10 us/frame floor. The source-cadence gain is 10%, with the existing 6 ms
smoothing-lag bound. Late readiness retains its 1 ms preparation allowance.

Per-band calibration remains useful because cadence corrections and arrival
variation can differ between game scenes. The calibration input is observed
GPU readiness plus stable preparation, relative to the smoothed source slot.
When synchronization starts more than the 300 us uncertainty margin after
producer completion, its upper-bound sample may reduce the buffer but cannot
increase it with worker delay. Prompt observations still learn slower readiness.
This protection also lets a conservative startup buffer release when the worker
initially begins observing frames late.

Partial rendering overlap and direct headroom credit remain replay parameters;
the selected policy uses full overlap and zero direct credit. Old captures
default the pipeline/credit/time-release extensions to zero, preserving their
original controller state and schedule.

## Captured replay results

Latency is CPU producer-completion to submission, not network-to-photon delay.
Interval change is the absolute difference between consecutive submission
intervals; none of these thresholds is a validated perceptual score.

| Input / policy | Mean latency | p99 interval change | Changes >0.5 ms |
| --- | ---: | ---: | ---: |
| Clair / original vrr13 | 14.425 ms | 8.616 ms | 1,361 |
| Clair / prior shared trial | 11.388 ms | 8.616 ms | 1,513 |
| Clair / selected | 10.661 ms | 7.254 ms | 1,061 |
| Clean section / original vrr13 | 11.401 ms | 3.153 ms | 3,893 |
| Clean section / prior shared trial | 8.446 ms | 4.221 ms | 4,181 |
| Clean section / selected | 9.582 ms | 3.183 ms | 1,771 |
| Independent holdout / original vrr13 | 13.033 ms | 8.964 ms | 11,686 |
| Independent holdout / prior shared trial | 10.923 ms | 10.134 ms | 11,203 |
| Independent holdout / selected | 13.421 ms | 4.063 ms | 7,475 |

The comparison uses 15,372 / 24,598 / 21,860 presented frame identities and
15,370 / 24,596 / 21,858 interval-change samples respectively. The clean section
excludes the first 35 seconds of the two-client session after replaying the full
history. Both complete captures have exact reference-controller reproduction.
Clair lost three trace rows and is a diagnostic counterfactual, not an exact
reference replay. No selected fixed-lifecycle replay reports worker saturation.

Clair's 40–59 FPS mean latency falls from 14.794 to 11.916 ms, a 2.878 ms saving.
There are still tradeoffs: in that band, changes over 1 ms increase from 41 to
87 of 1,606 interval-change samples; over 2 ms increase from 34 to 49. Across
Clair, the selected policy has 502 changes over 2 ms versus 522 for the prior
shared trial and 456 for vrr13. It is not universally smoother at every threshold.
On the holdout it spends 2.498 ms more than the prior shared trial and 0.388 ms
more than vrr13 to substantially reduce cadence variation. Higher coverage
candidates improved tails further but spent more of the latency gains.

The all-arrival queue model predicts selected/prior-shared drops of 199/201 on
Clair, 7/7 on the full two-client input, and 35/35 on the holdout. Queue input
counts are 15,573 / 27,477 / 21,896. It imputes preparation for frames originally
discarded without preparing and drains shutdown work, so these are modeled
counts rather than exact reproduction of every recorded lifecycle.

## Synthetic validation

52 distinct workloads contain 234,000 generated arrivals: 30/40/60/90/116 FPS,
clean, bounded 1/4 ms, alternating 6 ms and correlated jitter, multiple seeds,
GPU/render/network stalls, rate transitions, and a 140 FPS overload case.
Network arrival, serial decoder work, GPU availability and CPU preparation are
explicit. Full-run scoring includes startup and transitions. Separate scoring
retains all 4,500 arrivals per workload but measures only the final 1,000, so
startup calibration does not inflate the claimed steady-state latency savings.

| Steady workload | Oracle mean latency | Selected mean latency | Selected p99 interval change |
| --- | ---: | ---: | ---: |
| Clean, 30 FPS | 5.400 ms | 5.700 ms | 0.001 ms |
| Clean, 116 FPS | 5.400 ms | 5.701 ms | 0.002 ms |
| 0–4 ms jitter, 30 FPS, seed 7 | 7.448 ms | 7.666 ms | 0.015 ms |
| 0–4 ms jitter, 116 FPS, seed 7 | 7.448 ms | 7.671 ms | 0.007 ms |
| Alternating 6 ms jitter, 30 FPS | 8.400 ms | 8.700 ms | 0.001 ms |
| Alternating 6 ms jitter, 116 FPS | 8.400 ms | 8.701 ms | 0.002 ms |

Across all 35 bounded-jitter/clean workloads after warmup, the selected policy
has zero interval changes above 0.5 ms among 34,965 changes and zero drops.
Worst p99 interval change is 18 us. The oracle uses the actual future maximum;
it is not deployable and does not prove a universal optimum. Under these bounded
inputs the selected controller is about 0.22–0.30 ms above that lower bound.

Large stalls still produce hitches. The stall workload's fixed-phase oracle
needs roughly 26 ms mean decode-to-submit latency to hide every injected event,
far above the selected budget. A 140 FPS source cannot retain every source
interval on a 120 Hz display. These counterexamples rule out an unconditional
"smooth all the time" claim at arbitrarily low latency.

## Reproduction and checks

Artifacts are under `build-tests/vrr/headroom-20260905/`. `final-scenarios.json`
contains full, frozen controller snapshots for vrr13, the prior trial and the
selected policy; it avoids accidentally inheriting future session defaults.
`final-variants.json` is the equivalent input to `headroom-smoothness-study.py`.
The selected captured scores are in `pipeline-ready-clair`,
`pipeline-final-trial`, and `pipeline-final-holdout`. Full and steady synthetic
results are in `final-synthetic` and `final-synthetic-steady`.

Run the synthetic study inside `moonlight-dev` from the repository root:

```sh
python3 tests/vrr/pipeline-synthetic-study.py \
  --config build-tests/vrr/headroom-20260905/final-scenarios.json \
  --output build-tests/vrr/headroom-20260905/recheck-steady \
  --warmup-arrivals 3500
```

Controller, worker, rate-policy, replay-configuration and three CLI integration
checks pass. New deterministic checks cover combined-cost protection across
FPS, delayed-observation feedback, genuine slower readiness, time-normalized
release and reporting warmup without resetting controller state.

The native executable linked successfully at 17:50:23 CDT on September 5,
size 43,003,080 bytes. Its log label is `measured pipeline budget with shared
render deadline`; the preceding label is absent. The previous native binary is
saved as `moonlight-before-pipeline-budget`. No GUI was launched. Visible
smoothness requires a fresh logged session using the new executable.
