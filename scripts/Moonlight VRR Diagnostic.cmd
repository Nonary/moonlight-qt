@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%SystemRoot%"
set "PORTABLE=\\allytwo\ChaseShare\MoonlightPortable-x64-6.1.0-vrr-lite"
set "UPLOAD_DIR=\\allytwo\ChaseShare\vrr-traces"
set "TRACE_DIR=%USERPROFILE%\vrr-traces"
if not exist "%PORTABLE%\Moonlight.exe" goto missing
if not exist "%PORTABLE%\vrrreplay.exe" goto missing
tasklist /FI "IMAGENAME eq Moonlight.exe" 2>nul | find /I "Moonlight.exe" >nul
if not errorlevel 1 (
    echo Close Moonlight normally before starting a diagnostic session.
    pause
    exit /b 1
)
if not exist "%TRACE_DIR%" mkdir "%TRACE_DIR%"
if not exist "%TRACE_DIR%" exit /b 1
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss-fff"') do set "STAMP=%%t"
if not defined STAMP exit /b 1
set "PREFIX=Moonlight-vrr"
set "MOONLIGHT_VRR_ALIGN="
if "%~1"=="--align" (
    set "PREFIX=Moonlight-vrr-aligned"
    set "MOONLIGHT_VRR_ALIGN=1"
)
set "MOONLIGHT_VRR_CAPTURE="
set "MOONLIGHT_VRR_TRACE=%TRACE_DIR%\%PREFIX%-%STAMP%.vrrtrace"
set "MOONLIGHT_VRR_DEEP_TRACE=1"
set "MOONLIGHT_BFI_FORCE_TEARING="
set "SUMMARY=%TRACE_DIR%\%PREFIX%-%STAMP%-replay.json"
echo Capture: "%MOONLIGHT_VRR_TRACE%"
echo The first 60 minutes are retained before the 512 MiB cap applies.
echo Close Moonlight normally when finished so the capture can be verified.
start "" /wait "%PORTABLE%\Moonlight.exe"
set "MOONLIGHT_EXIT=%errorlevel%"
if not exist "%MOONLIGHT_VRR_TRACE%" (
    echo No VRR capture was saved. Check that a VRR stream was started.
    pause
    exit /b 1
)
"%PORTABLE%\vrrreplay.exe" "%MOONLIGHT_VRR_TRACE%" --require-exact-baseline --output "%SUMMARY%" > "%SUMMARY%.stdout.txt" 2> "%SUMMARY%.stderr.txt"
set "REPLAY_EXIT=%errorlevel%"
if not "%REPLAY_EXIT%"=="0" echo Exact replay failed. This capture is exploratory only; inspect its report.
if not exist "%UPLOAD_DIR%" mkdir "%UPLOAD_DIR%"
set "UPLOAD_EXIT=0"
for %%f in ("%MOONLIGHT_VRR_TRACE%" "%SUMMARY%" "%SUMMARY%.stdout.txt" "%SUMMARY%.stderr.txt") do (
    if exist "%%~f" (
        copy /y "%%~f" "%UPLOAD_DIR%\" >nul
        if errorlevel 1 set "UPLOAD_EXIT=1"
    )
)
if "%UPLOAD_EXIT%"=="0" echo Capture and replay report copied to "%UPLOAD_DIR%".
if not "%UPLOAD_EXIT%"=="0" echo Copy failed. The original files remain in "%TRACE_DIR%".
pause
if not "%MOONLIGHT_EXIT%"=="0" exit /b %MOONLIGHT_EXIT%
if not "%UPLOAD_EXIT%"=="0" exit /b %UPLOAD_EXIT%
exit /b %REPLAY_EXIT%
:missing
echo Moonlight.exe or vrrreplay.exe is missing from "%PORTABLE%".
pause
exit /b 1
