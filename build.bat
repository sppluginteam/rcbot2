@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem RCBot2 DoD:S Windows build: x86 + x64, then stage a release package.
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "NO_ARCHIVE=0"
if /I "%~1"=="--no-zip" set "NO_ARCHIVE=1"

if not defined SM_PATH set "SM_PATH=%ROOT%\deps\sourcemod"
if not exist "%SM_PATH%\core\logic\ExtensionSys.cpp" if exist "%ROOT%\sourcemod\core\logic\ExtensionSys.cpp" set "SM_PATH=%ROOT%\sourcemod"
if not defined MMS_PATH set "MMS_PATH=%ROOT%\deps\mmsource"
if not exist "%MMS_PATH%\core\metamod_plugins.cpp" if exist "%ROOT%\mmsource\core\metamod_plugins.cpp" set "MMS_PATH=%ROOT%\mmsource"
if not defined HL2SDK_ROOT set "HL2SDK_ROOT=%ROOT%\deps"
if not exist "%HL2SDK_ROOT%\hl2sdk-dods\public" if exist "%ROOT%\hl2sdk-dods\public" set "HL2SDK_ROOT=%ROOT%"

if not exist "%SM_PATH%\core\logic\ExtensionSys.cpp" (echo SourceMod not found & exit /b 1)
if not exist "%MMS_PATH%\core\metamod_plugins.cpp" (echo Metamod:Source not found & exit /b 1)
if not exist "%HL2SDK_ROOT%\hl2sdk-dods\public" (echo hl2sdk-dods not found & exit /b 1)
python -c "import ambuild2" >nul 2>&1 || python -m pip install --user ambuild

call :build_one x86 x86
if errorlevel 1 exit /b 1
call :build_one x64 x86_x64
if errorlevel 1 exit /b 1

set "DIST=%ROOT%\dist"
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%"
xcopy /E /I /Y "%ROOT%\build_x86\package\*" "%DIST%\" >nul
mkdir "%DIST%\addons\rcbot2\bin\x64" 2>nul
xcopy /E /I /Y "%ROOT%\build_x64\package\addons\rcbot2\bin\x64\*" "%DIST%\addons\rcbot2\bin\x64\" >nul

if "%NO_ARCHIVE%"=="0" powershell -NoProfile -Command "Compress-Archive -Path '%DIST%\addons' -DestinationPath '%ROOT%\RCBot2-dods-windows.zip' -Force"
echo Built: %DIST%
exit /b 0

:build_one
set "ARCH=%~1"
set "VCARCH=%~2"
set "BD=%ROOT%\build_%ARCH%"
if exist "%BD%" rmdir /s /q "%BD%"
mkdir "%BD%"
call "%VSINSTALLDIR%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul 2>&1
if errorlevel 1 (echo Run from a VS developer prompt or set VSINSTALLDIR & exit /b 1)
cd /d "%BD%"
python "%ROOT%\configure.py" --sm-path "%SM_PATH%" --mms-path "%MMS_PATH%" --hl2sdk-root "%HL2SDK_ROOT%" --sdks=dods --targets=%ARCH% --enable-optimize
if errorlevel 1 exit /b 1
ambuild
