@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "MOONLIGHT_DIAG_PORTABLE=%~dp0"
title Moonlight Deep Trace

if not exist "%MOONLIGHT_DIAG_PORTABLE%Moonlight.exe" (
    echo Extract this BAT beside Moonlight.exe, then run it again.
    goto failed
)

rem Trace I/O must stay on a local disk, including when a share is mapped.
rem Pass paths through the environment so quotes and shell characters are safe.
powershell.exe -NoLogo -NoProfile -Command "$ErrorActionPreference='Stop'; try { $p=$env:MOONLIGHT_DIAG_PORTABLE; if ($p.StartsWith('\\') -or ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($p))).DriveType -eq [IO.DriveType]::Network) { throw 'Extract the portable and this BAT to a local disk before tracing.' }; if (Get-Process Moonlight -ErrorAction SilentlyContinue) { throw 'Close Moonlight normally before starting this launcher.' } } catch { Write-Host $_.Exception.Message; exit 1 }"
if errorlevel 1 goto failed

set "STAMP="
for /f "delims=" %%s in ('powershell.exe -NoLogo -NoProfile -Command "(Get-Date -Format yyyyMMdd-HHmmss-fff) + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)"') do set "STAMP=%%s"
if not defined STAMP goto failed
set "MOONLIGHT_DIAG_OUTPUT=%MOONLIGHT_DIAG_PORTABLE%VRR-Logs\%STAMP%"
mkdir "%MOONLIGHT_DIAG_OUTPUT%"
if errorlevel 1 goto failed

set "MOONLIGHT_VRR_TRACE=%MOONLIGHT_DIAG_OUTPUT%\Moonlight.vrrtrace"
set "MOONLIGHT_VRR_DEEP_TRACE=1"

rem Record the exact executable and any inherited comparison overrides.
powershell.exe -NoLogo -NoProfile -Command "$ErrorActionPreference='Stop'; try { $exe=Join-Path $env:MOONLIGHT_DIAG_PORTABLE 'Moonlight.exe'; [ordered]@{ started_utc=[DateTime]::UtcNow.ToString('o'); executable=$exe; executable_sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash; file_version=(Get-Item -LiteralPath $exe).VersionInfo.FileVersion; trace=$env:MOONLIGHT_VRR_TRACE; deep_trace=$env:MOONLIGHT_VRR_DEEP_TRACE; separate_devices_override=$env:D3D11VA_FORCE_SEPARATE_DEVICES; alignment_override=$env:MOONLIGHT_VRR_ALIGN; sync_recovery_override=$env:MOONLIGHT_VRR_SYNC_RECOVERY } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $env:MOONLIGHT_DIAG_OUTPUT 'capture-info.json') -Encoding UTF8 } catch { Write-Host $_.Exception.Message; exit 1 }"
if errorlevel 1 goto failed

echo.
echo Start your stream with VRR enabled and reproduce the problem.
echo Then close Moonlight normally. Keep this window open until it finishes.
echo.
echo Saving logs and all trace segments to:
echo "%MOONLIGHT_DIAG_OUTPUT%"
echo.

pushd "%MOONLIGHT_DIAG_PORTABLE%"
if errorlevel 1 goto failed
rem Redirected stderr is Moonlight's supported session-log destination.
start "" /wait "%MOONLIGHT_DIAG_PORTABLE%Moonlight.exe" 1>"%MOONLIGHT_DIAG_OUTPUT%\Moonlight.stdout.log" 2>"%MOONLIGHT_DIAG_OUTPUT%\Moonlight.log"
set "MOONLIGHT_EXIT=%errorlevel%"
popd
>"%MOONLIGHT_DIAG_OUTPUT%\exit-code.txt" echo %MOONLIGHT_EXIT%

echo.
echo Moonlight exited with code %MOONLIGHT_EXIT%.
if not exist "%MOONLIGHT_DIAG_OUTPUT%\*.vrrtrace" (
    echo No VRR trace was created. Check that VRR was enabled for the stream.
    echo The session log is still available for troubleshooting.
)
echo Zip the ENTIRE folder below and send it back, including connection files:
echo "%MOONLIGHT_DIAG_OUTPUT%"
echo Also tell us approximately when the problem happened.
echo.
pause
exit /b %MOONLIGHT_EXIT%

:failed
echo.
echo Capture did not start. Resolve the message above and try again.
pause
exit /b 1
