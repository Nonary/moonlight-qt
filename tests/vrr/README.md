# VRR deterministic tests

The VRR test tree is opt-in so regular application and package builds do not
gain test targets. From an out-of-tree build directory, configure it with:

```powershell
& D:\Qt\6.10.1\msvc2022_64\bin\qmake.exe ..\tests\tests.pro CONFIG+=tests
nmake
.\vrr\release\tst_vrrtimingcontroller.exe
.\vrr\release\tst_vrrratepolicy.exe
.\vrr\release\tst_vrrpacingworker.exe
.\vrr\release\tst_vrrreplayconfig.exe
```

On Linux, use the selected Qt `qmake` in the same way and run `make`. The
timing-controller executable covers the platform-neutral source pacing policy
and only links `libavutil`; it deliberately does not
create an SDL window, decoder, renderer, network connection, or Qt event loop.
The policy executable is an app-less QtTest binary and only compiles the pure
FPS policy source.

The timing-controller executable also exercises cumulative RTP cadence
learning for every integer rate from 30 through 116 FPS on a 120 Hz-quantized
capture clock, a continuous one-FPS-per-second sweep in both directions,
isolated hitch rejection, rapid 30 FPS cutscene transitions, high-rate
recovery, and smooth projected targets from mixed 8.33/16.67 ms timestamp
intervals.

`tst_vrrpacingworker` supplies a fake frame presenter and a test-owned
`LiGetMicroseconds()` epoch. It verifies the bounded worker queue,
drop accounting, minimize/restore discard and fresh-frame behavior, deferred
AVFrame lifetime, presenter eligibility rejection, display-period spacing,
native submission timing across pre-submit work and blocking
returns, and the minimal prepare/present/cancel contract. D3D11 completes
queued rendering behind a GPU fence before the worker waits for its target,
then submits immediate frames with `DXGI_PRESENT_ALLOW_TEARING`. This keeps the
timed CPU submission boundary adjacent to a displayable back buffer while the
worker's display-period floor remains the tear-avoidance authority. Linux
presentation mode remains an immutable renderer choice selected when its
swapchain is created. The native backends still need their platform-specific
integration runs.
Set `MOONLIGHT_VRR_TRACE` to a local `.vrrtrace` path to capture a replay-grade
session. For example:

```powershell
$env:MOONLIGHT_VRR_TRACE = "$env:LOCALAPPDATA\Moonlight\capture.vrrtrace"
```

The trace emits one terminal row for every frame delivered to the VRR worker,
including frames rejected during suspension, evicted by queue capacity, or
discarded during shutdown. Raw RTP timestamps, decode completion, pacer
arrival, dequeue and decision times, queue state, controller inputs/outputs,
submission feedback, and an explicit disposition make it possible to replay
the original arrivals without silently omitting pre-schedule drops.

`.vrrtrace` files use independently recoverable 256 KiB CSV chunks compressed
by the background writer. Expand one with:

```powershell
python scripts\decode-vrr-trace.py capture.vrrtrace capture.csv
```

Use a `.csv` path only when directly readable output is more important than
write volume. A trace always preserves at least 60 minutes after capture starts,
including a 480 FPS stream. The 512 MiB physical cap is enforced only after
that hour; an unusually incompressible session may temporarily exceed it rather
than lose replay inputs. On Windows, direct UNC paths are rejected to keep
network I/O out of diagnostics; capture locally and copy the completed file
afterward.

Schema 5 retains the schema-4 tear signals and adds the complete resolved
controller parameter set, controller call duration and learned-model state,
stale-check age, render/target wait boundaries, both spacing-floor checks,
correction-wait boundaries, and terminal time. These inexpensive monotonic
observations are present in ordinary replay-grade traces. Native queries remain
opt-in because they can perturb the renderer.

Schema 4 introduced two deliberately separate tear signals and an explicit
`spacing_guard_feedback_us` value. The latter distinguishes a harmless wait at
the first spacing check from the rare second-boundary violation that actually
changes the controller's adaptive guard. `vrrreplay` remains backward
compatible with schema 3; for those captures it conservatively treats the
combined spacing correction as a wait rather than inventing guard feedback.

The tear signals are:

- `tear_classification` and `tear_risk` classify the submission against the
  physical display-period floor. `confirmed_safe_latched` is synchronized by
  the backend, while `adaptive_interval_violation` is a high-confidence risk.
- `latch_present_refresh_seq` records DXGI `PresentRefreshCount`. The replay
  tool correlates it with the presented-image sequence to count repeated
  refreshes and scanout anomalies.

Neither DXGI nor Vulkan exposes a literal optical "this frame tore" event.
An interval-safe adaptive present is therefore evidence that the client obeyed
the VRR contract, not proof that the driver, display setting, and panel all did.
External high-speed capture remains the authority for validating a suspected
hardware/driver failure.

Set `MOONLIGHT_VRR_DEEP_TRACE=1` for native flip diagnosis. It adds native-call
timing, DXGI present/frame-statistics values, and `gpu_ready_*` timing that
proves queued rendering completed before the target boundary. D3D11 reuses the
previous post-Present observation for the diagnostic before-state, so deep mode
does not add synchronous DXGI queries around `Present()`; the essential
post-Present submission/latch queries are collected in both modes.

