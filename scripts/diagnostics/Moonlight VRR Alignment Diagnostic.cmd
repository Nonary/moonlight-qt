@echo off
rem Schema 6 records lost_packets; use the matching replay to preserve loss eligibility.
rem Alignment adds driver queries around Present and may perturb timing.
rem Windows VRR uses DXGI at full refresh; MOONLIGHT_VRR_COMPOSITION=0 also forces it below refresh.
rem Use Moonlight VRR Composition A-B.cmd for early enqueue and decode overlap.
rem Main trace schema 6 and passive GPU sidecars support fresh exact replay.
if "%~1"=="--composition-gap-refresh" goto gap
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
:gap
call "%~dp0Moonlight VRR Diagnostic.cmd" --composition-gap-refresh --align
exit /b %errorlevel%
