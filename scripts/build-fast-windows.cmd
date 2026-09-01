@echo off
rem Configure and build the incremental Windows release with absolute tool
rem paths. This avoids relying on PATH/Path lookup in jom child processes.
setlocal EnableExtensions

set "MOONLIGHT_REPO=%~dp0.."
for %%I in ("%MOONLIGHT_REPO%") do set "MOONLIGHT_REPO=%%~fI"
set "MOONLIGHT_BUILD=%MOONLIGHT_REPO%\build\build-x64-release"
set "MOONLIGHT_QT_BIN=C:\Users\Chase\sources\.tools\Qt\6.11.1\msvc2022_64\bin"
set "MOONLIGHT_VC_BIN=C:\Users\Chase\sources\.tools\vs-buildtools\VC\Tools\MSVC\14.44.35207\bin\HostX64\x64"
set "MOONLIGHT_SDK_BIN=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"
set "MOONLIGHT_VCVARS=C:\Users\Chase\sources\.tools\vs-buildtools\VC\Auxiliary\Build\vcvarsall.bat"

call "%MOONLIGHT_VCVARS%" x64
if errorlevel 1 exit /b %ERRORLEVEL%

rem Keep both spellings synchronized. The managed shell can provide both and
rem jom may otherwise pass a PATH that cannot resolve cl.exe or mt.exe.
set "PATH=%MOONLIGHT_VC_BIN%;%MOONLIGHT_SDK_BIN%;%MOONLIGHT_QT_BIN%;%MOONLIGHT_REPO%\scripts;%PATH%"
set "Path=%PATH%"

if not exist "%MOONLIGHT_BUILD%" mkdir "%MOONLIGHT_BUILD%"
pushd "%MOONLIGHT_BUILD%"

"%MOONLIGHT_QT_BIN%\qmake.exe" -r "%MOONLIGHT_REPO%\moonlight-qt.pro" ^
    "QMAKE_CC=%MOONLIGHT_VC_BIN%\cl.exe" ^
    "QMAKE_CXX=%MOONLIGHT_VC_BIN%\cl.exe" ^
    "QMAKE_LINK=%MOONLIGHT_REPO%\scripts\windows-link.cmd" ^
    "QMAKE_LINK_SHLIB=%MOONLIGHT_REPO%\scripts\windows-link.cmd" ^
    "QMAKE_LIB=%MOONLIGHT_VC_BIN%\lib.exe" ^
    "QMAKE_RC=%MOONLIGHT_SDK_BIN%\rc.exe" ^
    "QMAKE_MT=%MOONLIGHT_SDK_BIN%\mt.exe"
if errorlevel 1 (
    popd
    exit /b %ERRORLEVEL%
)

"%MOONLIGHT_REPO%\scripts\jom.exe" release
set "MOONLIGHT_BUILD_RESULT=%ERRORLEVEL%"
popd
exit /b %MOONLIGHT_BUILD_RESULT%
