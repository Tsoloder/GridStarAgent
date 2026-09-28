@echo off
setlocal
set "ROOT=%~dp0"
set "QTDIR=D:\Application\Qt\Qt5.12.2\5.12.2\msvc2017_64"
set "APP=%ROOT%bin\demo.exe"
set "LIB=%ROOT%bin\QtChartWidget.dll"

if not exist "%QTDIR%\bin\Qt5Core.dll" (
    echo [ERROR] Qt runtime not found at "%QTDIR%".
    echo Edit QTDIR in this script if Qt is installed elsewhere.
    pause
    exit /b 1
)

if not exist "%APP%" goto build
if not exist "%LIB%" goto build
goto run

:build
echo QtChartWidget output is missing. Building demo...
call "%ROOT%build.bat"
if not exist "%APP%" (
    echo [ERROR] demo.exe is missing after the build.
    pause
    exit /b 1
)
if not exist "%LIB%" (
    echo [ERROR] QtChartWidget.dll is missing after the build.
    pause
    exit /b 1
)

:run
set "PATH=%QTDIR%\bin;%ROOT%bin;%PATH%"
set "QT_PLUGIN_PATH=%QTDIR%\plugins"
set "QT_QPA_FONTDIR=%WINDIR%\Fonts"
set "QT_QPA_PLATFORM=windows"
start "" /D "%ROOT%bin" "%APP%" %*
exit /b 0
