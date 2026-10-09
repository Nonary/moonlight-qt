# VRR frame tracing and log export

The tracing checkbox and timing changes are shared Windows/Linux code. A Linux
build does not publish or validate the Windows executable; follow AGENTS.md for
that separate build/deployment step.

## User workflow

For interpreting the blue/cyan client-submission and magenta display-event
lines, identifying host/network/decoder/GPU/scheduler/display causes, and
choosing controlled tests, see the
[client timing troubleshooting guide](client-timing-troubleshooting.md).

Enable **Show frametime graph while streaming** in Settings, or toggle it during a
stream with **Ctrl+Alt+Shift+F** or **Select+L1+R1+Y** (Triangle). It is separate
from the stats text (Ctrl+Alt+Shift+S / Select+L1+R1+X) and can be shown alone.
During VRR playback it shows three aligned frametime lanes covering the newest
240 frames:

- **Planned cadence** (gray): the interval between scheduled targets. Buffer
  steps show up here.
- **Client submissions** (cyan): the interval between actual submissions.
- **Display events** (magenta): the interval between OS-reported display events.

All three share one millisecond axis centred on the planned interval (+/-2 ms),
so a disturbance shows up in the lane where it starts. Variation within 1 ms
of the reference is drawn flat because it is not noticeable; larger changes are
drawn at their real size, and the captions keep raw values. When gray and cyan are
flat and magenta jumps, the variation was added after submission by the GPU,
compositor or display. Values beyond the axis are clipped with red markers; the
captions give the latest interval and the peak. Missing display feedback leaves
a gap rather than one long interval, and a backend without feedback shows
**unavailable**. A Present return or DXGI refresh reference is never substituted
for display feedback, and OS/compositor timestamps are not physical panel
measurements.

On a 4K output, the entire stats surface (text and chart) is about 30% larger.
It follows the drawable window's pixel size, including high-DPI windows, and
returns to normal size on smaller outputs. The chart refreshes at 10 Hz while it
is shown and does not require recording.

Below the Smoothness line, **Present timing issues (30s)** gives the share of
display intervals the display made uneven: the on-screen spacing missed the
planned spacing by more than the tolerance, and by more than the submitted
spacing did. A display that smooths out uneven submissions (for example by
queueing frames) is not blamed, and nothing faster than the panel's maximum
refresh is expected. It is kept out of Smoothness and never grows the buffer,
because more buffering cannot correct it. Smoothness only reflects timing the
buffer can fix. A GPU that cannot finish its work in time (high GPU decode or
rendering time in the stats) also shows up here. **Hitches** counts intervals
at least one whole frame worse on screen than they were submitted, and
**Worst** is the largest such delay in the window. A few large stalls are very
visible but barely change the percentage.

Host cadence stalls can propagate into all three graph lanes without being
display-added timing issues. Aligned spikes alone do not identify the host:
client drops and buffer/target changes can also lengthen the intervals between
surviving graph observations. Check source timestamps, frame continuity and
client-drop evidence to distinguish them.

The diagnostic assumes gaps over 20 ms (roughly below 50 fps) may enter the
driver's low-framerate-compensation regime. It reads **paused below VRR range**
and allows 250 ms to settle after such gaps. This is a scoring assumption,
not a measurement of every panel's actual adaptive-refresh floor. The line
reads **unavailable** without current OS display feedback.

If magenta jumps while the other lanes stay flat on an AMD GPU under Linux, try
**High-performance GPU power while streaming** in the video settings. It holds the
GPU at fixed high clocks for the stream so it cannot drop its clocks between
frames, at the cost of more power.

In Settings, under **VRR diagnostics**, check **Trace VRR frames for debugging**.
Enable VRR, connect and reproduce the problem, then disconnect. Recordings are
saved automatically under **vrr-diagnostics on your Desktop**, with a separate
timestamped folder for each stream. Use **Open diagnostics folder** to view them,
or **Export latest recording (ZIP)** to package the latest completed run beside
the recording folders. Include when the problem happened and what it looked like.

The checkbox defaults off and is remembered across application launches. Uncheck
it after debugging; reconnect after changing it. Deep tracing also expands the
stats overlay with buffer status and the GPU/queue/rendering delay breakdown.
Without deep tracing, the normal VRR17 queue delay and smoothness overview is
shown. External launchers setting `MOONLIGHT_VRR_DEEP_TRACE=1` enable the same
details; setting only a trace destination does not. Tracing
does not select old/new timing rules, change the configured buffer allowance, reset
calibration or change the user's latency preset or Reduce judder choice.

