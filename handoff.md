# Handoff: latency metric boundaries and the input-to-photon gap

## Summary

The latency telemetry now measures the video pipeline from the host compositor-present timestamp to the midpoint of client display scanout. It does not measure the complete causal path from a controller input to the physical display emitting the resulting pixels.

The measured video-pipeline number is internally consistent. In the post-restart test it was 23.2 ms. The user's independent input-to-photon result is approximately 35–40 ms. The missing approximately 12–17 ms is not an unexplained video-transport delay: it belongs primarily to the input-to-game-to-compositor portion before Sunshine's first timestamp, plus physical panel response after the client's scanout timestamp.

The overlay must therefore not call the video-pipeline subtotal “end-to-end latency.” The current source and shared test build call it `Estimated video-pipeline latency` and explicitly list the stages that are not included in input-to-photon latency.

Do not make the number match a particular test by adding a fixed frame duration. That would be a model with game-specific assumptions, not a measurement.

## Confirmed evidence

### Runtime timeline

The relevant client log is:

`\\allytwo\chaseshare\MoonlightPortable-x64-6.1.0-vrr10-framebuffer\Moonlight-1785606461.log`

1. The client correctly appended `clientLatencyTelemetry=1` to its launch/resume request.
2. The first completed run used the old Sunshine process that had remained alive after the new files were installed. It therefore used the normal short frame header and reported host transport as unavailable.
3. Vibeshine restarted at 2026-08-01 12:52:07. Its log is:

   `C:\Program Files\Sunshine\config\logs\sunshine-20260801-125207-002.log`

4. A new streaming session began against the restarted host at 12:52:34. The completed client statistics then contained host transport telemetry, proving that negotiation, the extended frame header, and client parsing worked.

### First completed run, before host-process restart

- Host processing average: 1.8 ms
- Host capture average: 1.0 ms
- Network RTT: 1 ms
- Decode average: 0.42 ms
- Frame queue average: 4.46 ms
- Render/presentation scheduling average: 0.66 ms
- Decode-ready to scanout-midpoint average: 16.89 ms
- Reported partial video path: 20.6 ms
- Host transport: unavailable

### Second completed run, after host-process restart

- Host processing average: 1.9 ms
- Host capture average: 2.1 ms
- Host transport preparation average: 0.1 ms
- Network RTT: 3 ms, of which the video-path estimator uses RTT/2 = 1.5 ms
- Decode average: 0.40 ms
- Frame queue average: 4.75 ms
- Render/presentation scheduling average: 0.71 ms
- Decode-ready to scanout-midpoint average: 17.04 ms
- Reported video path: 23.2 ms

The small residual after summing the printed values is client reassembly time plus rounding. Host transport was only 0.1 ms and cannot explain a 12–20 ms discrepancy.

The frame-queue and render/presentation diagnostic lines are not added separately when DXGI scanout feedback is available. They are already contained within the measured decode-ready-to-scanout interval. Adding them again would double-count client presentation.

### This test was fixed refresh, not VRR

The client log shows a 120 FPS stream, a 120 Hz display, and `V-sync enabled`. There was no adaptive-refresh headroom because source and display rates were equal. The run therefore exercised fixed 120 Hz presentation.

VRR and fixed presentation now use the same decode-ready-to-scanout endpoint when DXGI feedback is available. This makes their video-pipeline figures comparable, but it does not turn either figure into input-to-photon latency.

## What the current metric measures

With all telemetry available, the estimated video-pipeline latency is:

```text
host compositor-present to capture-ready
+ host capture/encode processing
+ host header/FEC/encryption/first-send preparation
+ estimated host-to-client network transit (RTT / 2)
+ client packet reassembly
+ client decode
+ client decode-ready to scanout midpoint
```

The host sends its stages as durations, so no host/client clock comparison is required. The client stages share Moonlight's local monotonic clock.

The Sunshine transport term is an opt-in extended-header field. It measures the preceding frame from frame-header construction through header/FEC/encryption preparation and the first packet send. A preceding-frame sample avoids a circular dependency because the current header is itself protected by the current frame's FEC. Averaging remains representative in steady state.

