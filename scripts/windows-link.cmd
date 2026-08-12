@echo off
rem Keep the Windows linker and its manifest tool discoverable from jom
rem workers, even when the invoking environment has duplicate PATH/Path
rem entries (as the managed development shell does).
setlocal
set "MOONLIGHT_VC_BIN=C:\Users\Chase\sources\.tools\vs-buildtools\VC\Tools\MSVC\14.44.35207\bin\HostX64\x64"
set "MOONLIGHT_SDK_BIN=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"
set "PATH=%MOONLIGHT_VC_BIN%;%MOONLIGHT_SDK_BIN%;%SystemRoot%\System32;%PATH%"
set "Path=%PATH%"
"%MOONLIGHT_VC_BIN%\link.exe" %*
exit /b %ERRORLEVEL%
