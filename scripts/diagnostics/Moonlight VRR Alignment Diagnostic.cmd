@echo off
rem Schema 6 records lost_packets; use the matching replay to preserve loss eligibility.
rem Alignment adds driver queries around Present and may perturb timing.
rem Waitable DXGI is the VRR default; MOONLIGHT_VRR_COMPOSITION=1 opts into composition.
rem Main trace schema 6 and passive GPU sidecars support fresh exact replay.
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
