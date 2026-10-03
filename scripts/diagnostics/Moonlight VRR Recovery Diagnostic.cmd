@echo off
rem Compatibility shortcut: synchronized recovery is no longer an active policy.
echo The old synchronized-recovery override is no longer used by this build.
echo Starting the current full timing capture with your existing pacing settings.
call "%~dp0Moonlight VRR Full Diagnostic.cmd"
exit /b %errorlevel%
