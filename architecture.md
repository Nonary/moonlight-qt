# Streaming, VRR, and timing architecture

This is the persistent technical orientation for this fork. Read it at the start
of a session working on streaming, decoding, rendering, VRR, latency, or replay.
It explains the implementation and the reasoning needed to investigate it;
it does not establish that a particular deployed executable matches the source.

Source baseline: `6f8e5e79` plus the local client-processing,
vrr14-style compact stats reporting, restored Reduce judder, reconnect
trace preservation, motion cadence telemetry, hard buffer ceiling, AMD low-latency decode request, observed-latency trace diagnostics, and removal of the latency oscillation test,
inspected 2026-09-11; now includes responsive readiness revision 3, desktop-rate isolation,
and fence-value-verified Windows readiness waits. The latency
presets and persistent Vulkan presentation changes remain active.
Windows and Linux now use the same responsive prediction-based buffer policy:
99% / 99.5% / 99.95% readiness targets over 30 / 60 / 120 seconds for
Lowest latency / Balanced / Smoothest, a two-second miss boost, and a 500 us readiness margin
within the shared three-frame queue and 0.5/1/2-configured-frame preset caps.
The minimum remains 1 ms (subject to capacity). Five-minute version-20 raw
readiness calibration is diagnostic only and cannot inflate the live request.
Linux's event-gated history-version-19
policy is retired from live selection; explicit historical parameters remain
supported for exact replay. Linux calibration keys are segregated from that
retired policy. Native synchronization and presentation remain backend-specific.
Display feedback is optional diagnostic evidence and does not steer this policy.
Updated 2026-09-11: displayed frame queue delay excludes the worker's explicit
GPU decode synchronization wait. The existing decoding statistic is unchanged;
no new overlay statistic is added. Full decoder-output-to-present-return timing
and the decode-wait duration remain in internal diagnostics. Queue plus rendering
alone therefore no longer equals full internal client processing time.
The initial map came from nine Luna Medium specialists, followed by
targeted source checks and corrections. No live capture, optical measurement,
build, or test run was part of this documentation investigation. Recheck the
named functions after changes; comments, diagnostic labels, and old experiments
can disagree with the active implementation. The historical D3D11 latch mismatch described below
is one concrete example; the current native boundary now forwards the selected interval.
Historical 2026-09-08 builds selected `playout_adaptive_only=1`. The current
production resolver uses `playout_adaptive_only=0` and per-frame latch requests;
source-rate/presentation safety decisions can therefore select a latched frame.
Explicit captured parameters preserve both behaviors in replay.
Updated on 2026-09-10: the forced-composition and per-frame repaint checkboxes
have been retired. Their saved settings (and the older Mailbox preference) are
ignored and removed on settings save. Session startup no longer invokes the
composition guard and passes repaint=false to the decoder. Dormant renderer
helpers and their deterministic tests remain available for development.
Production retains its original Immediate/WSI FIFO selection.

### Retired oscillating latency test (removed 2026-09-11)

The temporary oscillation checkbox, saved preference, session plumbing and
worker timer have been removed. The selected latency preset stays fixed until
reconnect. Saving preferences removes `vrrlatencyoscillation` from older settings.
Trace columns `session_latency_oscillation` and `latency_test_phase` remain zero
for schema compatibility. The report tool retains historical phase parsing so
existing captures remain readable; historical oscillation reports are not exact
fixed-policy A/B evidence. Calibration follows the selected preset normally.

### Windows buffer regression investigation (2026-09-11)

The user-confirmed 4K/116 FPS Balanced session at 18:52:58 recorded 39 GPU-ready
event timeouts, followed by decoder recreation, approximately every ten seconds.
The newest completed connection is `Moonlight-vrr-20260911-185258-376.vrrtrace`
(259,093 bytes, SHA-256
`CA2D3824F96865F8F30BC7EACCE70BE70DA7ABBF636333875EEB28D4EF2C51B5`).
Its 858 arrivals pass exact replay before and after this correction. Its final
8.24 seconds contain no source intervals above 25 ms, but 11% of presented
interval pairs have jerk above 2 ms. Earlier connection fragments from this
same application run supply the timeout evidence; they are not mixed into the
newest connection's baseline metrics.

Two reproducible controller defects are corrected in responsive revision 2:
compensating source jitter no longer permanently disables smoothing, and early
readiness slack pays for an advanced deadline before acquiring extra reserve.
All three preset fixtures reduce average jerk from 8 ms to about 2.95 ms for
9/17 ms source pairs. Clean desktop transition bounds and recovery still pass.
The original final 8.24-second segment's candidate replay is unchanged because these defects
are not triggered there; it must not be cited as a measured gameplay improvement.

Windows now validates fence values between short event waits rather than
depending on one event notification for readiness. Deterministic missing-event,
stale-event, timeout and device-removal tests pass. A local GPU-copy probe passed
1,000 asynchronous fence completions, including 500 suppressed event waits.
The original single-event wait did not reproduce the periodic failure in that
standalone probe. A subsequent 64.94-second gameplay check
(`Moonlight-vrr-20260911-192230-869.vrrtrace`, SHA-256
`47D95A875F91A7023C0EAC938B3483E74D6DC724B3CBE049CA9CEB24A3C5325F`)
recorded no GPU-fence timeouts or decoder recreation; the user reported noticeably
better motion. Readiness was still 83.9%, with mean preparation 5.96 ms, including
5.48 ms in the render-ready wait. It lost arrival row 2 at startup (footer:
6,975 allocated, 6,974 recorded, one dropped); exact replay correctly fails on
frame 35. Do not treat this incomplete capture as strict A/B proof.

The follow-up Windows change implements the existing pre-schedule decode wait
using the frame's captured D2R fence. Previously Windows inherited the no-op
implementation, so asynchronous decode waiting was mixed into rendering service
and its predictor. The same bounded, fence-verified wait is used, without holding
the decode context lock. Monitored fences use the shared worker event as a wake
hint; non-monitored fences use short sleeps and completion polls. Failed waits
invalidate adaptive preparation and request recovery without advertising readiness.
The interface overload retains existing frame-based Linux readiness behavior.
New per-frame capture data must establish how much of the former render wait was
decode work; moving the boundary alone is not proof of lower total latency.
The follow-up app and diagnostics were rebuilt, all six deterministic suites
passed, fresh single-frame and warm-history fixtures passed exact replay, and
five fault scenarios passed their interval and latency assertions. The original
complete capture also retained exact historical replay. ChaseShare and its ZIP
were updated and hash-verified; the installed follow-up executable SHA-256 is
`0A52F9DAA1A2D4E77F125EABB86FFDAB4BF93D8F9AB816F38E5D180A36BF83DA`.
The deployed UNC replay executable passed its runtime help smoke test.

The subsequent 117.71-second capture,
`Moonlight-vrr-20260911-193700-084.vrrtrace` (3,740,216 bytes, last write
2026-09-12 00:39:09 UTC, SHA-256
`2AAF8DC583805162CC7A4D17E0958D40151CDE39BF80A4C78E66F5256919DB24`),
contains all 12,824 arrivals and 12,816 presentations. It records no fence
timeouts; the user confirmed the spikes were gone and motion was much smoother.
Mean GPU decode wait is 5.47 ms; preparation is 1.34 ms (0.91 ms render-fence
wait). Full decoder-output-to-submission latency is 12.67 ms mean / 16.91 ms p99.
Presented jerk is 6.045 ms p99, with 11.9% of pairs over 2 ms; sender-spacing
error is 4.402 ms p99. Three raw sender intervals exceed 25 ms. These remain
separate from client readiness and do not establish optical display smoothness.

The user identified the fluctuating overlay score as `Client ready on time`.
Its global value is 74.9%; the overlay combines the current and preceding
one-second windows and treats any preparation completion after the target as
late. Among 3,215 late presentations the median miss is 64 us, p95 663 us,
and p99 1,226 us. The buffer is held at its Balanced 8,621 us cap; this score's
variation is not evidence of oscillating buffer depth. Its strict deadline
definition is retained, and it must not be called a visual smoothness score.

This capture exposed two replay bugs: a startup idle estimate shifted one busy
frame despite unchanged submissions, and the timestamp audit confused GPU
readiness with immutable CPU output. The corrected replay caps the idle floor
by the row's own evidence, validates both timing boundaries and the explicit
decode wait, and measures latency from immutable output. All targets,
submissions, tear/raster classes, refresh phases and required controller fields
now reproduce exactly, with zero missing arrivals and exit code zero. The
original launcher sidecar retains the pre-correction failure; the fresh exact
result is `build/buffer-readiness-complete-baseline.json`.
Its verified share copy is `Moonlight-vrr-20260911-193700-084-replay-verified.json`.
Final replay SHA-256 is
`DCA1A60C6E88D31EE53AC8A3DAC79A483F437DDE6C2228267C98F214A184058F`;
the diagnostic executable and refreshed ZIP were republished and hash-verified.
Final session-policy comparison is identical to the capture; the five stress
scenarios pass with zero modeled interval violations and 16.91--18.41 ms p99
decoder-output-to-submission latency. No scenario is worker-saturated.

### Preset on-time targets (2026-09-11)

Responsive revision 3 selects 99% / 99.5% / 99.95% readiness targets and
30 / 60 / 120-second learning windows for Lowest latency / Balanced / Smoothest.
The displayed outcome window is 30 seconds for every preset; client drops count
as misses and the overlay exposes lateness over 1 ms and 2 ms and a capacity
indicator. Existing preset caps and the 16 ms absolute ceiling still apply.

The latest completed capture selected before this change was
`C:\Users\Chase\vrr-traces\Moonlight-vrr-20260911-200538-640.vrrtrace`,
17,791,245 bytes, last write 2026-09-12 01:14:58.1621328 UTC, SHA-256
`331B3CB49E414FDA9C4F952F07ACA76C0B7ABC1CE9CCA4BE9CAC591D3B9885A6`.
Its clean footer reports two missing rows out of 57,497 allocated. The unchanged
replay exits 3 on frame 16,169; this capture is not strict before/after evidence.
All eight deterministic suites pass with the final diagnostic build, both fresh
schema-5 fixtures pass exact replay with complete sequences, and all five fault
scenarios pass interval and latency assertions. Gameplay validation is pending.
The iterative application build, complete ChaseShare tree and ZIP are published
and hash-verified. Application SHA-256:
`C8BEF3C3B889DA16CBA28AA7714804150FE617E788465D016C583708603D8DE7`.
The deployed UNC replay help check passes; both diagnostic launchers describe
revision 3 and retain their existing local capture and upload behavior.

### Historical per-frame Gamescope repaint experiment (retired 2026-09-10)

The retired Linux checkbox **Test Gamescope repaint after each frame**
(`gamescoperepaint`, default off) is snapshotted into decoder parameters on
connection. With `GAMESCOPE_WAYLAND_DISPLAY` present, Vulkan creates a private
Wayland connection on a helper thread. Successful adaptive and legacy video
submissions enqueue `debug_force_repaint`; cancelled/failed submissions do not.
The render thread only sets a coalesced atomic notification and writes a
nonblocking eventfd. It never runs a process or waits for a compositor reply.

The helper keeps one command outstanding and one pending notification. Slow
replies coalesce requests; discovery/acknowledgement timeout, protocol rejection,
or disconnect disables the helper for the stream with a warning. Teardown wakes
and joins the helper without a Wayland roundtrip. Logged acknowledgements prove
command execution, not scanout or motion improvement. This private version-1
protocol is experimental and may change with Gamescope.

This command sets Gamescope's base repaint flag, not its non-base-plane overlay
flag; it cannot bypass an in-flight flip or reproduce all Steam overlay policy.
The user reports forced composition did not fix the symptom. Per-frame repaint
was a separate experiment; its user-facing control is now removed and no visual remedy is established. Keep the
forced-composition checkbox off when testing this path to avoid its independent
startup validation failure. No fixed-rate timer or extra image submission is used.

