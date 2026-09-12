@echo off
rem Production VRR queue: revision 7, with 0.5 ms tolerance and severity-weighted 99/99.5/99.95 percent preset targets. Reconnect after changing the latency preset.
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
