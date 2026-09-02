# VRR deterministic tests

The VRR test tree is opt-in so regular application and package builds do not
gain test targets. From an out-of-tree build directory, configure it with:

```powershell
& C:\Users\Chase\sources\.tools\Qt\6.11.1\msvc2022_64\bin\qmake.exe `
    ..\tests\tests.pro CONFIG+=tests
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
intervals in the explicit smoothness mode. It also verifies that low-latency
mode counts only p99-minus-p50 render-tail insurance and readiness against a
half-scanout budget, while stable render work remains outside that ceiling.

`tst_vrrpacingworker` supplies a fake frame presenter and a test-owned
`LiGetMicroseconds()` epoch. It verifies the bounded worker queue,
drop accounting, minimize/restore discard and fresh-frame behavior, deferred
AVFrame lifetime, presenter eligibility rejection, display-period spacing,
two-source-period low-latency staleness and the opt-in additional smoothness
period, including regression coverage that ordinary one-period worker
occupancy does not manufacture a content drop,
native submission timing across pre-submit work and blocking
returns, clean-close trace accounting, explicit window-state rebase
provenance, and the minimal prepare/present/cancel contract. D3D11 completes
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
correction-wait boundaries, explicit worker-requested rebase cause, and
terminal time. Header-resolved schema-5 extensions also record the native
queue policy, render baseline, render-tail insurance, pacing-latency budget,
presentation backend/result, signed `GetLastPresentCount()` and
`GetFrameStatistics()` results, the exact DXGI Present sync interval/flags,
the renderer's VRR eligibility snapshot, desktop monitor count, the startup
probe, availability, exact signed HRESULT, and success state for
`DXGIDisableVBlankVirtualization()`, and the unmodified `SyncQPCTime` ticks and QPC
frequency. These inexpensive observations are present in ordinary replay-grade
traces and add no native queries. Older schema-5 headers remain readable, but
cannot pass the strict diagnostic-capture gate when these audit fields are
absent. Current extensions also preserve the immediate spacing recheck and
post-feedback corrected floor, exact Signal/Flush/SetEvent stage brackets, and
deep-mode call brackets for both post-Present DXGI queries. The additional
before-state, native-call, and GPU-ready observations remain opt-in because
they can perturb the renderer; the essential post-Present submission/latch
queries are collected in both modes, while their extra timing brackets remain
deep-only.

New captures use `controller.pacing_latency_budget_divisor=2`. A value of zero
is a replay-only compatibility mode that disables the baseline/tail ceiling;
older schema-5 headers missing this field resolve to zero and keep their
captured absolute render-lead ceiling. Production defaults never select zero.

`MOONLIGHT_VRR_ALIGN=1` adds observation-only Windows raster sampling to a
deep trace. It opens the GDI display's D3DKMT source once per display epoch and
brackets every native DXGI Present with `D3DKMTGetScanLine()`, recording the
exact signed setup/query results, call-time brackets, vertical-blank state,
scan line, and VidPn source ID. It does not poll, sleep, align, or otherwise
change presentation policy. A sample in active scanout proves where the raster
was around the CPU Present call; it does not prove when the queued flip took
effect or that the panel showed an optical tear.

Schema 4 introduced two deliberately separate tear signals and an explicit
`spacing_guard_feedback_us` value. The latter distinguishes a harmless wait at
the first spacing check from the rare second-boundary violation that actually
changes the controller's adaptive guard. `vrrreplay` remains backward
compatible with schema 3; for those captures it conservatively treats the
combined spacing correction as a wait rather than inventing guard feedback.
Current traces additionally retain `spacing_recheck_us` before that second
calculation and `spacing_corrected_floor_us` after guard feedback. Replay
independently reconstructs the original target/guard floor, first-check
deficit, physical-period recheck deficit, final correction flag, learned-guard
floor, and correction-wait boundary. A first-check wait followed by a clean
recheck is valid and does not fabricate guard feedback.

The tear signals are:

- `tear_classification` and `tear_risk` classify the submission against the
  display-period floor. The legacy `confirmed_safe_latched` label means the
  controller requested a non-tearing Present and the backend accepted it; it
  is not an optical observation. `adaptive_interval_violation` is a
  high-confidence client-side risk.
- `latch_present_refresh_seq` records DXGI `PresentRefreshCount`. The replay
  tool correlates it with the presented-image sequence to count repeated
  refreshes and scanout anomalies.

DXGI `SyncQPCTime` is stored in the schema-5 `latch_time_us` compatibility
column, but it is paired with `latch_sync_refresh_seq` (`SyncRefreshCount`).
It is not the timestamp of `latch_present_refresh_seq`
(`PresentRefreshCount`). Replay never uses the post-Present value as an image
latch timestamp. The one exact subset is reported separately: when
`PresentRefreshCount == SyncRefreshCount`, both counters identify the same
refresh and `SyncQPCTime` timestamps it. Replay correlates only those samples
by Present ID and reports recorded and candidate phase relative to that
recorded-world refresh. The submission boundary is the native `Present()` call
start. The trace retains the presenter's raw timestamp, its validity bit, and
whether the worker accepted it. Replay independently re-runs the worker's
operation-interval check and requires every presented frame to use that raw
timestamp. Deep traces then verify that the resulting
`submission_boundary_us` is identical to the separately bracketed native
`Present()` start. The display model then applies the calibrated
`sync_to_active_scanout_us` offset because DXGI does not identify the first
active scan line. Samples are classified as before-active, active,
boundary-uncertain, or after-active using the same tunable display window. The
JSON includes a same-Present-ID cross-table
from the broad pre-Present envelope to this equality-anchored subset, providing
direct calibration evidence for phase/transport sweeps.
Only rows with a valid native `Present()` start are admitted to this exact
subset; replay does not substitute a generic worker timestamp when that
measurement is missing.

