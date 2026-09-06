# Rendering architecture simulation — September 5, 2026

The tested redesigns recover approximately **2–3 ms of CPU decode-completion
to presentation-submission latency**, but do **not** establish a smoother or
unconditionally better replacement for vrr13. The simpler shared-deadline
schedule beats the modeled offscreen pipeline on latency. Both make output
intervals less even on these captures. Production sources and the native
application binary were left unchanged.

## What the current renderer actually does

The worker waits for VAAPI decode synchronization, computes a target, waits
until preparation should start, acquires a swapchain image, records rendering,
flushes GPU work, then waits for the target before presentation. GPU rendering
already overlaps the last wait. `prepare_us` measures CPU preparation; its
return does **not** establish completed Vulkan rendering.

The architectural questions were whether preparation could use the existing
playout window rather than extend it, and whether acquiring the swapchain
could be separated from rendering into an intermediate texture.

The earlier September 5 stream has mean producer-decode-to-submission latency
of 13.070 ms. GPU readiness is observed 8.443 ms after producer decode, leaving
4.627 ms between that observation and submission. Mean preparation is 0.704 ms
(acquire 0.087 ms, rendering calls 0.595 ms, flush 0.002 ms, plus other CPU
overhead). Its mean blocking decode-sync call is 5.673 ms.

This corrects the tempting interpretation that the approximately 5 ms smoothing
offset is all idle slack. The producer timestamp precedes GPU readiness; that
offset overlaps part of the readiness interval. The GPU observation is also
not an exact hardware completion timestamp. An earlier observation might be
possible with a different synchronization path, but these traces cannot
quantify that gain.

## Designs tested

All designs retain production jitter-buffer settings, smoothing gain, render
headroom, display spacing protection, and the swapchain acquisition guard.
The prototypes are generated copies of the real controller, outside the app.

| Mode | Architectural change |
| --- | --- |
| 0 | Unmodified vrr13 controller and sequential worker model. |
| 1 | Remove the preparation allowance from the source-based target, but still require the full preparation reserve after observed readiness. |
| 2 | One shared deadline: remove the additive preparation allowance, schedule preparation backward from the buffered/smoothed deadline, and present after actual preparation finishes if it misses that deadline. Learned preparation headroom remains available. |
| 3 | Mode 2, but advance smoothing from the source-derived slot without feeding execution/display-floor delays back into its phase. |
| 4 | Mode 2 with an independent bounded offscreen preparation stage; acquire/copy/present happens in the presentation stage. |

Mode 4 owns two offscreen textures: one held by presentation and one rendering
or ready. The headline comparisons use two producer queue slots, keeping the
same maximum four resident frames as production's three queued plus one active
frame. A three-slot producer queue was also tested separately; its extra frame
capacity must not be mistaken for a rendering improvement.

For mode 4, every frame keeps its entire recorded preparation cost, including
its original acquisition overhead, in the offscreen stage. A separate
0.25/0.5/1 ms acquisition-and-copy cost is charged in the presentation stage.
An additional 0.5/1/2 ms per frame tests unmeasured offscreen/GPU-fence costs.
These costs are **assumptions**, not measurements of an implemented backend.
Retaining the original acquisition cost is conservative in that one respect;
it does not make the whole pipeline model a proven bound.

## Capture and model validation

Included 332,301 delivered frames across three completed streams, approximately
66.5 minutes. All streams used the September 4 native binary linked at 16:16:09
CDT, after the relevant controller and worker source modifications.

| Snapshot | Origin | Frames | Reference replay |
| --- | --- | ---: | --- |
| `1446` | Sep 5 logged run `125644`, stream ending around 14:46; CSV preserved during the initial analysis | 43,040 | Complete footer/hash, exact controller state and submissions |
| `1452` | Same logged application run, later stream ending around 14:52 | 21,896 | Complete footer/hash, exact controller state and submissions |
| `0003` | Sep 4 logged run `215617`, stream ending Sep 5 around 00:03 | 267,365 | Complete footer/hash and exact reference controller state; full simulated submissions are not exact |

