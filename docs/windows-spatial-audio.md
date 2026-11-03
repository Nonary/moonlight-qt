# Optional Windows Spatial Audio 7.1 output

This Windows desktop renderer sends already-decoded 7.1 PCM to eight static
`ISpatialAudioObject`s through `ISpatialAudioClient`. It is disabled by default.
The normal SDL renderer, Opus decoder and channel negotiation are unchanged.
There are no host, driver, video, HDR, VRR or PyroWave changes.

## Enable and select an output

Select **7.1 surround sound** in Moonlight. Configure the desired spatial sound
provider on a compatible HDMI/DisplayPort output in Windows beforehand.
Moonlight neither enables a provider nor installs drivers or changes defaults.
The endpoint must support all eight static objects and mono float32 at 48 kHz.

Start Moonlight with the opt-in set for that process, for example in PowerShell:

```powershell
$env:MOONLIGHT_SPATIAL_AUDIO = '1'
.\Moonlight.exe
```

If `MOONLIGHT_SPATIAL_AUDIO_ENDPOINT` is unset or empty, the renderer resolves
the **current default render endpoint (console role)** when it initializes. It
requires an active digital-display output; it never picks an arbitrary monitor
or changes the default. A default-device change does not move an already working
stream; the default is resolved again at the next initialization. To pin a
specific output instead:

```powershell
$env:MOONLIGHT_SPATIAL_AUDIO = '1'
$env:MOONLIGHT_SPATIAL_AUDIO_ENDPOINT = '<endpoint ID>'
.\Moonlight.exe
```

Use the optional test utility's `--list-endpoints` command to list active
digital-display outputs, their names/IDs and the current default. Listing is
read-only and does not start a stream. See [test instructions](../tests/audio-spatial71/README.md).
Endpoint names are not used as identifiers. No manufacturer or model is assumed.

Only the exact opt-in value `1` enables this backend. Explicit `ML_AUDIO=sdl`
continues to take precedence. Unsupported formats (including stereo and 5.1),
unavailable devices or spatial initialization failures use the existing renderer
selection path. SDL fallback can use its regular default output; it does not
necessarily address the pinned spatial endpoint.

To disable the opt-in in the current PowerShell session:

```powershell
Remove-Item Env:MOONLIGHT_SPATIAL_AUDIO -ErrorAction SilentlyContinue
Remove-Item Env:MOONLIGHT_SPATIAL_AUDIO_ENDPOINT -ErrorAction SilentlyContinue
```

## Output and recovery

Decoded channel order is **FL, FR, FC, LFE, BL, BR, SL, SR**. Each channel maps to
its corresponding static object. There is no additional Opus remapping, gain,
clipping, height synthesis or dynamic-object metadata. The spatial object mask
`0x1FE` is distinct from the WAVE speaker mask `0x63F`.

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

`Spatial 7.1 active` confirms successful activation and an actual eight-object
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
The generalized default-endpoint selection is additional to that explicit-ID
hardware test. Automated tests do not substitute for hardware/provider testing.

The application supplies ordinary **7.1 audio**, not native Atmos passthrough,
7.1.4 content, MAT capture or bit-identical HDMI transport. The selected Windows
provider decides how to render/encode it. An Atmos indicator is an observed
output-mode result, not verification of source Atmos metadata or speaker routing.
Queue/fill timings do not measure acoustic latency or A/V synchronization. Audio
continues to follow the device clock independently of video presentation.

The implementation uses documented Microsoft Spatial Audio APIs. Development
was informed by SpatialAudioLab experiments, but no SpatialAudioLab source or
dependency is included. New source follows this repository's existing license.
