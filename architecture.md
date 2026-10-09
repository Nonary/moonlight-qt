# Streaming, VRR, and timing architecture

This is the technical orientation for this fork. Read it before working on streaming,
decoding, rendering, VRR, latency, or replay. It describes source contracts and
measurement boundaries; deployment and live behavior require separate verification.

Source audit: October 9, 2026, main repository
`0becddd0ba37a018b542fd201d37e77d18dde61d`. The inspected common-library checkout is
`9ab994975acba22423c819c0c441bf4a0c979856`; the main repository records
`9348def91b5bbe9f21ba5af4aa11fcadc85b5875` instead. This pre-existing checkout
discrepancy affects calibrated pacing negotiation in sections 3.3–3.4 and PyroWave tail
expiry in section 4. The application already uses the newer
`STREAM_CONFIGURATION::pyrowavePaceMbps` field, which the inspected older common-library
header lacks. These revisions must be distinguished when reading source or identifying a
build; the inspected checkout does not supply that required application interface.

The numbered sections describe current implementation, separating production selection
from retained replay compatibility and opt-in experiments. Use the focused documents in
`docs/` for dated investigation history and [client timing
troubleshooting](docs/client-timing-troubleshooting.md) for live symptom analysis.

## 1. Fundamental model

Moonlight is the streaming client. The host captures and encodes video, sends compressed
frames, and receives input. On the client, the video receive path decrypts packets when
enabled, orders and repairs them, and assembles codec frames. The decoder produces
frames for a renderer and presenter. Audio and input follow separate queues and timing
paths.

VRR changes when decoded frames are rendered and submitted. It cannot recover a frame
that the host did not produce, undo packet loss, or remove time already spent in
capture, encoding, transport, and decode. The controller can absorb some timing
variation by adjusting playout delay, within configured delay and queue bounds.
Submission time, GPU readiness, the native presentation call, OS presentation feedback,
and physical scanout are distinct events; the client does not directly observe optical
scanout.

```text
Host capture / encode                                      [outside client]
    |
    | UDP video: RTP timestamp + NV frame/packet identity + FEC
    v
VideoReceiveThreadProc -> RtpvAddPacket -> processRtpPayload
    | packet ordering, FEC repair, access-unit/frame assembly
    v
Bounded compressed decode-unit queue
    |
    +-- FFmpeg codecs -> FFmpeg decoder thread -> AVFrame
    |
    +-- PyroWave -> framed wavelet decoder -> AVFrame / shared GPU planes
    v
Renderer and pacer selection
    +-- legacy queues / V-sync path
    +-- VrrPacingWorker: bounded decoded-frame queue
          -> establish decode dependency and observe completion where available
          -> map source timestamp to a source slot and target
          -> choose render-start deadline; discard stale work when appropriate
          -> prepare rendering and establish completion/ownership state
          -> wait for target and applicable submission floor
          -> recheck lifecycle and deferred readiness; present adaptively
          -> native/OS feedback informs later decisions
          -> asynchronous trace writer

Audio UDP -> audio RTP queue -> Opus -> audio-device queue
SDL input events -> input queue / sender -> host
```

The diagram describes the main path, not a guarantee that every codec/backend uses the
same GPU synchronization or presentation feedback. PyroWave reuses the shared
decoder/pacer interfaces but decodes its independent wavelet frames with its own Vulkan
decoder; supported platform renderers can share GPU surfaces.

## 2. Source map and reading order

Paths are relative to the repository. `moonlight-common-c` is a nested submodule: its
outer directory contains the qmake wrapper and its inner directory contains the common
library. This audit used initialized common-library checkout `9ab9949`; the parent
repository currently records gitlink `9348def`. Sections 3–4 call out the behavior and
interface differences that affect this pipeline.

| Area | Source and entry points |
| --- | --- |
| User preferences | [streamingpreferences.cpp](app/settings/streamingpreferences.cpp): `reload()`, `save()`; [SettingsView.qml](app/gui/SettingsView.qml), [VrrTimingSettings.qml](app/gui/VrrTimingSettings.qml), [vrrtimingoptions.h](app/settings/vrrtimingoptions.h) |
| Session orchestration and resolved display policy | [session.cpp](app/streaming/session.cpp): `snapshotPresentationSettings()`, `initialize()`, `drSubmitDecodeUnit()`, stream event loop |
| FPS recommendations | [vrrratepolicy.cpp](app/streaming/vrrratepolicy.cpp) |
| Host launch and protocol configuration | [nvhttp.cpp](app/backend/nvhttp.cpp): `startApp()`; [Limelight.h](moonlight-common-c/moonlight-common-c/src/Limelight.h), [SdpGenerator.c](moonlight-common-c/moonlight-common-c/src/SdpGenerator.c), [RtspConnection.c](moonlight-common-c/moonlight-common-c/src/RtspConnection.c) |
| Host frame-limiter discovery | [framelimitercapabilities.h](app/backend/framelimitercapabilities.h), [nvcomputer.cpp](app/backend/nvcomputer.cpp) |
| Packet ingress | [VideoStream.c](moonlight-common-c/moonlight-common-c/src/VideoStream.c): `VideoReceiveThreadProc()`; [Video.h](moonlight-common-c/moonlight-common-c/src/Video.h) |
| Packet ordering, FEC and reassembly | [RtpVideoQueue.c](moonlight-common-c/moonlight-common-c/src/RtpVideoQueue.c): `RtpvAddPacket()`; [VideoDepacketizer.c](moonlight-common-c/moonlight-common-c/src/VideoDepacketizer.c): `processRtpPayload()`, `requestDecoderRefresh()` |
| FFmpeg decoder and shared PyroWave wrapper | [ffmpeg.cpp](app/streaming/video/ffmpeg.cpp): `ffGetFormat()`, `submitDecodeUnit()`, decoder thread; [ffmpeg.h](app/streaming/video/ffmpeg.h) |
| PyroWave framing and decode | [pyrowaveframing.cpp](app/streaming/video/pyrowave/pyrowaveframing.cpp), [pyrowavedecoder.cpp](app/streaming/video/pyrowave/pyrowavedecoder.cpp) |
| PyroWave bitrate, bandwidth and calibration | [pyrowavebitrate.h](app/streaming/video/pyrowave/pyrowavebitrate.h), [pyrowavebandwidth.h](app/streaming/video/pyrowave/pyrowavebandwidth.h), [pyrowavecalibrationpolicy.h](app/streaming/video/pyrowave/pyrowavecalibrationpolicy.h), [pyrowavecalibrator.cpp](app/streaming/video/pyrowave/pyrowavecalibrator.cpp), [pyrowavelinkpolicy.h](app/streaming/video/pyrowave/pyrowavelinkpolicy.h); paired-host probe orchestration in [nvhttp.cpp](app/backend/nvhttp.cpp) and UDP transport in [pyrowaveudpprobe.cpp](app/backend/pyrowaveudpprobe.cpp) |
| Renderer abstraction and pacer selection | [renderer.h](app/streaming/video/ffmpeg-renderers/renderer.h), [pacer.cpp](app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp) |
| Frame and presenter contracts | [vrrtypes.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtypes.h), [ivrrframepresenter.h](app/streaming/video/ffmpeg-renderers/ivrrframepresenter.h) |
| VRR execution and tracing | [vrrpacingworker.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.cpp) |
| Deadline waiting | [vrrtargetwaiter.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtargetwaiter.cpp) |
| Timing policy and learning | [vrrtimingcontroller.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.cpp), [vrrtimingcontroller.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h), [prediction.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/prediction.h), [reserve.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/reserve.h), [smoothnessfeedback.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/smoothnessfeedback.h) |
| Live interval buffer | [intervalbuffer.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/intervalbuffer.h) |
| Calibration persistence | [profile.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/profile.cpp) |
| Windows presentation | [d3d11va.cpp](app/streaming/video/ffmpeg-renderers/d3d11va.cpp), [d3d11composition.cpp](app/streaming/video/ffmpeg-renderers/d3d11composition.cpp) |
| macOS presentation and display range | [vt_metal.mm](app/streaming/video/ffmpeg-renderers/vt_metal.mm), [macdisplaytiming.mm](app/streaming/video/ffmpeg-renderers/macdisplaytiming.mm) |
| Linux Vulkan presentation and feedback | [plvk.cpp](app/streaming/video/ffmpeg-renderers/plvk.cpp), [plvkpresentation.h](app/streaming/video/ffmpeg-renderers/plvkpresentation.h), [wayland.cpp](app/streaming/video/ffmpeg-renderers/waylandfeedback/wayland.cpp), [vulkantiming.cpp](app/streaming/video/ffmpeg-renderers/vulkantiming.cpp) |
| Audio transport, decode, and device queues | [AudioStream.c](moonlight-common-c/moonlight-common-c/src/AudioStream.c), [RtpAudioQueue.c](moonlight-common-c/moonlight-common-c/src/RtpAudioQueue.c), [audio.cpp](app/streaming/audio/audio.cpp), [sdlaud.cpp](app/streaming/audio/renderers/sdlaud.cpp) |
| Input transport and DualSense waveform output | [InputStream.c](moonlight-common-c/moonlight-common-c/src/InputStream.c), [gamepad.cpp](app/streaming/input/gamepad.cpp), [dualsensehaptics.cpp](app/streaming/input/dualsensehaptics.cpp), [dualsensehid.cpp](app/streaming/input/dualsensehid.cpp) |
| Replay and its contract | [vrrreplay.cpp](tests/vrr/vrrreplay.cpp), [VRR test README](tests/vrr/README.md) |
| Statistics | [decoder.h](app/streaming/video/decoder.h): `VIDEO_STATS`; overlay formatting in `ffmpeg.cpp` |
| Test registration and CI | [tests.pro](tests/tests.pro), [vrr.pro](tests/vrr/vrr.pro), [Windows/macOS workflow](.github/workflows/build-win-mac.yml) |

For a timing change, start with the settings resolved for the session, follow
`VrrPacingWorker` through `schedule()` to the selected presenter, and trace the feedback
path back into the controller. Read replay afterward: it consumes recorded events and
does not reproduce packet ingress or live GPU behavior.

## 3. Settings, negotiation, and mode selection

### 3.1 Preferences are not proof of an active mode

`StreamingPreferences` persists user choices through `QSettings`. New profiles default
to V-sync on, VRR off, Balanced Target VRR timing, Reduce judder on, the moderate
cadence smoother on, legacy frame pacing off, and a 720p60 stream. Existing saved values
and migrations can change these settings; defaults do not identify the current profile.
`smoothvrrframetiming` is still the persisted key for the cadence-smoothing choice.

The FPS picker recommends rates; it does not activate VRR. It always offers 30 and 60
FPS, native refresh rates, and a saved custom rate. With both V-sync and VRR selected,
it additionally offers a conservative VRR rate `floor(refresh - refresh^2 / 3600)` and a
low-latency rate `floor(refresh / 6) * 5`. Native rates retain their ordinary fixed-rate
label when recommendations collide, and saved FPS is not rewritten when VRR is toggled.

At session start, `snapshotPresentationSettings()` snapshots the actual window's display
refresh and resolves effective presentation settings. If the refresh is unavailable,
legacy pacing may assume 60 Hz, but that fallback cannot qualify VRR. A requested FPS
more than 5 above display refresh disables effective V-sync. VRR requires a readable
refresh, effective V-sync, and stream FPS at or below the display maximum. On macOS it
also requires a native display range with variable refresh; the refresh lookup uses the
actual window's screen and native maximum rate. If qualified, the session forces desktop
fullscreen without overwriting the saved window preference. macOS requests native
fullscreen/Spaces before SDL initializes; its presenter also checks that the window
entered native fullscreen. If a display move or refresh change makes the snapshotted
timing invalid, VRR is disabled for that connection and the renderer is recreated on the
fixed-pacing path.

Renderer capability is a separate gate. The pacer constructs the VRR session
configuration, disallows an additional queued frame, passes the selected smoothing
options, checks presenter support, and starts the worker. Unsupported presentation or
failed worker initialization falls back to the legacy path. A checkbox or a qualified
session does not prove that adaptive native presentation succeeded or that the physical
panel changed refresh.

Diagnostic frame tracing is off by default. Its setting records diagnostic frames and
provides export/open actions for completed recordings; it does not change VRR timing
policy. The session owns its environment wrapper for the active stream and restores it
after decoder shutdown and logger drain. Existing external trace launchers take
precedence. See [VRR diagnostics](docs/vrr-diagnostics.md) for capture and lifecycle
details.

### 3.2 Host frame-limiter discovery

The client reads optional `/serverinfo` values `FrameLimiterSupported`,
`FrameLimiterEnabled`, `VirtualDisplayFrameLimiterEnabled`, and
`FrameLimiterFpsLimitMilliHz`. These are host-reported configuration and capability
fields, not per-game confirmation that a limiter applied or overrode another display or
application policy. Missing fields mean no advertised integration, and refreshed host
data replaces old capability state. The FPS limit value of zero means follow the stream
rate.

