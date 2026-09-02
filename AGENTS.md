# Moonlight development notes

## ChaseShare Windows build

The gaming build is an unsigned Windows x64 release. The canonical share is:

```text
\\allytwo\ChaseShare
```

**This PC is ALLYTWO.** It is the Moonlight client, the SMB server for every
share below, and the machine these instructions run on. `\\allytwo\...` paths
therefore resolve locally; copying to the share is a local copy. The Sunshine
host is a different machine on the LAN. On disk:

```text
\\allytwo\ChaseShare  ->  C:\Users\Chase\Network Share   (private drop, release builds, traces)
\\allytwo\AllyShare   ->  C:\AllyShare                    (open drop, see below)
```

Windows also exposes ChaseShare through this Network Shortcuts entry, but that
folder only contains a `target.lnk`; do not copy release files into the
shortcut folder itself:

```text
C:\Users\Chase\AppData\Roaming\Microsoft\Windows\Network Shortcuts\ChaseShare (ALLYTWO (this PC))
```

`AllyShare` is readable and writable by anyone on the LAN without an account
(guest logons, Everyone: Full on the share, Everyone: Modify on NTFS). It
exists so the Sunshine host can drop its own logs and captures for host-side
investigations. Never put release builds, `Moonlight.ini`, or anything from the
user profile in it, and do not make ChaseShare open the same way. The setup
that created it, with every system change listed so it can be reversed, is
`C:\Users\Chase\setup-allyshare.ps1`; it runs elevated and requires: the LAN
network profile Private, File and Printer Sharing on the Private profile, SMB
server signing/encryption not required, the Guest account enabled and not
denied network logon. A Windows 11 Pro/Enterprise 24H2 machine connecting to
it must additionally run, elevated,
`Set-SmbClientConfiguration -EnableInsecureGuestLogons $true -RequireSecuritySignature $false -Force`.

Client session logs for a stream (received frame rate, host processing
latency, network loss) are written by Moonlight to
`%LOCALAPPDATA%\Temp\Moonlight-<epoch>.log` on this PC, not into the portable
directory.

The live portable installation and distributable ZIP are:

```text
\\allytwo\ChaseShare\MoonlightPortable-x64-6.1.0-vrr-lite
\\allytwo\ChaseShare\MoonlightPortable-x64-6.1.0-vrr-lite.zip
```

The Start Menu `Moonlight.lnk` points to `Moonlight.exe` in that portable
directory. Keep the `vrr-lite` destination name stable even when the internal
diagnostic implementation changes.

### Choose the build path

Updating the existing ChaseShare gaming build is normally an iterative
deployment, not a new published release. For an app-only source change at the
same version, use the fast incremental path below. Do **not** run
`scripts\build-arch.bat`, clean the build directories, redeploy Qt, build the
MSI, or regenerate the symbol archive for a routine ChaseShare update.

Use the full clean release pipeline only when the user explicitly requests a
new published release. If an iterative update unexpectedly requires a clean
build or package/dependency regeneration, explain why and get direction before
switching to the full pipeline.

### Fast iterative ChaseShare update

**A successful fast build is not a successful ChaseShare update.** The helper
below only compiles and links into `build\build-x64-release`; it does not stage
the executable, recreate the portable ZIP, copy anything to ChaseShare, or
verify the live installation. Unless the user explicitly asks for a local
build only, always complete every build, staging, publication, and verification
step in this section before reporting that the update is finished.

Run the incremental Windows release helper from the repository root. It reuses
the configured `build\build-x64-release` tree and compiles and links only
changed targets:

```powershell
cmd /c .\scripts\build-fast-windows.cmd
```

For an app-only change, stage the newly linked executable into the existing
deploy tree. Preserve all other deployed dependencies, diagnostic tools, and
the inactive portable marker, then recreate the portable ZIP:

```powershell
Copy-Item .\build\build-x64-release\app\release\Moonlight.exe `
    .\build\deploy-x64-release\ -Force