On D3D11, Moonlight associates the decoded frame with a DXGI Present ID, later matches that ID to DXGI frame statistics, reconstructs scanout start, and adds half a physical scanout period to represent the average pixel position. Frames replaced before display are discarded from scanout statistics. VRR samples are accepted only when DXGI supplies a direct timing anchor; the code does not apply a fixed-refresh interval across unknown variable blanking periods.

If trustworthy scanout feedback is unavailable, the metric stops at renderer submission and labels itself partial rather than claiming display completion.

## What it does not measure

The current video-pipeline metric excludes:

1. Client input-device polling and Moonlight input capture.
2. Client-to-host input network transit.
3. Host virtual-input delivery and scheduling.
4. The game's input sampling boundary.
5. Game simulation and CPU work caused by that input.
6. GPU rendering and any game/driver render queue before compositor presentation.
7. Physical display processing and pixel response after scanout reaches the relevant pixel.

These omissions explain why a measured 23.2 ms video path can coexist with a real 35–40 ms input-to-photon result.

## Why VRR frame latency can be measured

VRR presentation has a continuous frame identity:

```text
received video frame
  -> decoded AVFrame
  -> prepared renderer frame
  -> DXGI Present ID
  -> DXGI-confirmed displayed Present ID
  -> scanout timestamp
```

Moonlight owns or observes every boundary in that sequence. The VRR worker's prepared-frame hold is also included because the final client-presentation duration starts when decode output becomes presentation-ready and ends at scanout midpoint.

This is why the fixed/VRR video-frame path is measurable without external hardware.

## Why input-to-photon cannot be measured generically

An input does not retain a comparable identity after it is delivered to the operating system and game:

```text
input packet ID
  -> virtual controller/keyboard event
  -> game-owned input queue
  -> unknown sampling point
  -> unknown simulation/render work
  -> one of several future compositor frames
```

Sunshine can timestamp when it receives an input packet. It can also timestamp captured compositor frames. What it cannot determine is which captured frame is the first frame whose pixels were causally changed by that input.

Attaching the most recent input timestamp to the next captured frame would not solve this:

- The game may already have sampled input for that frame.
- The input may not be consumed until a later tick.
- CPU/GPU queues may add one or more frames.
- Late input sampling or NVIDIA Reflex may reduce the delay.
- A particular input may cause no immediately identifiable visual change.
- Continuous mouse/controller traffic makes “most recent input” essentially unrelated to a specific visible response.
- Frame-generated images do not sample new game input and must not be treated like new source frames.

The missing item is not a timestamp API. It is a causal marker connecting a specific input to a specific rendered frame.

DXGI likewise reports presentation/scanout timing, not the display's internal processing or the moment a physical pixel reaches its new luminance.

## Why adding one frame is not a measurement

Frame duration is:

```text
frame period in milliseconds = 1000 / source FPS
```

Examples:

- 50 FPS = 20.0 ms per source frame
- 60 FPS = 16.67 ms per source frame
- 120 FPS = 8.33 ms per source frame

At 50 FPS, an input can arrive anywhere within the 20 ms source-frame phase. Even before considering rendering, the wait to the next input-sampling opportunity can range from nearly 0 to nearly 20 ms, averaging approximately 10 ms only if arrival phase is uniformly random. Rendering and queue depth follow that sampling point and are game/configuration dependent.

Therefore, “add one frame” is not generally correct. Depending on input-sampling strategy and queue depth, the host input-to-present portion may be less than one source frame or several source frames. Using generated FPS instead of real source FPS would be especially wrong for frame generation because generated frames do not incorporate new input.

A product could display a clearly labelled model such as “assumes one source frame of game response,” but it must not present that number as measured input-to-photon latency.

## What would make true input-to-photon measurable

At least one causal correlation mechanism is required:

### Game or engine integration

The game records an input marker and tags the first rendered frame containing its result. NVIDIA Reflex-style latency markers or an equivalent game integration can expose the otherwise hidden input/simulation/render interval.

### Controlled instrumented test application

A test application receives a uniquely identified input and renders a known visual transition tied to that identifier. Sunshine/Moonlight can then carry the identifier through the encoded frame, and the client can correlate it with scanout.

