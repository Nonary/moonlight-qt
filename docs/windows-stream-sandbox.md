# Windows stream timing sandbox

The replay trace reconstructs recorded client controller/worker decisions. The
GPU sidecar and OS trace provide independent evidence about the inputs and the
native work around those decisions. No images, encoded video payloads or audio
are recorded. Host RTP stamps describe the transmitted cadence; they do not
reconstruct the game's simulation or prove when the host captured an image.

## Capture

Close Moonlight, then run `\\allytwo\ChaseShare\Moonlight VRR Full Diagnostic.cmd`.
The batch passes the portable directory explicitly; direct PowerShell `-File`
invocation resolves an omitted directory in the script body, after automatic
script variables are initialized. Preflight must test that exact `-File` and
batch invocation, not just PowerShell's call operator.
Accept the Windows recorder UAC prompt. Moonlight starts normally, unelevated.
Connect using the same settings and pan for 45–60 seconds, then exit Moonlight
normally. Wait for postprocessing. The full launcher owns tracing; do not also
enable the in-app capture for that run. Settings and the recorded parameter
object determine the policy; this launcher does not enable Reduce judder.

Default capture includes no alignment driver probes. `-Alignment` opts into
those additional observations and must be treated as a separate experiment.
Use `-Label "RTSS Front Edge Sync"` or `-Label "RTSS previous mode"` to record
the host setting manually. Keep resolution, FPS, game scene and other settings
constant across separate captures. Never infer the host's RTSS setting from
the label or the client trace alone.

Local output is `%USERPROFILE%\vrr-traces\Moonlight-sandbox-<time>-<id>`. The
entire directory is copied to the private ChaseShare `vrr-traces\full` tree
after the application exits. There is no network recording during gameplay.
The file-mode ETL preserves the beginning rather than using a circular buffer.
The recorder stops at five minutes by default (`-MaxSeconds`, maximum 900),
below 5 GiB free disk space, if its parent exits, or when Moonlight closes.
An early limit marks coverage incomplete and never terminates the application.
ETLs can be large: the three-second desktop smoke capture was about 47 MiB.

The bundle contains raw `Windows.etl`, `PresentMon.csv`, the replay trace,
exact-baseline JSON and logs, GPU CSV sidecars, client log, display-adapter
metadata, WPR status/start/stop logs, and a manifest of binaries, overrides,
exit codes, artifact sizes and SHA-256 hashes. Keep the raw ETL: it retains
thread scheduling, ready-thread, DPC/interrupt and graphics events for WPA.
PresentMon's display/GPU metrics are derived from that ETL after recording.
No permanent tracing service or system setting is installed.

## Frame identity and clocks

All GPU sidecar CPU timestamps use `LiGetMicroseconds`; RTP PTS is a separate
90 kHz source domain. Existing main-trace frame numbers, timestamps, queue
decisions, fence observations and source rate fields remain authoritative.
The Windows additions to sidecar schema 4 are:

| Event | Meaning of `object_id`, `a` through `e` |
| --- | --- |
| `windows_clock` | object 0; PID, TID, reference QPC, QPC frequency, bracket width in ticks; begin/end are the corresponding Li time |
| `decoder_surface` | D3D11 texture pointer; frame number, pixel format, width, height, outstanding decoder frames |
| `decoder_d3d11_slice` | texture pointer; `a` is array slice |
| `d3d11_present` | swapchain pointer; PID, TID, before/after QPC ticks, QPC frequency; PTS and decoder-output time identify the prepared frame |
| `d3d11_present_result` | same swapchain/frame/times; HRESULT, sync interval, flags, decode fence value, composition-requested flag |

Existing thread spans record CPU microseconds in `b`, unavailable context-switch
counters as -1 in `c/d`, and native TID in `e`. Windows thread CPU time is coarse
accounting, not a substitute for ETW context-switch events. Diagnostics enqueue
bounded rows asynchronously; no new GPU synchronization or status queries occur.
Normal timing still has instrumentation overhead, which should be measured on
live runs rather than assumed zero.

Run `python scripts/correlate-vrr-windows.py <capture-directory>` to join the
sidecars to the PresentMon v1 QPC CSV. It requires the same PID and swapchain,
with exactly one unused Present event inside the recorded QPC bracket. It keeps
dropped frames and reports missing, ambiguous and duplicate matches instead of
guessing the nearest frame. Reconnect/probe sidecars remain separate artifacts.
Use the generated correlation CSV's PTS and decoder-output time to join the
decoded main trace. Unsupported composition/driver paths may remain unmatched.

## Fidelity gates and limits

Require exact replay exit zero, `baseline_exact`, and recorded sequence
integrity before claiming client replay fidelity. GPU sidecars need a clean
footer with zero truncation and dropped rows. Require WPR start/stop success,
full capture coverage, and inspect ETW lost-event diagnostics and PresentMon
stderr. A zero pre-stop WPR dropped-event count is useful but does not alone
prove complete final ETL/display coverage. The manifest deliberately leaves
`etw_loss_verified` and `display_correlation_verified` false pending analysis.
The correlation report separately verifies actual frame matching; a CSV's
existence alone is never proof of graphics coverage.

For a smoothing counterfactual, preserve the observed source arrivals/readiness
and replay historical revisions exactly, then change named policy parameters.
Lead with presented jerk and latency, report sender-spacing fidelity separately,
and distinguish CPU-submission timing from measured OS display timing. Replays
do not deterministically resimulate a changed Windows GPU queue, driver, DWM or
panel. The recorded admission/decode/native workload cannot predict every drop
or GPU stall a changed policy would cause. Host capture stalls are separate.
`AllowsTearing` means tearing was permitted, not that a tear was observed.
Physical tear-line confirmation still needs optical evidence; image capture
would be useful later for duplicate-content/motion questions, not as a clock.

Collector semantics follow [WPR command options](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options)
and [PresentMon 2.5.1 documentation](https://github.com/GameTechDev/PresentMon/blob/v2.5.1/README-ConsoleApplication.md).
