# Buffer quality and replay contracts

The accepted CPU-only implementation is commit `debe7d846a150439cc766c4d37ae2e4818b0b04f`.
The accepted lifecycle correction is `75e97d261a2413e7dbffc82e39a3f92c17cc3356`.
They do not include cadence/catch-up experiments. Native A/B packages qualified
before these commits remain separate immutable artifacts; these diagnostic
changes are not installed or physically validated.

## Capture phases

`playout_delay_us` is the input buffer applied to this decision. Historical
`input_percentile_current_us` and `input_percentile_target_us` are sampled after
feedback. Never substitute them for the decision's applied/target values.

An opt-in `MOONLIGHT_VRR_BUFFER_AUDIT=1` records scalar `buffer_before_*` and
`buffer_after_*` snapshots with their own timestamps and `buffer_audit_revision=1`.
The after snapshot is the state at trace emission: after presentation feedback when present, and before the pending drop notification on stale paths. Replay preserves this phase. Disabled/producer-only rows have revision zero, meaning unavailable. New fields
are additive to schema5; old captures retain historical parameter defaults.
The writer serializes the actual immutable controller parameters selected at
startup instead of resolving another policy on its own thread.

The target is the learned/clamped request, not a promise that the next step is
authorized. `last_slew_authorized_target_us`, `last_slew_before_us`,
`last_slew_delta_us`, and `last_slew_at_us` record the normal actuator's latest
authorization and bounded application. Learning, layer restore, headroom
coordinate changes and bounded fresh reacquisition can change state outside
that normal slew; use their flags, layer identities and before/after deltas.
Layer IDs are stable across caching/restoration and distinct after creation.
ID0 is an unconfirmed initial layer. Switch/create/restore/expiry counts explain
layer events; source FPS changes alone do not establish a layer switch.

Snapshot `flags` bits:1learning,2tail uncertain,4evidence valid,8clipped,
16expired unknown,32growth blocked,64layer transition,128fast drain,
256fresh reacquisition active,512last input eligible.

`eligibility_reasons` is keyed by `feedback_frame`:1not submitted,2cancelled,
4invalid interval,8missing preparation,16invalid input mapping,32source
ineligible,64capacity estimate not qualified,128serial overload,256no causal
hitch,512requirement beyond cap,1024missing slot. These bits describe specific
qualification/authorization facts; bit256 alone does not mean an otherwise
clean input was ineligible. Existing network/decode graph misses, source/RTP,
render/wait/service, native enqueue and display-validity fields remain separate.

No formatting, file I/O, quantile computation, locks or heap allocation are
introduced by a snapshot. Existing bounded8192-row trace handoff and dropped-row
footer accounting remain authoritative. Snapshots add fixed ring memory only
when tracing allocates that queue; the utility reports the exact payload size.
CPU microbenchmarks are scalar snapshot batch costs, not live frame overhead.

## Two different replay questions

Audit mismatch reports name each differing field and retain the first20 frame/value examples.

`vrrreplay --require-exact-baseline` reconstructs the recorded world. Admission,
drop/cancel lifecycle and native/GPU execution remain captured. Its timeline now
exports the same before/after snapshots; new captured audit fields participate
in exact fidelity, including negative signed values. Zero compared audit rows
means legacy evidence lacks them, not that they were checked successfully.

`vrrworkerreplay` uses the actual `VrrPacingWorker` with a deterministic clock,
production queue capacity, admission, stale/trajectory drops, waits, scheduling,
feedback and AVFrame lifetime. All arrivals are fed while service/waits advance,
so candidate-induced queueing and drops are recalculated. It supports the Deck
path only: latch-capable, native synchronization and native scheduling false.
The test-only between-iteration hook and writer throttling compile out of
production; virtual replay does not raise thread priority. Private preferences
and cache are used. No client, network, window or graphics device is opened.

This counterfactual keeps declared per-frame CPU service/completion bounds and
external cancellation inputs fixed. It does not predict GPU/resource contention,
decoder backpressure, host/game behavior, network transport, compositor or
physical scanout. Prepared-ahead renderers and other native-sync paths are
rejected. Unmeasured dropped-frame preparation requires explicit acknowledgement
of400us prepare/20us present assumptions. Captured startup history is decoded
when available. Generated rows have `trace_model_kind=2`; frozen timelines have
kind1. They cannot be mislabeled as original-stream display observations.

## Repeatable comparison

Build the opt-in `workerreplay.pro` and `replay.pro` targets with qmake. Keep
concurrency explicitly bounded, e.g. serial `make -j1` under nice+15 on Deck.
Expand a complete capture with `scripts/decode-vrr-trace.py` before feeding CSV
to the worker utility; the validation runner expands hash-pinned fixtures itself.

