@echo off
setlocal
set "PORTABLE=\\allytwo\ChaseShare\MoonlightPortable-x64-6.1.0-vrr-lite"
echo Windows will request permission for the timing recorder. Moonlight stays unelevated.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PORTABLE%\Moonlight-VRR-Full-Diagnostic.ps1" -Portable "%PORTABLE%" %*
if errorlevel 1 pause
