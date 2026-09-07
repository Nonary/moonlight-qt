# VRR14 improvements adapted to VRR13

The implementation keeps VRR13's RTP-to-local mapping, 20% cadence smoother,
period fitting, preparation scheduling, and asynchronous submission structure.
VRR14's presentation predictor and its recovery-headroom subtraction are not
used to schedule frames.

## Buffer and processing measurements

Production uses one five-minute, one-second-bucket histogram of readiness error
against the **smoothed VRR13 slot**, with 250 µs bins and an empirical p99.95.
The histogram survives changes in content FPS. Cold padding is bounded by the
display interval and rendering cost instead of growing toward a whole slow
source frame. The learned target does not deduct time until the following frame:
that time cannot make this frame meet its original VRR13 deadline.

Recent misses raise protection without waiting for the full window. The applied
delay retains VRR13's bounded attack; release begins after two seconds of clean
evidence and follows elapsed time, with recovery gaps bounded. Requested delay
is limited by three waiting slots plus the active frame, reserving a slot for
the next arrival and accounting for rendering and positive smoothing lag.

Host stalls are classified from RTP, with a cadence-scaled threshold so normal
30 FPS frames remain eligible. Steady-source receive stalls teach receiver
jitter. Decoder backlog episodes are held provisionally: temporary backlog
teaches its tail when it drains, while sustained overload is excluded. Rendering
lead excludes measured swapchain acquisition waits; rendering costs remain in
their existing lead model rather than being counted again as playout delay.

Versioned calibration profiles retain raw jitter history, age offline evidence,
reject incompatible data, and separate host/application, network, resolution,
codec, renderer/GPU/driver, display, stream settings, and smoothing preferences.
Display changes invalidate saving under the old environment's key. Cached
samples do not count as fresh successes. Native presentation coverage is not
currently sufficient to promote a run into a proven lower cold-start buffer;
profiles therefore reuse history without claiming that qualification.

## Native maximum and frame-specific protection

The FPS list offers native refresh, including 120 FPS, instead of a calculated
116 FPS maximum. Exact native rates qualify for VRR. Saved custom rates remain
available.

Each planned submission selects protection independently. There is no fixed
109–116 FPS exclusion or long latch cooldown after a cadence disturbance. D3D11
uses `Present(1, 0)` when a frame's slot is inside the protected display interval;
safe slots retain immediate VRR presentation. Backends without per-frame native
latching retain the software display-spacing floor. Native feedback remains
measurement and does not predict the next target.

## Overlays and diagnostics

Overlay rasterization and renderer notifications run on a background worker.
Updates coalesce, renderer detachment waits for outstanding callbacks, and
Vulkan alternates reusable textures only after image-local upload completion.
VRR13 already had per-frame D3D11 decode fences; their queue handoff is retained.

New trace rows include the original target before readiness/floor correction
and a complete immutable starting calibration. Replay restores the captured
history and checks original targets, while old captures retain their recorded
policy through default-disabled compatibility parameters. Histogram checkpoint
tests verify exact expiration and continuation across rate changes.

`scripts/vrr-deadlines.py` reports readiness, submission, drops, native coverage,
and startup/tail windows. It joins DXGI refresh identities without treating
`SyncQPCTime` as an unrelated presentation's timestamp. Missing feedback stays
missing. The restored diagnostic launchers capture locally, retain at least the
first hour before the existing size cap, run exact replay, and copy completed
captures and reports to ChaseShare after Moonlight exits.

## Validation

The incremental Windows build, all four existing deterministic suites, overlay
worker tests, profile tests, and Python deadline audits pass. Cached-start and
180-frame warm-up captures pass exact replay, including sequence integrity.
The five scenarios in `tests/vrr/configs/vrr13-history-stress.json` pass zero
modeled interval violations and a 30 ms p99 decode-to-submission bound.

These are regression and simulation checks, not proof of live visual superiority.
The latest user `.vrrtrace` selected on September 6 contains no frames and fails
exact-baseline validation. A fresh gameplay capture is needed for a live timing
comparison, especially at the native refresh ceiling.
