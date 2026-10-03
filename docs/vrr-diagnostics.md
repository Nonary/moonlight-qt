# VRR frame tracing and log export

The tracing checkbox and timing changes are shared Windows/Linux code. A Linux
build does not publish or validate the Windows executable; follow AGENTS.md for
that separate build/deployment step.

## User workflow

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
does not select old/new timing rules, change the 2/2/4 buffer allowances, reset
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
