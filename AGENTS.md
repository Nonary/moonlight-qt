# Moonlight development notes

## ChaseShare Windows build

The gaming build is an unsigned Windows x64 release. The canonical share is:

```text
\\allytwo\ChaseShare
```

Windows also exposes it through this Network Shortcuts entry, but that folder
only contains a `target.lnk`; do not copy release files into the shortcut
folder itself:

```text
C:\Users\Chase\AppData\Roaming\Microsoft\Windows\Network Shortcuts\ChaseShare (ALLYTWO (this PC))
```

The live portable installation and distributable ZIP are:

```text
\\allytwo\ChaseShare\MoonlightPortable-x64-6.1.0-vrr-lite
\\allytwo\ChaseShare\MoonlightPortable-x64-6.1.0-vrr-lite.zip
```

The Start Menu `Moonlight.lnk` points to `Moonlight.exe` in that portable
directory. Keep the `vrr-lite` destination name stable even when the internal
diagnostic implementation changes.

### Build prerequisites

Run commands from the repository root. The known-good local toolchain is:

```text
C:\Users\Chase\sources\.tools\Qt\6.11.1\msvc2022_64\bin
C:\Users\Chase\sources\.tools\7zip
```

Visual Studio 2022 and its x64 C++ tools must be installed. `build-arch.bat`
locates Visual Studio with `scripts\vswhere.exe` and initializes `vcvarsall`
itself. Put the Qt and 7-Zip directories on `PATH`, set the custom package
version, and build:

```powershell
$env:Path = "C:\Users\Chase\sources\.tools\Qt\6.11.1\msvc2022_64\bin;C:\Users\Chase\sources\.tools\7zip;$env:Path"
$env:CI_VERSION = "$(Get-Content .\app\version.txt)-vrr-lite"
cmd /c .\scripts\build-arch.bat release
```

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
utility. From an x64 Native Tools for Visual Studio 2022 command prompt, build
them separately:

```bat
mkdir build\tests-vrr
cd build\tests-vrr
C:\Users\Chase\sources\.tools\Qt\6.11.1\msvc2022_64\bin\qmake.exe ..\..\tests\tests.pro CONFIG+=tests
nmake
```

Run all deterministic tests before publishing:

```bat
vrr\release\tst_vrrtimingcontroller.exe
vrr\release\tst_vrrratepolicy.exe
vrr\release\tst_vrrpacingworker.exe
vrr\release\vrrreplay.exe --help
```

Before publishing a replay change, validate it against the latest schema-3 or
schema-4 capture on the share. An unchanged controller must reproduce every
target, submission timestamp, and tear classification exactly:

```powershell
.\build\tests-vrr\vrr\release\vrrreplay.exe `
    "\\allytwo\ChaseShare\vrr-traces\Moonlight-vrr-20260720-195353.vrrtrace" `
    --require-exact-baseline --output .\build\vrr-baseline.json
```

After a controller tweak, rebuild `vrrreplay.exe` and compare the same stream
with `--compare .\build\vrr-baseline.json --output candidate.json`. Use
`--timeline candidate.csv` only when per-frame deltas are needed because a
full-session timeline is intentionally much larger than the JSON summary.

Then add the replay and decoder tools to the deploy tree. These copies must
happen before recreating the share ZIP, or the portable directory and ZIP will
not contain the same diagnostic tools:

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
