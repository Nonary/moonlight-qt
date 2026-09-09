# Streaming, VRR, and timing architecture

This is the persistent technical orientation for this fork. Read it at the start
of a session working on streaming, decoding, rendering, VRR, latency, or replay.
It explains the implementation and the reasoning needed to investigate it;
it does not establish that a particular deployed executable matches the source.

Source baseline: `e63bbd45242f51cb07490f093ee39a009f10ba96` plus the confirmed-native-hitch
adaptation correction and removal of gap fill and reduced-rate VRR recommendations
(committed as `e0e7993d`), plus the incoming host smoothness overlay (`6db3919d`)
and its rolling 30-interval variance correction, inspected
2026-09-07; updated for readiness-driven padding, stable smoothness references,
and preservation of learned preparation lead on 2026-09-07; the subsequent
game-spacing correction disables production cadence smoothing and caps padding
at 16 ms. Production now gates buffer growth on matched native presentation
errors strictly greater than 3 ms and permits release only with 3 ms of
readiness headroom and recent smooth native evidence. The initial map came from nine Luna Medium specialists, followed by
targeted source checks and corrections. No live capture, optical measurement,
build, or test run was part of this documentation investigation. Recheck the
named functions after changes; comments, diagnostic labels, and old experiments
can disagree with the active implementation. The historical D3D11 latch mismatch described below
is one concrete example; the current native boundary now forwards the selected interval.
Updated on 2026-09-08: the dropdown again offers the original below-refresh
VRR recommendation and Low-latency VRR choice. Production uses
`playout_adaptive_only=1`: neither source-rate thresholds nor late frames
switch adaptive presentation to V-Sync. Historical latching policies remain
available for replay, with the new field defaulting to zero for old captures.
Updated on 2026-09-09: the Linux-only "Test SteamOS VRR fix (experimental)"
checkbox makes Gamescope Mailbox selection an opt-in A/B test, default off.
The persisted `gamescopemailbox` preference is snapshotted at session startup
and passed through decoder parameters. Reconnect after changing it. Enabled,
it checks Mailbox after Immediate and before the existing WSI FIFO fallback;
disabled, it restores the previous Immediate/WSI FIFO selection. Affected
SteamOS hardware has not yet validated it as a remedy for overlay-dependent stutter.

Updated on 2026-09-09: production requires verified display-event timing for
native feedback and client cadence reporting. DXGI refresh references are
excluded; without a display-event provider, the cadence field reports `N/A`.

[AGENTS.md](AGENTS.md) owns machine-specific build, deployment, and capture
procedures. This document owns the architecture explanation. Keep both current
when changing their respective contracts.

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
| Windows native presentation | [d3d11va.cpp](app/streaming/video/ffmpeg-renderers/d3d11va.cpp) |
| Replay and its contract | [vrrreplay.cpp](tests/vrr/vrrreplay.cpp), [VRR test README](tests/vrr/README.md) |
| Statistics | [decoder.h](app/streaming/video/decoder.h): `VIDEO_STATS`; overlay formatting in `ffmpeg.cpp` |

For a timing change, start with the resolved session parameters, follow
`VrrPacingWorker` into `schedule()`, then follow the actual presenter call and
feedback back into the controller. Read replay only after understanding what
the live path records and which parts replay holds fixed.

## 3. Settings, negotiation, and mode selection

### 3.1 Preferences are not proof of an active mode

`StreamingPreferences` persists ordinary settings through `QSettings`.
At the inspected revision, V-sync defaults on, VRR defaults off, smooth VRR
timing defaults on.
Legacy frame pacing defaults off. The default requested stream is 720p60.
These are defaults, not evidence of the user's current saved settings.

The FPS picker is advisory. Fixed 30 and 60 FPS remain available. When V-sync
and VRR are requested, usable display refresh rates contribute VRR choices at
`floor(refresh - refresh^2 / 3600)` and Low-latency VRR choices at
`floor(refresh / 6) * 5`, restoring the original dropdown calculations
(116 and 100 FPS at 120 Hz; 138 and 120 FPS at 144 Hz). Native rates remain
available with VRR disabled or as saved custom values.
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

