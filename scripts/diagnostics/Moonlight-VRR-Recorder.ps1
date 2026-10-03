[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Directory,
    [Parameter(Mandatory)][int]$ParentProcessId,
    [ValidateRange(3,3600)][int]$MaxSeconds = 900
)
$ErrorActionPreference = 'Stop'
$instance = 'MoonlightSandbox-' + [Guid]::NewGuid().ToString('N')
$presentMonSession = $instance + '-PresentMon'
$started = $false
$presentMon = $null
$result = [ordered]@{ instance = $instance; etw_mode = 'memory'; start_exit = $null; stop_exit = $null;
    presentmon_session = $presentMonSession; presentmon_exit = $null; stop_reason = $null; error = $null }
$profile = Join-Path $PSScriptRoot 'MoonlightVrrFull.wprp'
$presentMonExe = Join-Path $PSScriptRoot 'PresentMon-2.5.1-x64.exe'
$directoryItem = Get-Item -LiteralPath $Directory
if ($directoryItem.FullName.StartsWith('\\') -or $directoryItem.PSDrive.DisplayRoot) {
    throw 'ETW capture must use a local directory.'
}
try {
    # Memory mode: the scheduler detail lives in circular buffers and only the
    # most recent part is written, once, at stop.
    & wpr.exe -start "$profile!MoonlightVrrFull" -instancename $instance > (Join-Path $Directory 'wpr-start.txt') 2>&1
    $result.start_exit = $LASTEXITCODE
    if ($LASTEXITCODE -ne 0) { throw "WPR start failed: $LASTEXITCODE" }
    $started = $true
    # PresentMon consumes its own ETW session live for the whole capture and
    # writes only its CSV; nothing has to be parsed after Moonlight exits.
    $presentMon = Start-Process -FilePath $presentMonExe -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $Directory 'presentmon.stdout.txt') `
        -RedirectStandardError (Join-Path $Directory 'presentmon.stderr.txt') `
        -ArgumentList @('--process_name', 'Moonlight.exe',
            '--output_file', ('"' + (Join-Path $Directory 'PresentMon.csv') + '"'),
            '--v1_metrics', '--qpc_time', '--no_console_stats', '--no_track_input', '--track_gpu_video',
            '--session_name', $presentMonSession, '--stop_existing_session')
    # Reading the handle now keeps ExitCode available after the process exits.
    $null = $presentMon.Handle
    Start-Sleep -Milliseconds 500
    if ($presentMon.HasExited) { throw "PresentMon exited at start: $($presentMon.ExitCode)" }
    [IO.File]::WriteAllText((Join-Path $Directory 'recorder-ready'), $instance)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        if (Test-Path -LiteralPath (Join-Path $Directory 'recorder-stop')) { $result.stop_reason = 'requested'; break }
        if (-not (Get-Process -Id $ParentProcessId -ErrorAction SilentlyContinue)) { $result.stop_reason = 'parent_exited'; break }
        if ($timer.Elapsed.TotalSeconds -ge $MaxSeconds) { $result.stop_reason = 'time_limit'; break }
        if ($presentMon.HasExited) { $result.stop_reason = 'presentmon_exited'; break }
        $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($directoryItem.FullName))
        if ($drive.AvailableFreeSpace -lt 5GB) { $result.stop_reason = 'disk_limit'; break }
        Start-Sleep -Milliseconds 250
    }
}
catch { $result.error = $_.Exception.Message }
finally {
    # Preserve native stderr in the logs even if a provider fails. Do not
    # let Windows PowerShell's stderr ErrorRecord bypass stop/manifest.
    $ErrorActionPreference = 'Continue'
    if ($null -ne $presentMon) {
        if (-not $presentMon.HasExited) {
            # Stopping its session lets PresentMon flush the CSV and exit.
            & $presentMonExe --terminate_existing_session --session_name $presentMonSession > (Join-Path $Directory 'presentmon-stop.txt') 2>&1
            if (-not $presentMon.WaitForExit(30000)) { $presentMon.Kill() }
        }
        $presentMon.WaitForExit()
        $result.presentmon_exit = $presentMon.ExitCode
    }
    if ($started) {
        & wpr.exe -status -instancename $instance > (Join-Path $Directory 'wpr-status.txt') 2>&1
        & wpr.exe -stop (Join-Path $Directory 'Windows.etl') -instancename $instance > (Join-Path $Directory 'wpr-stop.txt') 2>&1
        $result.stop_exit = $LASTEXITCODE
        # Only our successfully started instance may be cancelled.
        if ($LASTEXITCODE -ne 0) { & wpr.exe -cancel -instancename $instance > (Join-Path $Directory 'wpr-cancel.txt') 2>&1 }
    }
    $ErrorActionPreference = 'Stop'
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Directory 'recorder-result.json') -Encoding utf8
}
if ($result.error -or $result.start_exit -ne 0 -or $result.stop_exit -ne 0) { exit 1 }