Historical update on 2026-09-09, based on `b60fa11a`: Windows VRR automatically used
the composition presentation API on Windows 11 build 22000.194 or newer when
the driver supports independent flip. Present IDs and independent-flip display
events provide native feedback. Unsupported systems retain DXGI presentation.
Submission intervals supply the production client-cadence estimate regardless
of display-event availability. Native display events remain separate tracking
evidence and DXGI refresh references remain excluded from verified measurements.
No checkbox is needed.
The helper adds no refresh wait or future-frame target; actual Ally latency,
VRR behavior, HDR, and fullscreen transitions still need hardware validation.

Current Windows presenter policy, updated on 2026-09-09 after `a5500a26`:
VRR defaults to the DXGI swapchain so the per-frame latch decision actually
selects synchronized or tearing-permitted presentation. Composition's native
ordering does not implement that switch; the 22:27 capture used composition
for all 12,799 submissions despite 2,654 logical latch transitions. This is a
renderer contract mismatch, not evidence that changing latch hysteresis will
remove the reported judder. `MOONLIGHT_VRR_COMPOSITION=1` opts into the existing
composition backend for diagnostic comparison, snapshotted at renderer setup.
DXGI retains submission-estimate cadence diagnostics; its refresh
references are not verified frame display events. Calibration identities now
include the active presenter so their readiness histories cannot cross-seed.
The capture lost one row and failed exact replay. Exploratory replay favored
retaining the current per-frame controller over rate protection or adaptive-only
spacing, but cannot model a change of native backend or prove a visual remedy.
A fresh gameplay capture is required for that comparison.

Current VRR timing choices (after `20fa2bc4`, 2026-09-09): the `VRR timing`
selector offers Lowest latency, Balanced, and Smoothest throughout the VRR
frame-rate range. `vrrlatencymode` persists IDs 2, 1, and 0 respectively.
Balanced is the new-user default. A saved mode takes precedence; otherwise an
existing `vrrlatencyfix=true` migrates to Balanced and `false` to Smoothest.
The selection is snapshotted through session, decoder, and pacer setup, so
reconnect after changing it. Fixed-refresh pacing is independent of this setting.

| Timing choice | Adaptive playout-buffer cap | Stale-work allowance with a successor |
| --- | --- | --- |
| Lowest latency (2) | Half a configured stream period | Two fitted source periods |
| Balanced (1, default) | One configured stream period | Two fitted source periods |
| Smoothest (0) | Two configured stream periods | Two fitted source periods |

Every preset is additionally capped at 16 ms. Production sets
`playout_delay_maximum_period_per_mille=0`: a slow desktop source must not
expand the absolute maximum. Historical captures retain their recorded
period multiplier for replay.
The allowance bounds extra padding, not total latency or native queue depth.
Source-clock mapping, rendering/readiness learning, per-frame latch decisions,
and applicable display-spacing safeguards remain active in every choice.
Consistent padding reserves time for uneven delivery or preparation so a late
frame can still reach its intended slot. Less padding offers faster response
but can expose more late or skipped frames; more padding can absorb more of
that variation. It does not regularize game-driven frame intervals. Stable
delivery may look the same across choices, and no universal percentage of lost
smoothness follows from the selected allowance.

With `playout_responsive_buffer` enabled, preset caps use the configured stream rate,
so a desktop transition from 120 to 19 or 30 FPS cannot expand the allowance.
The zero default retains fitted-period caps for historical replay.
`VrrSessionConfig::latencyMode` resolves the buffer cap into the trace/replay
parameter `playout_delay_cap_source_period_per_mille`: 500 for Lowest latency,
1000 for Balanced, and 2000 for Smoothest. A zero schema default means an older
capture has no source-relative cap and retains its recorded behavior. The
historical `latency_fix_enabled`, `latency_fix_all_rates`, and
`latency_fix_delay_period_per_mille` fields remain recorded so old captures
replay exactly and Balanced/Lowest retain their admission-age policy. The
internal session mode defaults to zero for historical tests and replay,
independently of the UI's Balanced default. Balanced and Lowest latency append
`|latency-mode=1` or `|latency-mode=2` before calibration-key hashing, while
Smoothest retains the ordinary key.

`VrrFrameDropPolicy` is shared by the real worker and all-arrival queue
simulation. Every non-metronome profile permits replacing work older than two
fitted source periods when a newer queued successor exists. One period is
ordinary occupancy for a single worker waiting on the preceding frame and is
not a stale condition. In the lower-latency profiles age starts at pacer
admission so GPU decode waiting cannot erase it, and is checked again after
waiting to render, before spending GPU work. Smoothest retains the
target-relative second check. The sole available frame is never discarded by
this policy. Local skips preserve the source clock and last submission. These
choices do not impose an FPS cap or change the native presenter.

Historical Latency fix checkbox (after `db596431`): `vrrlatencyfix` defaulted
off and applied the half-display-period allowance only near the refresh ceiling.
`VrrSessionConfig::latencyFix` remains for old tests and captures. With
`latency_fix_all_rates=0`, the fitted cadence still enters at the shared
near-refresh cutoff (116 FPS at 120 Hz) and exits below 114 FPS. Rejected excess
demand is clipped on exit so a near-ceiling miss cannot reappear as padding
solely because the cadence leaves that band; fresh lower-rate misses can acquire
ordinary protection. Historical enabled profiles used `|latency-fix=1`.

Native display cadence remains diagnostic. Windows retains predictive buffer
adaptation. Linux uses readiness-attributed submission spacing errors for growth
(section 9.2); these are submission estimates, not confirmed scanout measurements.

[AGENTS.md](AGENTS.md) owns machine-specific build, deployment, and capture
procedures. This document owns the architecture explanation. Keep both current
when changing their respective contracts.

Updated on 2026-09-09 after `5e759232`: the precise interrupt clock is resolved
through the realtime API set. On this machine kernel32 has no direct export,
which falsely rejected composition despite driver support. Hidden-window setup
now succeeds for both 8-bit and 10-bit presentation buffers. Production restores
per-frame native protection (`playout_adaptive_only=0`, `playout_per_frame_latch=1`)
to avoid accumulating a display-period-plus-guard delay at 120 FPS / 120 Hz.
Composition can honor protected slots through its native ordering, so it now
advertises that capability as DXGI does. Hardware setup is verified; gameplay
display-event coverage and physical presentation still require a fresh session.

The follow-up black-screen correction explicitly sets the presentation surface's
source rectangle to the allocated buffer dimensions in `resize()`, including
initial setup. A successful `SetBuffer`/`Present` did not establish that area:
the unconfigured surface produced no composition or independent-flip events.
The on-screen probe reproduced zero events before the correction and hundreds
of composition events afterward; a screen capture confirms its image is visible.
Independent-flip coverage remains unverified in this desktop/overlay environment.
The hidden `--check` validates initialization only and cannot gate visible output.

The subsequent timing correction replaces the interrupt-clock reference with
QPC scaled to 100 ns using `QueryPerformanceFrequency`. Raw independent-flip
events were present, but their timestamps were rejected as future because the
interrupt-clock epoch was about 20 ms behind QPC on this machine. The old
independent-frame counter counted only accepted timestamps, concealing the cause.
Both target time and feedback now use the same QPC-derived domain; every feedback
sample is freshly correlated to the worker clock. No fixed offset is applied.
Diagnostics count raw independent events separately from rejected timestamps.
A hardware probe with this correction measured 464 of 465 steady-state submissions
and 3.85 ms p99 submission-to-display latency at 116 FPS / 120 Hz. This is native
OS evidence, not optical validation or a full gameplay latency measurement.

Linux AMD decode policy, 2026-09-10: before Qt/SDL can initialize a graphics
screen, `main()` appends `lowlatencydec` to process-local `AMD_DEBUG` when VAAPI
support is built. Existing flags are preserved (including the `R600_DEBUG`
fallback when `AMD_DEBUG` is unset). `MOONLIGHT_AMD_LOW_LATENCY_DECODE=0` disables
the automatic addition for diagnosis; it does not erase flags supplied by the
user. The local moonlight-dev wrapper forwards those variables into Distrobox.
This requests Mesa's separate hardware decode policy; FFmpeg `LOW_DELAY` was
already set and is not equivalent. Mesa 26.1.7's installed RadeonSI library
contains the option, and matching source propagates it to the VCN decode
message. Other vendors do not use this AMD option. Older drivers may ignore it.
No fence wait is removed or shortened, no global power state is changed, and
acceptance/effect by the device firmware is not confirmed by a startup log.
It may use more power; live benefit remains to be measured.

## 1. Fundamental model

Moonlight is the client. The host captures and encodes video, sends compressed
frames, and receives input. The client receives and repairs packets, assembles
compressed frames, decodes them, renders the resulting image, and submits it for
display. Audio and input have their own queues and timing paths.

VRR changes the scheduling and presentation of decoded video. It cannot create
a missing host frame, reverse network loss, or remove the time already spent
capturing, encoding, transporting, and decoding. It can absorb some variability
by delaying frames to a more regular schedule. That costs latency, so the
implementation constrains both the frame queue and the learned delay.

```text
Host capture / encode                           [outside this client]
    |
    | UDP video: RTP timestamp + NV frame/packet identity + FEC
    v
VideoReceiveThreadProc -> RtpvAddPacket -> processRtpPayload
    | packet ordering, repair, access-unit assembly, recovery
    v
Bounded compressed-frame queue
    |
    v
FFDecoder thread: pull decode unit -> avcodec_send_packet
    |                            -> avcodec_receive_frame
    | AVFrame + retained identity/timing metadata + decode GPU boundary
    v
Pacer selection
    +-- legacy queues / V-sync source / renderer
    |
    +-- VrrPacingWorker: bounded decoded-frame queue
          -> wait for decode readiness
          -> controller computes source slot, target, render-start deadline
          -> discard stale work when a newer frame is available
          -> wait for render start
          -> prepare GPU rendering and establish readiness
          -> release reusable decoder surface
          -> wait for target and applicable submission floor
          -> recheck lifecycle -> presentAdaptive
          -> submission/native feedback -> future controller decisions
          -> asynchronous trace writer

Audio UDP -> audio RTP queue -> Opus -> audio-device queue
SDL input events -> input queue / sender -> host
```

Keep these separate: intended source slot, scheduled CPU submission, GPU
readiness, actual native call, OS presentation feedback, and physical scanout.
They are related observations, not interchangeable timestamps.

## 2. Source map and reading order

Paths are relative to the repository. The repeated `moonlight-common-c` directory
is intentional: the outer directory contains the qmake wrapper and the inner
directory contains the common library.

