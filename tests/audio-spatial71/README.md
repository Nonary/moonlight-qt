# Windows Spatial 5.1/7.1 tests

These opt-in tests cover the renderer for existing 48 kHz, six/eight-channel decoded
PCM. They do not change Opus negotiation or the video pipeline. The application
feature and its endpoint selection are documented in
[`docs/windows-spatial-audio.md`](../../docs/windows-spatial-audio.md).
For normal streaming, start `Moonlight.exe` directly and enable **Use Windows
Spatial Audio** in Audio Settings with 5.1 or 7.1 selected. No launcher or
environment variable is required. The tools below are optional developer tests.

## Portable queue suite

The C++ test includes the production `spatialaudioqueue.h`. It requires no Qt,
SDL, Windows API or audio device. For example, on Linux:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pedantic \
    tests/audio-spatial71/tst_spatial71queue.cpp -o Spatial71QueueTests
./Spatial71QueueTests
```

A standalone qmake project is also provided. From an x64 MSVC developer command
prompt with Qt's `bin` directory on PATH:

```bat
mkdir build\spatial-queue
cd build\spatial-queue
qmake ..\..\tests\audio-spatial71\spatial71-queue.pro CONFIG+=spatial_analyze
nmake
Spatial71QueueTests.exe
```

The suite checks both six and eight static object assignments, especially back
versus side channels; deinterleaving guards; negative, partial and oversized byte
counts; zero-filled underruns; wraparound; oldest complete-frame eviction;
reset and statistics; bitwise float preservation; and 10,000 deterministic
mixed operations per layout against an independent deque model. It also checks
6/8-channel transitions and rejection of invalid layouts without modifying
queued data or statistics. The capacity is 1,440
frames (30 ms at 48 kHz), not a measured end-to-end latency limit.

## Actual renderer harness

The Windows x64 harness compiles the same `spatialaudio.cpp` as Moonlight. It
supplies decoded 240-frame packets at the renderer interface, without an Opus
decoder, a host, a network connection or video. Qt Core and the repository's
Windows SDL dependencies must be available, as for the application build.

From the repository root in an x64 MSVC developer command prompt:

```bat
mkdir build\spatial-renderer
cd build\spatial-renderer
qmake ..\..\tests\audio-spatial71\spatial71-renderer.pro CONFIG+=spatial_analyze
nmake
Spatial71RendererTest.exe --help
```

Both MSVC projects use `/W4 /WX`. `CONFIG+=spatial_analyze` adds `/analyze`.
Put Qt Core and SDL runtime DLLs beside the harness or their directories on PATH.

Run the offline checks from the repository root:

```sh
python tests/audio-spatial71/test_harness_offline.py --binary build/spatial-renderer/Spatial71RendererTest.exe
```

They test help, argument rejection, read-only endpoint enumeration, generated
6/8-channel PCM and eleven failure configurations. Stereo, invalid channel
counts, unsupported rate/duration and a null configuration use an empty endpoint
selector and must be rejected before default-device lookup. Valid 5.1 and 7.1
configurations use an impossible endpoint ID. The producer test checks silence,
each tone's channel, amplitude and buffer bounds, without activating audio.
Failure checks require non-null decoder staging and `submitAudio(0) == false`,
so Session can reach its recovery path. They do not activate an audio stream
or exercise a complete Session/SDL fallback.

The separate `Spatial audio tests` workflow builds this harness on Windows and
runs the offline checks. A Linux job runs the portable queue with ASan/UBSan.
It does not upload binaries or require physical audio hardware.
On hosted runners, `--allow-unavailable-discovery` reports the optional inventory
as **SKIP**, not PASS, for known unavailable audio-service HRESULTs. Other errors
remain failures. No service is started or reconfigured. Even successful inventory
only verifies metadata enumeration, not Spatial Audio support or playback.

## Settings and UI tests

`spatial71-settings.pro` builds `Spatial71SettingsTests.exe` with Qt Quick,
Quick Controls 2 and Qt Test. Run with `QT_QPA_PLATFORM=offscreen` and
`QT_QUICK_BACKEND=software` for a display-independent test. It uses temporary
INI settings and does not initialize audio, discovery, or streaming.
From the repository root in an x64 MSVC developer command prompt:

```bat
mkdir build\spatial-settings
cd build\spatial-settings
qmake ..\..\tests\audio-spatial71\spatial71-settings.pro
nmake
set QT_QPA_PLATFORM=offscreen
set QT_QUICK_BACKEND=software
set QT_QPA_FONTDIR=%WINDIR%\Fonts
Spatial71SettingsTests.exe -o spatial-settings-results.txt,txt
type spatial-settings-results.txt
```

Qt's runtime DLLs, QML modules, and plugins must be available. The Windows
`Spatial audio tests` job builds and runs this target with the installed Qt.
The settings executable uses the project's regular warning flags. The stricter
`/W4 /WX /analyze` flags above apply to the renderer and queue targets.

The suite links the real `StreamingPreferences` and checks default-off,
true/false persistence across all three channel layouts, and preservation of
the other audio preferences. It executes the Audio Settings group and the
close/save handler directly from `SettingsView.qml`, using keyboard events to
toggle the actual checkbox in 5.1 and 7.1 and disable it in stereo, then saves
and reopens the controls. Unrelated system/gamepad services are inert test substitutes.
This tests the audio controls, not navigation through the complete settings
page or runtime renderer selection. The original QML source must be present.

Optionally set `SPATIAL_SETTINGS_SCREENSHOT_DIR` to a local output directory
to capture the real unchecked and checked controls. CI does not request
screenshots or publish build/test artifacts. Endpoint IDs are not printed by
the offline test runner.

## Manual playback on the intended client

List active HDMI/DisplayPort endpoints without opening a stream:

```powershell
.\Spatial71RendererTest.exe --list-endpoints
```

Copy the exact returned ID into the commands below. Alternatively, replace
`--endpoint-id 'ID'` with `--default-endpoint` to use the current Windows default
only if it is a compatible HDMI/DisplayPort render endpoint. That option never
selects the first available HDMI device or changes the Windows default.

```powershell
.\Spatial71RendererTest.exe --endpoint-id 'ID' --silence --seconds 60
.\Spatial71RendererTest.exe --endpoint-id 'ID' --tones --seconds 8
.\Spatial71RendererTest.exe --endpoint-id 'ID' --channels 6 --silence --seconds 60
.\Spatial71RendererTest.exe --endpoint-id 'ID' --channels 6 --tones --seconds 6
.\Spatial71RendererTest.exe --endpoint-id 'ID' --silence --seconds 30 --gap-at-ms 10000 --gap-ms 500
.\Spatial71RendererTest.exe --endpoint-id 'ID' --silence --seconds 10 --cycles 10
```

Tones use one channel per second: FL, FR, FC, LFE, BL, BR, plus SL/SR for eight
channels. The default is eight channels. Six channels uses Moonlight's back pair
and does not create silent side objects or upmix to 7.1. Peak amplitude
is -30 dBFS with 10 ms fades; LFE uses 60 Hz and the others 440 Hz. Confirm the
audible speaker direction separately. There are no synthesized height channels.

stdout reports producer JSON and stderr contains the renderer's diagnostics.
The intentional gap should produce silence and then resume. Reopen cycles
should stop and join cleanly. A manual device/provider change may cause the
standalone harness to return failure; in Moonlight, Session owns recreation and
fallback. A longer `--seconds 3600` run can examine stability. Interrupting the
process with Ctrl+C does not establish a completed run or graceful teardown.

Queue frames, event intervals, fill duration, producer lateness and stop/join
duration are software observations. They do not measure acoustic latency or
A/V synchronization. Underrun counts include absent PCM submissions, deliberate
gaps, focus-related mute and re-priming; received PCM containing zeros counts as
consumed data. The 480-frame/10 ms priming threshold is checked at callbacks,
so it is not a hard 10 ms startup-delay bound.

The test boundary cannot establish Dolby provider identity, MAT bitstream
contents, physical channel mapping or source height metadata. Actual receiver
mode, audible channels, device changes, complete Moonlight fallback and physical
A/V timing require client testing. Renderer selection and explicit `ML_AUDIO`
precedence are verified by source review, not a simulated factory test. The
preference's default-off and persistence behavior have executable tests above.
