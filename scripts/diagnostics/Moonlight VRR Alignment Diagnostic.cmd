@echo off
rem Alignment adds driver queries around Present and may perturb timing.
rem Main trace schema 5 and passive GPU sidecars retain exact historical replay.
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
