@echo off
rem Schema 5 appends lost_packets; use the bundled replay to preserve loss eligibility.
rem Composition is preferred; set MOONLIGHT_VRR_COMPOSITION=0 for DXGI raster comparisons.
rem The actual presenter and native timing source are recorded in the capture.
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