## Destination, files and privacy

The application resolves the actual Desktop using
`QStandardPaths::DesktopLocation`, including localized and redirected Windows
Desktop paths, then appends `vrr-diagnostics`. It does not use the executable
directory or a hidden application-data folder. An unavailable/unwritable or
network-backed Windows Desktop reports a capture error instead of silently
writing elsewhere. Tracing remains local to avoid network I/O during streaming.

Each stream gets a UTC timestamp and random identifier. Its folder contains:

- `Moonlight.vrrtrace` and any `Moonlight-connection-*.vrrtrace` segments from
  decoder recreation/reconnects within that stream;
- `Moonlight.log`, a per-capture copy of existing redacted session logging;
- `capture-info.json`, with requested stream settings, application version and
  executable SHA-256, OS, diagnostic overrides and capture completion state.

Export includes only those files, never `Moonlight.ini`, pairing keys,
certificates, crash dumps, symlinks, or other captures. Logs can still contain
host addresses, application names and hardware details. Review before sharing.
Moonlight does not upload recordings; existing Desktop synchronization software
may sync them. Session encryption-key/IV redaction runs before logging is copied.

The log is bounded to roughly 10 MiB per capture, independently of the original
process log. Trace retention is unchanged: the first hour is kept, then the
512 MiB cap applies. Deep tracing can add overhead and substantial disk use.
No automatic deletion of recordings is added.

ZIP export runs in the background, streams already-compressed traces into an
ordinary stored ZIP, and publishes atomically. No PowerShell, Python, 7-Zip,
private Qt APIs or administrator rights are required on the client. Every export
gets a unique filename. Input/space errors leave original captures intact.
ZIP32 archives approaching 4 GiB are rejected; those recordings can still be
archived manually. Normal app exit waits for an in-flight export.

## Lifecycle and external launchers

The checkbox sets only `MOONLIGHT_VRR_TRACE` and `MOONLIGHT_VRR_DEEP_TRACE=1`,
after the preceding session finishes cleanup and before starting the connection.
Their prior process values are restored after decoder/trace shutdown and logger
drain. No system/user environment settings or controller timing parameters are
modified. Frame delivery only queues trace records; compression and trace writes
remain on the writer thread. Log copies use the existing serialized logger.
Both tracer settings read the current process environment at worker creation,
not SDL2-compat's cached copy, so checking the box does not require restarting
Moonlight. It takes effect on the next stream connection.

An already-set `MOONLIGHT_VRR_TRACE` takes precedence. The checkbox does not
redirect an external launcher's recording; its status explains that Moonlight
must be launched normally to use Settings diagnostics. Existing diagnostic,
alignment and deep-trace launchers keep their own destinations. In-app export
covers Desktop recordings, not arbitrary external launcher paths.

Windows recordings reject UNC and mapped-network destinations and use wide
environment/file APIs for Unicode paths. Export uses a per-capture lock and
refuses the latest recording while it is active instead of exporting an older
run. An atomic latest-capture pointer avoids timestamp ties. Interrupted
recordings remain exportable after their process exits, without gaining a
clean-close claim. Trace completeness and replay fidelity still require a fresh
exact replay gate; export does not certify replay or smoothness.

## Validation contracts

`tst_vrrdiagnostics` tests the Desktop destination, scope/restoration, active
capture locking, unique and Unicode paths, external-launcher precedence, error
paths, connection preservation, interrupted manifests, latest-run selection and
ZIP allowlisting/streaming/non-overwrite behavior. `check_diagnostic_zip.py`
independently verifies the emitted ZIP's CRCs, bytes and manifest contents.

Windows CI builds and runs the diagnostics test and independent ZIP check, plus
exact replay of cold/warm worker fixtures. These are source/fixture checks, not
native Windows GPU or visual validation. A Windows build or deployment must
actually succeed before the Windows package is described as updated.

Local verification (2026-09-18): the Linux app build, QML syntax check, all nine
C++ suites, 35 Python tests, independent ZIP reader, cold/warm exact replays and
five stress scenarios pass. The supplied historical capture also retains exact
replay. The worker test enables diagnostics after SDL initialization through the
same process environment used by the checkbox and confirms deep observations
without changing presentation mode. An isolated offscreen GUI remains running
until the smoke test stops it. Artifacts are under `build/vrr-checkbox-f36FtU`.
This is not a gameplay or native Windows validation; no Windows package or
ChaseShare installation was built or updated during this change.
