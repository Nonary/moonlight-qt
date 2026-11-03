# Optional Windows Spatial Audio 5.1/7.1 output

This Windows desktop renderer sends already-decoded 5.1 or 7.1 PCM to six or eight static
`ISpatialAudioObject`s through `ISpatialAudioClient`. It is disabled by default.
The normal SDL renderer, Opus decoder and channel negotiation are unchanged.
There are no host, driver, video, HDR, VRR or PyroWave changes.

## Enable and select an output

In Moonlight, open **Settings > Audio Settings**, select **5.1 surround sound**
or **7.1 surround sound**, and check **Use Windows Spatial Audio**. The checkbox
is shown only on Windows and is available with either surround layout. It is off by default and saved
with Moonlight's other preferences, so normal use needs no launch arguments or
environment variables. Launching `Moonlight.exe` directly works; no command
script or test utility is required. Changes apply when the next streaming session starts.
Switching to stereo keeps the saved checkbox value but uses regular audio.

Configure the desired spatial sound provider on a compatible HDMI/DisplayPort
output in Windows beforehand.
Moonlight neither enables a provider nor installs drivers or changes defaults.
The endpoint must support the requested six or eight static objects and mono
float32 at 48 kHz.

The GUI renderer resolves the **current default render endpoint (console role)**
when it initializes. It
requires an active digital-display output; it never picks an arbitrary monitor
or changes the default. A default-device change does not move an already working
stream; the default is resolved again at the next initialization. The application
does not use the prototype's `MOONLIGHT_SPATIAL_AUDIO_ENDPOINT` override, so an old
device selection cannot pin it to a disconnected output after an HDMI cable change.
The standalone test utility still supports explicit endpoint IDs for diagnostics.

Use the optional test utility's `--list-endpoints` command to list active
digital-display outputs, their names/IDs and the current default. Listing is
read-only and does not start a stream. See [test instructions](../tests/audio-spatial71/README.md).
Endpoint names are not used as identifiers. No manufacturer or model is assumed.

The saved checkbox is the activation control; the prototype's
`MOONLIGHT_SPATIAL_AUDIO` toggle is no longer used. Explicit `ML_AUDIO=sdl`
continues to take precedence. Unsupported formats (including stereo),
unavailable devices or spatial initialization failures use the existing renderer
selection path. SDL fallback uses its regular output selection.

To disable spatial output, uncheck **Use Windows Spatial Audio** before
starting the next stream.

## Output and recovery

Decoded channel order is **FL, FR, FC, LFE, BL, BR** for 5.1, with **SL, SR**
added for 7.1. Each channel maps to its corresponding static object. The 5.1
back-channel positions follow Moonlight's negotiated WAVE mask `0x3F`; they are
not silently relabeled as side channels. There is no additional Opus remapping, gain,
clipping, height synthesis or dynamic-object metadata. The spatial object mask
is `0x19E` for 5.1 and `0x1FE` for 7.1, distinct from their WAVE speaker masks
`0x3F` and `0x63F`.

A dedicated MTA worker owns all COM interfaces, event updates and cleanup. A
preallocated interleaved queue holds at most 1,440 complete frames (30 ms at
48 kHz). Overflow discards oldest frames; missing frames are replaced by zeros.
All channels advance together, and every object is serviced on every update,
including silent/muted periods. The 480-frame/10-ms prefill target is checked at
render events, so it is not a hard bound on startup delay.

API failures and event timeouts reach the existing Session recovery path through
`submitAudio(false)`. A non-null decoder buffer remains available after worker
failure so Session can observe that result. Recovery retries spatial output and
can fall back to SDL. After a successful SDL fallback, starting a new session is
required to try spatial output again. Existing downtime-drop behavior is retained.
The worker's wait is cancellable; an in-progress system COM/driver call cannot be
forcibly cancelled safely, so initialization/cleanup has no hard wall-time bound.

## Diagnostics and validation limits

`Spatial Audio active` confirms successful activation and an actual six- or eight-object
update. Moonlight also opens the renderer during its audio preflight, which has
zero submitted PCM frames. A streaming result must include nonzero submitted
and consumed counters from the actual session.

Shutdown statistics report queued/consumed frames, overflow drops, missing-frame
silence, peak occupancy, update sizes and CPU/event timing. Missing-frame silence
can include an idle producer, intentional focus-mute and re-priming. Received
PCM containing zeros still counts as consumed data. These counters do not alone
identify audible dropouts, network loss or the cause of missing input. Queued
frames can also be abandoned during immediate stop or device recovery without
incrementing the overflow counter.

The explicit-endpoint prototype was exercised in a real Windows 11 7.1 streaming
session lasting approximately four minutes. The user observed an Atmos indicator
on the HDMI/eARC soundbar during streaming. Two deliberate DTS/Atmos provider
changes triggered successful spatial reinitializations (12 and 21 ms spent in
renderer initialization, not total audible interruption). An eight-second
standalone channel-tone run had no missing-frame or overflow events. The stream
had no queue overflows, but accumulated missing-frame silence whose cause was not
localized; uninterrupted long-duration playback is still a validation item.

The subsequent GUI/default-endpoint build was also tested on a Windows client
connected through a Samsung TV and eARC soundbar. The tester confirmed working
playback with **both 5.1 and 7.1 selected** and reported very good sound quality.
This is a user-reported listening test, not an instrumented speaker-routing or
latency measurement.
Automated tests do not substitute for hardware/provider testing.

The application supplies ordinary **5.1 or 7.1 audio**, not native Atmos passthrough,
7.1.4 content, MAT capture or bit-identical HDMI transport. The selected Windows
provider decides how to render/encode it. An Atmos indicator is an observed
output-mode result, not verification of source Atmos metadata or speaker routing.
Queue/fill timings do not measure acoustic latency or A/V synchronization. Audio
continues to follow the device clock independently of video presentation.

The implementation uses documented Microsoft Spatial Audio APIs and follows
this repository's existing license.