if (-not (Test-Path .\build\deploy-x64-release\portable.dat.inactive)) {
    throw "Fast deploy tree is missing portable.dat.inactive"
}
if (Test-Path .\build\deploy-x64-release\portable.dat) {
    throw "Fast deploy tree unexpectedly contains portable.dat"
}

Compress-Archive -Path .\build\deploy-x64-release\* `
    -DestinationPath .\build\installer-x64-release\MoonlightPortable-x64-6.1.0-vrr-lite.zip `
    -CompressionLevel Optimal -Force
```

Before using this path, confirm the existing deploy tree belongs to the current
`vrr-lite` build and already contains `vrrreplay.exe` and
`decode-vrr-trace.py`. Rebuild and restage the VRR utilities only when their
sources or dependencies changed. Use the process checks and hash verification
under **Publish safely** for every iterative deployment.

An iterative ChaseShare update is complete only after all of the following are
true:

1. `scripts\build-fast-windows.cmd` exited successfully.
2. The newly linked `Moonlight.exe` was copied from the build tree into the
   deploy tree.
3. Any changed VRR utilities were rebuilt and copied into the deploy tree.
4. The deploy tree still has `portable.dat.inactive`, does not have
   `portable.dat`, and contains `vrrreplay.exe` and `decode-vrr-trace.py`.
5. The `vrr-lite` portable ZIP was recreated from that deploy tree.
6. After confirming Moonlight is not running, the complete deploy tree and ZIP
   were copied to the canonical ChaseShare destinations.
7. Source and ChaseShare SHA-256 hashes match for `Moonlight.exe`,
   `vrrreplay.exe`, `decode-vrr-trace.py`, and the ZIP.
8. The ChaseShare copy of `vrrreplay.exe --help` runs successfully from its UNC
   path.

Do not infer publication from timestamps in the build tree. Before handoff,
inspect the build, deploy, and live ChaseShare copies separately. If publication
was not requested or cannot be completed safely, say explicitly that only a
local build was produced and do not describe the ChaseShare build as updated.

### Full build for a new published release

Run commands from the repository root. The known-good local toolchain is:

```text
C:\Users\Chase\sources\.tools\Qt\6.11.1\msvc2022_64\bin
C:\Users\Chase\sources\.tools\7zip
```

Visual Studio 2022 and its x64 C++ tools must be installed. On this machine
the build tools live under `C:\Users\Chase\sources\.tools\vs-buildtools`,
where `scripts\vswhere.exe` does not find them, so `build-arch.bat` fails
with "Cannot run compiler 'cl'" unless `vcvarsall` is initialized first. Put
the Qt and 7-Zip directories on `PATH`, set the custom package version, and
build inside the MSVC environment. For a GitHub release the version is the
tag without the `v`, which names the MSI and ZIP assets:

```powershell
$env:Path = "C:\Users\Chase\sources\.tools\Qt\6.11.1\msvc2022_64\bin;C:\Users\Chase\sources\.tools\7zip;$env:Path"
$env:CI_VERSION = "6.1.0-vrr12"   # or "$(Get-Content .\app\version.txt)-vrr-lite" for the share build
cmd /d /s /c "call C:\Users\Chase\sources\.tools\vs-buildtools\VC\Auxiliary\Build\vcvarsall.bat x64 && scripts\build-arch.bat release"
```

The script takes on the order of ten minutes (LTCG) and wipes the deploy
tree, so the ChaseShare staging steps must be repeated afterwards.

### Publishing a GitHub release

Releases live at `https://github.com/Nonary/moonlight-qt/releases`, tagged
`v6.1.0-vrrNN`, marked pre-release, with the previous release's body as the
template (`gh release view v6.1.0-vrrNN --json body`). The GitHub CLI is at
`C:\Users\Chase\sources\.tools\gh\bin\gh.exe` and is logged in. Create the
draft first, upload assets as they finish, fill the SHA-256 table from the
uploaded files, and publish last:

