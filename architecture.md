# Streaming and VRR architecture

Source baseline: master plus the decode-buffer accounting correction on 2026-10-30. The production worker
resolves one adaptive input policy, bounded catch-up and corrected input
accounting. The calibration key includes effective policy and presenter
capabilities; existing keys are not reused. Schema-6 capture and schema-2
replay configs support this current policy. Old compressed captures can still
be decoded, but older controller revisions require a fresh capture for replay.
The three former experimental environment switches are ignored with a warning.
Smoothing uses named off, worker phase-locked, native tolerance-bounded and
Absorb modes. Retired interval-scoring/release selectors and metronome state
are absent from the current capture and replay parameter contract. The follow-up
audit also removes metronome-specific frame dropping, its constant trace column,
the old absolute render ceiling, base-guard latch exit, and pre-schema queue
budget/disabled-budget alternatives. Current pacing budgets require a nonzero
divisor. The unused extra-frame session flag is also removed from traces,
replay and calibration identity; calibration keys use version 3.
The startup preparation predictor retains the later master safeguard: it uses
the bounded lead until eight samples exist and caps the learned prediction at
one source/display period. At the display ceiling, the current controller may
request replacement to shed standing phase; the worker reserves a proven-ready
successor first and preserves its observed decode boundary and wait. Replay
joins a recorded reservation to that successor's lifecycle. These are software
contracts, not proof of optical display timing.
See [historical streaming notes](docs/streaming-history.md)
and [master VRR history](docs/master-vrr-history-20261029.md) for past revisions;
they are not evidence of current behavior.

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
          -> establish the backend decode dependency; record any CPU wait
          -> controller computes source slot, target, render-start deadline
          -> discard stale work when a newer frame is available
          -> wait for render start
          -> prepare GPU rendering and establish completion/ownership state
          -> release only a source the presenter marks reusable
          -> wait for target and applicable submission floor
          -> recheck lifecycle -> verify deferred readiness -> presentAdaptive
          -> submission/native feedback -> future controller decisions
          -> retire deferred source ownership only at a safe backend boundary
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
| VRR execution and tracing | [vrrpacingworker.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.cpp), [vrrpacingtrace.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrrpacingtrace.cpp) |
| Deadline waiting | [vrrtargetwaiter.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtargetwaiter.cpp) |
| Timing policy | [vrrtimingcontroller.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.cpp), [vrrtimingcontroller.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h) |
| Active learning models | [prediction.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/prediction.h), [reserve.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/reserve.h), [smoothnessfeedback.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/smoothnessfeedback.h) |
| Calibration persistence | [profile.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/profile.cpp) |
| Windows native presentation | [d3d11va.cpp](app/streaming/video/ffmpeg-renderers/d3d11va.cpp), [d3d11composition.cpp](app/streaming/video/ffmpeg-renderers/d3d11composition.cpp) |
| macOS native presentation and display range | [vt_metal.mm](app/streaming/video/ffmpeg-renderers/vt_metal.mm), [macdisplaytiming.mm](app/streaming/video/ffmpeg-renderers/macdisplaytiming.mm) |
| PyroWave decode and framing | [pyrowavedecoder.cpp](app/streaming/video/pyrowave/pyrowavedecoder.cpp), [pyrowaveframing.cpp](app/streaming/video/pyrowave/pyrowaveframing.cpp) |
| macOS PyroWave shared output | [pyrowavemetal.mm](app/streaming/video/pyrowave/pyrowavemetal.mm) |
| macOS PyroWave calibration draw | [pyrowavemetalcalibrator.mm](app/streaming/video/pyrowave/pyrowavemetalcalibrator.mm) |
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
timing selector defaults to Balanced Target for new users, with the saved-checkbox
migration described above. Reduce judder defaults on, preserves the saved
`smoothvrrframetiming` choice, and enables the moderate cadence smoother below.
Legacy frame pacing defaults off. The default requested stream is 720p60.
These are defaults, not evidence of the user's current saved settings.

The FPS picker is advisory and independent of the VRR setting. It offers fixed
30 and 60 FPS, usable native display rates, and a plain below-refresh choice at
`floor(refresh - refresh^2 / 3600)` (116 FPS at 120 Hz). A saved custom FPS
remains selectable. Toggling VRR does not rewrite saved FPS;
`m_StreamConfig.fps` receives the requested preference.

`snapshotPresentationSettings()` resolves that request for the session:

1. Query the actual window display refresh. An unavailable refresh may fall
   back to 60 Hz for legacy behavior, but that fallback cannot qualify VRR.
2. Resolve effective V-sync. A requested FPS over refresh plus 5 disables it.
3. Require readable refresh, effective V-sync, and stream FPS no greater than
   display refresh for VRR.
4. Force effective desktop fullscreen when VRR is accepted, keeping the saved
   window preference intact. macOS uses native Cocoa fullscreen/Spaces for it;
   Windows/Linux retain the borderless desktop path.
5. If VRR was requested but rejected and effective V-sync remains enabled,
   enable fixed pacing even if the separate legacy pacing checkbox is off.

On macOS, the strict refresh query first uses `NSScreen.maximumFramesPerSecond`.
The FPS picker uses the same native maximum; a ProMotion mode whose SDL refresh
is zero therefore contributes its actual 120 Hz choices. Session VRR additionally
requires finite, positive native intervals and
`maximumRefreshInterval - minimumRefreshInterval > 1 us`. A fixed-refresh
screen cannot qualify merely by advertising a high maximum FPS. The window's
actual screen is queried on the main thread. Display-list queries match SDL
desktop bounds to CoreGraphics display IDs instead of assuming enumeration order.
Moving an active VRR window recreates the Metal renderer; an unavailable or
fixed range, or changed maximum refresh, disables VRR for that connection and
uses fixed pacing. Reconnect to qualify a later display again.
Before initializing SDL video, a requested Mac VRR+V-sync session forces
`SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=1`. This overrides saved borderless/notch
presentation choices for that connection because SDL caches the hint at Cocoa
driver startup. The Metal presenter additionally requires that the Cocoa window
actually entered native fullscreen; the hint alone does not qualify playback.

The renderer must subsequently support the mode. The Pacer constructs
`VrrSessionConfig`, fixes `allowAdditionalQueuedFrame=false`, passes smoothing
settings, checks presenter support, and starts the worker.
Unsupported presentation or failed worker initialization falls back to the
legacy path. A UI checkbox alone cannot establish DXGI capability, active
adaptive presentation, or that the physical panel is varying refresh.

`tracevrrframes` defaults to false. The Settings checkbox enables diagnostic
frame tracing only, with export of the latest completed recording and a button
to open `Desktop/vrr-diagnostics`. The platform's Desktop location is resolved
through Qt, rather than hardcoding an English profile path. The session owns its
environment wrapper after acquiring the active-session semaphore, restoring it
only after decoder shutdown
and logger drain. Existing external trace launchers take precedence. Recording is
fixed for the stream, including decoder recreation; full UI/file/lifecycle
contracts are in [diagnostics](docs/vrr-diagnostics.md).

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
remains available internally. Settings describe full-refresh streaming without requiring a host limiter.

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