| Area | Source and entry points |
| --- | --- |
| User preferences | [streamingpreferences.cpp](app/settings/streamingpreferences.cpp): `reload()`, `save()`; [SettingsView.qml](app/gui/SettingsView.qml) |
| Session orchestration | [session.cpp](app/streaming/session.cpp): `snapshotPresentationSettings()`, `initialize()`, `drSubmitDecodeUnit()`, stream event loop |
| FPS recommendations | [vrrratepolicy.cpp](app/streaming/vrrratepolicy.cpp) |
| Protocol configuration | [Limelight.h](moonlight-common-c/moonlight-common-c/src/Limelight.h), [Connection.c](moonlight-common-c/moonlight-common-c/src/Connection.c), [SdpGenerator.c](moonlight-common-c/moonlight-common-c/src/SdpGenerator.c), [RtspConnection.c](moonlight-common-c/moonlight-common-c/src/RtspConnection.c) |
| Packet ingress | [VideoStream.c](moonlight-common-c/moonlight-common-c/src/VideoStream.c): `VideoReceiveThreadProc()`; [Video.h](moonlight-common-c/moonlight-common-c/src/Video.h) |
| Packet repair and assembly | [RtpVideoQueue.c](moonlight-common-c/moonlight-common-c/src/RtpVideoQueue.c): `RtpvAddPacket()`; [VideoDepacketizer.c](moonlight-common-c/moonlight-common-c/src/VideoDepacketizer.c): `processRtpPayload()`, `requestDecoderRefresh()` |
| Decoder | [ffmpeg.cpp](app/streaming/video/ffmpeg.cpp): `ffGetFormat()`, `submitDecodeUnit()`, decoder thread; [ffmpeg.h](app/streaming/video/ffmpeg.h) |
| Renderer abstraction | [renderer.h](app/streaming/video/ffmpeg-renderers/renderer.h): `IFFmpegRenderer` |
| Pacer mode selection | [pacer.cpp](app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp) |
| Frame and presenter contracts | [vrrtypes.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtypes.h), [ivrrframepresenter.h](app/streaming/video/ffmpeg-renderers/ivrrframepresenter.h) |
| VRR execution and tracing | [vrrpacingworker.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.cpp) |
| Deadline waiting | [vrrtargetwaiter.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtargetwaiter.cpp) |
| Timing policy | [vrrtimingcontroller.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.cpp), [vrrtimingcontroller.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h) |
| Active learning models | [prediction.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/prediction.h), [reserve.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/reserve.h), [smoothnessfeedback.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/smoothnessfeedback.h) |
| Calibration persistence | [profile.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/profile.cpp) |
| Windows native presentation | [d3d11va.cpp](app/streaming/video/ffmpeg-renderers/d3d11va.cpp), [d3d11composition.cpp](app/streaming/video/ffmpeg-renderers/d3d11composition.cpp) |
| Replay and its contract | [vrrreplay.cpp](tests/vrr/vrrreplay.cpp), [VRR test README](tests/vrr/README.md) |
| Statistics | [decoder.h](app/streaming/video/decoder.h): `VIDEO_STATS`; overlay formatting in `ffmpeg.cpp` |

For a timing change, start with the resolved session parameters, follow
`VrrPacingWorker` into `schedule()`, then follow the actual presenter call and
feedback back into the controller. Read replay only after understanding what
the live path records and which parts replay holds fixed.

## 3. Settings, negotiation, and mode selection

### 3.1 Preferences are not proof of an active mode

`StreamingPreferences` persists ordinary settings through `QSettings`.
At the inspected revision, V-sync defaults on and VRR defaults off. The VRR
timing selector defaults to Balanced for new users, with the saved-checkbox
migration described above. Reduce judder defaults on, preserves the saved
`smoothvrrframetiming` choice, and enables the moderate cadence smoother below.
Legacy frame pacing defaults off. The default requested stream is 720p60.
These are defaults, not evidence of the user's current saved settings.

The FPS picker is advisory. Fixed 30 and 60 FPS remain available. When V-sync
and VRR are requested, usable display refresh rates contribute VRR choices at
`floor(refresh - refresh^2 / 3600)` and Low-latency VRR choices at
`floor(refresh / 6) * 5`, restoring the original dropdown calculations
(116 and 100 FPS at 120 Hz; 138 and 120 FPS at 144 Hz). Native rates remain
available as ordinary choices with VRR enabled or disabled. Native choices take
precedence when another display's calculated recommendation matches them, and
a saved maximum-refresh selection stays selected when VRR is toggled.
A saved custom FPS remains selectable. Toggling VRR does not rewrite saved FPS;
`m_StreamConfig.fps` receives the requested preference.

`snapshotPresentationSettings()` resolves that request for the session:

1. Query the actual window display refresh. An unavailable refresh may fall
   back to 60 Hz for legacy behavior, but that fallback cannot qualify VRR.
2. Resolve effective V-sync. A requested FPS over refresh plus 5 disables it.
3. Require readable refresh, effective V-sync, and stream FPS no greater than
   display refresh for VRR.
4. Force effective borderless desktop fullscreen when VRR is accepted, keeping
   the saved window preference intact.
5. If VRR was requested but rejected and effective V-sync remains enabled,
   enable fixed pacing even if the separate legacy pacing checkbox is off.

The renderer must subsequently support the mode. The Pacer constructs
`VrrSessionConfig`, fixes `allowAdditionalQueuedFrame=false`, passes smoothing
settings, checks presenter support, and starts the worker.
Unsupported presentation or failed worker initialization falls back to the
legacy path. A UI checkbox alone cannot establish DXGI capability, active
adaptive presentation, or that the physical panel is varying refresh.

### 3.2 Host frame-limiter discovery

Vibeshine advertises optional `/serverinfo` fields `FrameLimiterSupported`,
`FrameLimiterEnabled`, `VirtualDisplayFrameLimiterEnabled` (integer booleans),
and
`FrameLimiterFpsLimitMilliHz` (zero follows stream FPS). The protocol is
brand-independent and can also be implemented by Vibepollo. Missing flags
mean no advertised integration; capabilities are ephemeral and refreshed on
host polling, including clearing them after downgrading a host.

`FrameLimiterEnabled` reports limiting for the configured host display path:
either the manual limiter is enabled with an applicable provider, or virtual
display mode is selected and automatic virtual-display limiting is enabled.
Windows does not require the manual checkbox for the automatic path. Linux
also requires a selected Linux limiter provider. The separate virtual-display
flag reports automatic policy availability, not activation on physical outputs.
These are configuration reports, not per-game proof of provider availability,
successful application, or app/client display overrides.

The game list no longer displays a frame-limiter warning. Capability discovery
remains available internally. Settings describe full-refresh streaming and the
optional lower-rate VRR choices without requiring a host limiter.

### 3.3 What the host is told

Session setup fills `STREAM_CONFIGURATION` with FPS, dimensions, bitrate,
colorspace/range, encryption, codec capabilities, and other connection choices.
`clientRefreshRateX100` is a refresh hint in Hz times 100; SDP emits it as
`x-nv-video[0].clientRefreshRateX100`. RTSP negotiation determines the actual
codec/profile using host SDP and server codec flags, with AV1/HEVC/H.264 paths.
Renderer setup receives that negotiated format before video streaming starts.

Packet size is aligned down to a 16-byte multiple for FEC. Connection setup
also applies route-dependent packet-size limits. These are transport decisions,
not VRR scheduling parameters.

The client source proves which values it sends and how it interprets the
response. It does not prove how a particular Sunshine/GFE version captures,
timestamps, paces, or encodes in response. A refresh hint is not host/client
clock synchronization.

## 4. Packet ingress, frame assembly, and recovery

The video receive thread reads UDP into staging buffers, optionally decrypts
AES-GCM payloads, converts network-order RTP fields, and transfers packets to
`RtpvAddPacket()`. Video packets contain RTP sequence/timestamp identity plus
an NV header with stream packet index, frame index, SOF/EOF/picture-data flags,
and FEC metadata. RTP timestamps are in a 90 kHz domain.

The socket receive buffer accommodates roughly 2048 packets to absorb bursts.
Connection watchdogs detect prolonged absence of traffic or successful frames.
The receive path is not a frame scheduler: it should advance valid compressed
data without intentional presentation waiting.

`RtpVideoQueue` groups by frame and FEC block, tracks ordering and missing data,
and uses Reed-Solomon parity to recover losses when possible. Completed data
packets are delivered in order to the depacketizer. Unrecoverable gaps advance
recovery and notify the host. FEC status and frame-loss control messages should
not be described as individual RTP packet retransmission.

One frame's first receive time is reused for its packets. That gives a stable
assembly-duration boundary without taking a clock sample for every packet.
`processRtpPayload()` validates stream continuity, strips headers, identifies
frame boundaries/types, and assembles codec access units. H.264/HEVC Annex-B
NAL units become a linked buffer chain; IDR setup includes codec parameter sets.
AV1 follows its appropriate picture-data assembly path.

The resulting `DECODE_UNIT` owns the compressed buffer chain and carries
frame identity and timing metadata. Non-direct operation uses a bounded queue
of 15 decode units. Queue overflow flushes queued compressed frames and requests
IDR recovery, rather than allowing latency to grow without bound. Direct-submit
renderers can instead submit on the receive/depacketizer path, but the production
FFmpeg decoder advertises pull-renderer capability and owns its decoder thread.

Loss recovery respects codec dependencies. Dropping an arbitrary compressed
reference frame is different from discarding an already-decoded presentation
frame. Invalid packet continuity can drop the current frame and trigger IDR or
reference-frame invalidation recovery depending on capabilities. Repeated drops
eventually force IDR recovery; the inspected code has a 120-consecutive-drop
threshold. `requestDecoderRefresh()` flushes pending units and defers assembly
state reset to a suitable boundary. Completion releases the compressed buffers;
successful IDR completion establishes valid reference state.

## 5. Clock domains and latency boundaries

| Value | Domain / units | Meaning and limitation |
| --- | --- | --- |
| RTP timestamp | Host-origin 90 kHz counter | Source timing identity; wraps and must be unwrapped. Not a client wall-clock timestamp. |
| `presentationTimeUs` | Relative source presentation time, microseconds | Normally derived from RTP. Missing Sunshine PTS can use elapsed local receive time as fallback. |
| `receiveTimeUs` | Client monotonic microseconds | First packet arrival for the frame. Not host capture time. |
| `enqueueTimeUs` / reassembled time | Client monotonic microseconds | Complete compressed frame assembled/queued. |
| `decodeSubmitUs` | Client monotonic microseconds | Sampled immediately before FFmpeg packet submission. |
| `decoderOutputUs` | Client monotonic microseconds | Immutable timestamp captured immediately when FFmpeg returns the decoded frame; origin for client-processing reporting. |
| `decodeCompleteUs` | Client monotonic microseconds | Historical scheduling/readiness boundary, initialized from decoder output and advanced when later GPU readiness is observed. |
| Worker queue, decision, preparation, wait, submission times | Client monotonic microseconds | Distinct CPU-side lifecycle boundaries. |
| Shared fence values | GPU ordering identities | Establish dependencies/completion; not elapsed time by themselves. |
| Native DXGI QPC fields | QPC ticks plus frequency/correlation | OS timing evidence requiring identity and clock mapping. |
| Host processing latency | 1/10 millisecond units | Host-reported aggregate when present; zero means unavailable/inapplicable. |
| Audio samples | Audio stream/device cadence | Independent of video target scheduling. |
| `Reserve` internal time | Nanoseconds | Convert explicitly at the controller/model boundary. |

`LiGetMicroseconds()` calls the common platform clock. On Windows that is elapsed
QPC time from an opaque local epoch. Decoder and pacer timestamps therefore share
a monotonic domain. They are not automatically synchronized to host, GPU, audio
hardware, or panel clocks.

Useful differences are:

```text
assembly       = enqueueTimeUs       - receiveTimeUs
pre-submit     = decodeSubmitUs      - enqueueTimeUs
decode         = decoderOutputUs     - decodeSubmitUs
post-decode    = submissionTimeUs    - decoderOutputUs
client ingress = submissionTimeUs    - receiveTimeUs
client processing = presentationCallEndUs - decoderOutputUs
rendering         = preparationDurationUs + presentationCallDurationUs
queue/pacing      = client processing - rendering - explicitDecodeSyncWait
```

Check validity and ordering before subtracting. Post-decode includes queueing,
GPU dependencies, rendering/preparation, scheduler delays, deliberate pacing,
and native submission behavior. It is not simply the configured playout delay.
The performance overlay uses the vrr14 layout: frame queue delay and rendering
time, without a separate client-processing row. These use the current queue/pacing
and rendering quantities for successfully presented frames, with one shared frame count.
Queue/pacing excludes the explicit worker GPU decode wait. The two displayed
components plus that internal diagnostic wait partition client processing.
Queue/pacing includes queue residence, target waits and other time outside
preparation, presentation and explicit decode synchronization. “Client processing
delay” ends when the presentation call returns. It does not include unmeasured
time from that return until the image becomes visible on the display.
None of these differences alone measures click-to-photon or glass-to-glass
latency. RTT is a round trip, not measured one-way video delay.