```powershell
$gh = "C:\Users\Chase\sources\.tools\gh\bin\gh.exe"
& $gh release create v6.1.0-vrr12 --repo Nonary/moonlight-qt --draft --prerelease --target vrr12 --title "..." --notes-file notes.md
& $gh release upload v6.1.0-vrr12 --repo Nonary/moonlight-qt .\build\installer-x64-release\MoonlightSetup-x64-6.1.0-vrr12.msi .\build\installer-x64-release\MoonlightPortable-x64-6.1.0-vrr12.zip
```

The Windows assets are the MSI and the portable ZIP from `build-arch.bat`.
The script leaves the MSI as `build\build-x64-release\Moonlight.msi`; copy
it to `build\installer-x64-release\MoonlightSetup-x64-<version>.msi` to
match the release naming. Copy `vrrreplay.exe` and `decode-vrr-trace.py`
into the deploy tree and recreate the ZIP before uploading so the portable
package carries the diagnostics, as the release notes promise. The Linux AppImage and Flatpak
assets are not produced by this repository's Windows tooling; the CI
workflow builds an AppImage named by commit SHA on every push, and the
Flatpak comes from outside this repository.

The script cleans and recreates these directories, compiles Moonlight, deploys
the Qt/runtime dependencies, builds the MSI, and creates the base portable ZIP:

```text
build\build-x64-release
build\deploy-x64-release
build\installer-x64-release
build\symbols-x64-release
```

Setting `CI_VERSION` causes `portable.dat.inactive` to be emitted. That matches
the ChaseShare build's current settings behavior; do not silently change it to
`portable.dat` during deployment.

### Build and stage the VRR diagnostics

The regular application build does not build the opt-in VRR tests or replay
utility. Build them separately for a new published release or when VRR pacing,
controller, trace, replay, or diagnostic code changes. A routine app-only
update does not require rebuilding unchanged VRR utilities. From an x64 Native
Tools for Visual Studio 2022 command prompt:

```bat
mkdir build\tests-vrr
cd build\tests-vrr
C:\Users\Chase\sources\.tools\Qt\6.11.1\msvc2022_64\bin\qmake.exe ..\..\tests\tests.pro CONFIG+=tests
nmake
```

From an ordinary PowerShell session, use the repository's known Visual Studio
installation explicitly. Initialize the qmake tree only when its Makefile is
missing, then run `nmake` from that same build directory and require both exit
codes to be zero:

```powershell
$vrrBuild = ".\build\tests-vrr"
if (-not (Test-Path "$vrrBuild\Makefile")) {
    New-Item -ItemType Directory -Path $vrrBuild -Force | Out-Null
    Push-Location $vrrBuild
    try {
        & "C:\Users\Chase\sources\.tools\Qt\6.11.1\msvc2022_64\bin\qmake.exe" `
            ..\..\tests\tests.pro CONFIG+=tests
        if ($LASTEXITCODE -ne 0) { throw "VRR qmake initialization failed" }
    }
    finally { Pop-Location }
}
Push-Location $vrrBuild
try {
    cmd /d /s /c "call C:\Users\Chase\sources\.tools\vs-buildtools\VC\Auxiliary\Build\vcvarsall.bat x64 && nmake"
    if ($LASTEXITCODE -ne 0) { throw "VRR diagnostic build failed" }
}
finally { Pop-Location }
```

Run all deterministic tests before deploying a VRR-related change or creating
a new published release:

```bat
vrr\release\tst_vrrtimingcontroller.exe
vrr\release\tst_vrrratepolicy.exe
vrr\release\tst_vrrpacingworker.exe
vrr\release\tst_vrrreplayconfig.exe
vrr\release\vrrreplay.exe --help
```

Finish all pacing, controller, replay, and test-source edits before this build.
If any such source is edited afterward, run `nmake` again and rerun all five
checks. Do not assume a successful application build rebuilt the diagnostics.

The test executables need the deployed runtime DLLs and Qt on `PATH`. Run them
directly from PowerShell and check `$LASTEXITCODE` after each executable. Exit
code `-1073741515` (`0xC0000135`) means a DLL was not found; fix `PATH` and
rerun the test instead of diagnosing it as a VRR failure:

```powershell
$deploy = (Resolve-Path .\build\deploy-x64-release).Path
$qtBin = "C:\Users\Chase\sources\.tools\Qt\6.11.1\msvc2022_64\bin"
$env:Path = "$deploy;$qtBin;$env:Path"