When VRR presentation qualifies for the session
(`m_PresentationSettings.enableVrr`, decided before launch), `NvHTTP::startApp`
adds `clientVrrRequested=1` to the launch/resume query. Hosts that do not know
it ignore it. Vibeshine uses it for a low-latency WGC buffer and, in its
Automatic virtual-display capture mode (`vibe-test` 09b09ae8), holds the
virtual display at 1000 Hz. WGC stamps frames at DWM composition, which lands
on the virtual display's refresh grid; a 4x display at 116 FPS put every RTP
timestamp on a 2.156 ms grid (capture 20260922-195738 deep trace), while 1000 Hz
bounds that to 1 ms. Vibeshine also refines stamps from the game's DXGI Present
events at send time. This is host behavior the client only requests.

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
| `decoderOutputUs` | Client monotonic microseconds | Immutable timestamp captured immediately when FFmpeg returns the decoded frame; client-processing reporting origin. |
| `decodeCompleteUs` | Client monotonic microseconds | Post-output readiness observation; anchors production RTP-to-client mapping so hardware decode duration is absorbed in the sender offset. Linux samples it after `vaSyncSurface()`, Windows after the monitored decode-to-render fence wait. Every proven D3D11 completion, including a short wait or an already-ready fence, retains a post-observation upper bound. Historical captures and backends without completion evidence retain their output-time mapping fallback. |
| `decodeSyncWaitUs` | Elapsed client microseconds | Explicit CPU time spent by the worker on a decoder/backend completion primitive. It is serial service and latency accounting, not a source-clock timestamp. A zero value does not exclude a GPU-queued dependency. |
| Worker queue, decision, preparation, wait, submission times | Client monotonic microseconds | Distinct CPU-side lifecycle boundaries. |
| Shared fence values | GPU ordering identities | Establish dependencies/completion; not elapsed time by themselves. |
| Native DXGI QPC fields | QPC ticks plus frequency/correlation | OS timing evidence requiring identity and clock mapping. |
| Metal `MTLDrawable.presentedTime` | Core Animation seconds | OS-reported drawable presentation event, freshly correlated with `CACurrentMediaTime()` inside a bracket on the client clock; not the native call/return time or physical panel measurement. |
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
The performance overlay retains frame queue delay and rendering
time, without a separate client-processing row. These use the current queue/pacing
and rendering quantities for successfully presented frames, with one shared frame count.
Queue/pacing excludes the explicit worker GPU decode wait. In advanced tracing,
the two components plus the separate GPU decode synchronization line partition client processing.
Queue/pacing includes queue residence, target waits and other time outside
preparation, presentation and explicit decode synchronization. “Client processing
delay” ends when the presentation call returns. It does not include unmeasured
time from that return until the image becomes visible on the display.
On Windows with monitored fences, the worker waits for the decode fence before
scheduling, and that wait is the GPU decode synchronization line, as on Linux. The
GPU-side decode-to-render wait remains queued. The verified present-ready fence
wait occurs inside preparation, before the cadence hold, and is not a
decode-wait row. Its duration remains part of rendering time.
Linux VAAPI/Vulkan Mailbox output is asynchronous and
does not report a CPU output-completion sample; other Linux imports still poll
before the target hold. These accounting identities therefore partition the
observed CPU path; they do not expose every GPU stage.
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

The VRR queue admits four waiting frames plus one active frame in every
profile (`playout_queue_frames`; Smooth alone until 2026-09-26, and 0 = the
historical three in older captures). Separately, profile playout-delay
allowances default to half, one, or four fitted source frames for Low Latency,
Balanced Target, or Smooth and can be customized. The queue limit in
`playoutQueueLimitUs()` remains a safety
bound on those allowances:
waiting frames x period, minus render lead and the full Reduce judder retiming
budget. With three frames at 116 FPS that budget was ~16.9 ms once the retiming
cap rose to 6 ms, so Smooth's 24 ms ceiling was unreachable and live overlays
showed "limit 16.86 ms" (capture 20260922-221707). The decoder pool reserves
the extra surface (`extra_hw_frames` = classic pacer outstanding frames +
`VrrLargestQueuedFrames` - `VrrMaximumQueuedFrames`). This is a
decoded-frame queue, separate from the 15-unit compressed queue and native
swapchain buffers. Do not add these counts and treat the result as a fixed
latency: the queues have different owners, lifetimes, and service rates.
Backend-held source references can outlive the active worker step without
increasing decoded-frame admission. Their limits protect decoder-surface lifetime,
not a target amount of playout buffering.

At `submit()`, the presenter captures the decode boundary before subsequent
decoder GPU work can be queued. Under the queue mutex, stopped/suspended workers
reject frames; a full queue evicts the oldest waiting frame, marks a discontinuity,
and admits the new frame. Trace/counter work occurs outside the queue lock.

The worker also sheds stale work when a fresher queued successor exists and
age/capacity criteria apply (historical snapshots also retain floor-debt
and missed-tick rejection). All non-metronome profiles use a
two-source-period age allowance. Balanced Target and Low Latency measure it from
pacer admission, while Smooth uses the scheduled target for its second
check. A lone late frame may still be shown.
This differs from throwing away compressed reference frames and does not require
resetting the codec merely because an image was not presented.

### 7.2 One normal frame

1. Wake and consume pending window notifications. Before dequeuing for a blocking
   decode wait, reject expired queue fronts only while a newer adjacent source
   frame exists. Use two periods of the larger of fitted cadence and successor
   RTP spacing (four for historical metronome); skip this check on pending rebase
   or discontinuous stamps. Record `queue_stale` without a controller decision.
   The sole image remains eligible. Then dequeue the retained frame.
2. Check stop/suspend state and establish this frame's decode dependency. Record
   any explicit CPU wait as serial service. Production at `18602b1c` maps from
   decode completion; immutable `decoderOutputUs` remains the full-latency
   origin. Historical captured parameters can instead select decoder output.
3. Ask `VrrTimingController::schedule()` for the target, render-start deadline,
   latch request, and diagnostics using the current monotonic time.
4. Apply stale replacement policy when newer work is available.
5. Wait until render start, then recheck window/display epoch and lifecycle.
6. Call the presenter's `prepareFrame()` with the captured decode dependency
   and the selected `VrrPresentRequest`. Mode changes, rendering, and image
   acquisition belong inside this measured preparation interval; intentional
   target waiting does not. D3D11 keeps its mode selection at Present; Linux
   Vulkan keeps the swapchain's startup-selected mode.
7. If preparation supplies a completed renderer wait, feed that measured interval
   into the controller's bounded readiness history. A presenter may instead
   report completion after the target hold, or leave it unavailable. Future
   frames may start rendering earlier by an available learned lead; the source
   target and native latch decision are unchanged. Failed waits and incomplete
   timing are not training samples. The worker removes an explicit preparation-
   time GPU wait from the generic learned preparation cost so one stall cannot
   inflate both budgets.
8. Handle preparation failure/cancellation. If the presenter reports
   `sourceFrameReusable`, release the decoder surface before the target wait.
9. Wait for the target, then enforce the controller's currently applicable
   earliest-submission floor with another clock read and wait if necessary.
10. Consume final lifecycle notifications immediately before the native operation.
11. Call `presentAdaptive()`, capture result and timing, and consume any deferred
   completion result before recording submission. A deferred completion can
   train future readiness lead and bound current serial service; it cannot alter
   the target already issued for this frame.
12. Trace the outcome and retain/defer frame ownership as required by the presenter.
   Backend source retirement may continue after this worker step.

With `MOONLIGHT_VRR_OFFSCREEN_PREPARATION=1` on Linux VAAPI or PyroWave/Mailbox,
after the first ordinary frame establishes the real
swapchain format, admitted frames also receive cancellable preparation tickets.
A separate thread performs decode synchronization, source import, rendering
to an offscreen texture, and output-completion polling. It owns its own
libplacebo renderer and mapping textures; it never acquires a swapchain image.
The pacing thread waits for the ticket before scheduling, then acquires the
swapchain, copies the completed output, verifies that short copy has completed,
and applies the ordinary target wait. Only after the copy does preparation of
the next image proceed, preventing its GPU work from delaying that copy.

The 2026-09-21 freeze correction is based on `abd6b82d`. A completed
preparation ticket bypasses the two pre-render stale-replacement checks:
rendering has already finished, and a newer queued source is not evidence of
a ready replacement. Queue capacity and expiry still shed waiting work, and
shutdown, suspension and output-epoch checks still cancel active work. This
prevents a render/drop loop under sustained GPU load without altering source
timestamps or hiding preparation latency. The completed live capture
`20260921-002333-70409` reproduced exactly and contained only two presentations
in 45.8 seconds, with 2,528 completed staged images rejected as stale. This was
presentation starvation rather than a mutex deadlock. A worker regression
exercises repeated 40 ms preparation with newer queued images in all three
latency modes. It fails before the correction and passes afterward; all eleven
VRR/backend/profile/GPU trace suites, replay help, native startup and the latest
capture's exact replay check pass. Evidence is in `build/freeze-fix-validation/`.
Actual GPU throughput and visible recovery still need a live retest.

