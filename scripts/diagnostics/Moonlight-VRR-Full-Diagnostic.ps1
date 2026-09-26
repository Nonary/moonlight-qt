[CmdletBinding()]
param(
    [string]$Portable,
    [string]$TraceRoot = (Join-Path $env:USERPROFILE 'vrr-traces'),
    [string]$ShareRoot = '\\allytwo\ChaseShare\vrr-traces\full',
    [ValidateRange(3,900)][int]$MaxSeconds = 300,
    [string]$Label = 'RTSS mode not recorded',
    [switch]$Alignment,
    [switch]$DryRun,
    [switch]$RecorderSmokeTest
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell -File can evaluate parameter defaults before
# $PSScriptRoot is populated. Resolve the default in the script body.
if ([string]::IsNullOrWhiteSpace($Portable)) {
    $Portable = $PSScriptRoot
}
if ([string]::IsNullOrWhiteSpace($Portable)) {
    throw 'Cannot determine the portable directory; pass -Portable explicitly.'
}
function Artifact([string]$Path) {
    $item = Get-Item -LiteralPath $Path
    [ordered]@{ name = $item.Name; bytes = $item.Length; sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
}
function RunTool([string]$Exe, [string[]]$Arguments, [string]$Name) {
    $quoted = @($Arguments | ForEach-Object { '"' + $_ + '"' })
    $p = Start-Process -FilePath $Exe -ArgumentList $quoted -WorkingDirectory $Portable -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $directory "$Name.stdout.txt") -RedirectStandardError (Join-Path $directory "$Name.stderr.txt") -Wait -PassThru
    return $p.ExitCode
}
$dependencies = @('Moonlight.exe','vrrreplay.exe','PresentMon-2.5.1-x64.exe',
    'Moonlight-VRR-Recorder.ps1','MoonlightVrrFull.wprp')
foreach ($name in $dependencies) {
    if (-not (Test-Path -LiteralPath (Join-Path $Portable $name) -PathType Leaf)) { throw "Missing dependency: $name" }
}
New-Item -ItemType Directory -Path $TraceRoot -Force | Out-Null
$rootItem = Get-Item -LiteralPath $TraceRoot
if ($rootItem.FullName.StartsWith('\\') -or $rootItem.PSDrive.DisplayRoot) { throw 'TraceRoot must be a local drive.' }
& wpr.exe -profiles (Join-Path $Portable 'MoonlightVrrFull.wprp') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'WPR profile validation failed.' }
if ($DryRun) {
    [ordered]@{ trace_root = $rootItem.FullName; max_seconds = $MaxSeconds; alignment = [bool]$Alignment;
        binaries = @($dependencies | ForEach-Object { Artifact (Join-Path $Portable $_) }) } | ConvertTo-Json -Depth 5
    exit 0
}
if (Get-Process Moonlight -ErrorAction SilentlyContinue) { throw 'Close Moonlight before starting this capture.' }
$start = Get-Date
$directory = Join-Path $rootItem.FullName ('Moonlight-sandbox-' + $start.ToString('yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $directory | Out-Null
$manifest = [ordered]@{ schema = 1; label = $Label; start_utc = $start.ToUniversalTime().ToString('o');
    binaries = @($dependencies | ForEach-Object { Artifact (Join-Path $Portable $_) });
    process_id = $null; moonlight_exit = $null; exact_replay = $false; trace_integrity = $false;
    replay_exit = $null; presentmon_exit = $null; display_correlation_verified = $false;
    presentmon_rows = 0; presentmon_display_columns = $false; gpu_sidecars_complete = $false;
    etw_loss_verified = $false; recorder = $null; alignment = [bool]$Alignment;
    environment = @{}; errors = @(); artifacts = @() }
Get-ChildItem Env: | Where-Object Name -Like 'MOONLIGHT_VRR*' | ForEach-Object { $manifest.environment[$_.Name] = $_.Value }
$manifest | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $directory 'manifest.json') -Encoding utf8
$recorder = $null
$oldTrace = $env:MOONLIGHT_VRR_TRACE; $oldDeep = $env:MOONLIGHT_VRR_DEEP_TRACE; $oldAlign = $env:MOONLIGHT_VRR_ALIGN
try {
    # Elevate only the recorder. The app inherits this ordinary user process.
    $helper = Join-Path $Portable 'Moonlight-VRR-Recorder.ps1'
    $seconds = if ($RecorderSmokeTest) { 3 } else { $MaxSeconds }
    $code = "& '" + $helper.Replace("'","''") + "' -Directory '" + $directory.Replace("'","''") + "' -ParentProcessId $PID -MaxSeconds $seconds"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    $recorder = Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -EncodedCommand $encoded" -PassThru
    $deadline = (Get-Date).AddSeconds(45)
    while (-not (Test-Path (Join-Path $directory 'recorder-ready'))) {
        if ($recorder.HasExited -or (Get-Date) -gt $deadline) { throw "Recorder did not start; inspect $directory" }
        Start-Sleep -Milliseconds 100
    }
    if ($RecorderSmokeTest) { $recorder.WaitForExit() }
    else {
        $env:MOONLIGHT_VRR_TRACE = Join-Path $directory 'Moonlight.vrrtrace'
        $env:MOONLIGHT_VRR_DEEP_TRACE = '1'
        $env:MOONLIGHT_VRR_ALIGN = if ($Alignment) { '1' } else { '0' }
        $manifest.environment['MOONLIGHT_VRR_TRACE'] = $env:MOONLIGHT_VRR_TRACE
        $manifest.environment['MOONLIGHT_VRR_DEEP_TRACE'] = '1'
        $manifest.environment['MOONLIGHT_VRR_ALIGN'] = $env:MOONLIGHT_VRR_ALIGN
        Write-Host "Capture ready. Pan for 45-60 seconds, then close Moonlight. OS capture limit: $MaxSeconds seconds."
        $app = Start-Process (Join-Path $Portable 'Moonlight.exe') -WorkingDirectory $Portable -Wait -PassThru
        $manifest.process_id = $app.Id
        $manifest.moonlight_exit = $app.ExitCode
    }
}
catch { $manifest.errors += $_.Exception.Message }
finally {
    $env:MOONLIGHT_VRR_TRACE = $oldTrace; $env:MOONLIGHT_VRR_DEEP_TRACE = $oldDeep; $env:MOONLIGHT_VRR_ALIGN = $oldAlign
    [IO.File]::WriteAllText((Join-Path $directory 'recorder-stop'), 'stop')
    if ($null -ne $recorder) { $recorder.WaitForExit() }
}
try {
    $resultPath = Join-Path $directory 'recorder-result.json'
    if (Test-Path $resultPath) { $manifest.recorder = Get-Content $resultPath -Raw | ConvertFrom-Json }
    $manifest['etw_coverage_complete'] = $null -ne $manifest.recorder -and $manifest.recorder.stop_reason -eq 'requested'
    $manifest['etw_status_dropped_events'] = $null
    $statusPath = Join-Path $directory 'wpr-status.txt'
    if (Test-Path $statusPath) {
        $status = Get-Content $statusPath -Raw
        if ($status -match 'Dropped event\s*:\s*(\d+)') { $manifest.etw_status_dropped_events = [long]$Matches[1] }
    }
    $trace = Join-Path $directory 'Moonlight.vrrtrace'
    if (Test-Path $trace) {
        $baseline = Join-Path $directory 'Moonlight-replay.json'
        $manifest.replay_exit = RunTool (Join-Path $Portable 'vrrreplay.exe') @($trace,'--require-exact-baseline','--output',$baseline) 'replay'
        if (Test-Path $baseline) {
            $data = Get-Content $baseline -Raw | ConvertFrom-Json
            $manifest.exact_replay = $manifest.replay_exit -eq 0 -and $data.fidelity.baseline_exact
            $manifest.trace_integrity = [bool]$data.capture.recorded_sequence_integrity_valid
        }
    }
    $etl = Join-Path $directory 'Windows.etl'
    if ((Test-Path $etl) -and $null -ne $manifest.process_id) {
        $manifest.presentmon_exit = RunTool (Join-Path $Portable 'PresentMon-2.5.1-x64.exe') @('--etl_file',$etl,'--process_id',"$($manifest.process_id)",
            '--output_file',(Join-Path $directory 'PresentMon.csv'),'--v1_metrics','--qpc_time','--no_console_stats','--no_track_input','--track_gpu_video') 'presentmon'
        $csv = Join-Path $directory 'PresentMon.csv'
        if (Test-Path $csv) {
            $header = Get-Content -LiteralPath $csv -TotalCount 1
            $manifest.presentmon_display_columns = $header.Contains('QPCTime') -and $header.Contains('PresentMode') -and $header.Contains('msUntilDisplayed')
            $manifest.presentmon_rows = (Import-Csv -LiteralPath $csv | Measure-Object).Count
        }
    }
    $sidecars = @(Get-ChildItem -LiteralPath $directory -Filter 'Moonlight.vrrtrace.gpu-*.csv' -File)
    $manifest.gpu_sidecars_complete = $sidecars.Count -gt 0
    foreach ($sidecar in $sidecars) {
        if ((Get-Content -LiteralPath $sidecar.FullName -Tail 1) -ne '# closed=1; truncated=0; dropped_rows=0') { $manifest.gpu_sidecars_complete = $false }
    }
    $log = Get-ChildItem $env:TEMP -Filter 'Moonlight-*.log' -File | Where-Object LastWriteTime -ge $start | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($null -ne $log -and $null -ne $manifest.process_id) { Copy-Item -LiteralPath $log.FullName -Destination (Join-Path $directory 'Moonlight.log') }
    Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,PNPDeviceID,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate |
        ConvertTo-Json | Set-Content (Join-Path $directory 'display-adapters.json') -Encoding utf8
}
catch { $manifest.errors += $_.Exception.Message }
$manifest.artifacts = @(Get-ChildItem -LiteralPath $directory -File | Where-Object Name -ne 'manifest.json' | ForEach-Object { Artifact $_.FullName })
$manifest | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $directory 'manifest.json') -Encoding utf8
if ($ShareRoot -and -not $RecorderSmokeTest) {
    New-Item -ItemType Directory -Path $ShareRoot -Force | Out-Null
    $destination = Join-Path $ShareRoot (Split-Path $directory -Leaf)
    if (Test-Path $destination) { throw "Capture destination already exists: $destination" }
    Copy-Item -LiteralPath $directory -Destination $destination -Recurse
}
Write-Host "Capture saved: $directory"
if ($manifest.errors.Count -or $null -eq $manifest.recorder -or $manifest.recorder.start_exit -ne 0 -or $manifest.recorder.stop_exit -ne 0) { exit 1 }
if (-not $RecorderSmokeTest -and ($manifest.moonlight_exit -ne 0 -or -not $manifest.exact_replay -or -not $manifest.trace_integrity -or
        $manifest.presentmon_exit -ne 0 -or -not $manifest.etw_coverage_complete -or -not $manifest.gpu_sidecars_complete -or
        $manifest.presentmon_rows -eq 0 -or -not $manifest.presentmon_display_columns -or $manifest.etw_status_dropped_events -gt 0)) {
    Write-Warning 'Capture is incomplete or exploratory; inspect manifest.json before using it as evidence.'
    exit 2
}