Each native call's signed result is retained even on failure, so missing
submission IDs and frame statistics can be attributed to the native query
instead of being confused with a dropped trace field. Replay checks the
success/failure relationships among those results, `presented`,
`submission_id_valid`, and `latch_valid`. It also independently converts the
delta between consecutive raw `SyncQPCTime` samples with the captured QPC
frequency and requires agreement with the stored Moonlight-clock delta within
2 us. The trace also retains the stable process-wide QPC/reference-clock pair
used for the absolute translation plus the QPC bracket span around its
`LiGetMicroseconds()` sample. Replay reconstructs every absolute translated
timestamp from that pair, requires the reference to remain identical for the
session, and reports half the bracket span as a measured minimum clock-alignment
uncertainty. A successful frame-statistics query without raw QPC and
correlation evidence, a raw sample that failed translation, a frequency or
reference change, or an irreproducible translation all fail diagnostic
readiness. Raster readiness additionally requires
`display.phase_uncertainty_us` to cover that measured half-span plus one
microsecond for timestamp quantization. The backend field
is tied to the actual call rather than its worker disposition: Vulkan may
submit an acquired image while cancelling it. Vulkan's boolean libplacebo
result is serialized as 0 for success and -1 for failure; it is never
interpreted as an HRESULT or as DXGI raster evidence.
For DXGI, only `S_OK` is treated as a displayed submission. Positive success
statuses such as `DXGI_STATUS_OCCLUDED` remain in the trace but are classified
as not presented; replay does not equate generic `SUCCEEDED(hr)` with monitor
output. The strict DXGI contract audit also requires sync interval zero, exactly
`DXGI_PRESENT_ALLOW_TEARING` for adaptive Presents, no flag for latched
Presents, a flip-model borderless same-GPU output, an allow-tearing swap chain,
present-ready fencing, Moonlight as the foreground window, and no renderer
fallback on every DXGI Present. The renderer also records the raw LUID of the
adapter that created the D3D render device. Replay requires that identity to
remain stable and equal the matched DisplayConfig source-adapter LUID on every
DXGI attempt, so the same-GPU claim is independently evidenced rather than
trusted from the renderer's adapter-index boolean.
Those renderer booleans are not accepted as self-authenticating capability
claims. The trace also retains the exact `CheckFeatureSupport()` HRESULT and
returned allow-tearing value, the actual created swap chain's `GetDesc1()`
HRESULT, flags, and swap effect, the `GetFullscreenState()` HRESULT and
exclusive-state value, and the raw SDL window flags. Replay independently
rederives tearing support, the allow-tearing descriptor bit, flip-model state,
and borderless-window eligibility. Strict readiness requires `S_OK` for all
three native queries, flag 2048, swap effect 3 or 4, nonexclusive windowed
state, SDL fullscreen-desktop flags, and no capability-relevant snapshot change inside an
  epoch. For D3D11, size/display notifications refresh the renderer state
  synchronously before the pacing worker marks the next frame as a new snapshot
  epoch. Vulkan first marks its swapchain refresh pending; its next prepare
  completes that refresh before a frame can be presented in the new epoch. An explicit
  display/window rebase starts a new snapshot epoch; a change without one
  invalidates the capture.
  Raw SDL window-flag words are still counted, but volatile focus/visibility
  bits may change within an epoch without being misclassified as a swapchain
  capability mutation.
The trace preserves the exact size, display, minimize, restore, and background
notification bits that caused each external rebase. If a size/display change
is consumed after a frame has already been scheduled, that frame is cancelled,
its mid-frame flag word is retained, and the next frame carries the same cause
as its rebase. This prevents a decision made against the prior display epoch
from being presented or silently assigned to the refreshed physical timing.
The worker drains those notifications after the render wait, after
preparation, after the primary target wait, and once more after any final
spacing/correction wait immediately before native submission.