The subsequent `20260921-004719-85692` session log reported 54.17 and 31.90
rendered FPS for its two 3840x2160 streams, despite 105.36 and 107.74 incoming
FPS. Offscreen preparation introduces an extra target copy and CPU-observed
completion waits before allowing the next preparation job. It is now opt-in;
the default once again renders directly into the swapchain and retains the
mapped source until GPU reads retire, using the presentation semaphore for
completion. This removes the new staging cost; recovered 4K throughput still
requires live verification. The completed-ticket starvation correction remains
in place for experimental use.

Tickets refer to the existing three waiting admissions plus the active image;
they do not add another playout queue. Eviction and lifecycle discard cancel
the corresponding ticket. The preparation queue is independently bounded to
three pending jobs and one running job. Completed textures are reused. A
cancelled job that already submitted GPU work retains its source mapping until
the output completes; a timeout/error drains outstanding GPU work before
unmapping. Shutdown interrupts ticket waits and joins preparation before
renderer destruction. An output-size, representation, or colorspace mismatch
rerenders the same source into the current swapchain instead of displaying an
image encoded for the old output. Unsupported formats retain the existing path.

The main trace records `prepared_ahead` and `stage_*` fields in schema 6. Stage
decode, render-command, and output-ready boundaries precede the scheduling
decision and may precede pacing dequeue. The recorded pacing-thread decode wait
remains zero for these frames. Aggregate statistics include actual stage decode
wait and rendering service rather than counting them as queue residence.
The GPU sidecar adds `stage_render` and `stage_output_ready` spans. The latter
is a CPU completion observation, not a GPU execution timestamp or scanout proof.
Replay validates stage ordering independently of direct-worker readiness.
For direct-worker captures, serial-service revision 2 identifies post-wait
clock semantics independently of the source-mapping selection, fixing the
false exact-baseline rejection after Gemini restored decode-anchored mapping.
Counterfactual replay retains recorded stage readiness; it cannot predict
how changing GPU scheduling changes stage throughput or compositor service.

Preparation-stage validation (2026-09-21): the native incremental release and
offscreen startup help pass, as do eleven deterministic VRR/backend/profile/GPU
trace suites. Nine exact replay checks pass, including prepared-frame shutdown
while awaiting completion and the latest pre-change live capture
`20260920-231030-28049`. The signed playout-offset parser and direct readiness
clock audit were corrected without bypassing exactness checks. Evidence is in
`build/prepared-stage-validation/`. These tests do not exercise the complete
new GPU path in gameplay or establish lower physical judder; a matched live
capture is still required. No Windows build or ChaseShare deployment was made.

For performance reporting, the worker keeps the decoder-output timestamp
unchanged through this sequence. On a successful presentation it records the
full interval through presentation return and the sum of measured preparation
and presentation-call durations. Telemetry subtracts the explicit decode wait
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
coarsely until a bounded active region, then polls using
`SDL_CPUPauseInstruction()` near the deadline. Unlike the previous
`std::this_thread::yield()`, this CPU hint does not voluntarily surrender the
OS timeslice just before submission. The base active region is 500 microseconds;
learned target wake lead adds at most another 500 microseconds, for a maximum
1 ms region per wait. Clock-stall escape and absolute-deadline checks remain.
Historical trace `active_yield_count` fields now count active polling steps;
the hook name remains `yield` for deterministic tests. Windows prefers a high-resolution waitable timer
with a sleep fallback. Render and target wake-delay observations feed later
decisions, with separate limits.

`VideoThreadPriority` runs once at entry on the dedicated decoder, renderer,
V-sync, VRR pacer and optional Vulkan preparation threads. Windows first
registers each with MMCSS `Playback`, at relative HIGH for deadline threads
and NORMAL for decode/render/preparation. Registration is released on the same
thread at exit; the system AVRT DLL is loaded dynamically. If unavailable,
SDL provides the platform fallback. On other platforms SDL is the primary
API: deadline threads retain the TIME_CRITICAL request but now retry HIGH on
failure, while other dedicated video threads request HIGH. Each accepted or
rejected request is logged once. These requests need no GPU-specific CPU API;
OS permissions and scheduler policy still control effectiveness. Main-thread
renderers, codec-internal workers, network, audio and the compositor retain
their existing scheduling. No process priority, affinity or system settings
are changed. Priority and polling improvements require a fresh live comparison
of submit jerk and matched display feedback; replay cannot predict scheduling
or compositor changes. The bounded active wait consumes more CPU than yielding
when another runnable thread could otherwise use that time.

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

`vrrProductionSessionConfig()` resolves session settings before both the pacer
and worker use them. `vrrTimingParametersForSession()` turns the effective
settings and presenter capabilities into the controller policy. Follow the
actual session mode, smoothing preference, native scheduling and tearing
permission; default-constructed test parameters are not the production policy.

The source timeline tracks RTP frame identity and cadence, including rate
changes and discontinuities. It maps source time into the client monotonic
clock and builds the intended target, preparation deadline and presentation
floor. Requested FPS and display maximum are not evidence of a constant source
rate. The normalized producer mapping uses prompt GPU completion observations;
a delayed collection can include queue residence and is not treated as GPU
service. Real readiness still constrains the submitted frame.

Worker-paced Reduce judder uses bounded phase-locked smoothing. Native
future-target Reduce judder uses the tolerance-bounded source-gap path.
Absorb uses its fixed 2T pad and its own bounded smoothing; worker smoothing
owns its movement while native scheduled presentation retains its gate.
Smoothing is independently controlled by the session preference. The
controller also uses bounded phase-trajectory catch-up, readiness and display
floors, and presenter-specific latch protection. Applied input buffering,
planned extra delay, queue capacity and retained phase are distinct quantities.

## 9. Adaptive input buffer and calibration

`InputGoalBuffer` observes eligible input readiness against the unbuffered
source slot. The goal solver selects the smallest buffer meeting the selected
severity-weighted client timing goal over its configured window, aiming at a
fraction of the allowed loss. If the goal is unreachable within the cap, it
selects the best attainable score. Rise and release are rate limited; the
controller maintains rate-specific evidence layers and credits available VRR
headroom according to the profile. Eligible outcomes, actual input coverage,
interval quality, network and decoder stage attribution are separate metrics.
Packet loss, discontinuities and invalid observations have explicit eligibility
rules; downstream preparation or submission delay is not input lateness.

The existing `normalized_source_intrinsic_decode_gpu=1` path admits a GPU
service sample only when a blocking completion check began within 1 ms of
decoder output. A late check can include our own queue residence even when
the final wait blocks. Source mapping and input accounting share one median
of those prompt samples. Late observations use output plus that median,
bounded by observed completion; without a qualified sample they use output.
This is an accounting estimate: the worker still waits for actual readiness
before presentation. Promptly observed GPU stalls retain their full duration
for input feedback instead of being clipped to the median. No second policy
or additional user setting is introduced.

`profile.cpp` stores calibration under the cache path. The worker constructs
the key from device/session context, effective controller policy and presenter
capabilities before loading. The new key deliberately cold-starts rather than
borrowing an old policy's cached evidence. Cached history is not proof of live
coverage or physical presentation, and phase rebases and complete resets have
different lifecycle effects.
The `prompt-decode-accounting` cache marker additionally isolates both saved
startup delay and reserve history from the former queue-contaminated estimator.
Existing cache files are preserved, but their prior keys are not loaded.

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
`renderVideo()` queues `ID3D11DeviceContext4::Wait` for the captured boundary
before any copy or shader read; that GPU dependency is the correctness mechanism.
With monitored fences, `observeDecodeReadiness()` independently reports CPU
wait duration and completion evidence, taking no context lock and using its own
event with the 50 ms / fence-value-verified wait described below. The worker
records its post-observation upper bound as `decodeCompleteUs` even for short
waits and already-ready fences; the former 200 us threshold cannot determine
timestamp correctness. Unknown asynchronous readiness stays unknown in the
input graph. Production source mapping uses the observed completion boundary. Between
`78b99f1c` and 2026-09-22 this check was nonblocking, which left Windows mapped
from decoder output while Linux mapped from completion. Non-monitored fences and
shared devices remain nonblocking; there, a zero CPU decode-wait measurement
does not mean the decode dependency was already complete.

