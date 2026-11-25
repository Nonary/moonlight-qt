@echo off
rem Build just the virtual stream simulator and pass its arguments through.
setlocal EnableExtensions
for %%I in ("%~dp0..") do set "SIM_REPO=%%~fI"
set "SIM_BUILD=%SIM_REPO%\build\tests-vrr\vrr"
set "SIM_QT=%SIM_REPO%\..\.tools\Qt\6.11.1\msvc2022_64\bin"
if not defined VSCMD_VER (
    call "%SIM_REPO%\..\.tools\vs-buildtools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
    if errorlevel 1 exit /b 1
)
set "PATH=%SIM_QT%;%SIM_REPO%\libs\windows\lib\x64;%PATH%"
if not exist "%SIM_BUILD%" mkdir "%SIM_BUILD%"
pushd "%SIM_BUILD%" || exit /b 1
if not exist Makefile.buffersim (
    qmake -o Makefile.buffersim "%SIM_REPO%\tests\vrr\buffersim.pro" CONFIG+=release
    if errorlevel 1 goto build_failed
)
nmake /nologo /s /f Makefile.buffersim release
if errorlevel 1 goto build_failed
popd
"%SIM_BUILD%\release\vrrbuffersim.exe" %*
exit /b %ERRORLEVEL%

:build_failed
popd
exit /b 1