Windows display configuration is captured as rational signal timing rather
than inferred from the rounded `display_refresh_hz` session value. Each DXGI
attempt carries the matched active path flags, source/target adapter and path
IDs, connector technology, rotation, scaling, target availability,
desktop/virtual refresh numerator and denominator, physical signal VSync and
HSync rationals, pixel clock, active and total dimensions, and scan-line
ordering. The complete additional-signal-info word is also retained; replay
decodes its video standard and Miracast VSync divider, and requires the divider
and Windows-reserved bits to be zero for raster claims. The complete path flag
word is retained too; any bit outside Windows' current valid path-flag mask
fails raster readiness instead of being silently assigned old semantics.
Replay requires that snapshot to be complete, stable, and consistent with the
selected display. Every physical invariant is checked on every row in every
explicit display epoch—not just against the final snapshot—including rounded
capture rate, path/signal rational equality, known path flags, progressive
scan, identity rotation/scaling, VSync-divider/reserved bits, and any explicit
scanout-period or active-scanout calibration. A Windows 11 Dynamic Refresh
Rate boost or a desktop/virtual rate different from the physical signal is
reported explicitly and blocks raster readiness because DXGI may virtualize
the vblank clock.
Moonlight already calls `DXGIDisableVBlankVirtualization()` once at startup,
before Qt can create a swapchain. The trace retains whether that probe ran,
whether the entry point existed, and the exact signed HRESULT. Raster readiness
requires that exact startup call to return `S_OK`; matching path and signal
rates are not treated as proof that vblank virtualization was disabled.
Progressive physical timing, identity rotation, identity output scaling, and
agreement between the calibrated scanout period and physical signal are also
mandatory for raster claims. Replay independently checks that pixel rate,
horizontal sync, vertical sync, and total pixel/line geometry agree within
100 ppm; inconsistent driver timing metadata fails readiness instead of
silently selecting one field. Replay derives the nominal active-scanout
duration in picoseconds from the physical refresh rational and full
active/total pixel geometry. The interval runs from the first active pixel
through the final active pixel and excludes the last line's trailing
horizontal blank; using only the vertical line ratio would overstate the
visible tear window. An explicit `active_scanout_ps` must match exactly and an
explicit `active_scanout_us` must agree within one microsecond. Raster
classification uses the picosecond duration so a scanout boundary between
clock microseconds is not rounded onto the wrong side. This prevents a generic
percentage fallback from being mislabeled as calibrated. This evidence still does not reveal the
actual per-Present Independent Flip mode; Microsoft documents PresentMon/ETW
as the authority for that external observation.
Strict raster readiness additionally requires successful before/after
D3DKMT samples around every DXGI Present, correctly ordered around the native
call and bound to the same DisplayConfig source ID. Every successful scan line
must fall within the captured physical signal after normalization. Some
Windows display drivers expose the D3DKMT scan counter at an integer multiple
of the physical signal line count. Replay therefore makes a trace prepass and
infers the smallest capture-wide integer divisor that puts every successful
active/vblank sample inside the captured active/total geometry. It applies one
divisor to every sample, reports the raw and normalized distributions, and
fails readiness if any sample remains out of range. This inference uses only
physical geometry and observed counter range; it has no stream-FPS cases.
The DXGI sync timestamp is a periodic phase reference, so the calibrated
sync-to-first-active offset and active window may wrap through the nominal
period boundary rather than being rejected as a linear interval.
Replay also compares each
active/vblank observation, with the measured query span included in phase
uncertainty, against both the ideal-VRR and free-running hypotheses. For this
observation-only comparison, the D3DKMT vertically-active interval includes
the final active line's trailing horizontal blank; it is intentionally longer
than the visible-pixel tear-exposure interval described above. When D3DKMT
reports active scanout, replay predicts the physical scan-line index from each
hypothesis' picosecond phase and checks the recorded index using a reported
line tolerance derived from phase uncertainty, query duration, timestamp
quantization, and scan-line quantization. The signed residual is
`observed - predicted`, so its direction can drive later
`sync_to_active_scanout_us` calibration sweeps instead of exposing only an
absolute error. At least 100 hypothesis comparisons
must have a definite active/blank prediction, at least 100 active observations
must support a scan-line phase comparison, every such observation must match
at least one hypothesis within its explicit tolerance, and no observation may
contradict both hypotheses. Each before/after raster query uses the newest
integrity-checked `SyncQPCTime` anchor that is causally no later than that
query. This can be the current row's post-Present frame-statistics sample when
it timestamps a vblank that happened before the query; replay does not
artificially hold every sample to the previous row's cached anchor. The
pre-Present cache and post-Present observations are also merged through one
tested monotonicity check; cross-source sequence regressions, timestamp
disagreement for the same refresh, nonadvancing timestamps, or physically
implausible intervals invalidate readiness instead of poisoning anchor
selection.

Neither DXGI nor Vulkan exposes a literal optical "this frame tore" event.
An interval-safe adaptive present is therefore evidence that the client obeyed
the VRR contract, not proof that the driver, display setting, and panel all did.
External high-speed capture remains the authority for validating a suspected
hardware/driver failure.

Set `MOONLIGHT_VRR_DEEP_TRACE=1` for native flip diagnosis. It adds native-call
timing, DXGI present/frame-statistics values, and `gpu_ready_*` timing that
proves queued rendering completed inside preparation and before `Present()`.
The CPU return from the fence wait is only an upper bound on the actual GPU
completion instant. D3D11 records the fence-signal start and a bracketed
`GetCompletedValue()` poll, including the exact target and completed values.
It also records whether synchronization was attempted, the exact signed
HRESULT from `Signal()` and `SetEventOnCompletion()`, and the exact DWORD from
`WaitForSingleObject()`. Signal return, `Flush()`, and
`SetEventOnCompletion()` each have their own start/end bracket. Those native
results and the timestamps reached before a failure are preserved on
preparation-failure and cancellation rows instead of being replaced by a
generic cancelled outcome. Replay checks the native stage chain independently:
Signal must finish before Flush, Flush before SetEvent, and SetEvent before the
poll/wait; failed stages must leave every later stage absent. Successful
GPU-ready timing requires `WAIT_OBJECT_0`.
An incomplete poll provides the conservative lower bound and the successful
event-wait return provides the upper bound; if the poll already reports
completion, signal start and poll end bound it instead. Replay verifies the
derived completion bit, rejects the device-removal sentinel or impossible
fence lag, and independently rederives the bounds and their uncertainty. It
never labels the CPU wake timestamp as an exact GPU timestamp. If preparation
itself is late, this does not claim completion preceded the scheduled target.
D3D11 reuses the previous post-Present observation for the diagnostic
before-state, so deep mode does not add synchronous DXGI queries around
`Present()`; the essential post-Present submission/latch queries are collected
in both modes.
Diagnostic readiness requires exact `S_OK` results for both fence HRESULTs,
`WAIT_OBJECT_0`, valid GPU-ready timing, and a reproducible completion bracket
on every presented frame. It rejects any row where the
signal/poll/wait order falls outside preparation, the recorded lower/upper
bound cannot be derived from the raw observations, or the reported wait
duration disagrees with its start/end timestamps. It likewise verifies that
the worker-level presentation interval is causal and internally exact, and
that native `Present()` start/end lies inside that interval with an exact
reported duration. Deep-mode replay also requires the optional after-Present
raster query, `GetLastPresentCount()`, and `GetFrameStatistics()` to remain in
their exact observed order between native Present return and presenter return;
ordinary traces must leave only those extra timing brackets zero. Replay also
proves the producer relationships among
`spacing_deficit_us`, guard feedback, the correction-wait interval, and
`spacing_corrected`, including the exact pre-feedback recheck and
post-feedback floor, and bounds both arrival-time and completion-time queue
depth against the worker's actual three-frame capacity. These fields are
exported in the per-frame timeline instead of being accepted as inert
diagnostic decoration.
DXGI `SyncQPCTime` is converted through one stable QPC-to-Moonlight-clock
correlation per process, preventing repeated observations of the same refresh
from acquiring fresh cross-clock sampling jitter. The raw ticks and frequency
are captured independently of whether that correlation succeeds, so the
translator itself remains diagnosable.
The raster-phase envelope requires the deep trace's pre-Present sync sample;
ordinary traces retain the interval and post-Present refresh metrics but report
the phase envelope as unclassified rather than guessing.