Two mutexes serialize this renderer. FFmpeg's D3D11VA lock (`m_ContextLock`)
guards the decode device's immediate context; FFmpeg holds it for each frame's
entire decode submission. A separate presentation lock guards the render
context, swapchain and prepared VRR frame against window-change callbacks. The
presentation lock includes FFmpeg's lock only when decode and render share one
immediate context. On separate devices, preparation and `Present` never hold
FFmpeg's lock; `renderVideo()` and `captureDecodeBoundary()` take it only
around decode-context `Signal`/`Wait`/`Flush`. Lock order is presentation, then
context.

Preparation binds and clears the backbuffer, renders video and overlays, and sets
colorspace/HDR state. Direct decoder texture binding follows stock Moonlight on
Intel and on separate decode/render devices. AMD/NVIDIA single-device sessions
keep the compatibility copy below 4K; 4K streams bind when the GPU has Feature
Level 11.1+ or D3D11 fences. It holds the presentation lock while manipulating
render state. Immediately after recording the frame, preparation signals and
flushes a present-ready fence, arms its event, and performs one nonblocking value
poll. It publishes the in-flight marker under the presentation lock, then
verifies completion of that exact value during preparation. The lock is released
while waiting and reacquired before checking the prepared frame, display state
and device. Successful preparation caches that exact completion proof, reports
`sourceFrameReusable=true`, and releases the presentation lock before the worker's
cadence hold. The worker may then release the source `AVFrame`: the completed
rendering marker proves that every GPU read from it has retired. The separate-
device render-to-decode fence remains an additional ordering dependency.

At the target boundary `presentAdaptive()` reacquires the presentation lock and
checks the cached proof for the exact prepared marker before calling `Present`.
It does not poll or wait for GPU completion at that boundary. Cancellation or
replacement clears the proof, and completion returning from an interrupted wait
cannot revive a discarded frame. Final presentation still revalidates the display
and device; mutex acquisition and the native Present call may themselves block.
The preparation fence wait has a 50 ms
bound, blocks on the event in 1 ms slices, and checks the fence value between
waits; the value, rather than notification delivery alone, proves readiness. A
stale notification cannot release incomplete work, and a delayed notification
cannot hold already-completed work for 50 ms. Display/resize mutation drains an
outstanding prepared fence before invalidating its backbuffer. The device-removed
sentinel is rejected. A true fence timeout or wait failure disables the adaptive
path and requests recovery; its log includes the target, final completed value,
last event result and device status.
Decode-to-render and render-to-decode Signal/Wait failures likewise abort frame
preparation and request device recovery; an unsynchronized frame is never
submitted as a fallback.
`gpu_ready_wait_result` records the aggregate fence-wait status in Win32 wait
codes; individual slice timeouts are not whole-fence failures. The trace's poll
fields describe the first poll during the completion verification in preparation,
replacing the earlier nonblocking poll after Signal. Poll and final readiness
timestamps retain conservative completion bounds. Historical captures retain
their original preparation or Present-boundary observations unchanged.

When preparation verifies completion successfully, the VRR worker records its
verified wait as `gpu_readiness_applied_us` and the next controller
decision exposes the bounded `gpu_readiness_lead_us`. The estimator uses only completed waits in
the current ten-second window, takes the configured percentile (99% in live
sessions), adds a 500 us margin, and slews toward that demand. It is a render
start opportunity, not a promise of completion: a late fence still follows the
bounded failure/recovery path, and the current source target
is never moved to hide the stall.

Fence completion proves the prepared backbuffer work has finished before Present.
If the recorded poll found the value complete, its end is the completion upper
bound; otherwise the successful preparation wait is the upper bound. A historical
final-boundary poll may overstate service because the fence could have completed
earlier during the cadence hold. Replay accepts either a preparation poll or a final poll and
anchors counterfactual upper-bound mapping according to that poll's placement.
The controller uses the resulting uncertainty conservatively for the serial-
service gate. CPU poll/event brackets are not an exact hardware timestamp or a
measurement of total GPU execution time.

### 10.2 Native Present parameters and telemetry

On the DXGI path, `D3D11VARenderer::presentAdaptive()` creates one
`DxgiPresentParameters` value from the controller's latch request and native
predecessor protection. The same value supplies native
telemetry and `presentPreparedFrame()`, which forwards it to DXGI:

- Latched: `Present(1, 0)`.
- Adaptive: `Present(0, DXGI_PRESENT_ALLOW_TEARING)`.
- Legacy: interval zero with the existing `legacyPresentFlags()` value.

The controller can omit its software spacing floor for a latched decision;
passing interval one to DXGI is therefore part of the renderer contract.
`tst_dxgipresent` exercises the actual shared call boundary using a fake
swapchain, including transitions, parameter reporting, and native result
propagation. Actual display behavior still requires Windows validation.

For a controller-planned adaptive slot, native flip protection performs at most
one `GetFrameStatistics()` query, immediately before the native call. An accepted
predecessor ID must be known and `PresentCount` must equal it; missing identity,
failed/disjoint statistics, pending work and unexpected counters synchronize.
Only a fresh successful blank observation on the same uniquely identified physical
output can admit the adaptive candidate. Active, unavailable, stale, reversed or
uncertain raster evidence also synchronizes. Protection starts with the first
adaptive request, independently of diagnostic/alignment flags. The final query
follows metadata population and optional diagnostic probes, and its full age is
limited to min(250 us, display period / 16). There is no scanline spin or
refresh-age extrapolation, and repeated observation failures never disable safety.
The native override is reported as `flip_protection_latched`; its reference stays
zero because the raster-query time is not a measured predecessor flip timestamp.
This closes the pending-only guard's unknown/active-scanout admission holes; a
fresh blank cannot reserve the phase of a later asynchronous flip.
`MOONLIGHT_VRR_SYNC_FLIPS=1` retains the explicit all-synchronized diagnostic mode.

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
image-change instant. Current schema-6 captures record
`latch_time_kind` (0 unavailable, 1 refresh reference, 2 display event).

The fallback DXGI statistics provider supplies no verified display events.
The graph separately joins its image IDs and measured refresh-count/QPC anchors
to show `Display refresh (DXGI)`. That observation does not enter verified
display-event cadence, latency learning, or adaptation.
Production uses submission estimates for its client cadence report. Matched
DXGI refresh observations populate the separately labeled graph lane;
independent-flip events from composition populate display-event timing
and present-timing diagnostics.
Readiness prediction independently adapts padding in both directions. Composition-frame statistics also lack a verified frame
display instant, so the same estimator covers periods without independent-flip
events. This lower-confidence timing remains internal telemetry; it does not
claim native display coverage or learn display-service latency from it.
Linux Wayland presentation feedback and Gamescope actual-present timestamps
are explicitly marked as display events and remain eligible for measurement.

`MOONLIGHT_VRR_ALIGN=1` adds observation-only raster probes around Present.
The independent protection guard can record a before-only sample with alignment
off. Such samples require matching protection-query and native Present brackets;
replay accepts that provenance without relaxing the paired alignment contract.
DisplayConfig signal geometry and QPC correlation support phase modeling.
`D3DKMTGetScanLine()` reports raster position around a CPU observation; it does
not establish when a queued flip became visible. Cloned/ambiguous display paths
must not be silently treated as an exact calibration match.

Software timing, tearing permission, and modeled active-scanout exposure do not
confirm an optical tear or its absence. External display measurement is needed
for that claim.

### 10.4 Composition presentation and display timing