In Vibeshine's implementation (checked at `bfa42e2fa1b2e2f92dcecef3b2514f5e3eb277cc`),
`FrameLimiterEnabled` covers the configured display path: its manual limiter with an
applicable provider, or automatic virtual-display limiting when that mode is selected.
Linux additionally requires a selected Linux provider. The separate
virtual-display flag reports automatic-policy availability, not activation on physical
outputs. This discovery is advisory; VRR choices are available without a host limiter.

### 3.3 What the host is told

Session setup fills `STREAM_CONFIGURATION` with the requested FPS, dimensions, bitrate,
color format, encryption, supported codec formats, and other connection choices.
`clientRefreshRateX100` is defined as a display-refresh hint in hundredths of a hertz;
common-c serializes it as `x-nv-video[0].clientRefreshRateX100` when that SDP attribute is
emitted. The application never assigns it after `LiInitializeStreamConfiguration()`
zeroes the structure, so it currently carries zero rather than the snapshotted display
refresh. RTSP negotiation and host codec capabilities select the actual format and
profile. Packet size is aligned for FEC, with
route-dependent limits applied during connection setup; these are transport settings,
not VRR scheduling inputs.

When the session snapshot qualifies VRR, the launch/resume request includes
`clientVrrRequested=1`. Hosts that do not recognize the optional parameter can ignore
it. A compatible host may use it to choose more precise capture timing; the client code
only establishes what it requests, not the host's capture, timestamp, pacing, or
encoding behavior. The refresh hint is not clock synchronization.

PyroWave is user-selected and is never auto-selected. The client advertises its PyroWave
formats and, when the host reports support, RTSP negotiation selects the best mutually
supported profile in HDR 4:4:4, HDR 4:2:0, SDR 4:4:4, SDR 4:2:0 order. Its SDP
advertises record framing and disables the optional adaptive FEC and bitrate modes.
Current client sessions do not request the optional compression feature: old compression
settings are removed on load/save and the stream configuration defaults it off.
Compression version/feature parsing and framing support remain in shared code for
wire-compatibility and tests, not as active production negotiation. See [PyroWave
protocol](docs/pyrowave-protocol.md).

PyroWave's optional calibration is separate from VRR timing. It probes link capacity
and, when the host advertises paced UDP probes and link information is available, tests
complete frames at stream cadence to choose a loss-bounded frame-send pace. Moonlight
saves that pace in user preferences. At session start it recomputes the routed
wired-client link estimate and assigns it alongside the saved pace to the stream
configuration. There is a source seam in this checkout: `Session` assigns
`pyrowavePaceMbps`, but checked-out common-c `9ab9949` has no such
`STREAM_CONFIGURATION` field or SDP attribute. The parent gitlink `9348def` adds both;
calibrated pace reaches the host only
with that matching common-c interface. A client-side calibration result alone does not
prove streaming performance or visual smoothness.

### 3.4 PyroWave bitrate and calibration

The default PyroWave image bitrate follows the codec author's 35 dB quality regression,
including its HDR allowance, and is rounded to 5 Mbps increments. The calibration UI is
available when PyroWave is selected. Calibration runs outside an active stream and
requires a paired, online PyroWave-capable host. Its first step uses an authenticated
paired-host
UDP probe to find and freshly confirm a usable wire-rate budget. This is total transport
capacity: the conversion reserves packet, IP/UDP/RTP and encryption overhead, frame
headers, critical FEC parity, and an audio/control allowance. The Minimum and
Recommended targets refer to image bitrate; Moderate starts from 60% of the slower known
endpoint link but cannot go below Recommended wire cost or above the measured budget.
Maximum uses the full confirmed budget.

The second step measures local decode and rendering cost across its resolution, chroma,
and HDR test matrix at the selected stream FPS, against the wire budget for each
combination. It uses a synthetic frame and ordered GPU work; it does not measure
host encoding, compositor scheduling, display presentation, or physical scanout. Probe
accounting reserves a full frame period on Linux, while Windows and macOS use a
conservative 75% period share. Network capacity and local device service time are
separate limits, and the reported choice is the lower feasible image rate. A calibrated
burst send pace is tested separately from the capacity and device probes. It requires a
host advertising paced UDP probes. The session code stores and assigns that value, but
it reaches SDP only when common-c provides the newer `pyrowavePaceMbps` field described
above; the checked-out 9ab interface does not. Calibration grades are useful connection
guidance, not proof of smooth live gameplay.

## 4. Packet ingress, frame assembly, and recovery

`VideoReceiveThreadProc()` reads UDP datagrams into staging buffers, decrypts AES-GCM
video when negotiated, converts RTP sequence and timestamp fields to host byte order,
and submits packets to `RtpvAddPacket()`. The RTP timestamp is in a 90 kHz clock domain.
The NV video header supplies stream-packet and frame identity, frame-boundary/picture
flags, and FEC metadata. The receive buffer and queue absorb packet bursts and maintain
compressed-frame assembly and recovery state. PyroWave partial-frame expiry can be
bounded by a VRR-derived on-time reassembly deadline, as described below.

`RtpVideoQueue` groups packets by frame and FEC block, tracks sequence gaps, and uses
Reed-Solomon parity to recover missing data when possible. Ordered data packets are
passed to the depacketizer. Unrecoverable gaps may drop a compressed frame, advance
recovery, and report loss to the host through the control path. FEC status and
frame-loss notifications are not individual RTP packet retransmissions.

For ordinary H.264/HEVC streams, `processRtpPayload()` checks frame and stream packet
continuity, strips transport headers, identifies frame boundaries and types, and builds
codec access units from Annex-B NAL units. IDR setup carries codec parameter sets. AV1
uses its picture-data path. The queue samples one receive time at the start of each FEC
block and reuses it for that block's packets; it does not take a clock sample for every
packet. The depacketizer preserves RTP timestamp, frame identity, host processing
latency, and packet-loss metadata in the resulting `DECODE_UNIT`.

PyroWave frames are intra-coded and independent. The client accepts the current record
framing and retains parsing support for legacy length-prefixed frames. Record framing
identifies packet segments and critical data. For record-framed PyroWave only, an
incomplete final FEC block can be delivered with missing packet segments marked as lost
when the complete critical prefix is present. The receive queue waits for the final data
packet before applying its short reorder deadline, and may shorten that interval using
the VRR worker's per-frame on-time deadline. Missing critical data, parity-bearing
blocks, and unknown framing continue through ordinary boundary/recovery handling. In
checked-out common-c `9ab9949`, the deadline is the later of the mapped on-time deadline
and 250 us after the latest unique packet, capped by the ordinary 1 ms silence window.
An absent final packet waits for completion or a successor frame boundary. This differs
from the parent repository's common-c gitlink `9348def`, which adds absent-tail expiry
with longer 3 ms/1.2 ms windows. The common-c revision being built determines this
behavior. The decoder can render a partial record-framed frame with blur when the
coarsest wavelet level is intact; losing that critical level rejects the frame. Legacy
length-prefixed frames reject packet loss. A rejected PyroWave frame does not require an
IDR request because the next frame is independent.

After scheduling, the VRR worker publishes the raw on-time reassembly anchor `sourceTime
+ playoutDelay - p95(reassembly-to-completion minus decode hold) - 250 us` via
[receivedeadline.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/receivedeadline.h)
and `LiSetVideoReassemblyDeadlineCallback()`. The duration history uses 128 samples and
needs 32. Each sample starts at reassembly completion plus the intentional decode hold,
so the hold is subtracted while any other pre-submit/queue time remains included.
Only completion-qualified samples enter the history: completion must advance beyond
decoder output, or decoder output itself must be marked complete. Durations exceeding
a source period are excluded.
The packed atomic anchor is extrapolated at the 90 kHz RTP rate only within one second
and cleared when timestamp playout stops or the worker shuts down. The receiver drains
buffered datagrams before expiring a deadline and uses a short final poll for precise
release. This changes packet-assembly timing, outside controller replay.

Non-direct operation uses a bounded queue of 15 decode units. Queue overflow flushes
pending compressed frames and requests IDR recovery rather than allowing latency to grow
without bound. Direct-submit decoders can instead consume units on the
receive/depacketizer path. The FFmpeg decoder advertises pull-renderer capability and
owns a decoder thread; PyroWave passes through the shared wrapper but uses its own
decoder. Arbitrarily dropping a compressed reference frame is not equivalent to
discarding an already-decoded presentation frame. Invalid continuity can drop a frame
and trigger IDR or reference-frame invalidation recovery according to capabilities;
repeated drops eventually force IDR recovery. `requestDecoderRefresh()` flushes queued
units and defers assembly-state reset to a safe boundary. Successful IDR completion
establishes valid reference state.

## 5. Clock domains and latency boundaries

| Value | Domain / units | Meaning and limitation |
| --- | --- | --- |
| RTP timestamp | Host-origin 90 kHz counter | Identifies source timing; wraps and must be unwrapped. It is not a client wall-clock timestamp. |
| Source presentation time | Microseconds after conversion and client-clock mapping | VRR maps the unwrapped RTP timeline onto `LiGetMicroseconds()` using `decodeCompleteUs`. That field starts at decoder output and advances only when a later completion observation is recorded; the mapping can absorb observed asynchronous decode time, but is not universally anchored to GPU completion. The decoder-output origin remains separately available for full client-processing age. |
| `receiveTimeUs` | Client monotonic microseconds | First packet arrival for the frame. Not host capture time. |
| `enqueueTimeUs` | Client monotonic microseconds | Completed frame reassembly / enqueue to decoder. |
| `decodeSubmitUs` | Client monotonic microseconds | Sampled before FFmpeg packet submission (or PyroWave dispatch). |
| `decoderOutputUs` | Client monotonic microseconds | Captured immediately when FFmpeg exposes a decoded frame. Immutable origin for full client-processing reporting. Asynchronous PyroWave output may still be in flight. |
| `decodeCompleteUs` | Client monotonic microseconds | Source-mapping bound initialized to `decoderOutputUs`. An explicit worker wait over 200 microseconds advances it to a fresh post-wait sample; prepared-ahead frames can advance it from their recorded decode-ready time. A zero/short wait may mean already complete or no CPU blocking, but can leave this timestamp at decoder output rather than GPU completion. |
| `decodeSyncWaitUs` | Elapsed client microseconds | CPU time the pacing worker spent waiting on a decode/backend completion primitive. This is serial service, not a timestamp; zero can mean an asynchronous GPU dependency. |
| Queue, decision, preparation, target-wait, and submission times | Client monotonic microseconds | Distinct worker lifecycle boundaries. Do not conflate planned deadlines with actual wake or native-call times. |
| Shared fence values | GPU ordering identities | Establish dependencies and completion, not elapsed time by themselves. |
| DXGI QPC data | QPC ticks plus frequency/correlation | Native timing evidence requiring identity and clock mapping. |
| Metal `presentedTime` | Core Animation seconds | OS-reported drawable presentation event, correlated with `CACurrentMediaTime()`; not native call/return time or physical panel measurement. |
| Host processing latency | Host-reported 1/10 ms units | Aggregate when present; zero means unavailable or inapplicable. |
| Audio samples | Audio stream/device cadence | Independent of video target scheduling. |
| Reserve internal time | Nanoseconds | Convert explicitly at controller/model boundaries. |

`LiGetMicroseconds()` supplies the client monotonic clock used across decoder, queue,
controller, and worker timestamps. Its epoch is local and it is not synchronized
automatically to the host, GPU, audio hardware, or panel.

Useful differences, after checking validity and ordering, are:

```text
assembly               = enqueueTimeUs - receiveTimeUs
pre-submit             = decodeSubmitUs - enqueueTimeUs
decoder-output latency = decoderOutputUs - decodeSubmitUs
post-decode            = presentationCallEndUs - decoderOutputUs
client ingress         = submissionTimeUs - receiveTimeUs
client processing      = presentationCallEndUs - decoderOutputUs
rendering              = preparationDurationUs + presentationCallDurationUs
                       + preparedAheadRenderUs
queue/pacing           = max(0, client processing - rendering - accountedDecodeWaitUs)
```

Decoder-output latency is CPU-observed; shared PyroWave output can still have GPU decode
work in flight at that boundary, so it is not GPU execution duration.
`preparedAheadRenderUs` is zero on the ordinary path; prepared-ahead frames add the
offscreen stage's `max(0, readyUs - renderStartUs)` to both rendering and preparation
totals. Their accounted decode wait comes from that stage's `decodeWaitUs`, although the
pacing-thread trace's `decodeSyncWaitUs` stays zero; ordinary frames use the worker's
explicit decode wait. The timing accumulator bounds rendering by client-processing time
and decode wait by the remaining time so the queue/pacing remainder cannot be negative.
The post-decode interval includes queue residence, GPU dependencies, preparation,
scheduler delay, intentional pacing, and native submission. It is not the configured
playout delay. Queue/pacing removes only measured preparation, present-call, and
explicit CPU decode-wait durations; asynchronous GPU work is not exposed as a CPU wait.
The performance overlay reports frame queue delay and rendering time rather than a
distinct client-processing row. Successful presentation durations share one frame
denominator; failed and cancelled frames retain outcome diagnostics but do not enter
those paired totals.

