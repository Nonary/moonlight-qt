@echo off
rem Captures the versioned VRR15 smoothness-feedback predictor, headroom credit, and original scanout deadlines.
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