The controller keeps source periods in Q16 fixed point where needed. Do not
collapse that to rounded milliseconds when reasoning about long-run drift.
RTP-to-microsecond conversion and epoch/wrap handling must be followed at the
specific use site; a converted RTP number still needs a client-clock mapping.

## 6. Decoder ownership and renderer handoff

The production FFmpeg decoder starts a dedicated `FFDecoder` thread after
renderer setup. It waits through `LiWaitForNextVideoFrame()` when no packets are
outstanding and interleaves pulling compressed input with draining FFmpeg output
when work is in flight. “Pull decoder” describes ownership of the common-library
queue; FFmpeg still uses `avcodec_send_packet()` and `avcodec_receive_frame()`.
Individual FFmpeg codecs may execute more work at either call.

`submitDecodeUnit()` requires a suitable initial IDR, tracks frame-number gaps,
copies the `LENTRY` chain into the reusable packet buffer, and sends the packet.
After successful submission it retains a metadata copy and submission timestamp
in matching queues. The original compressed payload pointers become invalid
after completion and must not be retained as frame storage.

For each output `AVFrame`, the decoder associates queued metadata with that
output, stamps decode completion, and copies frame number, RTP identity,
receive/reassembly time, and decode-submit time into the VRR frame record.
Legacy rendering uses `frame->pts` for source timing and a local `pkt_dts` handoff
timestamp for queue measurements. Those fields should not be substituted for
the explicit VRR timing fields.

`ffGetFormat()` selects the pixel format expected by the chosen renderer and
refuses an incompatible ordinary FFmpeg fallback. Windows has DXVA2 and D3D11VA
paths; software output and other platforms use their corresponding renderers.
Hardware decode can keep image data on the GPU. An `AVFrame` being available
does not by itself mean every GPU read/write dependency has completed.

`Session::drSubmitDecodeUnit()` also protects decoder lifetime with a try-lock:
decoder destruction has main-thread/API constraints, and units can be ignored
while that lock is held, with refresh recovery after recreation. The FFmpeg
pull path is the important steady-state path for this fork.

Legacy Pacer queues drop old frames at their bounds and move frames according
to the V-sync/render path. They defer freeing a rendered frame to protect GPU
use. VRR replaces that pacing mechanism with its worker and explicit presenter
contract; it does not replace network assembly or codec reference handling.

## 7. VRR worker: queue, execution, and lifecycle

### 7.1 Queue ownership and backpressure

The VRR queue admits three waiting frames plus one active frame. This is a
decoded-frame queue, separate from the 15-unit compressed queue and native
swapchain buffers. Do not add these counts and treat the result as a fixed
latency: the queues have different owners, lifetimes, and service rates.

At `submit()`, the presenter captures the decode boundary before subsequent
decoder GPU work can be queued. Under the queue mutex, stopped/suspended workers
reject frames; a full queue evicts the oldest waiting frame, marks a discontinuity,
and admits the new frame. Trace/counter work occurs outside the queue lock.

The worker also sheds stale work when a fresher queued successor exists and
age/backlog/missed-tick criteria apply. All non-metronome profiles use a
two-source-period age allowance. Balanced and Lowest latency measure it from
pacer admission, while Smoothest uses the scheduled target for its second
check. A lone late frame may still be shown.
This differs from throwing away compressed reference frames and does not require
resetting the codec merely because an image was not presented.

### 7.2 One normal frame

1. Wake, consume pending window notifications, and dequeue a frame.
2. Check stop/suspend state and wait for this frame's decode readiness.
3. Ask `VrrTimingController::schedule()` for the target, render-start deadline,
   latch request, and diagnostics using the current monotonic time.
4. Apply stale replacement policy when newer work is available.
5. Wait until render start, then recheck window/display epoch and lifecycle.
6. Call the presenter's `prepareFrame()` with the captured decode dependency
   and the selected `VrrPresentRequest`. Mode changes, rendering, and image
   acquisition belong inside this measured preparation interval; intentional
   target waiting does not. D3D11 keeps its mode selection at Present; Linux
   Vulkan keeps the swapchain's startup-selected mode.
7. Handle preparation failure/cancellation. If the presenter reports
   `sourceFrameReusable`, release the decoder surface before the target wait.
8. Wait for the target, then enforce the controller's currently applicable
   earliest-submission floor with another clock read and wait if necessary.
9. Consume final lifecycle notifications immediately before the native operation.
10. Call `presentAdaptive()`, capture result and timing, and record submission
    and native feedback for later decisions.
11. Trace the outcome and retain/defer frame ownership as required by the presenter.

For performance reporting, the worker keeps the decoder-output timestamp
unchanged through this sequence. On a successful presentation it records the
full interval through step 10 and the sum of the measured preparation and
presentation-call durations. Telemetry subtracts the explicit decode wait
before classifying the remaining time as queue/pacing. Failed and cancelled presentations retain outcome diagnostics but
do not enter any of these paired duration totals or their frame denominator.

The spacing floor is policy-dependent. In production, a frame classified as
latched can have the software floor disabled. Therefore “every submission is
at least one display period plus guard apart” is not a universal invariant.
The worker enforces the floor the controller returns. See the native latch
contract and historical-capture caveat in section 10 before inferring hardware
protection from this choice.

### 7.3 Waiting and scheduler accounting

`VrrTargetWaiter` uses the same monotonic clock as the controller. It sleeps
coarsely until a bounded active region, then yields/polls near the deadline.
Active waiting is capped at 500 microseconds; learned target wake lead is also
bounded at 500 microseconds. Windows prefers a high-resolution waitable timer
with a sleep fallback. Render and target wake-delay observations feed later
decisions, with separate limits.

A timer returning is not permission to submit early. The worker rereads time
and loops until the applicable floor has actually been reached. Conversely,
an OS deschedule can make it late despite a correct target. Trace planned
deadlines, actual wakeups, and native call boundaries separately.

### 7.4 Suspend, restore, cancellation, and shutdown

Minimize/suspend immediately clears queued work and wakes the worker. An
in-flight prepared image can be cancelled; the worker then blocks until restore
or stop. Display/window epoch changes invalidate current assumptions, reconcile
presenter state, and cause controller rebase. Session-level display changes can
recreate the renderer or disable VRR when refresh becomes different/unreadable.

Cancellation is backend-specific: some presenters may need a native submission
to release an acquired image. The worker accounts for that feedback and any
required spacing instead of assuming cancellation has no timing effect.
D3D11 cancellation unbinds its render target and does not use Present to cancel.

Shutdown sets stop state, wakes and joins the worker, discards remaining queued
frames, closes the trace, and conditionally saves calibration. The final
presenter cancellation releases retained native state. Avoid destroying a
decoder surface or native image while a GPU operation can still reference it.

## 8. Controller: source timeline and target construction

### 8.1 Resolve the live policy before reading parameters

`VRR_TIMING_PARAMETER_FIELDS` defines the shared parameter/serialization schema.
Its initializer values preserve older behaviors for replay and tests.
`vrrTimingParametersForSession()` overrides them for production. A comment or
schema default is insufficient evidence of the current session policy.

The resolver enables timestamp playout, shared readiness history and adaptive
delay. Both Linux and Windows use prediction-only growth with
`playout_readiness_hitch_threshold_us=0`. Native-hitch adaptation is disabled.
The current latency presets and per-frame native latch requests are preserved.
Display smoothness feedback remains diagnostic. Historical Linux submission-error
attribution is retained for replay, but not selected by live sessions.
Historical feedback policies remain selectable for exact replay.
It disables the retired metronome and prepare-on-arrival experiment.
It also sets `latchedFloorDisabled=1` and disables the extra queue-mode budget.

| Production input | Value / meaning |
| --- | --- |
| Delay start seed | 6,000 us, then source/display/work/capacity scaling below |
| Delay minimum input | 1,000 us, capped by available capacity and the selected timing allowance |
| Delay maximum input | 16,000 us, then capped by available capacity and the selected source-frame allowance |
| Start-period ratio | 950 per mille of fitted source period |
| Maximum-period ratio | 0; source-rate reduction cannot expand the absolute ceiling |
| Delay attack | At most 500 us per update |
| Delay release input | 10 us, scaled by elapsed time at a 120 FPS reference rate |
| Prediction margin | Both platforms: 500 us above recent readiness demand, inside preset caps |
| Smoothing gain | 500 when Reduce judder is checked; 0 when unchecked |
| Smoothing period EMA | 100 per mille; active only with smoothing enabled |
| Positive smoothing lag cap | 2,000 us; active only with smoothing enabled |
| Render lead floor | 3,000 us |
| Preparation-start spacing input | 6,000 us after prior submission |
| Minimum preparation lead input | 2,500 us |
| Future-offset reseed requirement | 3 consecutive qualifying projections |

The old `kPlayoutMaximumUs=8000` constant and nearby historical comments do not
define the live maximum. Likewise the retained `playoutDelayPercentilePerMille`
input of 1000 is not the active production history estimator.

### 8.2 Source clock mapping and cadence

The controller validates frame identity and RTP progression, handles wrap, and
maintains an unwrapped source timeline. Backward/invalid movement, discontinuity,
or an excessive forward interval can rebase it. Source period is fitted from
sender span divided by frame span, retaining Q16 precision and using frame-number
deltas so locally skipped frames do not become an artificial slower source.
Negotiated FPS supplies fallback timing and bounds the fitted source rate.

Cadence history is bounded (6 minimum and 512 maximum samples by schema). Loose
and tight windows are 350 ms and 1 s. A major departure uses a provisional rate
candidate; production requires at least three candidate samples spanning 200 ms
before accepting a sustained change. Isolated gaps should not temporarily turn
a high-rate stream into a low-rate stream and resize every dependent budget.

With usable RTP, the source slot is:

```text
sourceTime = unwrappedRtpInMicroseconds + appliedClockOffset
offset observation = decodeCompleteUs - unwrappedRtpInMicroseconds
```

`observePlayoutOffset()` tracks a windowed minimum of these observations, with
warmup and bounded slewing. The inherited offset window is 3 seconds, warmup is
64 samples, and slew input is 20 us. This tracks relative clock drift without
following every arrival/decode spike. The minimum is an empirical client mapping,
not measured host capture latency or absolute host/client synchronization.

Timestamp mode zeroes the separate legacy readiness reserve/phase demand:
the timestamp playout delay is the jitter budget. Without valid RTP, the
controller uses its fallback cadence/readiness path, with phase and reserve
learning. Do not apply that fallback path's percentile-spread formula to normal
timestamp production behavior.

### 8.3 Cadence smoothing

The **Reduce judder** checkbox controls cadence smoothing
independently of the three VRR timing presets. It defaults on and preserves
existing saved choices. Session startup snapshots it, including across decoder
resets; reconnect after changing it. The stream CLI can override it with
`--vrr-smooth-frame-timing` or `--no-vrr-smooth-frame-timing` without saving.
The label rename preserves the `smoothvrrframetiming` INI key and
`smoothVrrFrameTiming` QML property, so existing enabled and disabled choices
carry over unchanged.

Unchecked, production follows relative RTP spacing while buffering delivery
variation. Checked, it blends the predicted source slot equally with the raw
mapped slot. This redistributes available waiting time to reduce adjacent
short/long intervals, at the expense of timestamp fidelity. It cannot guarantee
uniform motion between irregularly sampled images or prevent compositor jitter.
RTP values remain unchanged; their arbitrary epoch must not change scheduling.
Conceptually, with `raw = sourceTime + delayBeforeThisFrame`:

```text
trackedPeriod += 0.10 * (eligibleSourceInterval - trackedPeriod)
predicted      = previousSmoothedBasis + trackedPeriod
adjustment     = 0.50 * (predicted - raw)
adjustment     = clamp(adjustment, -delayBeforeThisFrame, 2000 us)
smoothedBasis  = raw + adjustment
```

The actual integer implementation also reseeds from the authoritative fitted
period when necessary and resets smoothing on rebases, rate/phase changes,
untrusted cadence, bursts/stalls, or excessive phase error. This is not a fixed
FPS generator. It follows genuine source-rate changes while attenuating adjacent
short/long timestamp pairs.

Production prediction anchors the next smoother state to the intended target,
not a later actual execution time. Otherwise one late frame would move later
frames and turn a temporary miss into persistent added delay. Older replay modes
retain execution-anchored smoothing and the retired metronome for compatibility.

The 2 ms cap bounds positive retiming, not total client latency. Readiness,
queue capacity, timing-preset buffer caps and applicable presentation floors
still constrain the schedule. Smoothing does not add a queued-frame allowance.
Its readiness calibration key gains `|frame-smoothing=500-100-2000` so profiles
from the period when the saved checkbox was inactive cannot cross-seed it.
Unchecked sessions keep their existing calibration identity. Historical traces
retain their recorded parameters and need no schema change.

The moderate policy was selected by exploratory replay of the completed
2026-09-10 19:34:42 local capture (76.85 FPS). It reduced submission-interval
jerk above 2 ms from 18.7% to 1.2%, with unchanged mean pacer residence and
0.28 ms more p99 residence. The exact gate failed native-outcome validation;
this is not strict A/B proof or a live visual result. Raw presented jerk and
sender-spacing residual must both be reported: deliberate retiming increases
the latter even when cadence becomes more regular. The deterministic
77 FPS alternating-jitter fixture with clean delivery adds 2.6–3.5 ms mean
delay across presets; spare buffering and workload determine the live cost.

### 8.4 Target, render start, and latch request

The general target construction is:

```text
target = sourceTime + readinessBudget + cadenceSmoothing
       + playoutDelay + renderOffset + presentationSafety
target = max(target, now + renderOffset + presentationSafety)
```

In timestamp production, `readinessBudget` is zero. With prediction enabled,
the nominal render contribution uses typical render work; preparation lead is
a separate scheduling budget. The mapped slot uses the delay in force before
this frame's update. Learning must not retroactively replace its already chosen
source slot with the next delay value.

Implausibly future projections can reseed phase. Production waits for three
qualifying projections, so one early timestamp does not shift the entire stream.
A late frame can clamp to the present execution opportunity while the next
frame retains its own source slot.

Production sets `playout_adaptive_only=0`, `playout_per_frame_latch=1`, and
`playout_rate_protection_enabled=0`. Before applying software spacing floors,
each target is compared with `lastSubmission + displayPeriod + guard`. If it
falls earlier and the presenter supports native protection, that slot is latched
and its software floor is disabled. Otherwise the adaptive floor applies.
DXGI uses `Present(1, 0)` for protected slots and
`Present(0, DXGI_PRESENT_ALLOW_TEARING)` with headroom. Diagnostic composition already
provides native ordering; its protection capability likewise permits a slot
without the extra CPU floor. It does not expose DXGI tearing flags.

This allows source-rate changes and recovery from late work without permanently
carrying a refresh-plus-guard delay into every subsequent frame. The explicit
adaptive-only policy remains replayable and takes precedence over latch flags.
Historical rate protection uses the shared below-refresh recommendation cutoff.
Backends without native protection retain their software spacing floors.

Prediction-only production ignores the presentation model's compositor lead
and scanout floor. Deadlines use the mapped source cadence, readiness protection
and measured local work/scheduler budgets. `earliestSubmissionUs()` supplies
the local submission-spacing floor. The existing `predicted_scanout_us` trace
field equals the submission target in this policy; it is not a calibrated
measurement of physical scanout. Historical policies can still learn a
compositor lead and scanout floor from native observations.

Normally that earliest submission is `lastSubmission + displayPeriod + guard`.
With `latchedFloorDisabled` and a latched decision it returns zero. This is a
deliberate reliance on native presentation behavior; it must be checked against
the actual renderer implementation, not inferred from the request flag.

Preparation starts ahead of the target using learned render/scheduler budgets.
The 6 ms post-submission preparation constraint addresses swapchain acquisition
that can block when preparation immediately follows a previous present. The
production `render_start_preserve_learned_lead=1` policy lets that constraint
consume only spare lead: it cannot reduce the learned render plus scheduler
lead to the legacy 2.5 ms minimum. Longer preparation therefore earns an earlier
render start, instead of being squeezed into the same narrow window behind a
larger playout buffer. The 3 ms render-lead
floor remains subject to the existing source-rate and capacity bounds.
Preparing immediately at arrival remains an experiment, not production default.

## 9. Active production learning and bounded delay

### 9.1 Readiness prediction

`schedule()` retains a pending probe: decoded time, intended source slot,
period, typical render cost, applied delay, guard, and decoder backlog.
Responsive production uses the raw mapped RTP slot for FIFO prediction and
accounts for any smoothing advance separately in the recent estimator. The
historical readiness-driven policy uses the smoothed slot with padding removed.
Preparation and scheduler measurements are recorded for future decisions.
On successful non-cancelled submission, `ReadinessPrediction` models expected
and actual FIFO service using those measurements, excluding intentional pacing
and acquisition waiting from work that should become learned reserve.

The model compares expected progress with actual readiness. Clean samples enter
the reserve immediately. Backlog and work/scheduler/decoder-queue episodes over
a source period are held until recovery. An episode that persists for 2 seconds
or fills the 512-sample holding array is treated as sustained overload rather
than ordinary jitter to absorb with more delay. Once service recovers, the
model can distinguish a recoverable burst from a pipeline that cannot sustain
the stream.

### 9.2 Reserve history and smoothness feedback

The version-18/19 descriptions below are historical. Current production uses
version 20 for five-minute raw FIFO-readiness diagnostics and a separate
`RecentReadiness` estimator for live control (section 9.3). Cached diagnostics
are not a live growth or release gate. Native feedback remains diagnostic.

Linux uses `ReadinessFeedback` and `Reserve(19)`. For consecutive eligible
submissions it compares actual spacing with intended target spacing, then
requires the delayed frame's decode readiness plus preparation/render-wakeup
work to exceed its intended target. Demand is that frame's existing buffer plus
the smaller of spacing error and readiness lateness, minus the 2 ms tolerance.
A catch-up interval uses the earlier delayed frame's buffer, avoiding a second
charge against a newly increased buffer. Native blocking alone, source cadence
variation alone, missing/discontinuous frames, and work or decoder backpressure
exceeding a source period cannot authorize growth. This is observable attribution,
not proof of a counterfactual display outcome; readiness waits include scheduling.

Version 19 rejects predictive version-18 profiles. Its history may retain or
release padding, but only a fresh eligible miss raises requested padding, so a
cached tail cannot drive a new session to the cap. The fixed 3 ms prediction
margin is not added. Existing attack/release limits, source-frame allowance,
and absolute 16 ms cap remain. Historical trace parameters default the new field
to zero. Normal replay selects the Linux rule for a declared Vulkan backend;
exact replay always uses captured parameters. Windows production is unchanged
by this rule.


Windows readiness history uses `Vrr13::Reserve(18)`. Version 18 isolates
prediction-only calibration from native-hitch release floors and older combined
feedback estimates. Readiness history controls both increasing and decreasing
padding; native and CPU submission interval errors do not become buffer demand.
Namespace/file version names do not mean the older algorithm
is active. Reserve uses nanoseconds, 250 us histogram bins, and one-second aging
buckets over approximately five minutes. Allocation is kept out of ordinary
frame observation.

The empirical quantile inside Reserve is p99.95 nearest-rank. All valid samples,
including successes, contribute to the denominator. Versions 15 and 16 consider a
readiness miss when required protection exceeds available protection by at least
3 ms. A recent miss affects trust and temporary boost rather than being silently
diluted by a long good history.

Release can start after a short warmup: at least 32 samples spanning 2 seconds,
with a recent miss blocking release for 2 seconds. That is separate from the
stronger reliability condition involving longer history and at most 0.05%
misses. Five-minute retention does not mean every startup waits five minutes
before adaptation.

There are separate submission and native `SmoothnessFeedback` instances for
diagnostics. Native intervals are compared against `sourceTimeUs`, preserving
relative game cadence while excluding changes in padding, render estimates and
compositor prediction from the desired interval. A measured cadence error must
exceed 3 ms after uncertainty handling to count as a hitch. Source-rate
transitions and host stalls retain the existing eligibility exclusions. These
observations never request or block prediction-only buffer changes. Native
confirmation is OS timing evidence, not optical proof of a perceived hitch.

In the historical native-hitch policy, stretch charges the current frame;
catch-up charges the preceding delayed frame
using that frame's original padding. Each newly confirmed miss supplies demand
once; historical histogram tails cannot repeatedly authorize growth. Missing,
out-of-order, ambiguous, or unmatched native feedback cannot manufacture a miss.
Legacy policies retain their inclusive threshold, original target/scanout
references, and readiness/combined-feedback adaptation for exact replay.
The new parameter defaults to zero when absent from older captures.

`PresentationPrediction` requires explicit display-event timestamps for production
cadence diagnostics and matches them to submitted present IDs. DXGI refresh references remain usable
only by the legacy replay policy; matching their refresh identity does not turn
them into display events. Stale, future, or too-uncertain observations are
ignored. The inspected implementation bounds sample age at 100 ms and native
uncertainty at 500 us, learns a rolling median ready-to-presentation lead, and
uses fresh matched observations for its floor in historical policies only.
Production ignores both the lead and floor. Its `smoothnessProtectionUs`
diagnostic and user-facing smoothness score use submission prediction even when
native events are available. Missing native feedback remains missing in the
separate tracking counters.

### 9.3 Delay update and capacity formulas

`updatePlayoutDelay()` dispatches directly to `updatePlayoutHistory()` when
history is enabled. The later per-rate-band reservoir/percentile branch is
legacy/replay behavior. In that branch 1000 per mille means p100, 999 means
p99.9, and 995 means p99.5. Those values must not be confused with the active
Reserve p99.95 implementation.

Production sets `playout_prediction_only=1` and `playout_responsive_buffer=3`.
The live estimator keeps 100 ms buckets over the selected preset's learning
window, including successes. Lowest latency uses 99% over 30 seconds, Balanced
99.5% over 60 seconds, and Smoothest 99.95% over 120 seconds. These are
best-effort targets within the existing latency caps. Historical revisions 1
and 2 retain their three-second p99 estimator for exact replay.
Raw readiness is measured against RTP
source slots in the FIFO model; an earlier smoothed deadline is a separate
cost applied before clamping away early-readiness slack. Revision 1 incorrectly
discarded that slack first, charging reserve even when an advanced deadline was
already covered. Large source changes disable smoothing until 200 ms of eligible
cadence falls within 25% of the fitted period. Revision 2 evaluates compensating
short/long outliers together, so alternating 9/17 ms intervals at a stable 77 FPS
do not keep Reduce judder disabled. A sustained same-direction change still
uses individual intervals and resets confidence. This confidence uses source
time, not a fixed frame count. Actual RTP
spacing remains eligible for delivery learning while the rate fit catches up.