Trace formatting remains on the background writer thread. The pacing thread
uses preallocated handoff buffers and never waits for the writer: it drops and
counts a diagnostic row if the handoff is busy or full. Replay detects the
resulting arrival-sequence gap rather than accepting capture-induced timing
skew. Terminal rows can legitimately be written out of arrival order when a
new arrival evicts an older queued frame, so replay sorts only the compact
sequence-ID list for its exact duplicate/missing-ID audit; harmless row-order
transitions are reported separately and do not fail readiness. On normal
shutdown, both compressed traces and direct CSV end with a
`#vrr_trace_footer` metadata line. It records the final allocated arrival ID,
rows accepted by the writer queue, rows dropped to protect pacing, cap state,
write-failure state, and a SHA-256 over the normalized decoded header and all
data rows. Replay independently recomputes that digest and requires both the
hash and exact footer accounting for diagnostic readiness, closing silent
content corruption and the otherwise-undetectable lost-tail case without
changing trace schema 5 or the meaning of a data row. Older version-1 or
footerless captures remain readable but cannot claim current diagnostic
readiness. Every trace is capped after the guaranteed one-hour window so an
accidentally long session cannot write indefinitely.

## Accelerated replay

The opt-in VRR test build also produces `vrrreplay`. It streams either a
compressed `.vrrtrace` or CSV without loading the session into memory and runs
two timelines in parallel. The observed timeline reconstructs every recorded
disposition, latency, execution cost, native submission, tear classification,
and DXGI refresh anomaly. It also evaluates each adaptive Present against the
deep trace's pre-Present `SyncQPCTime` anchor under two raster extremes:
ideal VRR flip-following and free-running fixed refresh. The result is a
lower/upper active-scanout exposure envelope across those two named
hypotheses, not a physical bound over every possible driver/panel behavior and
not a fabricated tear count.
Both hypotheses apply an explicit offset from the DXGI sync-clock marker to the
modeled first active scan line. The default offset of zero is only a sweep
starting point and is not treated as calibrated evidence.
For an all-adaptive fixed-lifecycle candidate, replay also propagates a
counterfactual free-running refresh clock between candidate Present
transitions. One captured sync anchor seeds each segment; later captured VRR
refreshes never re-anchor that counterfactual clock. It reports lower, central,
and upper counts for multiple candidate Presents in one refresh and refreshes
that repeat the prior image. Each interval's lower bound uses the
latest-possible prior transition and earliest-possible current transition;
the upper bound uses the opposite endpoints, applying
`display.phase_uncertainty_us` to both transition windows. The total
lower/upper values conservatively sum per-transition extrema and are not
claimed to be jointly attainable endpoints. A latched Present terminates the segment
because its counterfactual display-transition time was not measured. This
timeline is marked available only with calibrated raster readiness, matching
display rate, fixed recorded lifecycle, at least 99-percent phase coverage,
no latched candidate frames, a precise picosecond period, and a continuous
modeled clock. The period comes from an explicit
`display.scanout_period_ps` override when supplied; otherwise replay derives it
from the captured physical-signal refresh rational. Replay likewise derives
the exact active duration from that period and the captured active/total pixel
geometry unless `display.active_scanout_ps` is supplied. The same precise
period and active duration are used for raster-envelope phase classification.
Picosecond propagation uses an epoch-relative transition clock, so long system
uptime cannot overflow the conversion, and keeps one-hour integer-period
rounding drift below the trace clock's microsecond precision floor at common
refresh rates. Driver
queuing, low-framerate compensation, and panel behavior remain outside the
hypothesis.
Replay retains a bounded history of integrity-checked sync anchors, so a
candidate shifted earlier than the anchor attached to its recorded frame can
fall back to the newest captured anchor that actually precedes its simulated
submission instead of being compared to a future timestamp.
For active or boundary-uncertain samples it also reports a normalized active
scanout position in parts per million (`0` at the raster start, `1000000` at
the raster end), so phase sweeps can distinguish risks concentrated at
different parts of the scan without assuming a display resolution or physical
scan direction.
The simulated timeline feeds the identical
frame/RTP/decode stream through the currently compiled `VrrTimingController`
and reapplies each frame's recorded execution residual without sleeping, so an
hour-long capture runs in seconds.
That recorded-world default intentionally retains the alignment diagnostic's
pre-Present D3DKMT query cost. To approximate the same candidate without that
observer call, set
`execution.remove_pre_present_raster_probe_overhead=1`. Replay then
subtracts each successful query's exact `query_end_us - query_start_us`
duration from that frame's execution residual. The subtraction is clamped to
the reconstructed target/spacing/readiness floor, so it cannot create an early
Present fault. It does not guess that time between query return and Present
was diagnostic overhead; that interval can contain unrelated scheduler delay.
The JSON and per-frame timeline report requested, available, missing, measured,
removed, and floor-clamped evidence. Any requested presented row without a
successful, causally bracketed probe makes the transformation incomplete and
fails raster readiness instead of silently mixing normalized and
recorded-world frames. Fixed mode still preserves recorded admission and frame
lifecycle: it does not remove the after-Present raster probe or guess how a
shorter presenter return would have changed later queue/dequeue decisions.
The capture and summary retain that after-query duration separately so a
future lifecycle simulator can model it without needing another trace schema.

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace --output baseline.json
```

For a capture produced by the same controller, require a complete arrival
sequence plus exact reproduction of every captured controller decision field,
every post-feedback controller diagnostic state field, native submission, tear
classification, and raster-envelope classification:

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
corresponding source, submission, queue, spacing, DXGI latch context, and
per-frame free-running refresh-delta/anomaly bounds. All of this work is
offline: it adds no capture-thread observations or I/O.
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
validity bit was recorded. GPU completion is reported as lower/upper bounds
and an uncertainty distribution, separate from the exact CPU fence-wait
duration. The same object reports exact fence/wait result counts, partial
attempts, stage-relationship failures, and presented-frame exact-success
coverage. `diagnostic_readiness` applies explicit gates rather
than treating a successful process exit as proof: schema/sequence integrity,
exact reference-controller replay, deep-trace coverage, native Present boundary
identity, causal and exact worker/native Present intervals, GPU-ready timing,
native/query result coverage, independently reproducible raw-QPC translation,
pre-Present sync-anchor coverage and counter/timestamp integrity,
at least 99-percent fresh post-Present latch samples associated with a captured
Present ID, and equality-anchored refresh-correlation coverage. Replay also
rejects noncanonical or malformed CSV values and independently recomputes
disposition/decision consistency, drop and tear flags, prior-submission state,
submission spacing/margin, and submit error instead of trusting redundant
columns. Raster
readiness also requires at least 100 same-Present-ID equality validations,
complete validation accounting, zero cases where exact active scanout
or exact post-active scanout contradicts the broad envelope, and at least
99-percent observed and candidate anchor/classification coverage.
An observed or candidate world with no adaptive submissions treats its own
anchor/classification subset as not applicable; this permits a fully latched
recording to validate a deliberate `--no-latch` tear-exposure simulation
without weakening coverage for that adaptive candidate.
Repeated samples of the same DXGI refresh use
a fixed 50-us clock-translation tolerance; impossible shorter-than-maximum-rate
refresh intervals use a separate fixed 500-us tolerance. Neither integrity
gate can be weakened by changing the tunable display phase uncertainty.
The same sequence, timestamp, causality, jitter, and minimum-interval checks
also run directly on post-Present sync samples, including the final sample that
has no later pre-Present row to validate it.
DXGI documents swap-chain frame statistics as unreliable in many
multiple-monitor scenarios and potentially stale with Hardware Flip Queue.
Strict diagnostic readiness therefore requires one desktop monitor and at
least 99-percent fresh Present-count observations. It also requires Moonlight
to be the foreground window on every DXGI Present, reducing the documented
other-fullscreen-application ambiguity without claiming that a trace can
enumerate every background fullscreen process. Capture deliberately does not
call `DwmFlush()` to force updates because that would add synchronization to
the presentation path being measured.
Deep traces additionally require at least 99-percent exact carry-forward from
each post-Present count/frame-statistics observation to the next native
pre-Present snapshot. The first row of a session and first row after an
external rebase start a new comparison epoch because the trace cannot prove
whether a coalesced window event actually reached the backend as a suspend.
Controller replay, diagnostic capture, and raster simulation have separate
readiness booleans. All require a clean-close footer with zero dropped rows, no
cap or write failure, and exact agreement among allocated IDs, enqueued rows,
and decoded rows. `diagnostic_capture_ready` is deliberately independent of
panel calibration: it additionally requires deep telemetry for every row,
complete native Present/GPU-ready timing for every presented frame, exact
native result coverage for every native attempt, exact successful
Signal/SetEvent/wait results and all GPU stage brackets for every presented
frame, proof that every normal presentation attempt used DXGI, both query
result codes and their deep-mode call brackets for every accepted
Present, the exact DXGI Present flags and renderer capability state, a
complete raw actual-swap-chain capability proof, a stable render-adapter
identity matching the display source, a single-monitor
topology, a complete and stable matched Windows display-path and
physical-signal timing snapshot, reproducible raw-QPC translation, exact
before/after state carry-forward, exact coarse-timer/active-wait lifecycles,
continuous counters, integrity-checked
pre/post sync anchors with at least 99-percent raw pre-Present coverage, and at
least 100 same-Present-ID equality correlations. It answers whether the
capture contains the evidence needed to calibrate and simulate later, not
whether a chosen panel model is already calibrated.
Reference replay reconstructs the production callback lifecycle by disposition:
pre-render stale rows consume the pending decision, post-render stale rows
rebase it, and only the normal presentation path feeds scheduler/spacing
samples. `external_rebase_applied` distinguishes a worker/window-state rebase
from a reset that the controller inferred internally; older schema-5 headers
without that extensible field remain readable but fail controller readiness.
Schema-5 readiness also verifies exact controller-call and stale-age durations,
ordered render/target/floor/correction waits, preparation and GPU-fence
containment, worker/native presentation containment, and a terminal timestamp
after every recorded lifecycle event. The actual Present boundary must be at
or after the recorded absolute presentation floor. The two spacing checks and
any guard-derived corrected floor must reproduce exactly from prior submission,
display period, controller state, and captured clock readings. Cancellation
paths that may submit now
record their floor-check entry, final time, and enforced floor rather than
leaving a one-sided wait interval. Raster readiness also rejects a reset or
regression inside each submission/presentation counter epoch instead of
silently joining observations from two display timelines. An explicit external
rebase starts a new epoch, discards its ambiguous cached before-state anchor,
and begins again with post-Present evidence.
The readiness gate compares all numeric and boolean decision fields
and all schema-5 learned-state values and counters after every `decision_valid` worker
row. Producer-side queue/suspension terminal rows
carry zero controller diagnostics so tracing never reads worker-owned learned
state concurrently.

Automation can make each readiness result mandatory. These options still write
the JSON before returning failure, so the exact failed gate is retained:

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace `
    --require-diagnostic-capture-ready `
    --output readiness.json
```

