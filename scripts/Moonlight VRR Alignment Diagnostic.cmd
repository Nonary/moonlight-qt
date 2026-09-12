@echo off
rem Live A/B: V2 Queue off uses revision 4; on uses revision 7, with 0.5 ms tolerance and severity-weighted 99/99.5/99.95 percent preset targets. Reconnect after changing the checkbox.
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
