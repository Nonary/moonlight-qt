@echo off
rem Schema 5 appends lost_packets; use the bundled replay to preserve loss eligibility.
rem Alignment adds driver queries around Present and may perturb timing.
rem Default composition reports display events; raster alignment probes require MOONLIGHT_VRR_COMPOSITION=0.
rem Main trace schema 5 and passive GPU sidecars retain exact historical replay.
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