With VRR and configuration warnings enabled, the game list shows one inline
note when integration is absent, limiting is disabled, or an enabled limiter's
FPS override differs from Moonlight. Automatic virtual-display limiting counts
as enabled for the configured virtual-display path and suppresses the warning.
Only hosts without integration get installation guidance; integrated hosts get
limiter configuration guidance. Game V-Sync is not advertised as an alternative.
No new launch toasts or in-stream notifications are generated.

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
| `decodeCompleteUs` | Client monotonic microseconds | Decoder output became available to the client; GPU dependencies may still require synchronization. |
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
decode         = decodeCompleteUs    - decodeSubmitUs
post-decode    = submissionTimeUs    - decodeCompleteUs
client ingress = submissionTimeUs    - receiveTimeUs
```

Check validity and ordering before subtracting. Post-decode includes queueing,
GPU dependencies, rendering/preparation, scheduler delays, deliberate pacing,
and native submission behavior. It is not simply the configured playout delay.
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
age/backlog/missed-tick criteria apply. A lone late frame may still be shown.
This differs from throwing away compressed reference frames and does not require
resetting the codec merely because an image was not presented.

### 7.2 One normal frame

1. Wake, consume pending window notifications, and dequeue a frame.
2. Check stop/suspend state and wait for this frame's decode readiness.
3. Ask `VrrTimingController::schedule()` for the target, render-start deadline,
   latch request, and diagnostics using the current monotonic time.
4. Apply stale replacement policy when newer work is available.
5. Wait until render start, then recheck window/display epoch and lifecycle.
6. Call `prepareFrameForPresent()` with the captured decode dependency. Rendering
   and image acquisition belong here; intentional target waiting does not.
7. Handle preparation failure/cancellation. If the presenter reports
   `sourceFrameReusable`, release the decoder surface before the target wait.
8. Wait for the target, then enforce the controller's currently applicable
   earliest-submission floor with another clock read and wait if necessary.
9. Consume final lifecycle notifications immediately before the native operation.
10. Call `presentAdaptive()`, capture result and timing, and record submission
    and native feedback for later decisions.
11. Trace the outcome and retain/defer frame ownership as required by the presenter.

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

At the inspected baseline the resolver enables timestamp playout, prediction,
shared history, smoothness feedback, adaptive delay, and per-frame latch
requests. It disables the retired metronome and prepare-on-arrival experiment.
It also sets `latchedFloorDisabled=1` and disables the extra queue-mode budget.

| Production input | Value / meaning |
| --- | --- |
| Delay start seed | 6,000 us, then source/display/work/capacity scaling below |
| Delay minimum input | 1,000 us, capped by available capacity |
| Delay maximum input | 16,000 us, capped by available capacity |
| Start-period ratio | 950 per mille of fitted source period |
| Maximum-period ratio | 0; no additional period-ratio maximum input |
| Delay attack | At most 500 us per update |
| Delay release input | 10 us, scaled by elapsed time at a 120 FPS reference rate |
| Prediction margin | 300 us |
| Smoothing gain | 0; preserve relative game intervals |
| Smoothing period EMA | 100 per mille; inactive in production |
| Positive smoothing lag cap | 6,000 us; inactive in production |
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

Production disables the gain smoother. RTP is used for relative frame spacing;
the local offset only supplies a client-clock origin. An arbitrary RTP epoch
must not change scheduling. Game-driven interval changes are not client misses.
Assess controller-added spacing error against relative game intervals, with
raw presented jerk reported separately rather than used as the acceptance gate.
Padding absorbs delivery variability without regularizing the game's cadence.

The following smoother remains available for historical replay and explicit
experiments; it is not the active production policy.

The smoother adjusts local scheduling only; received RTP values stay unchanged.
Conceptually, with `raw = sourceTime + delayBeforeThisFrame`:

```text
trackedPeriod += 0.10 * (eligibleSourceInterval - trackedPeriod)
predicted      = previousSmoothedBasis + trackedPeriod
adjustment     = 0.80 * (predicted - raw)
adjustment     = clamp(adjustment, -delayBeforeThisFrame, 6000 us)
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

Both current smooth-frame-timing settings leave smoothing gain at zero. Timestamp playout,
adaptive delay, readiness constraints, and applicable presentation floors remain.

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

Production sets `playout_adaptive_only=1` and disables both source-rate and
per-frame latch selection. Initialization, timeline resets, and every scheduling
path keep `latchedPresentation=false`, including native-rate sources and late
CPU/GPU work. D3D11 therefore requests `Present(0, DXGI_PRESENT_ALLOW_TEARING)`;
Vulkan keeps its adaptive swapchain mode. The existing adaptive software floors
remain active. Sources above sustainable display throughput may shed frames;
this policy does not promise tear-free output or full-rate delivery at the ceiling.

Historical replay parameters can still select `Present(1, 0)` or FIFO. With
`playout_adaptive_only=0`, `playout_rate_protection_enabled` uses the shared
`floor(refreshHz - refreshHz * refreshHz / 3600)` cutoff; the older per-frame
rule compares the target with `lastSubmission + displayPeriod + guard`.
The adaptive-only field takes precedence when combined with either old mode.
The new field defaults to zero when absent so old captures retain their policy.