$tests = @(
    "tst_vrrtimingcontroller.exe",
    "tst_vrrratepolicy.exe",
    "tst_vrrpacingworker.exe",
    "tst_vrrreplayconfig.exe"
)
foreach ($test in $tests) {
    & ".\build\tests-vrr\vrr\release\$test"
    if ($LASTEXITCODE -ne 0) {
        throw "$test failed with exit code $LASTEXITCODE"
    }
}
```

### Reliable capture selection and replay workflow

Never reuse a trace path, decoded CSV, baseline JSON, or conclusions merely
because they were the newest ones earlier in the conversation. Immediately
before each analysis, re-enumerate `%USERPROFILE%\vrr-traces` and the canonical
share trace directory, then select the single newest completed capture unless
the user named a specific capture. If the user says that only the latest trace
is valid, do not mix any metric, baseline, or inference from older traces into
the analysis. Record the selected full path, length, last-write time, and
SHA-256 hash in the working notes. Match a replay sidecar by the complete trace
base name, not by a loose wildcard.

```powershell
$traceRoots = @(
    (Join-Path $env:USERPROFILE "vrr-traces"),
    "\\allytwo\ChaseShare\vrr-traces"
)
$trace = $traceRoots |
    Where-Object { Test-Path -LiteralPath $_ } |
    ForEach-Object { Get-ChildItem -LiteralPath $_ -Filter *.vrrtrace -File } |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1
if ($null -eq $trace) { throw "No VRR capture was found" }
$trace | Select-Object FullName, Length, LastWriteTimeUtc
Get-FileHash -Algorithm SHA256 -LiteralPath $trace.FullName
```

Before changing numbers, run a fresh exact baseline with the current replay
binary and check its actual exit code. Do not infer success from the presence
of an output JSON. Inspect `capture.recorded_sequence_integrity_valid`,
`fidelity.baseline_exact`, and the matching launcher sidecar as well. A missing
trace row, a false exact flag, or a nonzero exit code makes the capture
exploratory only even when most timestamps match; it cannot support a claim of
strict A/B proof. State that limitation before tuning, but exploratory sweeps
may continue if they remain useful.

An unchanged controller must reproduce every target, submission timestamp,
tear classification, and required diagnostic field exactly on the latest
replay-supported capture, including schema 5 captures:

```powershell
$replay = (Resolve-Path .\build\tests-vrr\vrr\release\vrrreplay.exe).Path
& $replay $trace.FullName --require-exact-baseline `
    --output .\build\vrr-baseline.json
if ($LASTEXITCODE -ne 0) {
    throw "Exact baseline failed with exit code $LASTEXITCODE"
}
```

For parameter tuning, use one versioned replay config containing many named
scenarios so `vrrreplay` evaluates the sweep in one invocation. This is both
faster and less error-prone than launching one process per number. Prefer
config objects over repeated `--set` arguments because the whole parameter
object is validated together. If `--set` is necessary, set
`controller.readiness_loose_percentile` before raising
`controller.readiness_low_percentile`; otherwise incremental validation can
reject a temporarily inconsistent pair.