### External optical measurement

An input actuator or electrical input marker combined with a photodiode, LDAT-class device, or high-speed camera measures the actual physical start and photon response. This also captures panel processing/response that software APIs do not expose.

Without one of these, only a model or range is possible for the unobserved input/game/panel portion.

## Current implementation state

### Moonlight source

Relevant paths include:

- `app/backend/nvhttp.cpp`
- `app/streaming/video/decoder.h`
- `app/streaming/video/ffmpeg.cpp`
- `app/streaming/video/ffmpeg-renderers/presentationtiming.h`
- `app/streaming/video/ffmpeg-renderers/d3d11va.cpp`
- `app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp`
- `app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.cpp`
- `moonlight-common-c/moonlight-common-c/src/Limelight.h`
- `moonlight-common-c/moonlight-common-c/src/VideoDepacketizer.c`

The current overlay wording is:

```text
Estimated video-pipeline latency (...): N ms
Not included in input-to-photon latency: client input capture/transmission, game response/rendering, and panel response
```

The shared extracted build and ZIP were updated at:

- `\\allytwo\chaseshare\MoonlightPortable-x64-6.1.0-vrr10-framebuffer`
- `\\allytwo\chaseshare\MoonlightPortable-x64-6.1.0-vrr10-framebuffer.zip`

Current shared `Moonlight.exe` SHA-256:

`12F640AB13C326CB536B371449278C3699A62D575A1DA139C3CFBA13F3422C77`

### Sunshine source

Relevant paths include:

- `src/nvhttp.cpp`
- `src/rtsp.cpp`
- `src/rtsp.h`
- `src/stream.cpp`
- `src/stream.h`

The installed post-restart host successfully delivered transport telemetry. The full package build had already passed the web build, native build, virtual-display-driver packaging validation, MSI generation, and bootstrapper generation.

### Repository state

The latency work remains uncommitted. Sunshine also contains unrelated pre-existing dirty web UI changes that were preserved. Packaging did not add tracked driver-asset churn in the final status.

## Conclusions

### Confirmed

- The original 8–16 ms claim was incomplete because it stopped before trustworthy scanout and omitted host transport and the VRR prepared-frame hold.
- The repaired telemetry measures the video path consistently for fixed refresh and VRR when DXGI feedback is available.
- The post-restart host transport telemetry works and averaged 0.1 ms in the inspected run.
- The post-restart video pipeline measured 23.2 ms from host compositor presentation to client scanout midpoint.
- That figure is not input-to-photon latency.
- A separate 35–40 ms physical result is compatible with a 23.2 ms video path because it contains stages outside the measured boundary.

### Not established

- The exact client-input-to-host-compositor latency for the tested game.
- Which game frame first reflected a particular input.
- The game's render-queue depth or late-sampling behavior during the test.
- The display's physical pixel-response delay.
- A universal frame-count correction that applies across games, FPS values, Reflex modes, and frame generation.

## Recommended product behavior

1. Keep the measured number labelled `video-pipeline latency`, with explicit endpoints.
2. Keep the input/game/panel exclusion visible near the number.
3. Do not add a fixed frame duration to make the number resemble an external measurement.
4. If a modeled input-to-photon figure is desired, require its assumptions to be visible: real source FPS, assumed input phase, assumed render depth, and assumed panel response.
5. Prefer a game marker or controlled optical test before presenting any value as true input-to-photon latency.

## Validation plan

1. Relaunch the latest shared Moonlight executable and confirm the overlay says `Estimated video-pipeline latency`, not `end-to-end`.
2. Confirm `Host transport preparation min/max/average` remains present after a fresh Vibeshine session.
3. Confirm `Average client presentation (decode-ready to scanout midpoint)` remains present. If absent, the estimator must label the endpoint as renderer submission and remain partial.
4. Run fixed-refresh and VRR sessions at the same real source cadence. Compare the video-pipeline stages rather than comparing either value directly with input-to-photon hardware results.
5. For true end-to-end validation, use an instrumented game/test application or an external optical device and correlate a specific input with the first affected frame.
6. Reject any implementation that simply adds `1000 / FPS` without documenting that it is an assumed game-response model.