A readiness shortfall of at least 1 ms renews a two-second burst boost. Only
fresh misses renew it; old histogram or cache tails cannot. The target adds
500 us to the larger of the selected recent percentile and that boost, bounded by the existing
minimum, configured-rate preset maximum, 16 ms ceiling, and queue capacity.
Growth is at most 500 us per update. Release requires 32 recent observations,
two seconds of live history, no miss for two seconds, and a sample within
250 ms. It approaches the recent target at approximately 1.2 ms/second, up to
twice that with spare display and processing capacity, at most 250 us per
frame. Thus the falling recent target probes downward without waiting for
five-minute diagnostic tails. Silence/overload is not clean release evidence.
Requested demand remains visible above the cap for capacity diagnostics, but
is recomputed every frame rather than stored as debt. No new queue is added.
Revision 3 retires live learning history on a confirmed material source-rate
change while preserving the applied buffer for gradual recovery. Host stalls
above max(25 ms, 1.5 source periods) and catch-up intervals below half a source
period do not train the estimator; their playback outcomes still affect the
displayed readiness result. The diagnostic five-minute calibration is unchanged.

Historical prediction-only mode (`playout_responsive_buffer=0`) behaves as follows:

- Required protection is readiness p99.95 plus any recent readiness-miss boost,
  then 3,000 us of headroom. The model includes delivery variation, FIFO render
  work and render scheduler delays, excluding intentional pacing, swapchain
  acquisition waits and post-submission display latency.
- Padding grows toward that requirement by at most 500 us per update. It does
  not wait for a displayed hitch. Existing protection is compared with the
  requirement, rather than added to the next demand. The cold-start seed is
  applied once and is not added again when calculating headroom.
- Padding shrinks toward the same requirement after readiness history warms
  (32 samples and 2 seconds) and any readiness miss has been absent for 2 seconds.
  No display sample is required. The 10 us release input scales by elapsed time
  at a 120 FPS reference, capped at 33,333 us of elapsed recovery per update.
- Readiness tails still age out over the existing five-minute window; a brief
  clean period does not immediately erase a recent burst or valid cached tail.
- Capacity and the selected timing allowance remain hard bounds
  in both directions. Lowest latency allows half a fitted source period,
  Balanced one period, and Smoothest two periods. These bounds can prevent the
  full 3 ms headroom.

The legacy readiness and combined-feedback laws remain available for replay.

Queue capacity is an independent hard bound:

```text
period          = min(fittedSourcePeriod, negotiatedStreamPeriod)
capacity        = 3 * period
occupied        = renderLead + presentationSafety
                + (smoothingEnabled ? maximumSmoothingLag : 0)
queueDelayLimit = max(0, capacity - occupied)
modeAllowance   = Smoothest: negotiatedStreamPeriod * 2000 / 1000
                | Balanced: negotiatedStreamPeriod * 1000 / 1000
                | Lowest latency: negotiatedStreamPeriod * 500 / 1000
maximumInput    = 16000 us
effectiveMin    = min(1000 us, queueDelayLimit, modeAllowance)
effectiveMax    = min(maximumInput, queueDelayLimit, modeAllowance)
```

The cold start first takes `max(6000 us, 0.95 * sourcePeriod)`, caps that by
`max(displayPeriod, renderLead)` for history mode, then clamps to effective
minimum/maximum. Consequently neither “the buffer always starts at 6 ms” nor
"the maximum is 8 ms" describes current production. The 16 ms absolute maximum is further reduced by the selected
source-frame allowance and three-frame storage limit. The allowance is not a promise of total
decode-to-submission latency because rendering and applicable native/CPU floors
remain outside the adaptive playout buffer.

More protection can improve jitter tolerance while consuming latency and queue
capacity. If the requested protection exceeds capacity, record the limitation
rather than presenting the capped policy as able to absorb all observed work.

### 9.4 Persisted calibration

`vrr13-calibration.json` lives under the cache path. The profile key includes
display identity, stream FPS, display refresh, smoothing settings,
and session context, including the active native presenter. Balanced and Lowest
latency add distinct timing-mode suffixes. Enabled smoothing also appends
`|frame-smoothing=500-100-2000` to isolate its readiness history.
Profiles expire after 14 days; saves require at least
240 observations, use locking/atomic replacement, and cap storage at 16 profiles.

Responsive production accepts version-20 raw readiness histograms for diagnostics
only. Startup uses the existing bounded cold-start seed; cached tails cannot
raise or hold the live request. The separate recent estimator starts empty.
Historical prediction-only mode accepts version-18 readiness histograms; versions
from native-hitch and earlier policies are rejected. Its requested protection
uses the readiness distribution and recent boost, not a display-validated
"proven buffer". Reserve ages the prior and replaces its mass
with live evidence over time. Short interrupted runs preserve a more protective
prior instead of automatically erasing it. Cached evidence is not proof of
current-session coverage. Display epoch changes invalidate calibration saving.
Full reset and phase rebase differ: a rebase can preserve learned playout state
while clearing transient timing predictors.

## 10. Windows D3D11 mechanics and native synchronization

### 10.1 Eligibility and GPU synchronization

The renderer prefers the adapter owning the display, creates a flip-discard
HWND swapchain with five buffers, and chooses appropriate RGBA/10-bit format.
It checks tearing support and uses `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` when
supported. Adaptive eligibility checks effective V-sync, borderless fullscreen,
flip-model state, refresh, renderer threading, readiness fencing, tearing
capability/flags, and render/output adapter compatibility. Actual swapchain and
fullscreen descriptors are rechecked through display transitions.

The source deliberately avoids assuming `SetMaximumFrameLatency(1)` is a free
latency improvement: with interval-zero presentation it can make Present block
in a V-sync-like way. Swapchain buffer count is not the worker queue capacity.

With separate decode/render devices, `captureDecodeBoundary()` signals a shared
decode-to-render fence when a decoder output is handed off. Rendering waits for
that exact value, so it need not wait for newer decode work. Render-to-decode
ordering protects texture reuse. The purpose is both correctness and avoiding
accidental waits for work belonging to subsequent frames.
Windows also waits for that captured decode boundary before controller scheduling.
A material wait advances scheduling readiness while preserving immutable decoder
output time, and enters the existing explicit decode-wait telemetry. This keeps
decoder synchronization out of the measured rendering service used by prediction.
The original GPU-side render dependency and final render-ready fence remain.

Preparation binds/clears the backbuffer, renders video and overlays, and sets
colorspace/HDR state. It uses the D3D/FFmpeg context lock while manipulating
shared state. Present-ready fence handling signals, flushes, polls completion,
and waits on an event, releasing the lock where needed so decoding can proceed.
The complete fence wait has a 50 ms bound. It blocks on the event in 1 ms slices
and checks the fence value between waits; the value, rather than notification
delivery alone, proves readiness. A stale notification cannot release incomplete
work, and a delayed notification cannot hold already-completed work for 50 ms.
The device-removed sentinel is rejected. A true fence timeout or wait failure
still disables the adaptive path and requests recovery; its log includes the
target, final completed value, last event result and device status.
`gpu_ready_wait_result` records the aggregate fence-wait status in Win32 wait
codes; individual slice timeouts are not whole-fence failures. The initial poll
and final readiness timestamp retain conservative completion bounds. Historical
captures retain their original single-event-wait observations unchanged.

Fence completion proves source texture reads have finished, allowing the
presenter to report `sourceFrameReusable` before the target wait. CPU poll/event
brackets bound GPU completion time; they are not an exact hardware timestamp.

### 10.2 Native Present parameters and telemetry

On the DXGI path, `D3D11VARenderer::presentAdaptive()` creates one
`DxgiPresentParameters` value from the controller's latch request. The same value supplies native
telemetry and `presentPreparedFrame()`, which forwards it to DXGI:

- Latched: `Present(1, 0)`.
- Adaptive: `Present(0, DXGI_PRESENT_ALLOW_TEARING)`.
- Legacy: interval zero with the existing `legacyPresentFlags()` value.

The controller can omit its software spacing floor for a latched decision;
passing interval one to DXGI is therefore part of the renderer contract.
`tst_dxgipresent` exercises the actual shared call boundary using a fake
swapchain, including transitions, parameter reporting, and native result
propagation. Actual display behavior still requires Windows validation.

Historical builds computed and recorded interval one but hardcoded zero in
the native helper. Their `latched_present` and native interval fields describe
intent, not proof that DXGI received interval one. Replay cannot repair that
old instrumentation or turn historical `confirmed_safe_latched` classifications
into independent scanout evidence. Check the executable used for each capture.

`restoreFixedPresentation()` disables VRR and retains the swapchain. The legacy
software-paced caller still explicitly uses interval zero; this correction does
not change its pacing mechanism.

### 10.3 Native evidence limits

Only `S_OK` is treated as a presented result. Failed calls request recovery;
non-display success statuses such as occlusion are cancellation outcomes rather
than proof of monitor delivery.

`GetLastPresentCount()` and `GetFrameStatistics()` can report earlier operations.
`PresentRefreshCount` and `SyncRefreshCount` are different identities;
`SyncQPCTime` timestamps the sync observation and is not automatically the
presentation timestamp of the accompanying present ID.

Production cadence diagnostics require `playout_require_display_events=1`. Presentation feedback
explicitly distinguishes unavailable timestamps, refresh references, and display
events. DXGI `GetFrameStatistics()` is marked as a refresh reference and cannot
teach compositor latency, authorize buffer growth/release, or create a measured
cadence sample, even when its refresh IDs match and its timestamp follows
submission. The Windows-host vrr14/vrr15 captures and the Ally GTA capture
demonstrate why: different present IDs can share an identical raw `SyncQPCTime`,
including frames submitted after that reference. A reference is not the frame's
image-change instant. The old inference remains available only through explicit
historical replay parameters; missing `playout_require_display_events` defaults
to zero to preserve old exact baselines. New schema-5 traces additionally record
`latch_time_kind` (0 unavailable, 1 refresh reference, 2 display event).

The DXGI statistics provider supplies no verified display events. Production
always uses submission estimates for its client cadence report.
Readiness prediction independently adapts padding in both directions. Composition-frame statistics also lack a verified frame
display instant, so the same estimator covers periods without independent-flip
events. This lower-confidence timing remains internal telemetry; it does not
claim native display coverage or learn display-service latency from it.
Linux Wayland presentation feedback and Gamescope actual-present timestamps
are explicitly marked as display events and remain eligible for measurement.

`MOONLIGHT_VRR_ALIGN=1` enables observation-only raster probes around Present.
DisplayConfig signal geometry and QPC correlation support phase modeling.
`D3DKMTGetScanLine()` reports raster position around a CPU observation; it does
not establish when a queued flip became visible. Cloned/ambiguous display paths
must not be silently treated as an exact calibration match.

Software timing, tearing permission, and modeled active-scanout exposure do not
confirm an optical tear or its absence. External display measurement is needed
for that claim.

### 10.4 Diagnostic composition presentation

DXGI is the Windows VRR default. Only `MOONLIGHT_VRR_COMPOSITION=1` enables
composition device flags and attempts to initialize the composition presenter.
The value is captured during renderer initialization, so a stream reconnect is
required. Startup logs identify the actual presenter, including setup fallback.

The renderer checks the actual OS version using `RtlGetVersion` (including
revision 194 on build 22000), loads `CreatePresentationFactory` dynamically,
and requires `IsPresentationSupportedWithIndependentFlip()`. OS version alone
is insufficient. The render device uses BGRA support and disables internal
threading optimizations as required by this API; an unsupported device retries
with the original DXGI device flags. Setup failure retains the DXGI path.
System-relative presentation time is QPC scaled to 100 ns with the actual QPC
frequency. It no longer depends on resolving an interrupt-clock export or assumes
that the interrupt-clock epoch equals the presentation clock's epoch.