Capture launchers should use `--require-diagnostic-capture-ready` after the
updated replay binary is deployed; an ordinary zero exit is not a capture
trust check. Controller-readiness failure exits with code 6;
raster-readiness failure exits with code 7; diagnostic-capture-readiness
failure exits with code 8. `--require-raster-ready` is the stricter later-stage
check and also requires an explicitly calibrated display model, a progressive
physical signal, identity rotation and output scaling, no DRR/vblank
virtualization, internal agreement among the physical signal's pixel/sync
rates and total geometry, and agreement between the configured scanout period
and active scanout duration and the captured physical timing. It also requires the
alignment diagnostic's observation-only D3DKMT samples around every DXGI
Present to range-check and validate the model envelope.
`--require-counterfactual-refresh-ready` exits with code 9 unless that raster
model can additionally produce a continuous all-adaptive fixed/free-running
refresh timeline.

After changing and rebuilding the timing controller, replay the identical
capture and ask for direct lower-is-better deltas:

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace `
    --compare baseline.json --output candidate.json
```

The summary compares modelled interval risks, raster-exposure lower and upper
bounds, raster and equality-anchored unclassified counts,
equality-anchored active/possible exposure, decode-to-submission and
arrival-to-submission latency, absolute submission error, cadence error,
rate-band residual/jerk, and paired submission drift. Comparison is accepted
only when the decoded trace SHA-256, replay-model version, display model,
display/stream rates, and replay mode match; otherwise the result is
`not_comparable` and the process exits with code 5 instead of manufacturing a
verdict from different physical assumptions. Display and negotiated stream
rates can still be explored without recapturing, but those runs are separate
scenarios rather than direct A/B comparisons. A changed display rate also
fails raster readiness because the capture has no counterfactual refresh
timeline for that display; controller/cadence results remain available:

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace --display-hz 120 --stream-fps 116
```

There is one VRR queue policy. It is a jitter buffer anchored to the sender
timestamps (`controller.timestamp_playout_enabled`): every frame targets its
RTP time mapped into the local clock plus a playout delay plus the render
lead. The delay is self-calibrated per source-rate band
(`controller.playout_delay_adaptive`): each band, the fitted source rate
divided by `playout_band_width_hz`, keeps a reservoir of frame lateness
against the mapped sender clock, excluding pairs that span a host stall, and
targets the `playout_delay_percentile_per_mille` lateness (p99.9) plus
`playout_delay_margin_us`. A band starts at `playout_delay_start_us` (6 ms),
rises at most `playout_delay_attack_us` per frame, releases at most
`playout_delay_release_us` per frame and only after
`playout_delay_release_samples`, and is clamped between 1 and 8 ms. The former
"smoothness" option, one extra source interval of queue age and one extra
source period of render-lead budget, was retired: on a render-bound client it
deepened the standing backlog and saturated the worker without reducing
hitches. Captures made under it still replay with that budget through
`additional_queued_frame` and `pacing_latency_queue_mode_extra`, which the
production policy sets to zero. A band unused for `playout_band_stale_us` re-converges from the
start value; a cadence change therefore starts high at the discontinuity
where nobody can see the step. The lateness statistic does not depend on the
delay chosen, so there is no feedback loop. With `playout_delay_adaptive`
off, `controller.source_playout_delay_us` is the fixed delay. The applied
delay is recorded per frame as `playout_delay_us`. The mapping offset is the windowed minimum of
decode-complete minus RTP time (`playout_offset_window_us`), slewed at most
`playout_offset_slew_us` per frame so clock drift is followed without moving
one frame relative to its neighbours. No learned readiness reserve is applied
or reported, and a late or early frame never re-anchors the clock: a frame
later than the delay clamps to "now" and the next frame returns to its own
slot. All resolved values are captured in schema 5, so exact replay and
candidate sweeps use the production policy without inferring it from the
queue-mode flag. Note that a `--set` or config scenario is treated as
customized and does not inherit the session policy: a smoothness sweep must
set `controller.timestamp_playout_enabled=1` explicitly alongside the delay.
The adaptive readiness parameters remain available for replaying older
captures and for explicit experiments.

The replay is intentionally a fixed-recorded-admission model: it preserves the
session's actual queue admission/drop and presentation lifecycle while using
the real arrival timestamps and exogenous renderer costs. One lifecycle instant
is re-derived rather than copied: the candidate's decision time follows the
worker-occupancy model (`decision_time_model`), which places it at the
simulated previous submission plus the recorded post-submission gap whenever
the recorded worker was still busy when the frame arrived, and at the recorded
instant otherwise. Copying the recorded instant verbatim clamped every
candidate that presented earlier than the recording to a stale "now" and
manufactured spacing errors that the live worker would not produce. The
unchanged reference policy still reproduces the recorded instant exactly. This makes
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
    --set controller.scheduler_learning_samples=32 `
    --set display.active_scanout_percent=95 `
    --set display.scanout_period_ps=8333333333 `
    --set display.phase_uncertainty_us=250
