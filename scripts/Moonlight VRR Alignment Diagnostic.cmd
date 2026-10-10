@echo off
rem Schema 5 appends lost_packets; use the bundled replay to preserve loss eligibility.
rem Default composition reports display events; raster alignment probes require MOONLIGHT_VRR_COMPOSITION=0.
rem Production VRR queue: revision 7, with 0.5 ms Low Latency/Balanced Target tolerance, 0.2 ms Smooth tolerance, and severity-weighted 99/99.5/99.99 percent preset targets. Reconnect after changing the latency preset.
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