Batch replay runs independent scenarios concurrently. Its automatic limit is
the available logical-processor count, capped at sixteen. Use `--jobs N` to tune
the bound for the machine and trace, and `--jobs 1` only for serial comparison
or debugging. Do not wrap a batch in another shell-level parallel loop: that
multiplies decompression, memory, and process pressure without improving the
replay model itself. Benchmark a representative batch before raising the cap;
GPU offload is not a useful default because each individual scenario is a
branch-heavy, sequential controller simulation.

Use the replay summary's p99.5, p99.9, and p99.95 distribution fields for tail
optimization. Generate `--timeline` CSV only when frame identity, causal
classification, or a percentile not present in the summary is actually needed.

For the cadence question that matters to the user, "does it look smooth", use
the presented-jerk fields the replay computes in-process
(`replay_presented_jerk_*`, `original_presented_jerk_*`,
`stock_presented_jerk_*`): the change in presented interval from one pair to
the next, and the share of pairs over 2 ms. The sender-spacing fields
(`replay_sender_*` and friends) score fidelity to the host's stamps instead;
those stamps jitter several milliseconds frame to frame, so a policy can
score near zero there while half the frames visibly stutter. Report both,
lead with presented jerk, and never call a capture smooth on the
sender-spacing fields alone. Do not post-process a timeline in PowerShell for
this, which takes minutes per scenario. A whole delay sweep is one parallel
batch:

```powershell
.\build\tests-vrr\vrr\release\vrrreplay.exe $trace.FullName `
    --config tests\vrr\configs\playout-delay-sweep-lowlatency.json --jobs 0 `
    --output .\build\sweep.json
.\scripts\vrr-sweep-table.ps1 .\build\sweep.json
```

The `session-policy` row is the production policy the capture would run
under today. Ignore any scenario the table marks `saturated`: its worker fell
behind the source cadence and the fixed-admission replay cannot shed frames,
so its latency is meaningless. Host capture stalls (sender intervals over
25 ms) are excluded from the hitch count by design; report them separately
from the trace's RTP timestamps when the user reports visible stutter, because
they are usually the cause and the pacer cannot fix them.

Keep the untouched baseline and candidate results in separately named files.
Evaluate cadence residual p95/p99, jerk p95/p99, decode-to-submission mean and
p50/p95/p99, submission drift, modeled interval violations, and raster bounds.
Choose a Pareto knee rather than the smoothest result at any latency cost. Then
run a second batched config with nominal, periodic decision, preparation and
submission, and scheduler-burst scenarios. Assertions must cover interval
safety and an explicit latency bound. A severe injected fault may use the
unchanged policy's result as its bound, but do not call that synthetic latency
normal operating latency.

After a controller tweak, rebuild `vrrreplay.exe` and compare the exact same
trace with `--compare .\build\vrr-baseline.json --output candidate.json`.
After the final source edit and final diagnostic rebuild, rerun the selected
candidate and stress config; results from a pre-rebuild executable are not
final proof. Use `--timeline candidate.csv` only when per-frame deltas are
needed because a full-session timeline is intentionally much larger than the
JSON summary.

Invoke replay directly from PowerShell with an argument array or individual
arguments. Do not use C-style `\"` quoting in PowerShell: backslash is not its
escape character. Do not combine the four deterministic tests, `--help`, the
exact gate, and simulations into one opaque command whose last failure hides
which earlier checks passed.

After rebuilding the diagnostics, add the replay and decoder tools to the
deploy tree. A full clean release build also requires these copies because it
recreates the deploy tree. They must happen before recreating the share ZIP, or
the portable directory and ZIP will not contain the same diagnostic tools:

```powershell
Copy-Item .\build\tests-vrr\vrr\release\vrrreplay.exe .\build\deploy-x64-release\ -Force
Copy-Item .\scripts\decode-vrr-trace.py .\build\deploy-x64-release\ -Force
Compress-Archive -Path .\build\deploy-x64-release\* `
    -DestinationPath .\build\installer-x64-release\MoonlightPortable-x64-6.1.0-vrr-lite.zip `
    -CompressionLevel Optimal -Force
```