```

The `display.*` parameters calibrate the raster envelope without changing trace
schema: scanout period (a microsecond compatibility value plus an optional
picosecond override used by both raster classification and the propagated
counterfactual clock),
active-scan percentage or explicit duration, the
DXGI-sync-to-first-active-line offset, CPU Present-to-display transport,
boundary uncertainty, and maximum sync-anchor age. Defaults use the captured
display period, a 95-percent active window, zero sync offset, zero transport,
and a 250-us uncertainty band. Those defaults are sweep starting points, not
measured display timing, so raster readiness remains false.
After supplying an explicit scanout period and active duration (using either
the microsecond compatibility values or the exact picosecond values) that
match the replay-reported physical-signal values, set
`display.calibration_confirmed=1` only when the chosen
`sync_to_active_scanout_us`, transport, and uncertainty values are also
independently justified. The uncertainty must be at least the replay-reported
QPC-correlation half-span plus one microsecond of timestamp quantization, and
normally must be larger to include transport and panel uncertainty. Supply the
separately calibrated `scanout_period_ps` only when intentionally overriding
the exact period derived from the captured physical-signal rational; otherwise
replay uses that rational for both models. Raster readiness rejects an override
that disagrees with that signal by more than one picosecond. The
configuration can structurally require an explicit period and active scanout
duration, but
the flag is the operator's assertion that the complete model is calibrated;
it cannot turn an assumption into a measurement.

The latch protection window defaults to a narrow absolute-headroom policy:
enter below 225 us and exit at 400 us. Headroom already excludes one physical
display period and the controller's learned spacing guard. At 120 Hz, 115 FPS
starts adaptive with roughly 263 us of headroom while the proven near-ceiling
116 FPS path remains latched at roughly 188 us. The wider exit threshold keeps
small cadence or guard fluctuations from bouncing a borderline stream between
the two modes.

At 120 Hz, the former three-period default latched the useful 30--116 FPS
range. With the narrow absolute default, 30 through 115 FPS initially take the
adaptive path; only the near-ceiling 116 FPS recommendation remains latched.
The same correction applies across display refresh rates rather than being a
special case for any one stream rate.

Production requires the full exit threshold after entering latch. The
`controller.latch_base_guard_exit` compatibility parameter restores the older
behavior of exiting at the entry threshold as soon as the learned guard reaches
its base value. It defaults to zero in production and is supplied as one when
replaying an older schema-5 trace that lacks the field. Schema-3/4 replay also
restores the historical 1.5 ms entry, 2 ms exit, zero period ratios, and
base-guard exit behavior explicitly because those traces contain no resolved
controller parameters.

The `controller.latch_enter_headroom_period_*` numerator/denominator pairs and
the corresponding `latch_exit_*` pair remain available for replay experiments
and exact reproduction of captures made with display-scaled protection. Their
production numerators are zero, so the absolute `latch_*_headroom_us` values
set the live boundary. Older schema-5 traces that predate the period columns
replay with their original absolute-only semantics, while traces containing
non-zero period ratios retain those captured ratios.

The fixed microsecond thresholds are intentionally a bounded first correction.
A future policy should derive them from measured scheduler wake error,
present-ready uncertainty, and native presentation/scanout evidence so latch
selection reflects whether the current system can reliably use its available
headroom.

Adaptive presentation also requires a coherent source phase. After startup,
a cadence-rate change, or a source-phase discontinuity, production uses the
latched path until `controller.cadence_stability_latch_frames` consecutive
clean cadence observations have completed (64 by default). This prevents a
source hitch followed by a decoder burst from placing multiple immediate
presents into one physical scanout interval. Stable sources with sufficient
headroom return to adaptive presentation after the recovery window. Older
schema-5 captures that lack this parameter replay it as zero so their original
presentation decisions remain exact.

`display.present_transport_us` is also the deterministic phase-sweep control.
Sweep it from zero through one scanout period to test whether a candidate
remains robust as the unknown driver/display transport phase moves. Judge the
`exposure_lower_bound` and `exposure_upper_bound` together; minimizing only the
lower bound can merely move samples into the uncertainty band. Leave
`calibration_confirmed=0` during an assumption sweep. The upper bound
conservatively counts unclassified adaptive submissions as possible exposure;
`classified_exposure_upper_bound` is reported separately for inspecting only
rows whose phase could be resolved.

Most zero-default `execution.*` parameters provide deterministic anomaly injection:
decision delay, render-wake delay, renderer preparation work, target-wake
delay, native-submission delay, display-transition delay, synthetic
spacing-guard feedback, and a periodic decision stall. Render/target wake
delays are scheduler faults, so replay first reconstructs the candidate wait
entry and applies them only when `VrrTargetWaiter` would actually take its
coarse-sleep path. A request on an already elapsed deadline or inside the
bounded active-wait region is reported as suppressed: it neither moves the
candidate execution boundary nor contaminates learned scheduler delay.
  For an eligible fault, replay shifts the captured coarse timer return rather
  than blindly shifting the frame. The recorded active-wait margin absorbs any
  part that still returns before the captured waiter completion; only the
  unabsorbed remainder moves preparation or presentation and scheduler feedback
  is rederived from the shifted wake. The summary and timeline also say whether
  each candidate wait stayed on the captured elapsed/active/coarse path. Replay
  translates the captured final-return residual only for a matching path; a
  path-changing candidate is reported explicitly and uses an ideal deadline
  return because the capture contains no OS scheduler residual for that
  counterfactual path. Each wait records its internal first clock
sample, exact active budget, coarse sleep count and requested duration,
requested wake and actual return, active-yield start/limit/count, clock-stall
and legacy yield-cap exits, and final time. Current waiters are bounded by the
monotonic active-time limit and the stagnant-clock guard; they do not return
early at a machine-dependent yield count. Strict capture readiness requires that
whole lifecycle on every executed render and target wait. Older traces remain
readable, but their wake injection is explicitly approximate and they fail
that readiness gate. Preparation/submission delays model work without
relabeling it as scheduler lateness. `spacing_guard_feedback_us` forces the candidate's
second-check feedback to at least that value while leaving the independently
replayed reference row untouched. The same periodic cadence has per-stage
variants. `periodic_stall_phase_frames` selects the modulo-period phase
(`0` retains the historical every-Nth-frame boundary, while `1` selects the
first scheduled frame), and `periodic_stall_burst_frames` selects consecutive
frames from that phase. Defaults preserve the original one-frame periodic
behavior. For
example, the following adds a 2-ms decision stall every 60 scheduled frames:

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace `
    --set execution.periodic_stall_every_frames=60 `
    --set execution.periodic_stall_us=2000
```