Unlatched predicted presentation can raise the target using a fresh scanout
observation, converting that floor back to submission time by subtracting
learned compositor lead, bounded at zero. `earliestSubmissionUs()` supplies
another lower bound.

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
The readiness-driven policy uses the scheduled source slot with playout padding
removed. Production now preserves raw relative game intervals; if a replay
explicitly enables smoothing, its smoothed slot remains the readiness reference.
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

With production prediction and smoothness enabled, the readiness history uses
`Vrr13::Reserve(17)`. Version 17 isolates release-floor evidence for the
native-hitch policy from earlier readiness-driven calibration. Readiness history
can veto shrinking but cannot independently increase padding.
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

There are separate submission and native `SmoothnessFeedback` instances. They
compare actual adjacent intervals against intended adjacent intervals, use a
3 ms tolerance and uncertainty checks, and require valid consecutive evidence.
Production `playout_native_hitch_adaptation=1` scores native intervals against
`sourceTimeUs`, preserving relative game cadence while excluding changes in
our own padding, rendering estimate, or compositor prediction from the desired
interval. A native interval error must be strictly greater than 3 ms even after
subtracting timing uncertainty to authorize growth. Errors at or below 3 ms,
CPU submission errors without native confirmation, and readiness estimates
cannot request more padding. Source-rate transitions and host stalls retain
the existing eligibility exclusions. Native confirmation is OS timing evidence,
not optical proof of a perceived hitch.

Stretch charges the current frame; catch-up charges the preceding delayed frame
using that frame's original padding. Each newly confirmed miss supplies demand
once; historical histogram tails cannot repeatedly authorize growth. Missing,
out-of-order, ambiguous, or unmatched native feedback cannot manufacture a miss.
Legacy policies retain their inclusive threshold, original target/scanout
references, and readiness/combined-feedback adaptation for exact replay.
The new parameter defaults to zero when absent from older captures.

`PresentationPrediction` requires explicit display-event timestamps in production
and matches them to submitted present IDs. DXGI refresh references remain usable
only by the legacy replay policy; matching their refresh identity does not turn
them into display events. Stale, future, or too-uncertain observations are
ignored. The inspected implementation bounds sample age at 100 ms and native
uncertainty at 500 us, learns a rolling median ready-to-presentation lead, and
uses fresh matched observations for its floor. Missing feedback remains missing.

### 9.3 Delay update and capacity formulas

`updatePlayoutDelay()` dispatches directly to `updatePlayoutHistory()` when
history is enabled. The later per-rate-band reservoir/percentile branch is
legacy/replay behavior. In that branch 1000 per mille means p100, 999 means
p99.9, and 995 means p99.5. Those values must not be confused with the active
Reserve p99.95 implementation.

Production updates padding as follows:

- A new confirmed native hitch requests protection based on the delayed frame's
  padding plus its interval error beyond the 3 ms tolerance. Requests slew upward
  by at most 500 us per update and remain bounded by capacity and the 16 ms cap.
- Without a pending hitch request, padding can shrink toward readiness p99.95
  plus 3,000 us. A higher readiness estimate only stops release; it cannot grow
  the buffer. Readiness history includes preparation and scheduler work but
  excludes deliberate waiting.
- Release requires warmed readiness history, smooth native evidence allowing
  release, and a native observation within 100 ms of the current decode time.
  Missing feedback does not authorize continued release. The existing 10 us
  release input scales by elapsed time at a 120 FPS reference, capped at
  33,333 us of elapsed recovery time per update.
- Capacity remains a hard safety bound; when insufficient, the requested
  headroom cannot be guaranteed. A capacity-clipped request cannot pin the
  buffer indefinitely or resurrect growth after capacity recovers.

The legacy readiness and combined-feedback laws remain available for replay.

Queue capacity is an independent hard bound:

```text
period          = min(fittedSourcePeriod, negotiatedStreamPeriod)
capacity        = 3 * period
occupied        = renderLead + presentationSafety
                + (smoothingEnabled ? maximumSmoothingLag : 0)
queueDelayLimit = max(0, capacity - occupied)
effectiveMin    = min(1000 us, queueDelayLimit)
effectiveMax    = min(16000 us, queueDelayLimit)
```

The cold start first takes `max(6000 us, 0.95 * sourcePeriod)`, caps that by
`max(displayPeriod, renderLead)` for history mode, then clamps to effective
minimum/maximum. Consequently neither “the buffer always starts at 6 ms” nor
"the maximum is 8 ms" describes current production. The 16 ms input is a
ceiling on padding, independently of the three-frame storage limit.