Trace formatting remains on the background writer thread. The pacing thread
uses preallocated handoff buffers and never waits for the writer: it drops and
counts a diagnostic row if the handoff is busy or full. Replay detects the
resulting arrival-sequence gap rather than accepting capture-induced timing
skew. Every trace is capped after the guaranteed one-hour window so an
accidentally long session cannot write indefinitely.

## Accelerated replay

The opt-in VRR test build also produces `vrrreplay`. It streams either a
compressed `.vrrtrace` or CSV without loading the session into memory and runs
two timelines in parallel. The observed timeline reconstructs every recorded
disposition, latency, execution cost, native submission, tear classification,
and DXGI refresh anomaly. The simulated timeline feeds the identical
frame/RTP/decode stream through the currently compiled `VrrTimingController`
and reapplies each frame's recorded execution residual without sleeping, so an
hour-long capture runs in seconds.

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace --output baseline.json
```

For a capture produced by the same controller, require byte-time fidelity of
every scheduled target and native submission plus every tear classification:

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace `
    --require-exact-baseline --output baseline.json
```

Use `--timeline replay.csv` when per-frame recorded/simulated values and deltas
are needed. It is optional because a long session creates a large CSV; the
default JSON contains complete distributions and outcome counts with minimal
additional disk I/O.

Replay also derives gap-aware cadence residual and jerk from the elapsed
projected source clock between frames that were actually presented. JSON
summaries split these values into rounded learned-rate bands, including a
combined 40--116 FPS optimization cohort and a retained 60--100 FPS diagnostic
slice. Edge bands cover 40--49, 50--59, 101--109, and 110--116 FPS so failures
near the adaptive range boundaries remain visible. Reports also include one-,
ten-, and sixty-second jerk anomaly windows. The optional timeline includes the
corresponding source, submission, queue, spacing, and DXGI latch context. All
of this work is offline: it adds no capture-thread observations or I/O.
Supplemental rate-band quantiles use a deterministic 32,768-value reservoir,
giving the added rate-band metrics a fixed memory bound even on multi-hour
captures. Counts, mean, standard deviation, minimum, and maximum remain exact,
and the JSON marks quantiles as approximate when the reservoir is active.

Core replay distributions use a fixed timing histogram instead of retaining
one value per frame. Quantiles remain exact at one-microsecond resolution
through 100 ms; the uncommon tail uses conservative 100-microsecond buckets
through one second and one-millisecond buckets through sixty seconds. This
keeps normal replay memory independent of capture duration without reducing
the precision of the latency and cadence ranges used for optimization.

The summary's `capture.telemetry_coverage` object distinguishes unavailable
native diagnostics from valid zero-duration measurements. Deep-trace native
Present and GPU-readiness distributions contain only samples whose matching
validity bit was recorded.

After changing and rebuilding the timing controller, replay the identical
capture and ask for direct lower-is-better deltas:

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace `
    --compare baseline.json --output candidate.json
```

The summary compares modelled tear risks, decode-to-submission and
arrival-to-submission latency, absolute submission error, cadence error,
latched-frame count, original drops, and DXGI scanout anomalies. Display and
negotiated stream rates can be explored without recapturing:

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace --display-hz 120 --stream-fps 116
```

The replay is intentionally a fixed-recorded-admission model: it preserves the
session's actual queue admission/drop and presentation lifecycle while using
the real arrival timestamps and exogenous renderer costs. This makes
timing-policy A/B results deterministic and permits an exact unchanged-policy
baseline. A controller change that also changes decoder backpressure, queue
admission, present-mode cost, or host/network latency still needs a live
validation run because a frame dropped before preparation has no counterfactual
renderer cost in the trace. Schema 5 records the controller parameters and
additional client-side timing needed to attribute controller and worker time;
the trace still cannot provide host capture,
encode, or network latency timestamps that the client never observed.

## Parameterized scenarios

Every behavior-affecting timing-controller number can be changed for replay
without rebuilding. List names or write a complete starting configuration:

```powershell
.\vrr\release\vrrreplay.exe --list-parameters
.\vrr\release\vrrreplay.exe --dump-default-config > replay-config.json
```

For a quick experiment, use one or more overrides. Resolved parameters and a
stable fingerprint are included in the JSON result:

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace `
    --set controller.guard_step_us=100 `
    --set controller.scheduler_learning_samples=32
```

Use `replay-scenarios.example.json` as the batch format. Top-level parameters
are inherited by each named scenario, scenario parameters override them, and
CLI `--set` values have final precedence. Assertions use dotted result paths
and `<`, `<=`, `==`, `>=`, or `>`; a failed assertion exits with code 4.

`mode: fixed` preserves recorded admission and lifecycle for rigorous A/B
controller comparisons. `mode: worker` requires schema 5 and audits recorded
arrival/queue depth against a candidate capacity. It does not synthesize an
alternate renderer lifecycle; DXGI scanout and optical tears are never predicted.
Schemas 3 and 4 remain readable in fixed mode for historical baseline checks.

To include these targets in a top-level developer build, the integration
project should add `tests` to its `SUBDIRS` only inside
`contains(CONFIG, tests)`. That wiring is intentionally outside this test-only
directory.
