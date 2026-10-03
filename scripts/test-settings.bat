@echo off
setlocal
set "SOURCE_ROOT=%~dp0.."
set "TEST_VS_INSTALL="
for /f "usebackq delims=" %%i in (`"%~dp0vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TEST_VS_INSTALL=%%i"
if not defined TEST_VS_INSTALL exit /b 1
call "%TEST_VS_INSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
set "Path=%PATH%"
set "QT_QPA_PLATFORM=offscreen"
set "QT_QUICK_BACKEND=software"
set "QT_QUICK_CONTROLS_STYLE=Material"
set "QT_QUICK_CONTROLS_MATERIAL_THEME=Dark"
set "QT_QPA_FONTDIR=%WINDIR%\Fonts"
call :runSuite navigation tst_navigation
if errorlevel 1 exit /b 1
call :runSuite profiles tst_profiles
if errorlevel 1 exit /b 1
call :runSuite game-settings tst_game_settings
exit /b %errorlevel%

:runSuite
set "TEST_BUILD=%SOURCE_ROOT%\build\ci-tests-%1"
if not exist "%TEST_BUILD%" mkdir "%TEST_BUILD%"
pushd "%TEST_BUILD%"
qmake "%SOURCE_ROOT%\tests\%1\%1.pro"
if errorlevel 1 goto suiteFailed
nmake /NOLOGO release
if errorlevel 1 goto suiteFailed
"release\%2.exe" -o results.txt,txt -o -,txt
if errorlevel 1 goto suiteFailed
popd
exit /b 0

:suiteFailed
popd
exit /b 1