More protection can improve jitter tolerance while consuming latency and queue
capacity. If the requested protection exceeds capacity, record the limitation
rather than presenting the capped policy as able to absorb all observed work.

### 9.4 Persisted calibration

`vrr13-calibration.json` lives under the cache path. The profile key includes
display identity, stream FPS, display refresh, smoothing settings,
and session context. Profiles expire after 14 days; saves require at least
240 observations, use locking/atomic replacement, and cap storage at 16 profiles.

The native-hitch policy accepts version-17 readiness histograms only as
release-floor evidence; they do not authorize startup growth. Older calibration
versions are rejected. Reserve ages the prior and replaces its mass
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

Preparation binds/clears the backbuffer, renders video and overlays, and sets
colorspace/HDR state. It uses the D3D/FFmpeg context lock while manipulating
shared state. Present-ready fence handling signals, flushes, polls completion,
and waits on an event, releasing the lock where needed so decoding can proceed.
The event wait has a 50 ms bound; only actual completion is accepted as readiness.
Failure disables the adaptive path and requests device recovery.

Fence completion proves source texture reads have finished, allowing the
presenter to report `sourceFrameReusable` before the target wait. CPU poll/event
brackets bound GPU completion time; they are not an exact hardware timestamp.

### 10.2 Native Present parameters and telemetry

`D3D11VARenderer::presentAdaptive()` creates one `DxgiPresentParameters`
value from the controller's latch request. The same value supplies native
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

Production requires `playout_require_display_events=1`. Presentation feedback
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

The current DXGI statistics provider supplies no verified display events, so
client cadence reports unavailable. Missing measurements neither count as
successes nor cause a fallback to readiness-driven buffer growth. Padding retains
the existing initial/learned policy and safety bounds; its native-feedback-based
learning waits for valid display evidence. An event-based timing provider is
needed for a measured adaptive-display cadence percentage.
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

## 11. Other presentation paths

The shared presenter interface separates support checks, decode-boundary
capture/readiness, preparation, adaptive presentation, cancellation, and feedback.
Its implementations can have different acquisition and cancellation semantics.
Do not transfer D3D11 fence or Present assumptions directly to Vulkan.

On Linux the VRR request prefers the Vulkan frontend. The adaptive mode is
selected for the surface at startup: Mailbox on ordinary Wayland, Immediate
on X11/KMSDRM, and Immediate on Gamescope. Gamescope additionally tries Mailbox
when the SteamOS experiment is enabled, according to exposed surface capabilities.

Gamescope WSI's FIFO compatibility exception is used when Immediate is unavailable
and the Mailbox experiment is disabled or Mailbox is unavailable. Although the WSI layer sends Mailbox to the underlying
driver, it forwards the application's original present mode to Gamescope, which
implements FIFO commit scheduling itself. Selecting Mailbox explicitly avoids
that FIFO policy. Steam's frame limiter can still override a request to FIFO.
Native presentation here remains compositor-owned, and submission success is
not physical scanout feedback. See [SteamOS VRR investigation](docs/steamos-vrr.md)
for source evidence and the reversible composition test for
performance-overlay-dependent stutter.

Unsupported combinations fall back to fixed pacing. DRM VRR property control
and environment choices are separate from the client scheduler's ability to
supply trustworthy presentation feedback. Wayland display/modeset constraints
likewise differ from Windows.

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

Rows carry frame identity, receive/assembly/decode times, queue lifecycle,
controller decisions and resolved parameters, preparation/wait/submission
timings, native results and IDs, GPU readiness bounds, and optional deep/raster
evidence. Terminal rows may be emitted outside the controller-owning worker
and intentionally lack its live diagnostic state.

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
not change buffer adaptation. The old `Client ready on time` overlay counted
preparation deadline misses with zero tolerance and has been replaced by
`Client cadence`. The new percentage counts only eligible, consecutive, verified
display intervals whose client-added spacing error does not exceed 3 ms after
uncertainty handling. It also shows measured coverage relative to the window's
submitted-frame count and the measured hitch count, with drops separate. No
verified intervals in the reporting window yields `N/A (display timing
unavailable)`, never 100%. Cumulative cadence counters are differenced into the
decoder's existing reporting windows; they are not the controller's expiring
five-minute adaptation histogram. Preparation lateness remains internal
diagnostic telemetry. The overlay no longer shows `Errors`; internal
failed-presentation diagnostics remain available.

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

These replay spacing fields use submission timing as a presentation proxy;
they are not the native-confirmed evidence that authorizes buffer growth.
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
