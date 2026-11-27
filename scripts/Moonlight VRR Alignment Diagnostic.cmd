@echo off
rem Schema 5 appends lost_packets; use the bundled replay to preserve loss eligibility.
rem Windows VRR uses DXGI at full refresh; MOONLIGHT_VRR_COMPOSITION=0 also forces it below refresh.
rem Use Moonlight VRR Composition A-B.cmd for early enqueue and decode overlap.
rem The actual presenter and native timing source are recorded in the capture.
if "%~1"=="--composition-gap-refresh" goto gap
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
:gap
call "%~dp0Moonlight VRR Diagnostic.cmd" --composition-gap-refresh --align
exit /b %errorlevel%