Observer normalization is applied before deliberate
`submission_advance_us` fault injection. This keeps measured tracing overhead
separate from a requested floor-bypassing fault and preserves independent
per-frame accounting for both:

```powershell
.\vrr\release\vrrreplay.exe alignment-capture.vrrtrace `
    --set execution.remove_pre_present_raster_probe_overhead=1 `
    --output no-alignment-observer.json
```

This four-frame burst starts 30 frames into each 120-frame period and combines
renderer wake lateness, target wake lateness, and synthetic guard feedback:

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace `
    --set execution.periodic_stall_every_frames=120 `
    --set execution.periodic_stall_phase_frames=30 `
    --set execution.periodic_stall_burst_frames=4 `
    --set execution.periodic_render_wake_delay_us=1500 `
    --set execution.periodic_target_wake_delay_us=1000 `
    --set execution.periodic_spacing_guard_feedback_us=250
```

`submission_advance_us` and `periodic_submission_advance_us` are deliberate
early-Present faults. They bypass the candidate target and prior-submission
spacing floors while remaining clamped to simulated buffer readiness. This is
the deterministic way to create interval violations and active-scanout
exposures for detector validation:

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace `
    --no-latch `
    --set execution.periodic_stall_every_frames=60 `
    --set execution.periodic_submission_advance_us=6000