The Sep 4 `161716` capture was also examined but excluded: it lacks a clean
footer, has four missing arrival rows, and fails exact controller replay.
Its results remain in the output directory for audit, not in the conclusions.

The original `vrrreplay` validates reference fidelity independently. The queue
simulation uses all arrivals and each frame's own work and GPU observation,
allowing candidate queue admission and drops to change. Its production mode
matches every original `vrrqueuesim` result field exactly on `1446`. It remains
an approximate execution model: for example it drains the last frame instead
of reproducing the recorded shutdown discard, yielding 30 versus 31 drops on
`1446` and 35 versus 36 on `1452`.

Every generated workload result was checked for exact frame accounting,
bounded queue occupancy, and zero submission-before-readiness violations.
Six thousand steady 60 FPS synthetic frames additionally recover the analytic
steady medians: 9.005 ms for modes 0/1, 7.005 ms for modes 2/3, and 7.505 ms for
mode 4 with a 0.5 ms copy. All have zero drops and 1 us p99 interval change.
A separate GPU-stall workload preserves readiness and frame accounting in all
five modes. Missing recorded preparation costs are imputed and reported by
the simulator; they are not measured work for newly retained frames.

## Latency results

Whole-capture mean producer-decode-to-CPU-submission latency, milliseconds.
Savings are differences between candidate and baseline simulations on identical
arrivals, not measured live latency improvements or paired-frame guarantees.

| Capture | vrr13 | Shared deadline (2) | Saving | Offscreen pipeline (4), 0.5 ms copy | Saving |
| --- | ---: | ---: | ---: | ---: | ---: |
| `1446` | 13.062 | 10.630 | 2.432 | 11.061 | 2.001 |
| `1452` | 13.027 | 10.763 | 2.264 | 11.192 | 1.835 |
| `0003` | 11.703 | 8.910 | 2.793 | 9.146 | 2.558 |

Mode 1 saves only 0.24–1.50 ms. Retaining the full reserve after readiness
prevents most of the overlap benefit in the two newest streams. Mode 3 saves
2.33–2.86 ms but has worse cadence tails than mode 2, especially in `0003`;
separating the smoothing phase is not a free improvement either.

## Smoothness and drops

Here “interval change” means the absolute difference between two consecutive
CPU submission intervals. It includes all intervals, including real source
rate changes and stalls. This measures output regularity, not optical
smoothness, tearing, or fidelity to genuine changes in game-frame timing.
It is distinct from the older queue simulator's `presentation_jerk_us`, which
measures changes in error relative to sender timestamps.

| Capture | p99 interval change, vrr13 / shared / offscreen (ms) | Changes >2 ms, vrr13 / shared / offscreen | Drops, vrr13 / shared / offscreen |
| --- | --- | --- | --- |
| `1446` | 5.010 / 5.376 / 5.397 | 42.49% / 43.78% / 44.30% | 30 / 30 / 11 |
| `1452` | 9.076 / 9.987 / 10.096 | 33.35% / 35.07% / 35.91% | 35 / 35 / 13 |
| `0003` | 5.391 / 6.054 / 6.359 | 8.18% / 10.49% / 12.25% | 17 / 17 / 17 |

The pipeline retains more frames in the newest streams even at equal total
frame capacity, but does not improve interval regularity. Its changed frame
selection also makes its smoothness comparison less directly paired than
mode 2. Shared deadlines retain the baseline drop counts but expose more
late preparation relative to their earlier deadlines. Those misses are not
automatically dropped frames or verified visible hitches.

## Cost sensitivity

With the same four-frame total capacity and a 0.5 ms final copy:

| Additional offscreen/fence work per frame | Saving in `1446` | Saving in `1452` | Saving in `0003` |
| --- | ---: | ---: | ---: |
| 0 ms | 2.001 ms | 1.835 ms | 2.558 ms |
| 1 ms | 1.086 ms | 0.890 ms | 2.047 ms |
| 2 ms | 0.127 ms | -0.075 ms | 1.497 ms |