Windows VRR sessions use waitable DXGI by default. The composition presenter
is requested only with `MOONLIGHT_VRR_COMPOSITION=1` on supported devices.
The value is captured during renderer initialization,
so a stream reconnect is required. Startup logs identify the actual presenter,
including setup fallback. Hardware support permits independent flip but does
not prove that every live present uses it.

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
are used. Buffer acquisition checks availability without waiting and returns
`S_FALSE` when all five buffers remain unavailable. Scheduled submission preserves
accepted predecessors and translates the worker's future display deadline into
QPC-derived 100 ns presentation time using a fresh clock bracket. Elapsed
deadlines request immediate presentation, without adding a period to hide
lateness. The legacy immediate overload remains available for probes.
Clock correlation is retried a bounded number of times when preemption makes
the bracket unreliable; persistent uncertainty drops that frame with `S_FALSE`
instead of treating a scheduling interruption as device loss.
`ForceVSyncInterrupt`
requests prompt statistics even with hardware flip queues. Buffer storage
is not a queue-depth target; OS/driver scheduling still needs measurement.
`CancelPresentsFrom(1)` is restricted to resize/reset, window/display epoch,
suspension, and worker shutdown rather than discarding accepted predecessors
before each new present. Retiring-fence, availability, and skipped/canceled
status evidence release the ownership accounting. Queued status alone does
not release ownership or establish display. Trace rows expose cumulative skip,
cancel, and status-overflow counts plus the latest drained status ID and value.

Only independent-flip statistics with the matching surface tag, output adapter,
source ID, and increasing present ID become display events. Their 100 ns system-relative
timestamps are correlated with scaled QPC through a fresh bracket on the worker
clock, with bounded age and uncertainty. Composition statistics do not become
display events. The backend is trace value 3; DXGI flags, query results, and raw
QPC fields remain unset. The explicit `native_synchronized_presentation` flag
keeps every controller decision latched without claiming DXGI flag switching.
The independent `native_scheduled_presentation` flag enforces a display-period
deadline floor and permits early CPU enqueue. `submission_us` records actual
native enqueue; `native_target_us` records an accepted deadline, and
`scheduling_boundary_us` is their maximum for accepted work. CPU holding occurs
after enqueue, outside the renderer lock. Failed enqueue does not reserve a
future anchor. Raw target/clock-bracket fields audit native deadline translation;
skipped/canceled counts distinguish acceptance from display. The raw
`native_displayed_duration_100ns` field is Windows' `GetPresentDuration()` value:
the driver-approved custom refresh duration, not measured image residency or
proof that the image remained visible for that long. A native display event
earlier than the requested target is diagnostic rather than capture corruption;
Windows documents the target as an attempt to display as close as possible.
Session-policy replay resolves
the current composition capabilities, but cannot predict native service timing
under a changed queue model. D3D11 calibration identity includes
`native-scheduled=1` so old scheduling history is not silently reused.
Resize replaces
buffers; display changes recreate the renderer and its output identity.

`compositionprobe --run` is an optional fullscreen Windows hardware diagnostic
for independent-flip coverage and submission-to-display latency. It does not
prove optical VRR, tear freedom, or end-to-end latency. The 2026-10-04 native
ALLYTWO probe confirmed independent-flip coverage with no rejected timestamps
or unavailable buffers. At 116 FPS/120 Hz, steady-state coverage was 464/465,
submission-to-display p50 was 7.057 ms and p99 was 15.968 ms. Its strict
below-one-refresh p99 gate failed (exit 2); native capability does not guarantee
sub-refresh latency or live VRR cadence. The 60 FPS probe also failed that gate.
These are synthetic presenter observations, not gameplay or optical validation.

Windows integration validation for this update: the incremental application
build and diagnostics build pass. All six required VRR suites, DXGI call-boundary
and presentation-clock suites pass. The overlay check passes on retry after a
stats-only asynchronous assertion in the unchanged fixture. The new composition
worker fixture populates 22 matched display intervals for 24 submissions and
passed exact replay under the then-current schema-5 tool; fresh schema-6 captures are required now. The newest
completed capture in the canonical trace roots (`20260926-131302-979`) remains
exploratory: old and new replay both exit 3 with identical fidelity results
(6,622 exact targets and tear classifications, 6,621 exact submissions). This
change has not been validated in a new live gameplay stream.

## 11. Other presentation paths

The shared presenter interface separates support checks, decode-boundary
capture/readiness, preparation, adaptive presentation, cancellation, and feedback.
Its implementations can have different acquisition and cancellation semantics.
Completion feedback may be available during preparation, deferred until the
native-present boundary, or unavailable when the backend instead retains source
ownership asynchronously. The worker treats those cases explicitly.
Do not transfer D3D11 fence or Present assumptions directly to Vulkan.

### macOS Metal presentation

An Auto VideoToolbox renderer request uses `VTMetalRenderer` when either VRR
playback or the matching startup renderer probe is requested. The probe does
not start adaptive presentation; it keeps color-range/HDR negotiation aligned
with the playback renderer. The native display helper qualifies both continuous
Adaptive-Sync ranges and discrete ProMotion ranges on macOS 12 or newer. A
requested checkbox, the 120 Hz maximum, or the existence of a Metal device
alone cannot establish a variable-refresh range. Playback also requires native
Cocoa fullscreen; a windowed or older borderless path falls back to fixed pacing.

Active Metal VRR uses a persistent `CAMetalLayer` with
`displaySyncEnabled=YES`, three drawable slots, and drawable acquisition timeout
enabled. `CAMetalDisplayLink` is bypassed only while the shared VRR worker owns
cadence. A synchronized layer can retain both its displayed drawable and a
submitted successor; the third resource lets early preparation proceed without
waiting for the compositor to retire either. The worker still prepares one
image at a time and submits at its existing target, with no additional playout
queue. Startup logging records drawable dimensions, bounds, count, and
transaction mode alongside the native display range.
Preparation acquires a drawable, encodes the existing video/color
conversion and overlays, commits the render command buffer, then observes its
completion with a 50 ms CPU wait bound. Acquisition is a separately measured
native backpressure cost; that 50 ms bound applies to command completion, not
the entire preparation call. Failed rendering or a timed-out wait requests
renderer recovery rather than submitting an incomplete image.

Each render command retains its source `AVFrame` and any CoreVideo texture-cache
wrappers until its GPU completion callback. Those references remain valid even
if the bounded wait fails and the worker/renderer is torn down. Successful
completion permits the worker to release its source before the target hold;
the prepared drawable remains owned until presentation or cancellation.
Cancellation releases an already-rendered drawable without a native submit.

At the worker's target, `presentAdaptive()` calls
`presentAfterMinimumDuration:` with the screen's native minimum refresh
interval. This constrains the previous drawable's visible duration; it is not
an added sleep or source frame period. Every VRR drawable is synchronized, so
Metal advertises native protected-slot support without switching modes for the
controller's per-frame latch request. On fixed fallback the layer returns to the
session's V-sync setting and three drawable slots, and the existing fixed
presentation/display-link path remains available. An active display change
requires renderer recreation even if both screens have the same maximum Hz,
because the native interval range and drawable epoch can differ.
Readiness calibration identity snapshots the Metal device registry ID, stable
CoreGraphics display UUID, and native minimum/maximum/granularity during main-
thread initialization. The worker therefore does not touch AppKit while using
or saving calibration history.

Metal is native backend value 4. Submission IDs are local increasing serials.
The native method returns void, so result 0 denotes accepted submission, not
proof the compositor showed the drawable. DXGI query results, tearing flags,
adapter snapshots, fence values and raw QPC fields remain unavailable. GPU-ready
fields describe the CPU-observed command-completion bracket, not an exact GPU
execution timestamp.

`addPresentedHandler:` obtains the drawable's OS-reported `presentedTime`.
A nonpositive time supplies no event. A fresh `CACurrentMediaTime()` reference
is sampled inside a `LiGetMicroseconds()` bracket; the shared clock translator
rejects future events, events older than 100 ms, and brackets wider than 500 us.
Callbacks retain only shared observation state, never a renderer pointer or
decoder surface. A later submit can report a previous serial's `DisplayEvent`
with correlation uncertainty. Shared diagnostics match that identity and keep
missing events as gaps; requested latch transitions do not reset Metal's fixed
native presentation mode. This is compositor evidence, not optical confirmation
of panel cadence, tearing, or end-to-end latency.

