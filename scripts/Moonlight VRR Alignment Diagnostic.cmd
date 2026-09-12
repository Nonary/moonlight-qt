@echo off
rem Live A/B: V2 Queue off uses revision 4; on uses revision 5, averaging missed-frame lateness with a 1 ms deadband. Reconnect after changing the checkbox.
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
