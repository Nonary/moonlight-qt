@echo off
rem Run the complete package workflow with absolute QMake tool paths. The
rem package script accepts MOONLIGHT_QMAKE_ARGS so jom workers do not depend
rem on PATH/Path lookup for the compiler or manifest tool.
setlocal EnableExtensions

set "MOONLIGHT_REPO=%~dp0.."
for %%I in ("%MOONLIGHT_REPO%") do set "MOONLIGHT_REPO=%%~fI"
set "MOONLIGHT_QT_BIN=C:\Users\Chase\sources\.tools\Qt\6.11.1\msvc2022_64\bin"
set "MOONLIGHT_VC_BIN=C:\Users\Chase\sources\.tools\vs-buildtools\VC\Tools\MSVC\14.44.35207\bin\HostX64\x64"
set "MOONLIGHT_SDK_BIN=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"
set "MOONLIGHT_GIT_BIN=C:\Users\Chase\sources\.tools\git\cmd"
set "MOONLIGHT_7ZIP_BIN=C:\Users\Chase\sources\.tools\7zip"
set "MOONLIGHT_VCVARS=C:\Users\Chase\sources\.tools\vs-buildtools\VC\Auxiliary\Build\vcvarsall.bat"

call "%MOONLIGHT_VCVARS%" x64
if errorlevel 1 exit /b %ERRORLEVEL%

set "PATH=%MOONLIGHT_GIT_BIN%;%MOONLIGHT_QT_BIN%;%MOONLIGHT_7ZIP_BIN%;%MOONLIGHT_VC_BIN%;%MOONLIGHT_SDK_BIN%;%MOONLIGHT_REPO%\scripts;%PATH%"
set "Path=%PATH%"

set /p MOONLIGHT_VERSION=<"%MOONLIGHT_REPO%\app\version.txt"
set "CI_VERSION=%MOONLIGHT_VERSION%-vrr-lite"
set "MOONLIGHT_QMAKE_ARGS=QMAKE_CC=%MOONLIGHT_VC_BIN%\cl.exe QMAKE_CXX=%MOONLIGHT_VC_BIN%\cl.exe QMAKE_LINK=%MOONLIGHT_REPO%\scripts\windows-link.cmd QMAKE_LINK_SHLIB=%MOONLIGHT_REPO%\scripts\windows-link.cmd QMAKE_LIB=%MOONLIGHT_VC_BIN%\lib.exe QMAKE_RC=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\rc.exe QMAKE_MT=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\mt.exe"

call "%MOONLIGHT_REPO%\scripts\build-arch.bat" release
set "MOONLIGHT_BUILD_RESULT=%ERRORLEVEL%"
exit /b %MOONLIGHT_BUILD_RESULT%
