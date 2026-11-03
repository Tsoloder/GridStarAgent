@echo off
setlocal
set "QTDIR=D:\Application\Qt\Qt5.12.2\5.12.2\msvc2017_64"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "ROOT=%~dp0"
call "%VCVARS%"
set "PATH=%QTDIR%\bin;%ROOT%bin;%PATH%"
if not exist "%ROOT%build\dockproj" mkdir "%ROOT%build\dockproj"
cd /d "%ROOT%build\dockproj"
qmake "%ROOT%examples\dock_example.pro" -spec win32-msvc "CONFIG+=release"
nmake