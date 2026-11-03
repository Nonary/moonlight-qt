# Windows Spatial 7.1 tests

These opt-in tests cover the renderer for existing 48 kHz, eight-channel decoded
PCM. They do not change Opus negotiation or the video pipeline. The application
feature and its endpoint selection are documented in
[`docs/windows-spatial-audio.md`](../../docs/windows-spatial-audio.md).

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

The suite checks the eight static object assignments, especially back versus
side channels; deinterleaving guards; negative, partial and oversized byte
counts; zero-filled underruns; wraparound; oldest complete-frame eviction;
reset and statistics; bitwise float preservation; and 10,000 deterministic
mixed operations against an independent deque model. The capacity is 1,440
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

They test help, argument rejection, read-only endpoint enumeration and seven
invalid renderer configurations. Stereo, 5.1, unsupported rate/duration and a
null configuration use an empty endpoint selector and must be rejected before
default-device lookup. A valid configuration uses an impossible endpoint ID.
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
.\Spatial71RendererTest.exe --endpoint-id 'ID' --silence --seconds 30 --gap-at-ms 10000 --gap-ms 500
.\Spatial71RendererTest.exe --endpoint-id 'ID' --silence --seconds 10 --cycles 10
```

Tones use one channel per second: FL, FR, FC, LFE, BL, BR, SL, SR. Peak amplitude
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
A/V timing require client testing. Backend default-off and explicit `ML_AUDIO`
precedence are currently verified by source review, not a simulated factory test.