With Windows monitored fences, the worker can wait for decoder completion before
scheduling and reports that CPU wait separately; the queued GPU decode-to-render
dependency remains distinct. On Linux, VAAPI synchronization can similarly produce an
explicit wait, while asynchronous Vulkan Mailbox/shared output may not provide a CPU
output-completion sample. Presenter-side present-ready waits can occur inside the native
presentation call and are not decode-wait time. These accounting boundaries describe
observed CPU work, not every GPU stage. None alone measures click-to-photon or
glass-to-glass latency. RTT is round-trip, not one-way video delay.

The controller retains source periods in Q16 fixed point where required; do not round
them to milliseconds when reasoning about long-run drift. Follow RTP unwrap, conversion,
and client-clock mapping at the specific use site.

## 6. Decoder ownership and renderer handoff

### 6.1 PyroWave handoff

PyroWave uses the `FFmpegVideoDecoder` thread and frame queues, but bypasses FFmpeg
codec send/receive: each independent frame is parsed and decoded by `PyroWaveDecoder`;
rejected frames need no IDR recovery. Incomplete packet framing supports partial decode
only when the coarse wavelet level is intact. Ordinary transport is the production
setting; independent LZ4 detail-group support remains in the parser/decoder for
compatibility and tests, but current production sessions leave it disabled.

On Windows, Vulkan decode writes three shared plane textures created by the D3D11
renderer. A per-frame decode-fence value lets rendering wait for that frame; after its
final read, D3D11 signals a release-fence value. The AVFrame reference retains the
surface identity and returns it to the pool with that release value when the last
reference is freed. The decoder waits for release before overwriting it. On Linux, the
decoder can use the renderer's same Vulkan device and libplacebo plane textures:
ready/done timeline semaphore values order decode and renderer reads, while AVFrame
ownership pins the pool surface. If shared-device output is unavailable and not
required, PyroWave uses a private Vulkan device and synchronous CPU readback into a
planar AVFrame. The macOS path also shares Vulkan planes with Metal and retains the
source until Metal sampling completes; its detailed presenter contract is in section 11.

Shared-surface decode returns after GPU submission, not completion. The decoder carries
frame-specific surface/fence identity in AVFrame buffer ownership; the presenter waits
on or queues that exact dependency and releases the frame only after its final read is
safe. The synchronous readback fallback returns complete CPU planes. Do not infer
completion from frame availability alone.

The worker also publishes a bounded decode-hold window through
[receivedeadline.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/receivedeadline.h).
Once decode-GPU and Present-call duration histories qualify, a PyroWave decode can wait
until the current Present returns to avoid competing with it. A hold requires a known
latest decode-start bound that leaves time for both decode and preparation; it ends on
window closure or after at most 4 ms. Its duration is recorded separately and excluded
from receive-deadline cost learning. It is scheduling allowance, not a decode-completion
fence.

### 6.2 Ordinary FFmpeg codecs

For ordinary codecs, the decoder owns a dedicated thread after renderer setup. When no
submitted packets await output, it blocks in `LiWaitForNextVideoFrame()`. When work is
in flight, it drains FFmpeg output and polls/submits more compressed input as needed.
This fork's steady-state path pulls from the common-library queue, but FFmpeg still
receives packets via `avcodec_send_packet()` and returns frames via
`avcodec_receive_frame()`; codecs may work in either call. The first submitted frame
must be an IDR. Frame-number gaps are counted, the `LENTRY` chain is copied into a
reusable packet buffer, and accepted packet metadata and submit times are kept in
matching queues. Compressed payload pointers expire after `LiCompleteVideoFrame()` and
are never frame storage.

For each FFmpeg output frame, the decoder captures immutable `decoderOutputUs` at the
receive boundary, associates it with queued metadata, and passes frame number, RTP
timestamp/validity, first receive time, reassembly time, and decode-submit time into
`PacedFrame`. Legacy pacing keeps RTP in `frame->pts` and decoder-output client-clock
time in `pkt_dts`; use explicit VRR fields for VRR timing. `ffGetFormat()` selects the
renderer-compatible format and rejects an incompatible ordinary FFmpeg fallback.
Hardware decode can keep surfaces on the GPU, so an available `AVFrame` does not
establish completion of all GPU reads or writes.

On Linux builds with VAAPI, [main.cpp](app/main.cpp) requests Mesa low-latency decode
before Qt/SDL can initialize graphics. It appends `lowlatencydec` to process-local
`AMD_DEBUG`, preserves existing flags, and uses `R600_DEBUG` when `AMD_DEBUG` is unset.
`MOONLIGHT_AMD_LOW_LATENCY_DECODE=0` disables the automatic addition. This is distinct
from FFmpeg LOW_DELAY and does not bypass synchronization; the startup log records a
request, not driver acceptance or measured benefit.

### 6.3 Shared worker ownership

At VRR handoff, D3D11 can capture a per-frame decode-fence boundary before later decoder
GPU work can be queued, so the dependency does not accidentally include subsequent
frames. Other presenters may return no generic boundary and identify readiness from the
frame's own surface/fence/semaphore in `waitForDecode()` or GPU preparation. The worker
may wait, or the backend may queue a GPU-side dependency without CPU blocking.
`decodeCompleteUs` records a later completion observation when available; immutable
`decoderOutputUs` remains the full-latency origin.

`VrrPrepareResult::sourceFrameReusable` is true only after every backend GPU read from
the decoder surface has completed. The worker may then free the `AVFrame` before waiting
for the presentation target, preventing high-rate pacing from exhausting the decoder
surface pool. Otherwise, the frame remains owned until the worker defers/releases it or
backend source retirement proves it safe. Vulkan shared source mappings may outlive a
worker step. On preparation failure, cancellation, interruption, or normal completion,
the worker records the outcome and releases ownership only under the backend contract.
Never release a decoder surface or native image while GPU work can still reference it.

`Session::drSubmitDecodeUnit()` try-locks decoder lifetime because destruction has
main-thread/API constraints. Units can be ignored while that lock is held;
renderer/decoder recreation uses refresh recovery. The FFmpeg pull path remains the
normal steady state for ordinary codecs.