First external-display investigation (2026-10-04): clean-close capture
`20261004-222950-425-c89ef903-1755-412c-bf82-cf0abaaea792/Moonlight.vrrtrace`
(SHA-256 `415d1cb8f8914cafa4c3b05d4ebc7d784fd0289dc4babe1d3d76312716baee19`)
requested 4K90 PyroWave 4:4:4 10-bit on the LG TV's advertised 40-120 Hz range.
The log reported 90.19 incoming FPS, 71.02 rendered FPS, and 21.05% client pacing
drops. Recorded drawable acquisition averaged 9.01 ms, versus 1.45 ms rendering;
native display intervals were predominantly 16.67 ms. Trace sequence/footer
integrity passes and all controller targets and submissions replay exactly,
but five queue-depth semantic errors reject the strict baseline gate. This is
recorded execution evidence, not strict counterfactual A/B proof.

A host-free 600-frame native smoke at 90 FPS on the same display reproduced
401 submissions and 199 drops with two resources. Three resources produced
600 submissions and zero drops and passed exact replay. The observed display
intervals still alternated around 8.33/16.67 ms; successful submission throughput
must not be described as adaptive display-cadence success. The constant-rate
`check_metal_cadence.py` hardware gate independently checks matched OS display
events and rejects that result. The user confirmed that macOS and the TV are
already in VRR mode; asking to enable VRR again does not resolve this failure.

Follow-up native experiments explicitly requested 90 FPS through a
`CADisplayLink`, and separately passed the controller's fitted 11.111 ms source
period to `presentAfterMinimumDuration:`. Neither produced uniform 90 Hz OS
presentation events. The fitted-period run submitted all 600 frames, matched
99.65% of steady display events, but only 0.35% of consecutive intervals were
within 500 us of the requested period; acquisition rose to 4.63 ms on average.
Both experiments were reverted. A standalone Cocoa/Metal control using
`CAMetalDisplayLink`, a 90 FPS preferred range, and its supplied drawables also
received predominantly 120 Hz callbacks. A display-link preference is a
best-effort request, not proof of adaptive scanout, and its drawables reject
`presentAfterMinimumDuration:`; mixing the two APIs throws a native exception.
These experiments do not establish the root cause or justify a new production
timing policy. The remaining investigation is native presentation eligibility,
including direct versus composited presentation; optical panel validation also
remains outstanding. The resource correction must not be presented as a VRR fix.

Continuation on 2026-10-04 separated environmental throttling from native cadence.
The user observed the TV's numerical refresh rate vary toward 90 Hz in the native
control. The Metal HUD then exposed Low Power Mode, composited presentation, and
about 60 presented FPS during a 90 FPS request; the control's display events were
predominantly 16.67 ms. The user changed AC power mode to Automatic. A fullscreen
settle and a focus check were also necessary: an earlier unsettled smoke dropped
245 of 900 frames and cannot establish steady fullscreen performance.

With Automatic power, fullscreen settled, and no focus loss, the production
presenter submitted all 900 frames with zero drops and passed exact replay.
Recorded display intervals were distributed around 11.11 ms rather than a fixed
120 Hz grid. The strict constant-cadence gate still failed: 44.71% of consecutive
events were within 500 us, with 2.111 ms p95 interval error. Passing the fitted
source period to Metal also submitted all 900 frames but yielded similar cadence
(47.97%, 2.249 ms p95); that experiment was reverted. Neither gate failure alone
proves VRR is inactive, nor does zero-drop throughput prove uniform scanout.
Optical validation and an end-to-end stream remain separate from this smoke.

Initialization now logs Low Power Mode and warns when it is enabled. The advertised
`NSScreen` range remains a capability snapshot, not proof that the current power
policy permits the requested presentation rate. The native smoke logs fullscreen,
activation, focus, and power state, waits for fullscreen to settle before the
worker run, and rejects focus loss during frame delivery.

### Linux Vulkan presentation

Linux package capabilities must be qualified independently of launch/version.
The daily.183 AppImage disabled Moonlight Wayland, lacked matching Qt Wayland
plugins, and built SDL without its Wayland backend; its X11 launch test did not
qualify native feedback or HDR. The corrected packaging candidate enables all
three layers, keeps DRM/EGLFS renderer policy separate, and uses host libwayland
with host Mesa/EGL to avoid an older bundled client library hiding host symbols.
AMDGPU stable-pstate capability detection accepts distribution header layouts
while retaining required ioctl operations and the existing stream-scoped hold.
Package gates inspect actual SDL drivers, Qt plugins, feedback/HDR code and GPU
capability; isolated feedback tests and private headless Wayland startup complement
the existing X11 smoke test. They do not establish hardware HDR surface negotiation,
physical VRR, GPU state changes or calibration quality. Matched Deck runtime
validation remains required; backend/present-mode calibration caches are preserved
and must not be cross-seeded from Immediate into Mailbox.

On Linux the VRR request prefers the Vulkan frontend. The adaptive mode is
selected for the surface at startup: Mailbox on ordinary Wayland, Immediate on
X11/KMSDRM, and Immediate on Gamescope. Gamescope additionally tries Mailbox
when the dormant SteamOS experiment is enabled, according to exposed surface
capabilities.

The startup decoder probe that asks the host for a color range must use that
same Linux Vulkan preference without activating VRR presentation.
`chooseDecoder()` still forbids `enableVrr` in test-only mode; the probe passes
`preferVrrRenderer` from the session VRR snapshot instead. Otherwise an EGL
probe can request limited-range video while VRR playback interprets it as full
range, which washes out SDR. Vulkan's AMF AV1 full-range mapping override
applies only when the negotiated stream range is full. HDR remains gated on the
client's Enable HDR preference; VRR does not advertise 10-bit formats by itself.

For VAAPI hardware frames, the worker performs the explicit `vaSyncSurface()`
readiness check once and records its CPU wait. Preparation no longer repeats that
explicit synchronization; libplacebo's `AV_HWFRAME_MAP_READ` import still
validates/maps the dependency. A failed explicit VA sync disables the adaptive
path and requests renderer recovery before preparation can import or read the
surface. This removes redundant CPU serialization without treating decoder
output as proof of GPU completion.

Hardware Vulkan preparation retains the mapped `pl_frame`, including libplacebo's
AVFrame reference and imported source textures, until their GPU reads finish.
Retained hardware output is submitted asynchronously in every presentation mode:
libplacebo transitions the output image, signals a render-complete semaphore, and supplies it to
`vkQueuePresentKHR`. CPU completion is not required for this handoff. Preparation,
presentation and cancellation retire idle source mappings and preparation reports
`sourceFrameReusable=true` only when no retained mapping remains. This releases
the worker's surface before the target hold without recycling external decoder
memory while Vulkan reads it. Cancellation and failure can leave mappings pending;
they remain owned until idle or healthy-GPU teardown finishes them.

The first asynchronous output-wait bypass was withdrawn after the 2026-09-19 23:54:19
clean capture: 117.67 incoming FPS, 85.29 rendered FPS, 27.52% client drops and
zero network drops. Concurrent Vibeshine capture was reported to cause a hard
stall. Restoring the barrier did not establish the cause. The subsequent trace
proved decode-wait discard starvation, corrected in `78b99f1c`. The 00:30:01
launch then produced 76.11% client timing / 100.23 rendered FPS at 301.5 Mbps,
versus 99.67% / 117.37 FPS at 57 Mbps, both Smooth without concurrent capture.
After 20 seconds from the first arrival, high-bitrate presented frames averaged 9.559 ms of serial
decode-wait + preparation + submission service against an 8.333 ms period.

The updated path pairs asynchronous retained-hardware output and the corrected stale
policy with preparation on arrival. It retains up to four source mappings to
prevent capacity stalls, applies bounded retirement backpressure before acquiring another swapchain image, and
never calls an unavailable output-completion sample a zero-duration completion.
Immediate presentation omits that CPU completion check
when the source mapping is retained. GPU-delayed flips may still bunch despite
correctly spaced CPU submissions. Software and unretained imports keep the 50 ms / 100,000-observation output-poll bound.
D3D11 fence/event fields remain unset on Vulkan rows. Windows explicitly flushes the decoder context
after signaling its cross-device boundary, so dispatch does not depend on a later input frame.