`D3D11CompositionPresenter` owns five displayable textures, a presentation
manager/surface, and a DirectComposition visual bound to the streaming window.
Initial allocation and each resize explicitly set the surface source rectangle
to the full buffer; omitting this leaves successful submissions with no image.
The same shaders, overlays, colorspace, GPU-ready fence, and pacing deadline
are used. Buffer acquisition checks availability without waiting. Submission
cancels older pending presents and targets the current QPC-derived presentation time, with
no added source period or wait for a presentation event. `ForceVSyncInterrupt`
requests prompt statistics even with hardware flip queues. Buffer storage
is not a queue-depth target; OS/driver scheduling still needs measurement.

Only independent-flip statistics with the matching surface tag, output adapter,
source ID, and increasing present ID become display events. Their 100 ns system-relative
timestamps are correlated with scaled QPC through a fresh bracket on the worker
clock, with bounded age and uncertainty. Composition statistics do not become
display events. The backend is trace value 3; DXGI flags, query results, and raw
QPC fields remain unset. Native ordering lets the controller omit the software
floor for a protected slot, as on DXGI, without claiming DXGI flag switching. Resize replaces
buffers; display changes recreate the renderer and its output identity.

`compositionprobe --run` is an optional fullscreen Windows hardware diagnostic
for independent-flip coverage and submission-to-display latency. It does not
prove optical VRR, tear freedom, or end-to-end latency. No live hardware result
is claimed by the platform-neutral tests or cross-compilation.

## 11. Other presentation paths

The shared presenter interface separates support checks, decode-boundary
capture/readiness, preparation, adaptive presentation, cancellation, and feedback.
Its implementations can have different acquisition and cancellation semantics.
Do not transfer D3D11 fence or Present assumptions directly to Vulkan.

On Linux the VRR request prefers the Vulkan frontend. The adaptive mode is
selected for the surface at startup: Mailbox on ordinary Wayland, Immediate
on X11/KMSDRM, and Immediate on Gamescope. Gamescope additionally tries Mailbox
when the SteamOS experiment is enabled, according to exposed surface capabilities.

The selected adaptive mode remains immutable for the lifetime of one persistent
swapchain. Per-frame controller requests never destroy or recreate that chain.
Persistent Mailbox provides synchronized, stale-image-replacing presentation,
so it advertises protected latch support without a native mode change or the
controller's redundant software spacing floor. Immediate retains that floor
because it may tear. A FIFO-only compatibility path likewise does not advertise
adaptive latch support because it may accumulate queued frames. Actual resize,
reset, or fallback can recreate the swapchain and restores the cached
colorspace/HDR hint before the next acquisition. Deterministic tests do not
establish compositor or physical scanout behavior.

Gamescope WSI's FIFO compatibility exception is used when Immediate is unavailable
and the Mailbox experiment is disabled or Mailbox is unavailable. Although the WSI layer sends Mailbox to the underlying
driver, it forwards the application's original present mode to Gamescope, which
implements FIFO commit scheduling itself. Selecting Mailbox explicitly avoids
that FIFO policy. Steam's frame
limiter can still override a request to FIFO. Native presentation here remains
compositor-owned, and submission success is not physical scanout feedback.
See [SteamOS VRR investigation](docs/steamos-vrr.md) for source evidence and the
reversible composition test for performance-overlay-dependent stutter.
Unsupported renderer combinations
fall back to fixed pacing. Windows Vulkan remains rejected and the macOS path
does not gain on-demand mode switching. DRM VRR properties, compositor policy,
and physical scanout evidence remain separate from the client's mode request.

Gamescope timing rejection counters are cumulative across swapchain resets and
logged at renderer teardown. They distinguish empty queries, returned and emitted
records, unmatched IDs, timestamps before submission or in the future, stale or
invalid timestamps, clock-correlation failures, and skipped warmup. They do not
relax validation or change pacing. Sparse usable feedback alone does not establish
that the compositor dropped the other frames. The installed Gamescope 3.16.23.5
source supplies its scheduled vblank target as `actualPresentTime`; these records
must not be treated as an independent physical scanout measurement.

The worker presents only newly received frames and waits for queue activity
when empty. It no longer retains and re-presents the last image to fill gaps.
Display-side low-frame-rate compensation remains the display's responsibility.
Historical `gap_fills_before` and `gap_fill_last_us` CSV columns remain reserved
and zero-valued to preserve trace compatibility.

## 12. Audio, input, and end-to-end latency

Audio has its own UDP/RTP queue, Opus decoding callback, and renderer/device
queue. See [AudioStream.c](moonlight-common-c/moonlight-common-c/src/AudioStream.c),
[audio.cpp](app/streaming/audio/audio.cpp), and
[sdlaud.cpp](app/streaming/audio/renderers/sdlaud.cpp).
Audio packet duration controls its sample cadence, independent of video FPS.

The SDL renderer requests at least 480 samples (10 ms) or three Opus frames,
providing buffering for audio jitter. It uses pending-audio and device-queue
limits (including 30 ms and 50 ms checks) rather than video timestamps to control
backpressure. Audio startup intentionally discards an initial backlog of about
500 ms; renderer reinitialization similarly prevents downtime becoming permanent
queued audio latency. Muting can suppress audio processing without retiming VRR.

Input goes from SDL handlers to common-library input APIs and a separate sender.
See [InputStream.c](moonlight-common-c/moonlight-common-c/src/InputStream.c) and
[input handlers](app/streaming/input). Mouse movement is coalesced/batched with
a 1 ms interval; the stream event loop normally sleeps 1 ms when idle, with
platform differences. Input does not wait for the next video target to be sent.

The inspected paths show no client mechanism that makes audio playout follow
VRR targets or makes VRR follow the audio device clock. Added video protection
therefore must not be assumed to produce a corresponding audio delay.

An end-to-end interaction contains client input collection/sending, host
simulation, host capture/encode, network transit, client receive/decode,
presentation, and physical scanout. Client statistics expose only portions.
Host-processing reports, local decoder/pacer timings, and RTT cannot be summed
into a precise physical latency measurement without defining non-overlapping
boundaries and obtaining the missing evidence.

## 13. Trace architecture and replay fidelity

### 13.1 Capture mechanics

`MOONLIGHT_VRR_TRACE` enables worker tracing. Local `.vrrtrace` output begins
with `MLVRR1\n` and stores independently compressed CSV chunks. A `.csv` path
selects CSV output. UNC capture paths are rejected to keep network I/O away
from frame delivery. `MOONLIGHT_VRR_DEEP_TRACE` requests deeper instrumentation;
alignment is the separate native raster option described above.

The writer consumes a bounded 8192-row queue. Producer handoff uses `tryLock()`;
contention or a full queue drops trace rows rather than blocking presentation.
The size policy uses a 512 MiB cap only after at least an hour of arrival-time
coverage. Clean-close footer accounting, row sequences, dropped rows, write
failures, and cap state therefore matter to replay fidelity.

The launcher chooses one canonical trace path per application run. Before a new
worker reuses an existing file at that path, it archives the previous connection
as `<base>-connection-1.<suffix>` (then 2, 3, etc., without overwriting an existing
archive). If archiving fails, tracing is disabled instead of destroying the old
capture. The canonical path and launchers' latest-trace links continue to refer
to the current connection. Each archived file keeps its own schema and footer.

Rows carry frame identity, receive/assembly/decode times, queue lifecycle,
controller decisions and resolved parameters, preparation/wait/submission
timings, native results and IDs, GPU readiness bounds, and optional deep/raster
evidence. Terminal rows may be emitted outside the controller-owning worker
and intentionally lack its live diagnostic state.

The optional schema-5 diagnostic extension records `decoder_output_us` separately
from `decode_complete_us`. The former is immutable FFmpeg output; the latter may
advance after GPU synchronization. Overlay client processing is
`present_end_us - decoder_output_us`, and queue/pacing subtracts `prepare_us` and
`present_call_us` and the explicit `decode_sync_wait_us` from that same interval. Older traces cannot reconstruct this
boundary exactly; readiness-to-submission is not an interchangeable latency metric.

Every row also records `session_latency_mode`, `session_readiness_hitch_feedback`,
`calibration_loaded`, `initial_cached_samples`, and `history_version`. Worker
decision rows have `history_state_valid=1` and scalar snapshots of history samples,
misses, duration, and release eligibility. These snapshots are taken at trace
enqueue, after the outcome, rather than at the earlier scheduling decision.
Nondecision rows have invalid/zero history snapshots. Cached sample count is the
startup prior, not current live evidence. No additional histogram calculation,
formatting, or I/O occurs on frame delivery; these additions do not steer policy.
The existing initial-profile column identifies the complete calibration snapshot.
Columns are appended without changing schema, retention, launcher environment,
or existing field meanings, so older replay readers can ignore the extension.

Presenter-reported submission time is used only when valid inside the observed
native-operation bracket; otherwise the worker boundary is used. Present return
time is not silently promoted into scanout time.

### 13.2 What exact replay means

The parser supports schemas 3, 4, and 5, but the inspected strict
`fidelity.baseline_exact` gate requires schema 5. Some operational prose still
calls the launchers schema-4/replay-grade. Inspect actual captured schema and
gate results rather than trusting that label.

The baseline reconstructs the controller using recorded parameters, arrivals,
execution costs, configuration, and lifecycle. Exactness includes complete
sequence/footer accounting, valid timing relationships, required native/GPU
fields, matching controller decisions/diagnostics, simulated submissions,
refresh/raster classifications, and valid execution residuals. A JSON file's
existence or mostly matching timestamps is insufficient; require process exit
success and inspect `capture.recorded_sequence_integrity_valid` and
`fidelity.baseline_exact`.

Exactness proves deterministic reproduction within the recorded model and
evidence. It cannot repair incorrect instrumentation, such as a requested native
parameter recorded differently from the actual API argument. It does not prove
that a candidate policy would cause the same real host, network, GPU, or panel
events.

Current-policy replay and queue simulation select the shared prediction policy
regardless of native backend. Exact replay continues to use recorded parameters,
including historical Linux thresholded-event demand. The strict Windows/raster
diagnostic gate is unchanged and may reject otherwise reproducible Vulkan captures.

### 13.3 Counterfactual model limits

Fixed replay retains recorded frame admission and lifecycle while changing
controller decisions. The `worker-occupancy-v1` decision-time model shifts
candidate decisions according to simulated prior submission and captured
post-submission gaps when the recorded worker was occupied. This improves on
reusing a stale decision time, but it is not a complete alternate execution.

A changed policy could change live stale-frame shedding, decoder backpressure,
queue admission, acquisition behavior, GPU cost, and later occupancy. Fixed
replay cannot synthesize all those changes. Worker-mode auditing checks candidate
capacity but does not provide a complete alternate renderer lifecycle or the
same raster simulation readiness.

For timing-preset or historical latency-fix admission changes, use the actual
worker's deterministic backlog tests and `vrrqueuesim`'s all-arrival event
simulation. The latter shares
the production stale policy but reuses captured service samples in sequence;
it does not reproduce counterfactual native blocking, decoding backpressure or
physical scanout. Its raw presented jerk, source-spacing residual, drop clusters
and latency distributions are distinct metrics. Unsupported display-only
injections are rejected. Do not treat fixed replay's unchanged admission or
its `stock_*` row as an actual fixed-refresh session.

`worker_saturated` identifies scenarios whose candidate occupancy shift exceeds
the model's useful cadence range (the implementation uses a median shift over
one source period). Their latency/cadence results must not be treated as valid
live predictions because fixed admission cannot shed frames like the worker.

Raster replay models software-visible phase exposure from native anchors,
geometry, and probe brackets. It can compare modeled VRR-following and
free-running scenarios. `optical_tear_confirmation_available` remains false.

## 14. Metrics and a useful investigation method