```sh
vrrworkerreplay input.csv --lifecycle 0 --trace control.vrrtrace --output control-model.json --allow-missing-costs
vrrworkerreplay input.csv --lifecycle 2 --trace candidate.vrrtrace --output candidate-model.json --allow-missing-costs
python3 tests/vrr/buffer-quality.py control.vrrtrace --candidate candidate.vrrtrace --output comparison.json
python3 -B tests/vrr/test_buffer_quality.py
```

For fresh validation, supply a full controller parameter JSON snapshot (or an
object with a `controller` member). The plan fixes target995000/window120s/T500us
before execution, rather than weakening the user's goal. The runner saves plan,
generator and executable hashes before running any case and retains each
case's failed/unknown gates and latency/jerk tails.

```sh
nice -n15 python3 -B tests/vrr/run-buffer-validation.py \
  --worker-replay /absolute/build/vrrworkerreplay \
  --parameters /absolute/controller-parameters.json \
  --output-dir /absolute/results
```

`fixtures/buffer-bouncing-episodes.json` identifies consumed private recordings
by hash and episode clocks, without checking trace bytes into Git. Optional
`--capture-root` retains the full prefix for state reconstruction and scores the declared episode window. Integrity still covers every arrival; the after-first5s cut uses global session origin, not the episode start. These are
regression fixtures, never fresh holdouts. Fresh predeclared source/rate/gap,
delivery, rendering, decode, expiry and drain workloads remain distinct.

The quality report separates full-session and after-first5s diagnostic cuts;
records min/mean/p50/p95/p99/max with N; uses source-relative interval/jerk as
well as submission jerk; scores causal misses only on eligible same-frame
proof; reports severity, consecutive runs and one-second clusters. B reversals
use raw/1us/T/2/T/2T directional-change thresholds, confirmed cycles and swing
distributions, per-layer segmentation, repeated-bounce episodes, fixed high-B
plateaus, time above target and positive excess-B integral. Missing legacy
decision targets remain unknown. Display events require matching identities,
freshness/readiness and uncertainty bounds; no event means unknown, not success.
Settling/drain is available only when a workload declares its disturbance end;
unknown or censored recovery cannot pass a required recovery gate.


## Verification and gates

```sh
nice -n15 python3 -B tests/vrr/verify-worker-replay.py complete-expanded-input.csv \
  --worker-replay /absolute/build/vrrworkerreplay \
  --replay /absolute/build/vrrreplay --lifecycle 2 \
  --output-dir /absolute/empty-verification-dir --allow-missing-costs
```

This portable verifier runs audit on twice and off once, checks identical decoded
runs and every prior field, requires exact reconstruction, and re-signs a changed
audit value (including a stale-row value when available) so a semantic failure
cannot be mistaken for a checksum failure. Dropped-row traces must be rejected by
both native readers. All commands, exit codes and binary/source hashes are saved.
The output directory must be empty to preserve earlier evidence.

`buffer-quality.py --candidate ... --require-gates` and the validation runner's
`--require-gates` return3 after saving reports if any applicable gate fails or is
unknown. Without that flag they report outcomes without treating policy failure
as an execution error. Gates cover latency and source-relative jerk/error tails,
threshold rates/severity, causal miss runs/clusters, unsubmitted frames, B cycles,
bounce episodes/swings, plateau duration and explicit settling/drain endpoints.
No OS observations in either trace is explicitly inapplicable to the software
comparison and leaves physical evidence unknown. Losing baseline OS observations
fails coverage and leaves candidate latency unknown. Ambiguous submission IDs and
conflicting display events are excluded. A required unknown recovery fails even
when both variants are unknown. Target/window/tolerance come from captured
parameters or the versioned audit, including zero tolerance.

The runner records reporter/generator copies and hashes before execution. Existing
captured workloads are consumed regression inputs. Re-evaluations with stronger
reporting gates must be identified as such rather than described as fresh policy
validation. To study a missing recovery endpoint, predeclare the same extended
quiet tail in both variants, preserve the original prefix, and retain original
censored outcomes. Do not change production policy or relax the endpoint.

`vrrworkerreplay --audit-benchmark --output scalar.json` measures128 batch means
of1024 scalar snapshots. The reported ring snapshot payload excludes alignment,
other trace fields and allocator overhead. Paired `--audit 0/1` whole replay wall
timings include writer formatting, compression, I/O and startup; they are not
per-frame live pacing measurements. Audit-off still has fixed diagnostic state
bookkeeping and additive zero trace columns; neither that default cost relative
to an older binary nor live tracing overhead is established by these microtests.