```

The summary reports requested versus actually applied advance and how many
frames were clamped by buffer readiness, then cross-tabulates those injected
frames against interval violations and every raster-envelope outcome. Injected
frames that also receive an equality-qualified DXGI refresh anchor get a
separate exact-refresh outcome table. It also reports requested, applied, and
suppressed render/target wake lateness, the portion absorbed by active margin,
the portion that became execution delay, and synthetic spacing feedback, so a
sweep cannot silently invent a scheduler wake on a frame that never slept or
confuse scheduler learning with renderer/native-call cost.

`display_transition_delay_us` and
`periodic_display_transition_delay_us` add delay after the candidate CPU
submission boundary. They do not alter submission spacing, decode-to-submit
latency, or controller learning. Instead, they move the modeled scanout
transition, use the freshest integrity-checked sync anchor that is causal for
that delayed transition, and feed the raster envelope and propagated
free-running-refresh anomaly model. This makes it possible to simulate a
driver/display-side delay while keeping a clean CPU timeline:

```powershell
.\vrr\release\vrrreplay.exe capture.vrrtrace `
    --no-latch `
    --set execution.periodic_stall_every_frames=60 `
    --set execution.periodic_display_transition_delay_us=6000
```

The summary reports every delayed frame's raster outcome and the exact-refresh
classification subset. A delayed synchronized/latched Present remains
explicitly suppressed because the trace does not reveal its physical display
transition.

Use `replay-scenarios.example.json` as the batch format. Top-level parameters
are inherited by each named scenario, scenario parameters override them, and
CLI `--set` values have final precedence. Assertions use dotted result paths
and `<`, `<=`, `==`, `>=`, or `>`; a failed assertion exits with code 4.
Batch scenarios run concurrently because each scenario is independent. The
automatic limit is the available logical-processor count, capped at sixteen;
use `--jobs N` to select an explicit limit or `--jobs 1` to force serial
execution. Results remain in configuration order regardless of completion
order, and the batch summary records `parallel_jobs` and `elapsed_ms`.
Distribution summaries include p99.5, p99.9, and p99.95 in addition to the
standard p50/p90/p95/p99 values, so extreme-tail sweeps do not require a large
per-frame timeline.

Every summary also carries sender-spacing cadence: each consecutive pair of
presented frames is compared against the sender's own RTP spacing, excluding
pairs whose sender or arrival interval exceeds 25 ms, and a pair presented more
than 2 ms wider than the sender spacing is a hitch attributed to late arrival
(ready offset above the playout delay), a render-lead jump, the display floor,
or other. The same metric is computed for the recorded session
(`original_sender_*`, `observed.sender_cadence`), the candidate
(`replay_sender_*`, `simulation.sender_cadence`), and a stock-style
present-on-render emulation built from decode completion plus the recorded
prepare and present durations (`stock_sender_*`). Because the replay's own
band residual measures each policy against its own source clock, use the
sender-spacing fields to compare policies with different clock models.
`replay_worker_saturated` is set when the worker-occupancy decision model
shifts the median decision by more than a source period: the candidate keeps
the single worker busier than the source cadence, the live worker would shed
frames through its stale check, and this fixed-admission replay lets latency
run away instead, so treat that scenario's numbers as invalid. The playout
delay sweeps are `tests\vrr\configs\playout-delay-sweep-lowlatency.json`
(the production budget) and `playout-delay-sweep-smoothness.json` (the retired
extra render-lead budget, which saturates render-bound captures);
`mode-compare.json` pits the two retired mode policies against the single
production policy. Run one with `--config ... --jobs 0 --output sweep.json`
and print the table with `scripts\vrr-sweep-table.ps1 sweep.json`.

`mode: fixed` preserves recorded admission and lifecycle for rigorous A/B
controller comparisons. `mode: worker` requires schema 5 and audits recorded
arrival/queue depth against a candidate capacity. It does not synthesize an
alternate renderer lifecycle and therefore cannot pass raster-simulation
readiness; use fixed mode for candidate phase/exposure modeling. The raster
envelope brackets software-visible phase exposure, while optical tears are
never predicted.
Schemas 3 and 4 remain readable in fixed mode for historical baseline checks.

To include these targets in a top-level developer build, the integration
project should add `tests` to its `SUBDIRS` only inside
`contains(CONFIG, tests)`. That wiring is intentionally outside this test-only
directory.