The overlay's `Incoming smoothness (host)` uses the last 30 valid source frame
intervals. Compute their population variance around their own mean, rather
than an expected interval derived from the requested FPS. For standard deviation
`sigma` in milliseconds, the score is `100 / (1 + (sigma / 6)^4)`. This soft curve
assigns almost no penalty to small variation: 1, 2, and 3 ms standard deviations
score 99.92%, 98.78%, and 94.12%. The 6 ms knee is a UI heuristic, not a measured
perceptual threshold or a probability of noticing stutter. It is deliberately
independent of the controller's 3 ms native hitch threshold.

Stable 30, 50, 60, or 120 FPS all score 100%. A 60-to-50 FPS step remains above
99.4% even while the window contains both rates, and settles to 100% after 30
new intervals. Larger cadence changes can temporarily lower the score while
both rates are in the window; timestamp data alone cannot establish intent.
Source stalls are included and age out after 30 subsequent intervals.

Measure raw host RTP intervals at decode-unit ingress before decoding and
pacing, with no local arrival timestamps or fallback presentation timestamps.
Missing, duplicate, or out-of-order frames and repeated/backwards timestamps
clear the window; normal RTP and frame-number wrap remain valid. Require 30
complete intervals (31 consecutive frames) before displaying a percentage;
otherwise display `N/A`. The tracker owns the window across overlay refreshes.
Stats aggregation selects the newest sequence-tagged snapshot, including a
newer unavailable result, instead of widening the window or averaging scores.
The existing roughly one-second overlay refresh cadence is unchanged; the
session-end log likewise shows the final window, not a whole-session percentage.
This identifies uneven host-supplied timing, which includes capture behavior;
it cannot isolate the game engine or detect repeated image content from timing
alone. It is independent of the native-confirmed client hitch metric and does
not change buffer adaptation.

The stats overlay and session summary show client readiness over a rolling
30-second outcome window, alongside the selected target and a buffer-limit
indicator. Revision 3 measures preparation completion against the intended
smooth deadline before late-readiness and display-floor recovery clamps.
Client playback drops count as misses; deliberate shutdown, suspension, and
interruption discards are excluded. A second row reports the percentages late
by more than 1 ms and 2 ms and the 30-second drop count. Drops have no invented
lateness magnitude. Window snapshots are selected by timestamp, never summed
across overlay refreshes. The window uses 100 ms buckets and starts with the
available outcomes before 30 seconds have elapsed. This remains a readiness
measurement, not a visible-smoothness score. With no eligible frames, the line
shows the starting state. Motion cadence, queue residence and GPU waits remain
internal diagnostics.

Submission cadence, motion jerk, queue residence, decode waits, and buffer
counters remain collected internally. Submission cadence counts eligible
consecutive submission-predicted intervals with client-added spacing error over
3 ms. Motion jerk counts adjacent submission intervals differing by over 2 ms,
including host cadence changes, source stalls, and local drops. Neither proves
physical scanout or drives buffering. Native display/compositor intervals remain
separate tracking evidence. Cumulative counters are differenced into decoder
reporting windows, independently of the controller's five-minute histogram.
Failed-presentation diagnostics also remain internal.

Visible smoothness and source-timestamp fidelity answer different questions:

```text
presentedInterval[i] = presentedTime[i] - presentedTime[i-1]
presentedJerk[i]     = change between adjacent presented intervals
senderResidual[i]    = presentedInterval[i] - corresponding RTP interval
```

Use the replay's in-process `replay_presented_jerk_*`,
`original_presented_jerk_*`, and `stock_presented_jerk_*` fields, including tail
values and the share above 2 ms, when discussing overall visible cadence.
For the controller-only 99.95% goal, game-driven interval changes are not
failures. The new `simulation.sender_cadence.client_spacing_accuracy_percent`
uses a strict error greater than 3 ms; `client_spacing_errors_over_3ms` and
`client_spacing_pairs` expose the exact numerator and denominator. It excludes
source intervals over 25 ms, counts them separately as `source_stall_pairs`,
and does not exclude long local arrival gaps when RTP is steady. The older
`spacing_accuracy_percent` and 2 ms fields retain their historical contract,
including their sender/arrival exclusions, for comparison.

These replay spacing fields use submission timing as a presentation proxy.
Neither these metrics nor native confirmation authorizes buffer growth in the
prediction-only policy; readiness prediction controls padding.
Report `smoothness_feedback.native_window_samples` and `native_window_misses`
separately. Sparse or missing native observations cannot establish 99.95%
visible smoothness, even when the observed miss count is zero. Counterfactual
native timing retains recorded service latency shifted with candidate submissions.
Raw presented jerk also includes the game's cadence and must be reported without
attributing all such motion to the client.

Desktop/idle captures are not gameplay tuning evidence. Confirm that the latest
capture contains the workload being optimized before selecting a latency versus
smoothness tradeoff. Keep source stalls, pre-arrival delivery gaps, decoder work,
and post-submission blocking separate when interpreting a sweep. A growing buffer
cannot necessarily fix a delay that moves with the submission target.

For an actual capture investigation:

1. Re-enumerate both `%USERPROFILE%\vrr-traces` and
   `\\allytwo\ChaseShare\vrr-traces` immediately before analysis. Use the newest
   completed capture unless the user names one. Record full path, length,
   UTC modification time, and SHA-256. Match sidecars by complete basename.
2. Run a fresh exact baseline with the current replay executable and check the
   actual exit code and fidelity fields. Failed exactness means exploratory
   evidence, not strict A/B proof.
3. Keep untouched baseline and candidate output separate. Batch named scenarios
   in one versioned config; use native replay parallelism instead of an outer
   shell loop. Automatic jobs are capped at 16.
4. Compare jerk/cadence tails, decode-to-submission mean and p50/p95/p99,
   submission drift, modeled spacing violations, raster bounds, and saturation.
   Report excluded source stalls and evidence gaps.
5. Choose a useful latency/smoothness tradeoff, then exercise nominal and injected
   decision/preparation/submission/scheduler disturbances with explicit interval
   and latency bounds. Severe fault latency is not normal operating latency.
6. After the final code change, rebuild the relevant diagnostic binaries and
   rerun candidate/stress results on the exact same trace. Pre-rebuild output
   cannot validate the final source. Generate a timeline only when per-frame
   causality or unavailable summary statistics require it.

Distinguish observed symptoms by boundary: packet/frame loss, assembly delay,
decode service, GPU dependency wait, preparation/acquisition, scheduler lateness,
intentional queue protection, submission behavior, and native/display evidence.
An average FPS counter alone can conceal all of these.

## 15. Tests, deployment boundaries, and maintenance

The deterministic suites are
[tst_vrrtimingcontroller.cpp](tests/vrr/tst_vrrtimingcontroller.cpp),
[tst_vrrratepolicy.cpp](tests/vrr/tst_vrrratepolicy.cpp),
[tst_vrrpacingworker.cpp](tests/vrr/tst_vrrpacingworker.cpp), and
[tst_vrrreplayconfig.cpp](tests/vrr/tst_vrrreplayconfig.cpp).
They cover timing arithmetic, timestamp wrap/rebase, cadence changes, delay and
history behavior, queue/drop/cancellation/suspension, ownership, wait floors,
trace integrity, native diagnostic modeling, and replay configuration/contracts.
Consult the test names and assertions for the specific behavior being changed;
the existence of a broad suite is not proof that a native API argument is tested.

The ordinary application build does not build the opt-in replay/tests.
Follow AGENTS.md to build diagnostics separately and run all four suites plus
`vrrreplay --help` when required. Missing runtime DLLs are environment failures,
not pacing failures. No deterministic test here establishes optical tearing,
full host behavior, or physical A/V synchronization.

For the current Windows setup, ALLYTWO is this client and also the SMB server;
the Sunshine host is another LAN machine. The private live portable installation
is `\\allytwo\ChaseShare\MoonlightPortable-x64-6.1.0-vrr-lite`.
Keep its stable name. `AllyShare` is an open host-log drop and must not receive
release builds or profile/settings data.

An incremental local link is not publication. A requested ChaseShare update
also needs staging, changed diagnostics, refreshed ZIP, process-safety checks,
complete copy, source/live hashes, and the UNC replay smoke test. Preserve
`portable.dat.inactive`, dependencies, and tools; do not overwrite a running
gaming installation. Follow the complete commands in AGENTS.md rather than
reconstructing them from this architectural summary.

When maintaining this document:

- Update the baseline and affected explanations when active behavior changes.
- Follow resolver values through effective formulas, not only declarations.
- Follow intended native parameters through the actual API call and telemetry.
- Keep live policy, fallback behavior, historical replay modes, and optional
  experiments distinct.
- Preserve clock units, frame identity, queue ownership, and lifecycle order in
  every new diagnostic or model.
- Revisit exact replay whenever schema, feedback, policy state, or execution
  boundaries change; update both share launchers when their contract changes.
- Keep native Present arguments and telemetry aligned; validate the native call
  boundary and real Windows behavior before interpreting optical results.

The durable debugging approach is to trace an observed frame through the full
chain, identify the first boundary that differs from its intended behavior,
and check how that difference propagates into subsequent frames and feedback.
That keeps host stalls, local overload, scheduling policy, native behavior,
and measurement limitations from being conflated.


### Historical buffer-lag correction, 2026-09-10 (Linux growth policy retired 2026-09-11)

This update supersedes the expanding maximum and prediction-only Linux growth
policy described above. The absolute playout maximum is now 16 ms; a lower
source rate cannot expand it. Source-relative preset limits can still reduce it.

Linux enables `readinessHitchFeedback`, recorded as
`playout_readiness_hitch_threshold_us=2000`. Consecutive eligible submissions
must have a spacing error attributable to decode/preparation readiness beyond
the intended target before buffering can grow. Demand uses the delayed frame's
existing buffer plus the smaller of readiness lateness and spacing error, minus
2 ms. Catch-up uses the earlier frame's buffer, avoiding duplicate charges.
Host jitter, native blocking, or cached predictions alone cannot authorize growth.
Work or decoder backpressure beyond a source period breaks eligibility.

Version-19 reserve history retains/releases demand but cannot raise a cold-start
request by itself. It rejects old prediction profiles and does not add a second
3 ms margin. Existing attack/release and capacity bounds remain. Windows retains
its predictive growth rule. The new parameter defaults to zero for historical
traces; replay recognizes version-19 profiles and selects the current Linux rule
for declared Vulkan captures. Submission attribution is not physical scanout proof.

Controller, profile round-trip, replay configuration, and low-rate cap regressions
cover this policy. Live Desktop Mode evidence showed buffering releasing rather
than remaining at the cap; Windows-level visual parity remains unverified.

### Shared buffer policy and queue-stat accounting (2026-09-11)

The historical 2026-09-10 Linux readiness-hitch sections above describe the retired policy.
Production session setup leaves `readinessHitchFeedback=false` on both platforms,
selecting responsive adaptation with version-20 diagnostic history. The earlier
shared version-18 prediction-plus-margin law remains replayable.
Current-policy replay also selects this shared policy regardless of captured
backend; exact replay and explicit scenario parameters preserve version 19.
The changed queue statistic subtracts the explicit worker decode wait after
bounding it to non-render client time, preventing unsigned underflow. Native
render preparation costs keep their existing classification. This is an accounting
correction, not evidence that the GPU became faster.

Responsive-policy validation (2026-09-11): all four required deterministic
suites and profile tests pass. Eighteen 56-second controller fixtures cover
all presets, smoothing on/off, delivery/render/scheduler faults, and repeated
120/19/30 FPS transitions: clean padding stays at 1 ms and recovers near 1 ms
within eight seconds of the injected faults ending. A synthetic worker capture
passes exact replay and the five-scenario responsive-buffer stress config.
The latest completed live capture (20260911-181549-892) fails original-deadline
replay at frame 8355 in both the old and new binary, exit 3; no live A/B or
physical smoothness improvement is established. Existing history_* trace fields
refer to five-minute diagnostics, not the new live release gate.