The legacy pacer bounds its queues and defers freeing rendered frames while GPU work can
use them. VRR replaces legacy pacing with its worker and explicit presenter contract;
network assembly and codec reference handling remain separate. FFmpeg hardware-surface
allowance includes the VRR queue's extra admission (`VrrLargestQueuedFrames -
VrrMaximumQueuedFrames`). The compressed 15-unit queue, decoded-frame queue, and native
swapchain buffers have different owners and service rates; do not add their capacities
as one fixed latency.

## 7. VRR worker: queue, execution, and lifecycle

### 7.1 Queue ownership, capacity, and stale work

The current production VRR queue admits four waiting decoded frames plus one active
frame. The controller profile's playout delay is separate from this admission limit;
`playoutQueueLimitUs()` also bounds the delay against source period, render lead, and
policy allowance. Decoder surfaces may remain referenced after the active worker step,
but such references do not increase queue admission. The compressed input queue and
native swapchain images are separate queues with separate lifetimes.

`submit()` captures the decoder boundary before later decode work is queued. Under the
queue mutex, a stopped or suspended worker rejects incoming frames; when full, it evicts
the oldest waiting frame and admits the fresher one. Capacity evictions set a coalesced
pending discontinuity bit, attached to the next dequeued frame's trace row; multiple
evictions before dequeue coalesce. The controller ignores that bit because local queue
omission is not a source-clock epoch change. Ticket
creation/admission is bounded; dropped-frame trace and counter work runs after releasing
the queue lock.

Before it dequeues a frame and enters a potentially blocking decode wait, the worker
prunes expired queue fronts only when a newer adjacent source frame is present. It
requires valid consecutive frame numbers and plausible RTP spacing, uses two periods of
the greater of fitted cadence and successor spacing (four for metronome mode), and
protects the configured playout delay. It skips pruning on a pending rebase. Such rows
are terminal `queue_stale` outcomes and do not advance the controller. A sole late image
remains eligible; queue pruning is not permission to discard the only image after a host
stall.

After decode synchronization, stale replacement checks also require a fresher queued
frame. The normal age horizon is two source periods; Smooth has no extra third-period
allowance. When latency-fix is active, discard age is measured from queue admission;
otherwise the pre-render age uses the selected source-mapping boundary and the
post-render-wait check uses the scheduled target. The adaptive playout delay is
protected from age rejection. For admission-age checks, subtract only this frame's
explicit `decodeSyncWaitUs` from discard age: that CPU service is already paid, and a
queued successor does not prove its decode is ready. Keep full elapsed age for clock
mapping and latency reports. The pre-decode queue pruning handles already-expired queue
residence before spending GPU wait. If offscreen preparation has completed, skip the
pre-render stale replacement checks: completed work is ready, while a newer queued
source is not known ready. Queue capacity and early expiry still bound queued work.
Deliberately omitted compressed reference frames are a different matter and can require
codec recovery; dropping a decoded image does not by itself require an IDR.

### 7.2 One normal frame

1. The worker wakes and consumes window notifications. It prunes expired queue
fronts under the conditions above, dequeues the retained frame, and checks stop/suspend
state.
2. It waits for a queued preparation ticket when present. A successful ticket
supplies its recorded decode-ready boundary; a failed or cancelled ticket falls back to
ordinary decode synchronization and preparation unless a lifecycle interruption requires
discarding the frame. Only actual CPU wait time on the pacing thread is
`decodeSyncWaitUs`; asynchronous GPU dependency insertion is not charged as CPU wait. If
the wait is substantial, the completion timestamp is sampled after the wait, not
reconstructed by adding duration to decoder output.
3. `VrrTimingController::schedule()` chooses the source slot, target,
render-start deadline, latch request, and diagnostics using the current monotonic time.
The worker publishes the receive-side deadline from this decision and evaluates stale
replacement when a successor is queued.
4. The worker waits until render start, then consumes window/display-epoch
changes and may repeat the stale check. It calls `prepareFrame()` (or activates
completed preparation) with the captured decode boundary and selected
`VrrPresentRequest`. Preparation includes mode selection where needed, rendering, and
image acquisition; intentional target waiting is outside that measured interval. D3D11
selects at Present; Linux Vulkan uses the startup-selected swapchain mode.
5. Successful, complete renderer-wait observations can train bounded readiness
history for later frames. The learned lead moves render start earlier; it does not move
the source target or native latch decision. Failed/incomplete waits are not training
samples. Explicit preparation-time GPU wait is excluded from generic learned preparation
cost to avoid charging one stall to two budgets.
6. On preparation failure or lifecycle interruption, the worker cancels when
required. A backend reports whether abandoning an acquired image may require native
submission; the worker owns any required submission-floor wait and records the
authoritative cancellation result. If preparation proves the decoder source reusable,
the worker frees it before target waiting.
7. The worker waits for `targetUs`, then rereads the clock and enforces the
controller's applicable earliest-submission floor. A timer return alone is not
permission to submit early.
8. Immediately before the native operation, the worker consumes final lifecycle
notifications. It calls `presentAdaptive()`, records native result and timing, and
consumes deferred completion feedback before recording submission. That completion can
train future readiness and bound current serial service, but cannot change the target
already issued for this frame.
9. The worker records the outcome and retains or defers frame ownership per the
presenter contract. Backend source retirement can continue after this worker step.

Offscreen preparation on Linux VAAPI or PyroWave/Mailbox is opt-in. After the first
ordinary frame establishes swapchain format, admitted frames can receive cancellable
preparation tickets. A separate thread owns its renderer and mapping textures,
synchronizes decode, imports and renders offscreen, then polls output completion; it
never acquires a swapchain image. The pacing thread waits for the ticket, acquires the
image, copies the completed output, verifies the short copy, and applies the ordinary
target wait. It starts preparation of the next image only after that copy, so
preparation GPU work cannot delay the copy. The preparation queue is bounded to three
pending jobs and one running job. Tickets do not add another playout queue.
Eviction/lifecycle discard cancels its ticket; if a cancelled job already submitted GPU
work, its source mapping stays alive until output completes. Timeout/error drains GPU
work before unmapping. Shutdown interrupts ticket waits and joins preparation before
renderer teardown. Format, representation, or colorspace mismatch rerenders the same
source for current output. Completed tickets bypass pre-render stale replacement because
the output is already ready; capacity, expiry, and lifecycle cancellation still apply.
Stage trace fields precede the pacing decision and may precede dequeue; the recorded
pacing-thread decode wait remains zero for prepared-ahead frames. Replay validates stage
ordering but cannot predict changed GPU/compositor throughput.

The worker's performance report keeps `decoderOutputUs` unchanged. On successful
presentation, it records through presentation-call return and sums measured preparation
and present-call durations. Prepared-ahead frames also add their offscreen
renderStart-to-ready interval to preparation and rendering, and account the preparation
thread's decode wait separately. Queue/pacing subtracts accounted decode wait from the
remaining client-processing time. Failed/cancelled outcomes are traced but excluded from
paired duration totals and their shared frame count. The software spacing floor depends
on policy: a frame classified latched can have that floor disabled. The worker enforces
the floor actually returned by the controller; it is not universal that every submission
is one display period plus a guard after its predecessor. See the native presentation
contract before inferring hardware protection.

### 7.3 Waiting and scheduler accounting

`VrrTargetWaiter` uses the controller's monotonic clock and an absolute deadline. It
sleeps coarsely until at most 500 microseconds before the deadline, plus up to 500
microseconds of learned target wake lead, then performs a bounded active wait. The
active path must reread time until the deadline; it can escape if the clock stops advancing.
Windows uses a high-resolution waitable timer when available, with `sleep_for` fallback.
The waiter default active hook is a CPU pause hint, but the production worker injects
`std::this_thread::yield()` as its hook; this means the actual worker currently yields
the OS timeslice during its final active region. Trace labels such as
`active_yield_count` count these active steps. Scheduler overshoot and
deadline-already-elapsed cases remain distinct.

`VideoThreadPriority` is applied at entry to the dedicated VRR pacer and the other
dedicated decoder, renderer, V-sync, and optional Vulkan preparation threads. Windows
dynamically registers MMCSS `Playback`, requesting relative HIGH for deadline threads
and NORMAL for work threads; it reverts on thread exit. If registration is unavailable,
SDL is the fallback. Other platforms use SDL; deadline threads request TIME_CRITICAL and
retry HIGH on failure, while work threads request HIGH. Each accepted/rejected request
is logged once. These are thread requests subject to OS policy, not guarantees.
The video policy does not change main-thread rendering, codec-internal, network, or
compositor priority. Audio sample delivery has its own existing SDL HIGH request on the
first callback, except on Steam Link. No process priority, affinity, or system setting is
changed. Replay
cannot predict scheduler or compositor changes, so verify priority/wait behavior with
fresh live wake and submission timing evidence.

### 7.4 Suspend, restore, cancellation, and shutdown

Minimize/suspend marks the worker suspended, clears queued frames, and wakes it. An
in-flight preparation can be cancelled; the worker waits for restore or stop.
Display/window epoch changes invalidate current assumptions, reconcile presenter state,
and request controller rebase. Session display changes can recreate the renderer or
disable VRR if refresh becomes different or unreadable.

Cancellation is backend-specific: some presenters must submit an acquired image to
release it. Such feedback and any required spacing are recorded rather than assuming
cancellation is timing-free. D3D11 cancellation unbinds its render target and does not
use Present to cancel.

Shutdown sets stop state, wakes and joins the pacing worker, stops/joins optional
preparation before renderer destruction, discards remaining queued frames, clears
receive-deadline state, closes the trace, and conditionally saves calibration history.
The presenter is finally cancelled to release retained native state. Keep decoder
surfaces and native images alive until all GPU reads and writes that reference them have
completed.

## 8. Controller: source timeline and target construction

### 8.1 Resolve the live policy before reading parameters

`VRR_TIMING_PARAMETER_FIELDS` is the shared trace/replay schema. Its initializer values
preserve historical behavior; `vrrTimingParametersForSession()` resolves the live
policy. Normal sessions select interval-buffer revision 9 and
`playout_readiness_hitch_threshold_us=0`; the older thresholded readiness policy is not
the production growth policy. The diagnostic capture checkbox does not change the timing
policy. Replay uses the parameters recorded in the capture, with compatibility defaults
only for fields absent from an older schema.

The three timing presets change the interval-quality target, history, tolerance, and
source-frame buffer allowance. They share admission, growth/release, and late-frame
recovery behavior.

| Preset | Source-frame allowance | Interval-quality target | Score history | Interval tolerance |
| --- | ---: | ---: | ---: | ---: |
| Low Latency | 0.5 frame | 99% | 60 s | 500 us |
| Balanced Target | 1 frame | 99.5% | 120 s | 500 us |
| Smooth | 4 frames | 99.99% | 300 s | 250 us |

These are preset defaults for four independently editable values. Runtime resolution
clamps the allowance to 0.25–4 source periods, interval-quality target to 90.00–99.99%,
history to 10–300 s, and tolerance to 250–2,000 us in 250-us increments. The target
applies to the severity-weighted client interval-quality score, not an on-time-frame
percentage.

The live delay starts from a 6 ms seed, has a 1 ms minimum, and is capped by the
selected source-frame allowance and the capacity of four waiting frames. Both preset cap
and maximum use the fitted source period; queue capacity uses the faster of fitted and
negotiated cadence, not display refresh. The interval controller needs
at least 500 ms and 32 consecutive valid intervals for initial calibration. It requests
growth only for fresh, attributable client-added submission error when the complete
serial service path and decoder queue fit the intended interval. Growth is limited to
250 us per 250 ms request and 125 us per frame. The common release rate is 250 us/s
after eight seconds of qualified clean evidence. Short sequence breaks preserve earned
clean recovery; long debt remains available for scoring and renewed attack evidence. The
preset's longer quality history does not by itself hold the buffer above the current
pressure policy.

Production maps sender timestamps from `decodeCompleteUs`
(`playout_source_mapping_decoder_output=0`). That timestamp starts at decoder output,
which already includes work completed before the decoder exposes the frame, and can
advance to a later recorded pre-schedule GPU-ready observation. A zero/short worker wait
can leave it at decoder output rather than GPU completion. The general FIFO-readiness
budget is zero for timestamp playout; adaptive playout delay is its buffer. Cadence
smoothing has a separate reserve described below, which does not grow the interval
buffer. GPU readiness learning affects preparation lead only, not the presentation
target; its lead uses recent completed backend waits at p99 plus 500 us, attacks by at
most 1 ms per observation, releases at 250 us/s, and is capped at the
smaller of 12 ms and the fitted source period. Asynchronous completion that provides no
usable wait measurement does not train this term.

Reduce judder is an independent preference, enabled by default while retaining saved
choices. It is snapshotted at stream start and can be overridden for one stream by
`--vrr-smooth-frame-timing` or `--no-vrr-smooth-frame-timing`. With it enabled,
production smoothing gains 150 per mille, tracks period with a 25 per mille EMA plus
20,000-per-million phase feedback, and may retime by up to 6 ms. Its learned readiness
reserve is p98 of recent smoother-caused shortfalls minus 500 us, capped at 3 ms and
released at 500 us/s. With the preference off, the controller follows mapped RTP spacing
and disables smoothing and that reserve; adaptive buffering and display protection
remain active. Historical captures keep their recorded smoothing parameters and cadence
gate.

Preparation starts early enough to use the existing playout interval
(`playout_prepare_on_arrival=1`); there is no extra delay after the previous submission.
The minimum preparation-lead input is 2.5 ms and the render-lead floor is 3 ms. GPU
readiness, CPU render work, and scheduler wake leads affect when work starts, not the
chosen presentation deadline. The queue can hold four waiting frames in addition to the
one active frame.

### 8.2 Source clock mapping and cadence

The controller checks RTP timestamp progression and frame identity, unwraps RTP wrap,
and rebases on frame-number reset or an invalid RTP delta (zero, backward/ambiguous,
or beyond the configured forward limit). Unavailable RTP timestamps use the
negotiated-FPS/frame-number fallback. With valid RTP, the source period is the Q16
endpoint-span fit:
elapsed unwrapped RTP time divided by source-frame-number span. Using frame-number
deltas keeps locally skipped frames from appearing as a slower source. Negotiated stream
FPS supplies fallback timing and bounds.

Cadence history retains 6 to 512 samples. Its window moves between 350 ms and 1 s
according to available source-period headroom over display period plus guard. A
departure greater than 3.5:1 in either direction starts a provisional rate segment.
Production accepts that candidate after at least three samples spanning 200 ms. A
candidate is abandoned when an interval returns within 5:4 of the previous fitted
period. Thus isolated gaps do not temporarily resize source-rate-dependent budgets.
Ordinary accepted fits follow the cumulative sender timeline rather than a fixed-FPS
generator.

For timestamp playout, the mapped source slot is

```text
sourceTime = unwrappedRtpTime + appliedClockOffset
offset observation = decodeCompleteTime - unwrappedRtpTime
```

The offset estimator keeps the minimum observation within a 3 s window and uses 64
observations for initial warmup. In production, cadence/phase breaks retire stale
observations without discarding the applied offset; ineligible samples cannot move it.
Correction is paced at 2,400 us per second, with at most 100 us per observation, using
unwrapped RTP as the elapsed-time clock. This makes convergence independent of frame
rate and prevents worker or GPU backlog from aging the sender-clock model. The learned
minimum is a client-side mapping estimate, not host capture latency or absolute clock
synchronization. Historical replay modes retain their captured mapping clock, warmup,
and per-frame slew settings.

When RTP is unavailable, the controller follows the negotiated-FPS/frame-number fallback
and its readiness/phase learning path. Do not apply the fallback readiness-spread
calculation to normal timestamp production.

### 8.3 Cadence smoothing

With Reduce judder enabled, the controller blends 85% of its predicted source slot with
15% of the raw mapped slot. This redistributes existing waiting time to reduce adjacent
short/long intervals; it does not make irregularly sampled images uniform or prevent
compositor jitter. RTP values remain unchanged, and their arbitrary epoch does not
control scheduling. For an eligible interval, the model is approximately:

```text
trackedPeriod += 0.025 * (eligibleSourceInterval - trackedPeriod)
predicted      = previousSmoothedBasis + trackedPeriod
error          = predicted - (rawSlot + readinessReserve)
trackedPeriod -= 0.02 * error
requestedShift = clamp(0.85 * error,
                       -(delayBeforeFrame + readinessReserve),
                       6000 us - readinessReserve)
smoothedBasis  = rawSlot + readinessReserve + requestedShift
```

Production requires four consecutive valid intervals for smoothing cadence qualification
and uses compensated-burst handling. The live readiness bound then limits early retiming
to known decode readiness while preserving the raw target's typical render allowance.
Smoothing-only shortfalls train the smoothing reserve; they do not grow or renew the
independent interval buffer. The reserve is applied to all timestamp frames while
smoothing is enabled, including frames the smoother cannot place, so a cadence reset
does not step the schedule by the reserve. Production prediction anchors the next
smoothing state to the intended target, not a later execution time. Rebases,
cadence/rate/phase changes, bursts, stalls, and excessive phase error reset the fit. The
6 ms limit bounds positive retiming, not total latency; source-frame buffer caps, queue
capacity, readiness, and presentation floors still constrain delivery.

### 8.4 Target, render start, and presentation protection

The timestamp target uses the source slot, any applicable readiness budget, cadence
adjustment, adaptive delay, typical render allowance, and presentation safety:

```text
target = sourceTime + readinessBudget + cadenceAdjustment
       + playoutDelay + typicalRender + presentationSafety
