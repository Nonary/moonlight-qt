[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Directory,
    [Parameter(Mandatory)][int]$ParentProcessId,
    [ValidateRange(3,900)][int]$MaxSeconds = 300
)
$ErrorActionPreference = 'Stop'
$instance = 'MoonlightSandbox-' + [Guid]::NewGuid().ToString('N')
$started = $false
$result = [ordered]@{ instance = $instance; start_exit = $null; stop_exit = $null; stop_reason = $null; error = $null }
$profile = Join-Path $PSScriptRoot 'MoonlightVrrFull.wprp'
$directoryItem = Get-Item -LiteralPath $Directory
if ($directoryItem.FullName.StartsWith('\\') -or $directoryItem.PSDrive.DisplayRoot) {
    throw 'ETW capture must use a local directory.'
}
try {
    & wpr.exe -start "$profile!MoonlightVrrFull" -filemode -recordtempto $Directory -instancename $instance > (Join-Path $Directory 'wpr-start.txt') 2>&1
    $result.start_exit = $LASTEXITCODE
    if ($LASTEXITCODE -ne 0) { throw "WPR start failed: $LASTEXITCODE" }
    $started = $true
    [IO.File]::WriteAllText((Join-Path $Directory 'recorder-ready'), $instance)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        if (Test-Path -LiteralPath (Join-Path $Directory 'recorder-stop')) { $result.stop_reason = 'requested'; break }
        if (-not (Get-Process -Id $ParentProcessId -ErrorAction SilentlyContinue)) { $result.stop_reason = 'parent_exited'; break }
        if ($timer.Elapsed.TotalSeconds -ge $MaxSeconds) { $result.stop_reason = 'time_limit'; break }
        $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($directoryItem.FullName))
        if ($drive.AvailableFreeSpace -lt 5GB) { $result.stop_reason = 'disk_limit'; break }
        Start-Sleep -Milliseconds 250
    }
}
catch { $result.error = $_.Exception.Message }
finally {
    if ($started) {
        # Preserve native stderr in the logs even if a provider fails. Do not
        # let Windows PowerShell's stderr ErrorRecord bypass stop/manifest.
        $ErrorActionPreference = 'Continue'
        & wpr.exe -status -instancename $instance > (Join-Path $Directory 'wpr-status.txt') 2>&1
        & wpr.exe -stop (Join-Path $Directory 'Windows.etl') -instancename $instance > (Join-Path $Directory 'wpr-stop.txt') 2>&1
        $result.stop_exit = $LASTEXITCODE
        # Only our successfully started instance may be cancelled.
        if ($LASTEXITCODE -ne 0) { & wpr.exe -cancel -instancename $instance > (Join-Path $Directory 'wpr-cancel.txt') 2>&1 }
        $ErrorActionPreference = 'Stop'
    }
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Directory 'recorder-result.json') -Encoding utf8
}
if ($result.error -or $result.start_exit -ne 0 -or $result.stop_exit -ne 0) { exit 1 }
