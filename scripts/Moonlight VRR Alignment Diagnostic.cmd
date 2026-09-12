@echo off
rem Captures responsive buffer revision 3 with preset readiness targets/windows and version-20 diagnostic history; older policies remain exactly replayable.
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
