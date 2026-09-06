@echo off
setlocal
set "QTDIR=D:\Application\Qt\Qt5.12.2\5.12.2\msvc2017_64"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "ROOT=%~dp0"
call "%VCVARS%"
set "PATH=%QTDIR%\bin;%ROOT%bin;%PATH%"
if not exist "%ROOT%build\lib" mkdir "%ROOT%build\lib"
if not exist "%ROOT%build\demo" mkdir "%ROOT%build\demo"
echo ==== build QtChartWidget.dll ====
pushd "%ROOT%build\lib"
"%QTDIR%\bin\qmake.exe" "%ROOT%QtChartWidget.pro" -spec win32-msvc "CONFIG+=release"
nmake
popd
echo ==== build demo.exe ====
pushd "%ROOT%build\demo"
"%QTDIR%\bin\qmake.exe" "%ROOT%demo\demo.pro" -spec win32-msvc "CONFIG+=release"
nmake
popd
echo ==== build host_example.exe ====
if not exist "%ROOT%build\example" mkdir "%ROOT%build\example"
pushd "%ROOT%build\example"
"%QTDIR%\bin\qmake.exe" "%ROOT%examples\examples.pro" -spec win32-msvc "CONFIG+=release"
nmake
popd
echo ==== output dir ====
dir /b "%ROOT%bin"
if /i "%~1"=="shot" "%ROOT%bin\demo.exe" --shot "%ROOT%shot.png"
endlocal