target = max(target, now + typicalRender + presentationSafety)
```

For production timestamp playout, `readinessBudget` is zero. The delay in force before
current-frame feedback is used to form the raw smoothing basis and score readiness.
After that update, the controller clamps the effective delay used in target construction
to the current source-period and queue limits. A late frame may clamp to the current
execution opportunity without shifting subsequent source slots. A future projection must
qualify on three consecutive frames before the live source-clock mapping is reseeded.

Production compares each planned target with the prior spacing anchor plus one display
period and guard (`playout_per_frame_latch=1`). A slot inside that boundary is latched
when the presenter supports synchronized presentation; there is no additional 225/400 us
hysteresis allowance in this production mode. A slot outside it may tear. Production
also latches the first slot at least 20 ms after the anchor to cover display repeats
below the VRR range. The controller applies the software spacing floor when native
protection is unavailable. Adaptive-only and constant native-synchronized modes remain
explicit policy choices for replay/session configuration.

A latched submission advances the spacing anchor to the predicted flip, `max(present
call, previous anchor + display period)`, rather than merely its call time. For an
eligible tearing submission, the worker asks the backend to check one display period of
native flip protection. DXGI can latch that frame when frame statistics show its
predecessor still pending or scanning out; its synchronized path uses `Present(1, 0)`,
while an unprotected adaptive frame uses `Present(0, DXGI_PRESENT_ALLOW_TEARING)`.
`MOONLIGHT_VRR_SYNC_FLIPS=1` opts into synchronizing every DXGI flip. The renderer's
composition backend can provide native ordering, but does not use DXGI
tearing flags. Replay's tear-risk estimate does not model this native flip queue, so it
cannot score this protection path.

## 9. Active production learning and bounded delay

### 9.1 Readiness and FIFO-service evidence

Production uses two separate evidence paths. `ReadinessPrediction` models the FIFO
readiness of the next frame from its decoded time, expected source-slot progress,
typical render cost, scheduler delay, and outstanding backlog. It removes intentional
pacing and swapchain-acquisition waiting from work that could justify stored playout
delay. Raw RTP slots remain the predictor basis; the smoother's advance is recorded
separately. Frames with lost packets are ineligible as readiness or absorbability
evidence because PyroWave supplies zero-filled detail instead of waiting for
retransmission, so extra client delay cannot restore their content. Their observed
output spacing can still appear in the interval-quality score, but cannot authorize
growth or clean recovery.

For responsive production, this FIFO model writes a version-20 `Reserve` used for
five-minute readiness diagnostics and cached history. It is not the live buffer
controller: `RecentReadiness` is only passed into the predictor for responsive revisions
below 5, while production resolves to revision 9. The reserve bins valid raw readiness
errors in 250 us steps, including successes, over a five-minute monotonic-time window
(one-second aging buckets). Its nearest-rank p99.95 and cache state must not be
described as the source of revision-9 growth or release. Sustained backlog, decoder
queue above one source period, or work plus scheduler delay above a period is held as an
episode; recovery flushes its samples into the diagnostic reserve. An episode lasting
two seconds or filling 512 held samples is marked overloaded and discarded rather than
learned as jitter to absorb.

The interval controller separately measures serial service for growth eligibility. It
subtracts swapchain-acquisition time from raw preparation, then combines explicit
decoder-sync wait, render-scheduler delay, preparation service, and the conservative
deferred-GPU wait. This serial-service check is not readiness lateness: readiness
attributes which frame arrived too late, while the service check asks whether standing
buffer can absorb the work. In production the qualified one-second sums of serial
service and decoder queue must each fit within summed intended interval time.

### 9.2 Revision-9 interval buffer and feedback

The session resolver selects `playout_responsive_buffer=9` on every ordinary VRR
backend. The interval buffer compares each pair of adjacent eligible submissions against
intended mapped source-slot spacing, including deliberate cadence smoothing when enabled:
`error = abs(actual submission spacing - intended mapped-slot spacing)`.
Revision 9 averages the per-interval excess over the configured tolerance, weighted by
evaluated time, over the
preset's quality window. A source-rate, phase, missing-frame, or other sequence break
makes the next pair unqualified; unknown gaps do not count as clean intervals. The
one-second mean remains diagnostic. Preset quality target and window tune the shared
policy; they do not select separate control algorithms.

An increase requires current quality pressure, a fresh interval error above tolerance,
positive lateness of the delayed frame relative to its readiness deadline, and
absorbable serial service. The delayed frame is the later frame for a stretch and the
earlier frame for catch-up; its original buffer is the growth base so a catch-up cannot
charge the same miss against a newly raised buffer. Each request is limited by the
quality excess, readiness lateness, fresh interval excess, and 250 us, with at least 250
ms between attacks. Old score debt may hold protection, but cannot by itself trigger
another increase. Native presentation timing and CPU submission hitches are diagnostic
evidence; neither can independently grow this buffer.

Revision 9 applies the tolerance to each interval before averaging, so clean intervals
do not erase the scored excess of isolated hitches. With production settings, pressure
renews the eight-second hold only for a fresh, attributable, absorbable late-work miss.
Clean recovery accrues only from eligible adjacent intervals whose one-second
serial-service and decoder-queue totals fit their intended time. Short sequence breaks
preserve earned recovery, while prolonged ineligible periods or a long observation gap
clear it. After the hold and without a fresh pressure event, the target releases at 250
us per second, bounded by the configured minimum. Applied increases slew by at most 125
us per frame; decreases follow the observer's time-based target. A short clean run can
therefore begin recovery without waiting for the five-minute diagnostic history.

Submission and native `SmoothnessFeedback` are separate diagnostic streams. They compare
consecutive eligible intervals with intended source cadence, classify errors around a 3
ms threshold after uncertainty handling, and retain only newly observed intervals;
ambiguous threshold crossings are unavailable evidence. Native presentation observations
require a fresh matched display event in production. DXGI refresh references remain
available only to legacy replay and are not display timestamps. The submission fallback
is lower confidence and does not enter verified display counters. Because production
sets prediction-only adaptation, neither smoothness-feedback stream controls revision-9
delay. A submission-spacing score is not optical evidence of displayed smoothness.

Older responsive revisions, the readiness-hitch `ReadinessFeedback` path, native-hitch
adaptation, and the original readiness percentile laws remain in the parameter schema
for exact replay and explicit experiments. Revision 8 retains its 250 us tolerance;
revision 9 uses the resolved preset tolerance. `Reserve` revisions 13 through 20
likewise preserve historical models and captured replay. A class or file named `Vrr13`
does not identify the active production revision.

### 9.3 Delay update and capacity formulas

Revision 9 learns a requested target bounded by the minimum of the source-rate preset
cap and available queue capacity. Production's preset cap uses the fitted source period:
0.5 source periods for Low Latency, one for Balanced, and four for Smooth. The available
queue allowance uses the faster of fitted and negotiated cadence, multiplied by the
configured queued-frame count, then subtracts render lead, presentation safety, and
(when enabled) maximum smoothing lag. In equation form:

```text
queue period = min(fitted source period, negotiated stream period)
queue allowance = max(0, queue period * queued-frame capacity
                        - render lead - presentation safety
                        - enabled maximum smoothing lag)
