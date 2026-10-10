@echo off
rem Schema 5 appends lost_packets; use the bundled replay to preserve loss eligibility.
rem Waitable DXGI is the VRR default; MOONLIGHT_VRR_COMPOSITION=1 opts into composition.
rem The actual presenter and native timing source are recorded in the capture.
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
