@echo off
setlocal EnableExtensions
echo Enable Variable Refresh Rate in Moonlight for both recordings.
choice /C 1234 /N /M "Choose 1 for baseline, 2 for early enqueue, 3 for decode overlap, or 4 for the 120 FPS gap refresh fix: "
if errorlevel 5 exit /b 1
if errorlevel 4 goto gap
if errorlevel 3 goto overlap
if errorlevel 2 goto early
if not errorlevel 1 exit /b 1
call "%~dp0Moonlight VRR Diagnostic.cmd" --composition-baseline
exit /b %errorlevel%

:early
call "%~dp0Moonlight VRR Diagnostic.cmd" --composition-early
exit /b %errorlevel%

:overlap
call "%~dp0Moonlight VRR Diagnostic.cmd" --composition-overlap
exit /b %errorlevel%

:gap
call "%~dp0Moonlight VRR Diagnostic.cmd" --composition-gap-refresh
exit /b %errorlevel%
