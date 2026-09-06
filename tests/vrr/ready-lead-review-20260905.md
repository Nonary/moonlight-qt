# Shared-deadline follow-up: protect late readiness

The user identified the sharp readiness recovery as the end of the second-client
section. GPU observation lag drops from roughly 8.6 ms to 4.3 ms around 31 seconds;
preparation readiness reaches approximately 98% by 34 seconds. Exclude the first
35 seconds, measured from the first presented frame's producer completion.
The retained 24,598 presented frames contain 879 late preparations (3.57%).
The full-run 7.45% rate is not used to score this follow-up.

The complete trace is replayed to preserve controller history. Results are
filtered afterward by the original frame identities, so cutting the opening
does not manufacture a fresh controller startup at 35 seconds. This is the
completed 16:37:10 logged application run, with a validated footer/hash and
exact baseline controller, diagnostic, and submission reproduction.

## Selected adjustment

`playout_ready_lead_us=1000` gives a late-ready frame at least 1 ms before its
target, bounded by learned preparation headroom. It applies to the readiness
floor, including phase-reseed recovery, rather than adding 1 ms to all mapped
source targets. The 3 ms preparation headroom, shared playout buffer, smoothing
settings, and display-spacing/acquisition safeguards are retained.

The schema default is zero, preserving exact replay of the first shared-deadline
trial. The native session policy selects 1,000 us. The startup log identifies
the new build with `shared render deadline with 1 ms late-ready allowance`.

## Replay after the cutoff

These are CPU submission estimates, not measured GPU completion or displayed
smoothness. A submission error is submission time minus its scheduled target.
Output interval change is the absolute difference between consecutive output
intervals. No diagnostic threshold is a universal visibility threshold.

| Readiness allowance | Mean decode-to-submit | p99 submission error | Submissions >0.5 ms late | p99 output interval change |
| --- | ---: | ---: | ---: | ---: |
| 0 ms, first trial | 8.419 ms | 0.992 ms | 626 | 4.451 ms |
| 0.5 ms | 8.430 ms | 0.552 ms | 320 | 4.270 ms |
| **1 ms, selected** | **8.446 ms** | **0.188 ms** | **61** | **4.221 ms** |
| 1.5 ms | 8.471 ms | 0.110 ms | 49 | 4.663 ms |
| 2 ms | 8.516 ms | 0.105 ms | 47 | 5.227 ms |
| 3 ms | 8.750 ms | 0.100 ms | 37 | 6.062 ms |
| Original vrr13 schedule | 11.401 ms | 0.100 ms | 35 | 3.153 ms |

All submission counts use the same 24,598 retained presented frames. For the
24,596 interval-change samples, changes above 0.5 ms decrease from 4,420 to
4,181, and changes above 2 ms decrease from 733 to 686 with the selected guard.
Thus timing regularity improves modestly; the roughly 90% reduction in late
submissions must not be described as a 90% improvement in perceived smoothness.
Larger guards reduce deadline error further but worsen interval regularity.
The original vrr13 policy still has better modeled regularity at higher latency.

The separate all-arrival queue simulation predicts 27,470 presentations and
7 drops for both the first trial and the selected adjustment across the entire
capture. This differs from the recorded 8 drops because the simplified simulator
drains the final frame rather than reproducing the shutdown discard. No candidate
fixed-lifecycle replay reports worker saturation. Counterfactual GPU/compositor
work remains unmeasured; a new live trial is required.

## Validation and artifacts

Controller tests verify that ordinary frames keep their source target, late GPU
readiness has preparation time, and the guard is not an unconditional phase
reserve. The controller, worker, rate-policy, replay-configuration, and both
replay-tool integration tests pass. The first trial's capture reproduces exactly
with the new field set to zero, including reference diagnostics and submissions.

Native build succeeded and linked `build/app/moonlight` at 16:56:13 CDT on
September 5. The new log label and trace field were found in the binary, and the
previous trial's exact log label was absent. No GUI was launched.

Inputs, scenario definitions, per-frame timelines, filtered scoring, and queue
checks are in `build-tests/vrr/ready-lead-20260905/`. Run `run.py` inside
`moonlight-dev` to regenerate the candidate replays, then `score.py` to score
the retained section. The cutoff and preserved input are under
`build-tests/vrr/shared-live-20260905/`. The first trial executable is saved as
`ready-lead-20260905/moonlight-first-shared-trial`; the original vrr13 executable
remains in `architecture-20260905/moonlight-before-shared-deadline`.