The selected adaptive mode remains immutable for the lifetime of one persistent
swapchain. Per-frame controller requests never destroy or recreate that chain.
Persistent Mailbox provides synchronized, stale-image-replacing presentation,
so it advertises protected latch support without a native mode change or the
controller's redundant software spacing floor. Immediate retains the
display-period-plus-guard floor because it may tear. Explicit historical
revision 2 adds a 225 us entry margin to that floor; current revision 1 does not.
That historical extra spacing can reduce sustainable throughput near native refresh;
worker stale-frame replacement remains responsible for bounded backlog.
It does not turn Immediate into a tear-free native presentation mode.
A FIFO-only compatibility path likewise does not advertise
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
fall back to fixed pacing. Windows Vulkan remains rejected for VRR, and macOS
VRR uses the native Metal path above rather than Vulkan swapchain mode switching.
DRM VRR properties, compositor policy,
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

#### KWin scanout guard (2026-10-11)

On Plasma Wayland, a fullscreen stream's handoff-to-screen latency
(`latch_time_us - presenter_submission_time_us`) has a floor of about 1.4 ms
with direct scanout and about 6 ms when KWin composites. KWin 6.4.3 never sets
the presentation ZERO_COPY flag, so that floor is the evidence. A TV hotplug
left two KWin states that composite every frame until something resets them:

- A short animation effect (`fadingpopups`) stayed active forever. Its
  timeline only advances when its window is painted, and any active effect that
  does not opt out blocks direct scanout on every output.
- The atomic test that moves the HDR gamma 2.2 to PQ conversion into the CRTC
  `GAMMA_LUT` failed, so KWin fell back to a shadow buffer: an extra 4K GPU pass
  per frame that delayed PyroWave 4K 4:4:4 decode (decode wait p50 3.7 ms
  -> 8 ms, about 20% client pacing drops).

`streaming/kwinscanoutguard.cpp` runs on its own thread while a stream is
active. Every 2 s while fullscreen it reads KWin's `activeEffects` over D-Bus
and reloads a short animation effect active for 6 s, at most three times per
effect. KMS checks identify active HDR connectors with no CRTC `GAMMA_LUT`;
that can indicate hardware-color fallback. Recovery requires two consistent
fullscreen observations at least 2 s apart and consumes a single attempt per
stream, including failures or uncertain replies. Intentional color-accuracy
outputs are left alone. Unknown KMS state and output hotplug reset only the
settling observation, never a consumed retry budget.

Output discovery supports legacy `kde_output_device_v2` globals through
version 16 and `kde_output_device_registry_v2` at version 21. Initial state
must include `done`; removed outputs/modes, duplicate descriptions, stale
state and configuration failures are handled without applying stale values.
Registry-based KWin receives an output-configuration apply with no setting
setters. Legacy KWin receives only the current SDR reference value, after a
freshness check; no brightness nudge or mode/HDR/VRR/calibration setter is sent.
Source-path fixtures for KWin 6.7.3 and 6.4.3 verify that an unchanged reference
creates the change set and reaches `tryKmsColorOffloading()`. The 6.7.3 apply
path adds current DPMS state even to an empty configuration. These fixtures
verify the retry entry, not GPU acceptance. A live KWin 6.7.3 test restored a
nonzero CRTC gamma LUT at the unchanged 300-nit reference; the user confirmed
correct color and no flicker. The remaining present-timing issue was not
resolved. Hardware gamma offload alone does not establish direct scanout or
correct display cadence. An accepted configuration is logged separately from
the later KMS result.

The guard links neither libwayland-client nor libdrm (the AppImage disables
both). It loads libwayland-client at runtime. A single 2 s deadline covers the
whole short-lived output-management operation, including enumeration,
freshness and reply waits. The Flatpak needs `--talk-name=org.kde.KWin` for the
effect check. The KMS check and HDR retry use the Wayland socket and `/dev/dri`.
`MOONLIGHT_KWIN_SCANOUT_GUARD=0` disables the guard. The isolated
`tests/vrr/tst_kwinscanoutguard` fixture checks registry/legacy requests,
unchanged settings, hotplug, timeouts and the one-attempt policy under sanitizers.

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

Windows/Linux DualSense waveform feedback (updated 2026-10-05) runs separately
from video and ordinary stream audio. A Bluetooth Sony DualSense/Edge with an
exact SDL HID device path can advertise controller capability
`LI_CCAP_HAPTICS_PCM` (`0x8000`) after its output backend opens successfully.
The client also advertises `ML_FF_HAPTICS_PCM` (`0x04`) in SDP. Vibeshine captures
48 kHz S16LE actuator channels 3/4 from its virtual USB audio interface and sends
5 ms stereo blocks using encrypted control type `0x5601`, unreliable ENet
channel `0x08`. Both flags and the versioned payload are a coordinated extension
in these forks; they are not an upstream Moonlight protocol guarantee.

The session registers the waveform receive callback on both Windows and Linux
with SDL 2.24 or newer. The common library advertises the SDP flag only when
that callback is present; enabling the controller backend alone cannot enable
waveform delivery. A Linux-only session callback guard previously left Windows
controllers advertising `LI_CCAP_HAPTICS_PCM` while the connection omitted
`ML_FF_HAPTICS_PCM`, so the host discarded their PCM feedback.

The receive callback validates exact length, version, format, controller range,
reserved fields and sample count before copying into a bounded per-controller
queue. It never accesses Session/InputHandler objects that may be tearing down.
The shared worker resamples to 3 kHz signed 8-bit stereo with SDL's audio stream,
then sends SAxense-derived Bluetooth reports. Linux uses the controller's hidraw
node and verifies its kernel bus identity. Windows opens SDL's exact device
path with shared access, verifies Sony VID/PID and the Bluetooth gamepad HID
collection's report lengths, and writes through overlapped Windows HID I/O.
Output is padded to the descriptor's maximum report length while preserving the
142-byte waveform report's CRC offset. A pending write has a 40 ms completion
wait; timeout cancels and drains the operation before its buffer can be reused
or freed. This is not a hard bound on a faulty driver's cancellation completion.
Input stays with SDL; no kernel module or Bluetooth reconfiguration is added on
the client. Windows USB additionally matches the exact SDL Sony USB controller's
HID device container to its active four-channel WASAPI render endpoint. The
worker submits 48 kHz float samples to actuator channels 3/4 while keeping
channels 1/2 silent; endpoints whose mix format is not four channels are rejected.
Shared-mode conversion uses the endpoint's channel mask. Startup buffers 10 ms
(or waits at most 10 ms); the software FIFO is capped at 40 ms, separately from
the requested 20 ms engine buffer. The actual engine buffer size is queried.
Idle output drains to silence, and disconnect joins the worker before SDL closes
the controller. Linux USB and other platforms keep ordinary rumble. Native game
haptics must originate from the host's controller audio endpoint. Linux uses
its composite DS5; Windows uses the optional local usbip-win2 composite DS5,
selected per application with `dualsense-haptics` or globally with `usbip_ds5`.
The wired host identity is independent of the physical client's USB/Bluetooth
connection. USB/IP stays on the Windows host; no USB/IP transport is required
on the Moonlight client. Game soundtrack audio is not a substitute.

Bluetooth playback drops old/duplicate packets, resets conversion history on
packet loss,
bounds its input and converted queues, sends silence on underflow/idle/removal,
and joins its worker before SDL closes the controller. It cancels SDL emulated
rumble when waveform playback starts and suppresses legacy rumble while active;
LED, motion and adaptive-trigger callbacks retain their own paths. Shared
receive admission rejects late/duplicate sequences, including wrap, before
changing rumble priority; the sequence remains tracked across idle silence.
A silent PCM stream cannot start playback. Both workers idle 60 ms after the
last nonzero actuator sample, send/drain final silence, and stay inactive while
an open host audio endpoint continues to submit zeros. This prevents silent
audio from indefinitely suppressing rumble or overwriting it with silent
Bluetooth reports. Windows HID transport includes the SDK before SDL so
`CancelIoEx` remains declared when compiled separately. Write failure
stops that waveform worker and logs the need to reconnect; ordinary controller
input continues. Hardware coexistence with other applications writing the same
controller still requires physical testing. The USB backend has not been validated
on Windows hardware; Linux deterministic tests cover the shared input queue and
existing Bluetooth worker, not WASAPI endpoint discovery or physical output.