An isolated additional 2 ms preparation cost once per 1,000 arrival positions
was also simulated for production, shared deadlines, and the pipeline. The
shared-deadline mean saving remained approximately 2.26–2.79 ms and its drop
counts remained unchanged. This is one deterministic stress pattern, not a
guarantee about arbitrary driver stalls. The pipeline spike runs used its
three-slot producer queue and are supplemental, not the equal-capacity table.

## Decision

There is evidence for a roughly 2–3 ms scheduling opportunity, with a real
latency-versus-regularity trade. The tested two-texture pipeline does not justify
its extra backend complexity on latency alone: the simpler shared deadline
saves more, and unmeasured GPU/fence work can erase much of the pipeline gain.
No tested design earns the claim “much better overall” while preserving the
existing smoothness.

The next implementation candidate would be a shared-deadline prototype with
proper GPU-completion instrumentation, followed by live validation of CPU
submission, completed rendering, and visible cadence. Larger gains from decode
synchronization require measuring when the GPU actually completes relative to
the blocking `vaSyncSurface()` return. The present capture cannot confirm them.

## Reproduction and outputs

Generator: `tests/vrr/render-architecture-study.py`. It copies the production
controller and replay configuration, applies the documented design changes to
the copies, and adds a bounded preparation stage and diagnostics to a copy of
the queue simulator. No production parameter values are retuned.

```sh
python3 tests/vrr/render-architecture-study.py build-tests/vrr/architecture-20260905/pipeline-src
podman exec --user deck --workdir /home/deck/sources/moonlight-qt/build-tests/vrr/architecture-20260905/pipeline-src moonlight-dev qmake6 study.pro
podman exec --user deck --workdir /home/deck/sources/moonlight-qt/build-tests/vrr/architecture-20260905/pipeline-src moonlight-dev make -j4
podman exec --user deck --workdir /home/deck/sources/moonlight-qt --env VRR_STUDY_MODE=2 moonlight-dev build-tests/vrr/architecture-20260905/pipeline-src/architecture-sim build-tests/vrr/architecture-20260905/1446.csv --output build-tests/vrr/architecture-20260905/shared-example.json
podman exec --user deck --workdir /home/deck/sources/moonlight-qt --env VRR_STUDY_MODE=4 --env VRR_STUDY_COPY_US=500 moonlight-dev build-tests/vrr/architecture-20260905/pipeline-src/architecture-sim build-tests/vrr/architecture-20260905/1446.csv --queue-capacity 2 --output build-tests/vrr/architecture-20260905/pipeline-example.json
```

Snapshot CSVs, original compressed traces where available, reference replay
JSON, generated C++, synthetic inputs, scenario results, and the initial
pipeline task matrix are retained in `build-tests/vrr/architecture-20260905/`.
`m0`–`m3` files contain sequential scheduling designs; `m4-cap2-*` files contain
the equal-capacity offscreen comparisons. All candidate latency results are
CPU submission estimates, not input-to-photon or scanout measurements.

## Follow-up: native trial enabled

At the user's request, mode 2 was subsequently implemented for native testing
as `playout_shared_render_deadline=1`. Preparation headroom remains intact;
the added presentation lead and overlapping render reserve in telemetry are
zero. Missing flags in older traces resolve to zero for exact legacy replay.
The startup log identifies the trial with `shared render deadline`.

The native executable was linked September 5 at 15:19:52 CDT. The controller,
worker, rate-policy, and 26 replay-configuration checks passed, as did both
replay-tool integration tests. The implemented scheduler matches the study's
mode-2 queue-simulation metrics exactly on `1446`; replaying the older policy
also preserves exact reference decisions, diagnostics, and submissions.

The previous executable is retained as
`build-tests/vrr/architecture-20260905/moonlight-before-shared-deadline`.
No GUI was launched during the build. Live smoothness and latency validation
remain pending a fresh stream through the logged development shortcut.