delay maximum = min(source-period preset cap, queue allowance)
```

The source-frame allowance is not a count of frames waiting in a new queue and may be
clipped by the existing queue ownership and work already occupying it. The production
cold-start input is `max(6,000 us, 0.95 * fitted source period)`, capped by the larger
of display period and render lead, then clamped to the current delay bounds. A cached
calibration may seed this cold-start input only through the separately stored bounded
start-delay entry; it does not load a revision-9 target or authorize growth. Applied
delay remains within the live bounds after source-rate changes. Capacity telemetry can
expose requested demand above the cap, but clipped demand is not retained as growth
debt.

### 9.4 Persisted calibration

The profile file is `vrr13-calibration.json` under the cache path. Production constructs
`Reserve(20)` for responsive sessions and persists that reserve's readiness histogram,
not the live interval-buffer target. The key is a SHA-256 of the base calibration
identity plus display name, stream and display rates, smoothing state, effective custom
timing values, late-recovery policy value, and buffer ratio. Enabled smoothing further
includes its gain, period EMA, lag cap, cadence/recovery settings, catch-up value,
readiness-bound setting, and smoothing-reserve/period-feedback values. Linux appends its
shared-readiness policy marker. This separates histories when the relevant timing policy
changes.

Profiles are version-checked, expire after 14 days, and are aged on load. Saving
requires at least 240 observations; storage uses a lock and atomic replacement and keeps
at most 16 profiles. A cached histogram can inform the diagnostic five-minute
distribution, but it does not qualify current-session coverage or change the revision-9
interval target. The worker separately stores a start-delay seed from settled session
samples under the same identity. That seed only changes the initial delay within current
bounds. Display-epoch changes invalidate calibration saving.

The initial revision-9 interval-calibration flag is session-local and is not loaded from
either cache. It qualifies after at least 500 ms and 32 consecutive eligible intervals;
once complete it survives later sequence breaks and FPS changes. After a break,
adaptation must requalify for one second and at least two eligible intervals. The long
quality history, eight-second clean-time hold, 250 ms growth cooldown, and gradual
release remain independent of this startup qualification. Captures missing the
calibration fields retain the historical one-second/two-interval replay behavior.

## 10. Windows D3D11 mechanics and native synchronization

### 10.1 Eligibility, decode ordering, and prepared-frame lifetime

The Windows D3D11 VRR presenter is eligible only with effective V-sync enabled, a valid
display refresh, a borderless fullscreen flip-model swapchain, a renderer thread
supported by the pacing worker, present-ready GPU fencing, DXGI tearing support and the
tearing swapchain flag, and the render adapter owning the output. The renderer
re-queries the actual swapchain descriptor, exclusive-fullscreen state, window state,
and output match on display changes. These checks establish client-side capability; they
do not establish that the display is currently using variable refresh. The five-buffer
swapchain is not the worker queue capacity. The code deliberately does not set maximum
frame latency to one: with interval-zero presentation that can make `Present` block like
V-sync.

At decoded-frame admission the worker captures a decode boundary before later decoder
work can be queued. On separate decode and render devices this is a shared-fence signal
on the decode context followed by `Flush()` to submit the signal; it does not wait for
GPU completion. The render context queues a wait on that exact value before sampling the
decoder surface. A render-to-decode fence orders later surface reuse after the
renderer's reads. A shared device has no cross-device decode boundary and relies on
normal immediate-context ordering. Signal, wait, and flush failures abort preparation
and request device recovery; the adaptive path does not submit a frame with missing GPU
ordering.

Before preparing a frame, the pacing worker calls `waitForDecode()` for its captured
boundary. If the monitored fence is already complete, or CPU monitoring is unavailable,
this returns without waiting; the render-context GPU wait still preserves correctness.
With a monitored fence and a pending boundary, the worker waits on a dedicated event in
bounded slices and verifies the fence value. That CPU wait supplies the
decode-completion observation used by production source mapping and its duration is
recorded as `decodeSyncWaitUs`; it is not a second GPU correctness dependency. PyroWave
follows the same split: its decode fence wait on the render context orders sampling,
while `waitForPyroWaveDecode()` is the CPU readiness observation.

Preparation clears and binds the target, draws video, draws enabled overlays, updates
the target colorspace for SDR/HDR, then signals and flushes a present-ready fence. This
fence marker follows the frame's recorded GPU work. The renderer makes one nonblocking
value poll and returns without claiming completion. It keeps the `AVFrame` alive because
GPU reads may still be in flight; the worker defers releasing it until after
presentation, and the render-to-decode fence protects the decoder surface on separate
devices. The overlay path snapshots COM references under a short lock before drawing, so
an overlay update can replace references without destroying resources in use by the
render context.

The worker performs its cadence hold while the GPU runs. At the target boundary,
`presentAdaptive()` verifies the same present-ready fence value. It releases the
presentation lock during the bounded event wait, allowing decode and display callbacks
to progress, then reacquires the lock and revalidates the prepared frame and display
state before submission. The wait polls the fence value between 1 ms event waits; event
notification alone is not accepted as completion. A resize or display replacement drains
an outstanding marker before invalidating its render target. Timeout, device removal, or
a failed ordering primitive cancels the frame and enters renderer recovery. A successful
wait proves the prepared GPU commands completed before submission; its poll/wait bracket
is a conservative completion bound, not an exact GPU timestamp or total GPU duration.
The readiness telemetry records the verified residual wait at the target and feeds a
bounded lead estimate; it does not move the source target to conceal a late frame.

### 10.2 DXGI submission policy and telemetry

The worker resolves the presenter's constant native-synchronization capability after
renderer initialization. That capability is separate from per-frame latch selection. On
the ordinary DXGI backend, each adaptive submission passes one `DxgiPresentParameters`
object to the swapchain call and records those same parameters: a latched frame uses
`Present(1, 0)`, an adaptive frame uses `Present(0, DXGI_PRESENT_ALLOW_TEARING)`, and
legacy presentation retains its existing interval-zero flags. Optional flip protection
can latch a risky frame based on a predecessor still pending or a recent refresh
reference; with an unambiguous native raster source it can wait for the vertical blank,
with a two-display-period bound. `MOONLIGHT_VRR_SYNC_FLIPS=1` explicitly selects
synchronized DXGI flips for all frames. DXGI reports requested/forwarded API parameters,
not whether a panel tore or which scanout mode the driver used.

Only `S_OK` counts as a successful presentation. Failed calls request device recovery;
other success statuses, including occlusion, are cancellations rather than evidence that
an image reached a monitor. The submit boundary is timed on the Limelight microsecond
clock. Optional `GetLastPresentCount()` and `GetFrameStatistics()` observations can
refer to an earlier present. In DXGI statistics, `PresentRefreshCount` and
`SyncRefreshCount` identify different things, and `SyncQPCTime` is a timestamp for the
sync observation, not the presentation instant of `PresentCount`. The code labels this
sample a refresh reference and retains the raw QPC ticks, frequency, and correlation
evidence; it must not be used as a per-frame display event or compositor-latency sample.

### 10.3 Native evidence limits

QPC is correlated to `LiGetMicroseconds()` using a process-wide midpoint sample
bracketed by QPC reads and the actual QPC frequency. DXGI sync-reference ticks are
translated relative to that correlation. The bracket span supplies an uncertainty bound;
if the correlation or native query is unavailable, the timestamp remains invalid.
`D3DKMTGetScanLine()` samples raster position around a CPU observation when
`MOONLIGHT_VRR_ALIGN=1`; DisplayConfig supplies signal geometry only for one unambiguous
path. Those readings can support modeled phase/raster analysis, but they do not reveal
when a queued image became visible.

### 10.4 Composition presentation and display timing

For an eligible VRR D3D11 session with a valid DisplayConfig output-path identity, the
renderer attempts the composition presenter unless `MOONLIGHT_VRR_COMPOSITION=0`; an
unset value and `1` both request the attempt.
The setting is read at renderer initialization, so changing it requires a stream
reconnect. The code checks the actual Windows version with `RtlGetVersion` (including
the Windows 11 build 22000 revision requirement), loads `CreatePresentationFactory`
dynamically, and asks the factory whether independent flip is supported. It creates a
BGRA-capable D3D11 device without internal threading optimizations for this attempt,
then retries device creation with the normal flags if the API or driver is unsupported.
An unavailable display-path identity skips composition. An unsupported composition
device retries ordinary device creation. If that adapter fails, initialization tries
other adapters before failing. Composition presenter setup failure keeps the existing
DXGI swapchain.
Startup logging reports which presenter initialized; independent-flip
capability does not guarantee that every frame uses independent flip.

The composition presenter owns five displayable textures, a presentation
manager/surface, and a DirectComposition visual attached to the stream window. The
source rectangle is set to the full buffer on allocation and resize. Buffer acquisition
polls availability without waiting; when all buffers are busy, preparation returns
unavailable so the bounded worker can discard stale work. After rendering and GPU
readiness, submission cancels older pending presents, sets the target time to the
current QPC-derived system-relative time, and calls the presentation manager without
adding a source period or waiting for display statistics. `ForceVSyncInterrupt` requests
prompt statistics delivery; it is not a presentation wait. The buffer count does not
describe OS/driver queue depth.

The presentation manager emits composition-frame and independent-flip statistics.
Composition-frame events are counted but are not treated as display events. An
independent-flip sample is accepted only when its content tag, adapter, source ID, and
strictly increasing present ID match this presenter. Its system-relative displayed time
is correlated to QPC converted to 100 ns units inside a fresh, at-most-500-us
worker-clock bracket; samples older than 100 ms or inconsistent with the bracket are
rejected. The resulting timestamp and uncertainty are eligible display-event telemetry.
This is OS presentation feedback, not optical scanout measurement, tear verification, or
end-to-end latency.

Composition is a constant synchronized presenter capability: the worker marks
`native_synchronized_presentation`, keeps controller decisions latched, and omits its
software spacing floor. This does not mean DXGI interval/flag switching occurred;
composition uses the presentation manager API and reports backend value 3, while
DXGI-specific flags and query results remain unset. DXGI fallback statistics are refresh
references only and cannot create measured cadence events. The client cadence report can
therefore use submission estimates where no accepted native display events exist, while
the display graph and present-timing diagnostics separately show accepted
independent-flip events.

Composition buffers are replaced on resize, and display changes recreate the renderer
and its output identity. The optional `compositionprobe --run` utility can measure
synthetic presenter coverage and submission-to-display feedback on a Windows machine. It
does not establish optical VRR, tear absence, gameplay cadence, or end-to-end latency.
Those claims require live gameplay and, for optical behavior, external display
measurement.

## 11. Other presentation paths

### Shared presenter contract

`IVrrFramePresenter` is the boundary between the platform-neutral pacing worker and a
native renderer. The worker asks whether a split prepare/present path is supported,
establishes any decoder dependency, prepares a frame without intentionally waiting for
the presentation target, then presents or cancels it. Preparation reports decoder
synchronization, drawable/swapchain acquisition, rendering and flush costs when
available. `sourceFrameReusable` is true only when the backend has finished every GPU
read from the decoder-owned frame; otherwise the worker keeps that frame alive.
Cancellation can itself require a native submit for an acquired Vulkan image. A
presenter advertises per-frame latch support only when it honors that request. These
contracts do not make one backend's completion or present semantics transferable to
another. If worker startup fails, the presenter must restore fixed presentation before
legacy rendering begins; failure to restore it fails pacer initialization.

Calibration history is backend-specific. Metal snapshots its device, display identity
and native timing range during main-thread initialization, then returns that stable
identity to the worker; the worker need not query AppKit. Vulkan calibration identity
includes the physical device vendor/device/driver, pipeline-cache UUID and selected
presentation mode, so Immediate and Mailbox histories do not seed each other. Readiness
waits are observations of CPU waits or polls: their return time bounds GPU completion
but is not an exact GPU execution timestamp.

### macOS Metal presentation

When VideoToolbox is selected with Auto rendering and either VRR playback or the
matching startup probe is requested, the probe uses `VTMetalRenderer` so its color-range
and HDR negotiation matches playback. The probe does not enable adaptive presentation.
Active VRR requires V-Sync, a non-test renderer, native Cocoa fullscreen, and a valid
variable interval range reported by `NSScreen` on macOS 12 or newer. Both continuous
Adaptive-Sync and discrete ProMotion ranges qualify; fixed-refresh ranges do not. SDL's
current display mode, a requested setting, a 120 Hz maximum, or a Metal device alone
does not establish VRR eligibility. Windowed playback and ineligible displays use fixed
pacing.

The active renderer keeps one `CAMetalLayer` and enables synchronized presentation. It
allows three drawable slots with drawable-acquisition timeout, so the displayed drawable
and its submitted successor can remain compositor-owned while the renderer prepares the
next image. The worker still prepares one frame at a time and sets submission cadence;
the extra slot is not another playout queue. The normal Metal display-link path is
bypassed while the VRR worker owns cadence. On fixed fallback, the layer follows the
session V-Sync setting and the ordinary display-link/fixed-render path remains
available.

Preparation measures `nextDrawable` acquisition separately, encodes the existing video
conversion and overlays, commits a command buffer, and waits up to 50 ms for its
completion. That bound applies to command completion, not drawable acquisition or the
whole prepare call. A failed or incomplete command requests renderer recovery. The
command retains the source `AVFrame` and CoreVideo texture wrappers until GPU
completion. After a successful bounded wait, the worker may release the decoder frame
before its target hold; the prepared drawable stays owned until present or cancellation.
Cancellation releases a rendered drawable without submitting it. Metal's GPU-ready
timestamp is the CPU-observed completion bracket, not an exact hardware execution time.

At the worker target, `presentAdaptive()` calls `presentAfterMinimumDuration:` with the
display's native minimum refresh interval. This constrains the previous drawable's
visible duration; it is not an added sleep or source-period delay. Every VRR drawable is
synchronized, so Metal advertises protected-slot support without switching native modes
for each controller latch request. A display change recreates the renderer even when
maximum refresh rates match, because the native interval range and drawable epoch may
differ.

Metal readiness calibration identity captures the Metal device registry ID, stable
CoreGraphics display UUID, and native minimum/maximum intervals and update granularity
on the main thread. Metal is native backend value 4; its increasing submission IDs are
local serials. Since the native present method returns `void`, result 0 means the call
was accepted, not that the compositor displayed the image. Metal has no DXGI query
results, tearing flags, adapter snapshots, fence values, or raw QPC fields.
`addPresentedHandler` contributes an OS-reported `presentedTime` when positive.
Moonlight brackets a `CACurrentMediaTime()` sample to translate clocks and rejects
unusable, future, over-100-ms-old, or wider-than-500-us samples. A later submit may
carry an earlier serial's display event; correlation preserves that identity and leaves
missing events as gaps. This is compositor evidence, not optical proof of panel cadence,
tearing, or end-to-end latency. Requested latch transitions do not reset Metal's fixed
synchronized presentation mode.

macOS PyroWave uses `VTMetalRenderer` plus a shared Vulkan-to-Metal surface pool. The
pool selects a Vulkan device whose exported Metal device registry ID matches the
presenter's and requires timeline semaphores, subgroup-size control/full subgroups, and
Metal object export support. It lazily allocates up to eight three-plane R8/R16 images
and samples their exported Metal textures directly, with no CPU readback/upload. Each
planar `AVFrame` retains its pool surface and per-frame completion value. Before VRR
scheduling, Metal waits for that frame's timeline value for at most 50 ms when it is not
already complete; failure requests renderer recovery. The Metal command retains the
frame until its reads finish. Exhausted surfaces drop output, and failed shared-GPU
setup fails initialization rather than silently switching to CPU copies. Local
calibration uses the same shared path; it checks decode and Metal draw completion within
a shared 50 ms readiness bound. These ownership and reconstruction checks do not prove
live-stream throughput or physical presentation.

### Linux Vulkan presentation

On Linux, a qualified VRR request uses the Vulkan/libplacebo presenter when its renderer
and surface support the worker split. Wayland selects Mailbox when exposed. X11/KMSDRM
selects Immediate when exposed. Gamescope prefers Immediate; if unavailable, it may
select Mailbox when that option is enabled and exposed. Its FIFO WSI compatibility path
requires Gamescope detection, `ENABLE_GAMESCOPE_WSI=1`, and exposed FIFO support;
the client does not independently verify that the WSI layer loaded. Ordinary desktop FIFO,
unsupported surfaces, and missing adaptive modes fall back to fixed pacing. Windows
Vulkan is explicitly rejected for VRR; macOS VRR uses native Metal rather than Vulkan
swapchain mode selection.

The startup decoder probe carries the same `preferVrrRenderer` decision so range/HDR
negotiation uses the playback-capable renderer, but test-only probing does not enable
adaptive presentation. Vulkan's AMF AV1 full-range mapping override applies only to a
negotiated full-range stream; VRR itself does not request 10-bit formats. HDR remains
controlled by the client's Enable HDR preference.

For Linux VAAPI, the worker performs one explicit `vaSyncSurface()` readiness wait
before mapping/preparation. Failure disables adaptive presentation and requests recovery
before Vulkan can read the surface. Libplacebo's subsequent hardware-frame map still
validates/imports the dependency. This explicit barrier does not apply to every Vulkan
input: PyroWave frames instead wait on their per-frame Vulkan completion timeline in
`waitForDecode()` before the worker uses their source timing.

On the hardware asynchronous output path, a mapped `pl_frame` and its imported source
textures remain retained until Vulkan source reads retire. The Vulkan render-complete
semaphore is consumed by `vkQueuePresentKHR`; CPU output completion is not required for
that handoff. Preparation reports the source reusable only after retained mappings have
retired, and the renderer applies bounded retirement backpressure before it accumulates
more source mappings. Software or unretained imports keep the bounded CPU
output-readiness poll. Mailbox defers swapchain acquisition until the presentation
target. A separate offscreen render/copy preparation path is opt-in through
`MOONLIGHT_VRR_OFFSCREEN_PREPARATION=1` and is disabled by default; an acquired Vulkan
image may need to be submitted even when cancelled. Submission spacing does not prove
GPU completion or physical flip spacing. D3D11 fence/event fields remain unavailable on
Vulkan rows.

The adaptive mode belongs to the lifetime of the persistent swapchain. Per-frame latch
requests do not recreate it. Mailbox supplies synchronized stale-image replacement and
advertises native latch protection. Immediate may tear and retains the controller's
software spacing floor; it does not become tear-free through worker pacing. Gamescope
FIFO compatibility also keeps the software floor and compositor-owned scheduling.
Resize, reset, or fallback can recreate the swapchain; cached HDR/colorspace intent is
restored before the next acquisition.

Wayland presentation feedback is attached to the next actual surface commit and remains
observation-only. Gamescope WSI timing uses `VK_GOOGLE_display_timing` when exposed, but
its timestamps are compositor scheduling evidence. In
[Gamescope 3.16.23.5](https://github.com/ValveSoftware/gamescope/blob/3.16.23.5/src/steamcompmgr.cpp#L6446-L6489),
the implementation passes a scheduled vblank target as `actualPresentTime`; verify the
deployed version before interpreting this field. Sparse or
absent feedback does not establish dropped frames, and neither path supplies independent
physical scanout proof. Submission success means the compositor accepted work, not that
the panel showed every image. Gamescope timing rejection counters describe the feedback
queries and clock validation; they do not alter pacing.

The Linux High-performance GPU power while streaming preference creates a session-scoped
[GpuPerformanceHold](app/streaming/gpuperformancehold.cpp) when supported by the AMDGPU
build and kernel. It requests a peak stable pstate on eligible display GPUs only when
the existing performance level is `auto`, verifies the request, and releases its owned
contexts at stream end. Externally selected levels are left alone. This optional power
request is separate from VRR timing policy and provides no Windows GPU-power control or
measured smoothness guarantee.

The worker presents only newly received frames and waits for queue activity when empty.
It does not re-present the last image to fill gaps; low-frame-rate compensation belongs
to the display. Historical gap-fill trace columns remain reserved and zero-valued for
compatibility.

## 12. Audio, input, and end-to-end latency

### Audio playout

Audio is a separate UDP/RTP stream. The transport and FEC path is implemented in
[AudioStream.c](moonlight-common-c/moonlight-common-c/src/AudioStream.c) and
[RtpAudioQueue.c](moonlight-common-c/moonlight-common-c/src/RtpAudioQueue.c); Moonlight
decodes and selects its renderer in [audio.cpp](app/streaming/audio/audio.cpp). The
common C library negotiates and binds an audio socket, sends the required audio ping
before RTSP PLAY for GeForce Experience compatibility, receives/decrypts RTP audio,
restores recoverable packets through its audio FEC queue where supported, and hands Opus
payloads to Moonlight's audio callback. The callback's selected renderer owns device
playback. Audio packet duration sets the source sample cadence; it is independent of
video frame rate and VRR targets.

The receiver deliberately drops the first 500 ms of audio data to discard host backlog
accumulated before the client is ready. For the SDL renderer, the requested device
buffer is at least 480 samples (10 ms at 48 kHz) or three Opus frames, whichever is
larger, to absorb jitter. The common receive queue and SDL device queue have separate
backpressure: SDL discards the current decoded sample without retry when more than
30 ms is pending in the common queue. While its own queue exceeds 50 ms, it waits for
up to 100 iterations of a 1 ms delay and checks for a stopped device. Once that bound
is reached, it queues the sample even if the queue remains above 50 ms. These are queue
controls, not audio/video synchronization. If the audio device
fails, renderer recreation drops samples for the time spent reinitializing so device
downtime does not become persistent playout delay. Muting skips decode/playback in the
callback; it does not alter video timing or VRR.

### Input and controller feedback

SDL keyboard, mouse, touch, pen, and gamepad handlers in
[app/streaming/input](app/streaming/input) submit input through common-library APIs in
[InputStream.c](moonlight-common-c/moonlight-common-c/src/InputStream.c). A dedicated
common-library input sender drains queued packets onto the control stream for modern
hosts. Mouse motion is coalesced in the client handler and the sender batches at a 1 ms
cadence; absolute motion keeps the newest position. Button transitions are not delayed
to a video target. The SDL event loop blocks on platform event support on modern SDL,
with a documented 1 ms polling fallback when a joystick is connected; older SDL builds
explicitly poll with a 1 ms delay (10 ms on Steam Link). Thus there is no single universal client input polling
interval.

The controller mask is initialized from attached devices before the stream begins. In
single-controller mode, an attached PS5/DualSense-type controller recognized by SDL is
announced before other controllers so the host's first-arrival player association can
select it;
multi-controller mode preserves enumeration order and numbering. Regular input stays
with SDL. Adaptive-trigger feedback is sent through SDL's controller-effect API on
supported builds, with dispatch on the input/event thread to serialize against
controller removal. It is independent of PCM waveform support.

### DualSense PCM waveform extension

The Windows/Linux client, through
[dualsensehaptics.cpp](app/streaming/input/dualsensehaptics.cpp) and
[dualsensehid.cpp](app/streaming/input/dualsensehid.cpp), can expose PCM waveform output
for an attached Sony DualSense or DualSense Edge only when a usable output backend opens
successfully. The client then adds `LI_CCAP_HAPTICS_PCM` (`0x8000`) to that controller's
arrival capabilities. Separately, the common library advertises `ML_FF_HAPTICS_PCM`
(`0x04`) in Sunshine SDP feature flags only when the registered `controllerHaptics`
receive callback is non-null. Both conditions matter: opening the local backend without
registering the callback does not negotiate PCM delivery. This `0x5601` control-message
payload and its flags are a coordinated Vibeshine/Sunshine extension in these forks, not
a general upstream Moonlight protocol guarantee. The client validates version, format,
controller index, reserved field, frame count, and exact payload length before dispatch.

The receive callback copies valid PCM into a bounded per-controller queue and returns;
backend workers own conversion, pacing, and device writes. Admission rejects
duplicate/late sequences (including wrap-aware ordering) before changing playback or
rumble priority. Queue overflow drops queued history. Silence received while inactive
does not start waveform mode, and continuous zero samples from an open USB audio
endpoint do not hold rumble priority indefinitely. Both output paths return to silence
after 60 ms without a nonzero actuator sample; Bluetooth sends a final silent report,
while USB lets already submitted samples drain and then renders silence. Starting
waveform playback cancels SDL-emulated rumble, and legacy rumble is suppressed while the
waveform backend is playing. A transport write failure stops that worker and asks for
controller reconnection; ordinary SDL input remains available.

For Bluetooth, the backend opens the exact SDL HID device path and verifies Sony
identity. Linux additionally requires the hidraw bus to identify as Bluetooth. Windows
checks the Sony VID/PID and Bluetooth gamepad HID collection/report lengths, uses shared
overlapped HID I/O, and pads writes to the descriptor's maximum output-report length
while leaving the waveform report CRC at its protocol offset. PCM is resampled from 48
kHz signed 16-bit stereo to 3 kHz signed 8-bit stereo for SAxense-derived Bluetooth
reports. Old chunks and late/duplicate sequences are discarded; packet gaps clear
resampler history rather than replaying pre-gap samples. The worker paces reports and
does not burst old samples after a scheduler stall. Windows waits up to 40 ms for a
pending HID write; timeout cancels and drains the operation before buffer reuse. A
defective driver may still delay cancellation completion, so 40 ms is not a hard upper
bound.

Windows USB uses a separate WASAPI backend. It matches the SDL Sony USB controller's HID
interface to an active render endpoint through the shared device-container ID, requires
a four-channel mix endpoint, and writes 48 kHz float samples to actuator channels 3/4
while keeping channels 1/2 silent. Shared-mode conversion uses the endpoint channel
mask. It requests a 20 ms engine buffer and queries the actual size; the software FIFO
is capped at 40 ms and starts after 10 ms of samples or a 10 ms prebuffer timeout. The
backend is opened before the controller is announced, so failed endpoint discovery
leaves ordinary controller support active without advertising PCM. Linux USB and other
platforms use ordinary rumble rather than this USB PCM path. The host's native game
haptics must be routed through its DualSense controller audio endpoint; host-side USB/IP
selection, where used, is independent of whether the Moonlight client connects over USB
or Bluetooth. Game soundtrack audio is not a substitute for that controller endpoint.

### End-to-end latency limits

A game interaction crosses client input collection and sending, host input handling and
simulation, host capture and encode, network transit, client receive and decode, frame
scheduling/presentation, display scanout, and (for audio) a separate audio device clock
and queue. The client has timings and counters for selected stages, such as network RTT,
host-reported processing, decode, and presentation, but these describe different
boundaries and may overlap. They cannot be added into a precise input-to-photon or
audio/video synchronization measurement without non-overlapping definitions and evidence
for the unmeasured host/display stages. The inspected client paths do not synchronize
audio playout to VRR presentation targets or make VRR follow the audio device clock;
video pacing changes therefore do not imply a corresponding audio delay change.

## 13. Trace architecture and replay fidelity

### 13.1 Capture format and evidence

`MOONLIGHT_VRR_TRACE` enables pacing-worker tracing; `MOONLIGHT_VRR_DEEP_TRACE=1` adds
native and GPU diagnostics. Windows UNC trace destinations are refused so frame delivery
does not write to a network path. A `.csv` destination writes readable CSV; other
suffixes use the chunk-compressed format with `MLVRR1` followed by a newline, which
`scripts/decode-vrr-trace.py` expands to CSV. Current worker output writes schema 5
rows. The replay reader remains backward-compatible with schemas 3, 4, and 5; schema 4
requires spacing-guard feedback, and schema 5 requires its captured controller state and
lifecycle timing columns. Worker-mode simulation requires schema 5. The schema-5 header
is extensible: optional later columns can be absent in older schema-5 captures and are
audited when present. Do not equate a schema-5 row with every newer optional diagnostic
being available.

The writer queues row copies to a bounded, non-blocking diagnostic path; formatting and
I/O run on a separate thread. It can drop diagnostic rows to protect pacing. The
compressed format has a magic prefix and independently length-prefixed compressed
chunks. A clean-close footer (format version 2) accounts for allocated arrival
sequences, enqueued rows, dropped rows, size-cap and write-failure state, and a SHA-256
over the decoded CSV header and row body, excluding the footer line itself. The 512 MiB
cap applies only after at least 60 minutes of capture coverage. A capture without a
clean footer, with drops, a cap, a write failure,
or sequence gaps/duplicates cannot pass strict full-session replay. Footer hash validity
is checked separately: a mismatch fails diagnostic-capture readiness, but is not by
itself part of the `fidelity.baseline_exact` predicate or the exact-baseline CLI gate.

Rows preserve frame identity and RTP timing, receive/reassembly/decode and queue
lifecycle, controller decisions and parameters, preparation/wait/submission times, GPU
readiness observations, presentation outcomes, and native diagnostics. Native evidence
includes backend and Present arguments/results, submission and latch IDs, DXGI
PresentCount/frame-stat snapshots and refresh sequence counters, and optional raster
probe brackets. These are evidence counters and observation brackets with validity
fields, not continuous hardware scanout timestamps. The VRR trace does not carry
per-frame network packet-loss counts; use the session log or other network diagnostics
for loss totals. Terminal rows can be emitted outside the controller-owning worker and
do not have live decision/controller state. Schema extensions should be treated as
optional unless a replay gate requires them; older captures do not gain values for
fields they never recorded.

### 13.2 Exact-baseline contract

`vrrreplay --require-exact-baseline` is a strict schema-5 replay gate. A successful gate
requires exit code 0 and `fidelity.baseline_exact=true`; inspect
`capture.recorded_sequence_integrity_valid`, `capture.clean_close_footer`, and
`capture.normalized_decoded_csv_sha256`/footer hash fields as well. The
sequence-integrity flag requires a clean footer, internally consistent
allocated/enqueued/dropped row accounting, no cap or write failure, zero dropped rows, a
complete sequence beginning at 1 and ending at the allocated count, and no missing,
duplicate, or gapped arrival sequence. Exactness also requires valid row semantics and
timing relationships, matching recorded session configuration and controller
parameters/diagnostics, exact target and simulated submission reproduction, tear
classifications, execution residuals, and required recorded-refresh/raster evidence
wherever that comparison applies. A mostly matching timeline or an output JSON file
alone is not a pass. Schema 3/4 remain readable for historical fixed replay but cannot
satisfy the strict current exact-baseline gate.

An exact baseline demonstrates deterministic reproduction under the trace's recorded
model and evidence. It cannot correct inaccurate instrumentation or establish that a
candidate policy would produce the same host, network, decoder, GPU, native
presentation, or panel events. Native result/counter and raster fields are
validity-tagged; absence is not success. The Windows/raster readiness gate is narrower
and backend-specific, so a reproducible Vulkan, Composition, or Metal capture can fail
it. Raster classifications describe software-observed/modelled phase exposure, not
optical confirmation of tearing. DXGI refresh sequence counts and timestamps must keep
their recorded meanings; in particular, SyncQPCTime is paired with SyncRefreshCount,
while PresentRefreshCount is a counter rather than the timestamp of the latch.

### 13.3 Counterfactual model limits

Schema 5 does not record per-frame lost-packet counts, and replay reconstructs frames
with zero lost packets. Live scheduling excludes loss-affected frames from
readiness-prediction and smoothness evidence. Replay therefore cannot reproduce
loss-aware eligibility: a lossy partial-frame capture may fail exact diagnostic
matching, and an exact loss-free capture does not validate policy behavior under packet
loss.

Fixed `vrrreplay` retains captured frame admission and lifecycle while changing
controller decisions. Its `worker-occupancy-v1` decision-time model adjusts candidate
decision times using simulated prior submissions and captured post-submission worker
gaps when the recorded worker was occupied. This accounts for part of worker occupancy
but does not synthesize changed stale-frame shedding, decoder backpressure, queue
admission, acquisition behavior, GPU cost, or a complete alternate renderer lifecycle. A
`worker_saturated` scenario has a median occupancy shift beyond one source period; treat
its latency and cadence as outside the useful prediction range because fixed admission
cannot shed frames as the live worker can.

`vrrqueuesim` is a separate exploratory all-arrival event simulation. It runs arrivals
through the production controller and simulated bounded queue, but does not execute the
native worker or renderer. It shares later stale checks but does not reproduce early
queue pruning or service-aware decode-age handling; it reuses captured
preparation/native service samples by service ordinal, keeps captured GPU-ready times
fixed, and cannot predict changed backpressure or native costs. Its presentation metrics
are CPU submission metrics, not verified scanout. Its `baseline_exact` is always false;
passing scenario assertions is not an exact replay. Do not infer fixed-refresh behavior
from the replay `stock_*` comparator, and do not treat either simulator as physical
scanout proof.

Interpret cadence metrics by what they compare. Presented-jerk fields measure changes
between successive presented intervals and the fraction above 2 ms;
sender-spacing/residual fields measure agreement with host timestamps. Report presented
jerk first when discussing visible cadence, because low sender residual alone does not
establish smooth presentation. Replay summaries provide distribution percentiles; use
the timeline only when frame identity or a needed per-frame classification is required.
Replay's presented-jerk tracker excludes source or local decode/arrival intervals over
25 ms and resets continuity across those gaps. Report its pair coverage and the
excluded stalls separately; a low jerk score does not establish smoothness through
those gaps. The client-spacing metric retains local stalls when source cadence is
steady (see §14).
Keep synthetic scenario results separate from observed session measurements.

## 14. Metrics and a useful investigation method

For graph interpretation, causal troubleshooting, controlled tests, and capture
analysis, see the [client timing troubleshooting
guide](docs/client-timing-troubleshooting.md). Identify the executable and capture
policy before applying current source behavior to an older session.

### Read the measurements at their boundaries

`Incoming smoothness (host)` is a soft consistency score over the latest 30 valid RTP
source intervals, measured at decode-unit ingress before decoding and pacing. It uses
population variance around that window's own mean and the curve `100 / (1 + (sigma_ms /
6)^4)`. The 6 ms knee is a UI heuristic, not a perceptual threshold or probability of
stutter. Stable cadence at any rate can score 100%. It requires 30 intervals (31
consecutive frames); non-adjacent frame numbers, repeated RTP timestamps, or
backward/ambiguous RTP deltas break qualification and show `N/A` until the window
refills. This observer has no separate timestamp-validity input. RTP and
frame-number wrap are handled. This score describes source timestamp consistency,
including host capture behavior; it does not inspect image content or establish visible
smoothness.

The timing graph shows planned target intervals, successful client submission intervals,
and matched OS `DisplayEvent` intervals in separate lanes. It keeps up to 240 submitted
observations, uses one shared scale centered on the visible median planned interval,
draws deviations within 1 ms flat, and clips values outside its usual ±2 ms range. A
display interval requires valid feedback for both adjacent observations in the same
generation and backend; missing feedback is a gap. Delayed feedback is attached to its
original submission. These are timing observations, not optical scanout measurements.
Flat intervals can coexist with high fixed latency or repeated image content; neither
the graph nor FPS averages examines the images. See the guide for graph limits and
symptom patterns.

Ordinary drop statistics describe different events. Network frame drops count missing
frame-number observations; client pacing drops are local output discards; pre-decode
skips are stale compressed work skipped locally. They have different denominators and
must not be added together or treated as interchangeable network loss. The displayed
network percentage uses inferred missing frame numbers over `totalFrames`; pacing drops
use `decodedFrames`; pre-decode skips use `totalFrames`. The average network latency row
is control-connection RTT and variation, not one-way video transit or per-frame UDP
delivery time.

PyroWave can deliver a frame with unrecovered detail holes, so whole-frame loss can
remain zero while the image blurs or shimmers. The separate shimmering warning counts
unrecovered data-packet placeholders across delivered PyroWave frames, not lost frames.
Over packet-counted windows of about three seconds, it appears at at least 30% loss in
one window or at least 15% in two consecutive windows; it clears at 5% or less. It
resets after the path is inactive or observations stop for more than 2.5 seconds. The
warning is enabled by the client pacing warnings setting. Such frames remain delivered.
Their lost-packet count makes their readiness/prediction evidence ineligible to
authorize buffer growth or clean recovery, while the resulting output spacing can still
enter revision 9 interval-quality scoring. This does not restore detail or guarantee
regular display timing.

The client pacing warning is also conditional. It requires qualified interval
measurements, the applied buffer at its cap, quality at or below 99%, and a fresh
late-preparation or client-drop event. A qualified overloaded service window selects
“Client processing cannot keep up”; otherwise the warning reports the buffer limit. It
waits at least three seconds after activation and two seconds of candidate evidence,
rate-limits a new warning to 30 seconds, and clears after five seconds without a
candidate while its qualification gates remain met; clearing a gate hides it
immediately. It is not triggered by a poor score alone. Its bitrate/codec and Smooth
suggestions are diagnostic prompts, not a diagnosis of the faulty stage.

### Interpret client quality, display timing, and latency separately

Current revision 9 `Client timing` / `Smoothness` is severity-weighted client interval
quality over the preset's history window. For each valid adjacent submission pair, it
compares actual and intended intervals, applies the configured tolerance (500 µs by
default) to that pair's error, and weights excess severity by evaluated time. The
overlay's one-second mean interval error is a separate diagnostic. The score is not the
share of perfect frames, not a frame-drop rate, and not physical display smoothness.
Unobserved sequence gaps are excluded; report the separate 30-second drop count and
qualification/sample coverage. The longer score window can retain earlier error after
the current second improves. A PyroWave loss frame can affect interval quality, but its
readiness/prediction evidence cannot authorize buffer growth or clean recovery.

`Present timing issues` is a separate diagnostic of post-submission variation. It scores
only adjacent submitted frames with matched display feedback. It compares display
interval error against the larger of planned spacing and the panel's fastest reported
display period, subtracts both observations' timing uncertainty, and counts an issue
only when display error exceeds both tolerance and submission error by more than
tolerance. A hitch is a counted interval with added error at least one planned frame and
one display period. It reports the issue percentage with numerator/denominator, hitch
count, and worst added error. Scoring pauses when the source period or submitted interval
exceeds the controller's 20 ms low-refresh-compensation floor, and for 250 ms of settling
after the last such interval. It uses OS-reported display
events, not physical-panel measurements. It does not lower Smoothness or request buffer
changes: production buffering responds to eligible pre-submission readiness evidence.

In the advanced VRR overlay, per-frame averages are over successfully presented VRR
frames. `GPU decode wait` is explicit synchronization wait; `Frame queue` is queue
residence plus pacing/other time; `Rendering` is preparation plus the submission call.
The nested queued/pacing and prepare/submit rows partition their displayed totals.
For prepared-ahead frames, preparation includes offscreen renderStart-to-ready time plus
pacing-thread activation; `GPU decode wait` uses the offscreen stage's decode wait,
while the pacing-thread trace keeps that wait at zero.
GPU-ready wait is averaged only over valid samples and reports its coverage against
presented frames. On Windows, residual present-ready waiting is inside the
submission-call/rendering time. Some Linux paths have no CPU output-ready sample. These
CPU wait and call durations are not total GPU execution. Applied buffer is a schedule
allowance, not another processing stage to add. For sums, use one common frame
population; separate stage percentiles do not add to a total percentile. The
non-advanced/ordinary rows retain their existing rendered-frame denominator.

### Use replay and captures for the question they answer

For actual capture analysis, re-enumerate both `%USERPROFILE%\vrr-traces` and
`\\allytwo\ChaseShare\vrr-traces` immediately before selecting the newest completed
capture unless a specific file is named. Record its full path, size, UTC modification
time, and SHA-256; match sidecars by the complete trace basename. Run a fresh exact
baseline with the current replay binary, check its exit code plus
`capture.recorded_sequence_integrity_valid` and `fidelity.baseline_exact`, and inspect
the matching launcher sidecar. A missing required trace row, false integrity/exact flag,
or nonzero exit makes the capture exploratory rather than strict A/B evidence.

Replay's presented-jerk metrics use submission timing as a presentation proxy; replay
does not generate optical display events. Lead discussion of cadence with
`replay_presented_jerk_*`, `original_presented_jerk_*`, and `stock_presented_jerk_*`,
including p99.5/p99.9/p99.95 tails where available and the share of adjacent intervals
changing by more than 2 ms. This reflects presented cadence more directly than sender
residual, but includes game-driven cadence changes and does not prove optical
smoothness. Only consecutive intervals whose source and local decode/arrival gaps are
each at most 25 ms contribute; larger gaps reset continuity. Report
`presented_jerk_pairs` coverage and `source_stall_pairs` alongside the tails; the summary
has no dedicated local-stall count, so inspect trace/timeline decode-arrival gaps when
needed. Report sender-spacing fidelity separately.

The main replay distributions, including presented jerk, use full-population histograms:
1 us buckets through 100 ms, 100 us through 1 s, and 1 ms through 60 s. Quantiles report
bucket upper bounds; the overflow bucket reports the observed maximum. Supplemental
cadence-band, raster, and paired-delta distributions instead use a deterministic sample
capped at 32,768 values and expose `sample_count` and `quantiles_approximate`. Check those
fields before treating rare-tail changes as exact; their count, moments, minimum, and
maximum still include every observation.

The 3 ms `simulation.sender_cadence.client_spacing_accuracy_percent` uses
`100 * (1 - client_spacing_errors_over_3ms / client_spacing_pairs)` when pairs are
present, otherwise 0. Errors are intervals differing by more than 3 ms; the metric
excludes source intervals over 25 ms, reported as `source_stall_pairs`, but does not
excuse long local arrival gaps when source cadence is steady. Historical
`spacing_accuracy_percent` retains its older sender/arrival stall exclusions. Do not use
either sender metric alone to claim smooth motion. Sparse or absent native-window samples
cannot prove a visible-smoothness target even when observed misses are zero.

For a tuning sweep, keep untouched baseline and candidate outputs separate and batch
named scenarios in one config using replay's own parallelism. Compare presented-jerk
tails, sender cadence residuals, decode-to-submission mean and tail latency, submission
drift, drops, modeled interval violations, raster bounds, and saturation. Ignore
saturated scenarios as live latency predictions: fixed recorded admission can fall
behind where a running client would shed work. Report source stalls separately. Choose a
latency/cadence tradeoff, then evaluate nominal and injected decision, preparation,
submission, and scheduler disturbances with explicit safety and latency bounds. Severe
synthetic-fault latency is not normal operating latency. Replay cannot validate
alternate packet assembly, codecs, network adapters, GPU work, or physical display
behavior. Use a timeline only when per-frame causality or a metric absent from the
summary is needed; perform live confirmation for claims about visible output.

For symptom investigations, start from a repeatable gameplay workload and locate the
first boundary that changes: packet delivery and assembly, decoder queue/service, GPU
readiness, preparation, scheduling, submission, or matched display feedback. Preserve
the exact executable/configuration and compare the same scene and output mode. Read
interval tails, drops, sample coverage, and latency together; an average FPS or rounded
quality score alone can conceal the incident. A larger buffer can absorb eligible
variation within sustained capacity, but cannot create missing image data, increase
throughput, repair source stalls, or correct post-submission work.

## 15. Tests, deployment boundaries, and maintenance

The application/package build does not compile the opt-in test tree. `tests/tests.pro`
registers it only when qmake is invoked with `CONFIG+=tests`; that tree includes update,
VRR, haptics, and PyroWave projects, plus the controller-navigation project on macOS or
Unix when SDL2 is available. `tests/vrr/vrr.pro` adds platform-specific targets: macOS
display timing; Windows composition probe; and Linux Vulkan, Wayland, Gamescope repaint,
GPU-performance, and GPU-trace tests when their development packages are present. The
common Windows VRR targets include 16 `tst_*.exe` test programs; macOS adds
`tst_macdisplaytiming`; Linux adds the targets permitted by the installed packages.
`vrrreplay` and `vrrqueuesim` are command-line tools, not `tst_*` suites, and
`compositionprobe` is a manual Windows hardware probe rather than a deterministic test.
Treat these as registration counts, not proof that every target ran on every platform.

Windows CI configures and builds the opt-in tree, runs every discovered
`build/tests-vrr/vrr/release/tst_*.exe`, checks the diagnostic ZIP independently, and
separately runs the DualSense haptics test. It also smoke-tests the staged replay tool
and requires exact replay, sequence integrity, and exact-baseline fidelity for cold and
warm worker fixtures. Its minimum test-count check is only a floor; the project files
determine the registered programs. These hardware-free tests and replay fixtures do not
establish live host behavior, optical tearing, physical A/V synchronization, or display
behavior on a particular device. Native presenters and hardware probes require their own
platform or device validation.

For the ChaseShare Windows gaming build, the required focused gate before deploying a
VRR-related change or publishing a new release is the six deterministic executables
named in `AGENTS.md` (`tst_vrrtimingcontroller`, `tst_vrrratepolicy`,
`tst_vrrpacingworker`, `tst_vrrreplayconfig`, `tst_vrrrenderpolicy`, and
`tst_d3d11bindpolicy`) plus `vrrreplay.exe --help`; run each separately and distinguish
missing runtime DLLs from test failures. The ordinary application build does not rebuild
these opt-in diagnostics. Rebuild and stage them when their source or dependencies
change, or for a new release. Windows CI's broader registered suite and exact-replay
checks supplement this focused deployment gate.

`vrrreplay` models frame-level policy after packet reassembly; it cannot reproduce a
packet bottleneck before decode. For opt-in Linux bandwidth testing,
[moonlight-link-test.py](scripts/moonlight-link-test.py) redirects incoming IPv4/IPv6
traffic on a selected interface through an IFB device and bounded TBF queue. It tests
packet delivery and reassembly before decode, not a physical gigabit link or production
policy. The helper refuses existing ingress rules and rolls back failed setup;
activation, counters, limits, and removal are documented in
[network-link-testing.md](docs/network-link-testing.md). Use its dry run before
activation and its status output to distinguish queue backlog and drops from frame-level
timing.

For an app-only update to the existing ChaseShare build at the same version, use the
incremental build, copy the linked executable into the existing deploy tree, preserve
its dependencies, diagnostics, and `portable.dat.inactive` marker, then recreate the
portable ZIP. Confirm Moonlight is not running before copying the complete deploy tree
and ZIP to the stable `\\allytwo\ChaseShare\MoonlightPortable-x64-6.1.0-vrr-lite`
destination. Verify source and share SHA-256 hashes for the executable, replay tool,
decoder, and ZIP, and run `vrrreplay.exe --help` from the share path. A successful local
build alone is not publication. Follow the current [AGENTS.md](AGENTS.md) procedure for
commands and all staging and verification steps; `AllyShare` is only an open host-log
drop and must not receive release builds or profile/settings data. Use the full clean
release pipeline only for an explicitly requested new published release, because it
recreates the deploy tree.

When maintaining this document, compare its source baseline with the active code. Follow
resolved settings through effective formulas, intended native parameters through the
actual API call and telemetry, and frame identity, clock units, queue ownership, and
lifecycle order through diagnostics and replay. Keep production policy distinct from
fallbacks, historical replay modes, and experiments. Revisit exact replay when trace
schema, feedback, policy state, or execution boundaries change, and update both
share-root launchers when their contract changes. Keep native Present arguments and
telemetry aligned; validate the native call boundary and real platform behavior before
interpreting optical results.

The durable debugging method is to trace an observed frame through the pipeline, find
the first boundary that differs from its intended behavior, and follow that difference
into later frames and feedback. This keeps host stalls, local overload, scheduling,
native presentation, and measurement limits distinct.