If a differently named test build directory is used, adjust the
`vrrreplay.exe` source path accordingly.

### Publish safely

Do not overwrite the portable tree while Moonlight is running from it. Windows
locks `Moonlight.exe` and several deployed DLLs, which can leave a partial
update. Check the executable path and wait for the session to close normally;
do not terminate a gaming session just to deploy:

```powershell
Get-Process Moonlight -ErrorAction SilentlyContinue |
    Select-Object Id, Path, StartTime
```

Once no process is using the share installation, publish the complete deploy
tree and refreshed ZIP:

```powershell
$shareRoot = "\\allytwo\ChaseShare"
$portable = Join-Path $shareRoot "MoonlightPortable-x64-6.1.0-vrr-lite"
Copy-Item .\build\deploy-x64-release\* $portable -Recurse -Force
Copy-Item .\build\installer-x64-release\MoonlightPortable-x64-6.1.0-vrr-lite.zip `
    $shareRoot -Force
```

Verify at minimum that the source and destination SHA-256 hashes match for
`Moonlight.exe`, `vrrreplay.exe`, `decode-vrr-trace.py`, and the ZIP. Also run
the replay utility from the UNC directory with `--help` so missing DLLs are
caught before handoff.

For the UNC `--help` check, launch the executable from its deployed directory,
redirect stdout and stderr to local files, wait for completion, require exit
code zero, and require nonempty help output. Use a fresh PowerShell process
whose environment has not been manually given both `Path` and `PATH` keys;
that case-insensitive duplicate can make `Start-Process` fail before launching.
Use `-Wait -PassThru` rather than calling timed `WaitForExit()` and immediately
reading the PowerShell wrapper's sometimes-unpopulated `ExitCode`. Keep the
window hidden because this is a noninteractive smoke test:

```powershell
$portable = "\\allytwo\ChaseShare\MoonlightPortable-x64-6.1.0-vrr-lite"
$helpOut = Join-Path (Resolve-Path .\build) "unc-vrrreplay-help.stdout.txt"
$helpErr = Join-Path (Resolve-Path .\build) "unc-vrrreplay-help.stderr.txt"
$process = Start-Process `
    -FilePath (Join-Path $portable "vrrreplay.exe") `
    -ArgumentList "--help" `
    -WorkingDirectory $portable `
    -WindowStyle Hidden `
    -RedirectStandardOutput $helpOut `
    -RedirectStandardError $helpErr `
    -Wait -PassThru
if ($process.ExitCode -ne 0 -or (Get-Item $helpOut).Length -eq 0) {
    throw "Published vrrreplay --help smoke test failed"
}
```

### Tracing launchers

The two tracing shortcuts are batch launchers stored at the share root, outside
this repository:

```text
\\allytwo\ChaseShare\Moonlight VRR Diagnostic.cmd
\\allytwo\ChaseShare\Moonlight VRR Alignment Diagnostic.cmd
```

They must continue to launch the stable `vrr-lite` portable directory. Traces
are written locally under `%USERPROFILE%\vrr-traces` while gaming and copied to
`\\allytwo\ChaseShare\vrr-traces` only after Moonlight exits. Do not change the
capture path to a UNC path: the tracer deliberately rejects UNC destinations
to keep network I/O out of frame delivery.

Both launchers enable schema-4 replay-grade `.vrrtrace` capture and deep diagnostics,
retain at least the first 60 minutes, apply the 512 MiB cap only afterward, and
run `vrrreplay.exe --require-exact-baseline` after capture to upload a JSON
summary beside the trace. This exact check must continue to pass before a
capture is treated as a trustworthy A/B baseline.
The alignment launcher additionally sets `MOONLIGHT_VRR_ALIGN=1`.

Whenever trace schema, environment variables, retention behavior, replay CLI,
or portable directory names change, update both share-root launchers as part of
the same deployment.