Adaptive triggers already use `SDL_GameControllerSendEffect` on Windows and
Linux, independently of PCM support. SDL owns Bluetooth framing/CRC. The shared
47-byte effect builder sets only trigger validity bits, preserving waveform,
rumble, LED and audio state. Callback admission and event consumption reject
controller indices outside 0–15; allocation and event-push failure release the
report safely. Failed SDL effect submission is logged. Trigger dispatch runs on
the input thread so controller removal cannot race its SDL handle.

The pinned SAxense source, license, research credit and adaptation notes are in
[`third-party/saxense`](third-party/saxense/PROVENANCE.md). Every binary embeds the
original/adapted covered source and both license texts, printable headlessly with
`--haptics-license`. Hardware-free validation is `tests/haptics/haptics.pro`;
passing it does not establish actual Bluetooth/game behavior. The suite runs on
Windows, Linux and macOS using a recording output and SDL's real resampler; a
virtual controller checks trigger payload dispatch. Windows CI builds the native
HID transport and runs the suite. Local 2026-09-19 validation passed on macOS
using isolated headers from the pinned common-library revision; Windows native
build, simultaneous physical waveform/trigger playback and deployment remain
unverified. The pre-existing local common-library checkout was left unchanged.

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

`MOONLIGHT_VRR_TRACE` enables a local CSV or compressed `.vrrtrace` capture;
`MOONLIGHT_VRR_DEEP_TRACE` adds diagnostic detail. The bounded producer queue
and background writer keep formatting, compression and I/O off frame delivery.
When the queue fills, diagnostic rows are dropped instead of blocking the pacer.
Capture retains at least an hour before applying the 512 MiB cap. UNC paths
are rejected so network writes do not enter the frame path. Settings recording
and the launchers manage capture lifecycle and export; see
[VRR diagnostics](docs/vrr-diagnostics.md).

The worker emits schema 6 and freezes its resolved controller parameters and
startup seed for each trace. Rows include frame identity, source timestamps,
queue lifecycle, controller decisions, preparation and submission times,
presenter feedback, and optional GPU/raster evidence. The writer footer records
sequence, integrity, write-failure and cap state. Older compressed traces remain
decodable by `scripts/decode-vrr-trace.py`, but replay rejects older schemas and
retired controller policies. Schema-2 scenario configs derive the current
session policy rather than selecting historical revision families.

An exact baseline requires a successful replay process, complete recorded
sequence, `capture.recorded_sequence_integrity_valid`, and
`fidelity.baseline_exact`. A JSON file alone is insufficient. Exact replay
checks deterministic reproduction in the recorded model; it is not an optical
measurement and cannot establish the real effects of a changed policy on the
host, network, GPU, compositor or panel. See [VRR tests](tests/vrr/README.md)
and `AGENTS.md` for capture selection, baseline and scenario workflows.

## 14. Metrics and investigation

Follow matching frame identities and source/RTP timestamps across boundaries.
Separate host source cadence, packet ingress, decode output and GPU completion,
worker queue residence, preparation, CPU submission, OS-reported display
feedback and physical scanout. A long interval alone is not a stall: an idle or
lower-rate source can produce it legitimately. CPU enqueue spacing may vary
while a native presentation queue maintains display cadence. OS reports require
matching identities and do not establish optical output.

Compare presented-interval jerk and sender-spacing fidelity together with
latency, drift, interval violations and modeled raster bounds. Input coverage is
a binary measurement and differs from the severity-weighted client timing
score; neither by itself proves visible smoothness. Fixed-admission replay can
become saturated where the live worker would shed frames, invalidating its
latency result. Use matched live A/B evidence for visible behavior.

## 15. Tests, deployment boundaries, and maintenance

For opt-in Linux bandwidth testing, [moonlight-link-test.py](scripts/moonlight-link-test.py)
redirects incoming IPv4/IPv6 traffic on a selected interface through an IFB with a bounded TBF
queue (default 1000 Mbps, 2 ms of queue service). It exercises packet delivery
and reassembly before decode; existing frame-level VRR replay cannot model this
bottleneck. It is an approximate software link, not physical gigabit timing,
and does not alter production policy. An optional host filter limits only that
host's incoming UDP. See [network-link-testing.md](docs/network-link-testing.md)
for activation, counters, limitations and removal. Isolated Linux namespace
checks cover IPv4/IPv6 setup/removal, real IPv4 UDP redirection and overflow,
host isolation, existing-rule refusal and failed-setup rollback; live gameplay
and sustained gigabit throughput remain separate validation.

The deterministic suites are
[tst_vrrtimingcontroller.cpp](tests/vrr/tst_vrrtimingcontroller.cpp),
[tst_vrrratepolicy.cpp](tests/vrr/tst_vrrratepolicy.cpp),
[tst_vrrpacingworker.cpp](tests/vrr/tst_vrrpacingworker.cpp),
[tst_vrrreplayconfig.cpp](tests/vrr/tst_vrrreplayconfig.cpp),
[tst_vrrrenderpolicy.cpp](tests/vrr/tst_vrrrenderpolicy.cpp), and
[tst_d3d11bindpolicy.cpp](tests/vrr/tst_d3d11bindpolicy.cpp).
The last covers the 4K decoder-bind vs compatibility-copy rule.
They cover timing arithmetic, timestamp wrap/rebase, cadence changes, delay and
history behavior, queue/drop/cancellation/suspension, ownership, wait floors,
trace integrity, native diagnostic modeling, and replay configuration/contracts.
Consult the test names and assertions for the specific behavior being changed;
the existence of a broad suite is not proof that a native API argument is tested.
`tst_vrrdiagnostics` additionally exercises capture scope, environment restoration,
export locking, file preservation and ZIP bounds; the independent Python ZIP
check verifies contents and CRCs. Windows CI includes that test and exact replay
of cold and warm-history worker fixtures under the unchanged production policy.

The ordinary application build does not build the opt-in replay/tests.
Follow AGENTS.md to build diagnostics separately and run all four suites plus
`vrrreplay --help` when required. Missing runtime DLLs are environment failures,
not pacing failures. The macOS native presenter smoke test exercises the actual
Metal/VideoToolbox renderer separately from the hardware-free worker fixture.
The display-range test checks range validation independently of a particular
attached display. See [Mac build and use](docs/macos-vrr-pyrowave.md) for build
commands and validation outcomes. No deterministic test here establishes optical
tearing, full host behavior, or physical A/V synchronization.

Mac validation (2026-10-04, Apple M5 Pro): 17 deterministic VRR suites and
seven CPU PyroWave/haptics suites pass. Native shared-plane regression passes
12 cases through 4K and 3024x1964 with exact Metal/Vulkan byte agreement,
exhaustion, poison/reuse and retained-lifetime checks. The native calibration
smoke passes 288 draws across eight format/resolution combinations through
1080p into a 1440p target, including grayscale/color/alpha and matrix checks.
An actual encode/framing/asynchronous shared-decode-to-Metal smoke presents
128x96 4:2:0 8-bit and 4:4:4 10-bit patterns adaptively, including cancellation,
source recycling and decoder teardown. A 4:4:4 10-bit shared frame also passes
the fixed/display-link path.
Its native worker fixture submits 100 synthetic frames at 116 FPS with no worker
drops and 97 matched native display intervals. The then-current schema-5 trace
passed exact replay, as did success/failed-preparation Metal feedback fixtures;
11 corrupted-evidence checks remain rejected. These validate native ownership,
rendering and diagnostic contracts without a live host, network or game.
External-display behavior, sustained live throughput and physical display/A/V
results remain unmeasured. Runtime evidence here is arm64; the PyroWave static
library also cross-compiles for x86_64, without an Intel runtime test.

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
- Keep live policy, fallback behavior, historical notes, and optional
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
