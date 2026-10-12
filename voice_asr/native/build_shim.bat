@echo off
rem ============================================================================
rem  Build asr_shim.dll -- run once on a machine that has MSVC; the resulting
rem  DLL ships with the package to offline/intranet machines.
rem
rem  Why this layer exists: hand-writing whisper_full_params in Python (ctypes)
rem  is unreliable -- one field off and ctypes back-fills the wrong size, so
rem  whisper_full reads garbage or faults (seen: loops of "body's body's...").
rem  The C++ compiler computes the layout here, and Python only passes scalars,
rem  so no struct ever crosses the DLL boundary.
rem
rem  IMPORTANT: keep this file pure ASCII. cmd.exe parses it with the OEM code
rem  page (936 here), and non-ASCII bytes in comments break the parse with
rem  errors like "'OUTDIR' is not recognized".
rem
rem  Usage:  build_shim.bat  [dir with whisper.h]  [dir with whisper.dll]
rem  Defaults:  ..\runtime\include  and  ..\runtime
rem             (%HERE% already ends in native\, so .. is voice_asr\)
rem ============================================================================
setlocal enabledelayedexpansion

set HERE=%~dp0
set RUNTIME=%HERE%..\runtime
set INCDIR=%~1
if "%INCDIR%"=="" set INCDIR=%RUNTIME%\include
set DLLDIR=%~2
if "%DLLDIR%"=="" set DLLDIR=%RUNTIME%
set OUTDIR=%HERE%bin

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

rem --- locate vcvars64.bat (try the common VS installs) ---
set VCVARS=
for %%V in ("C:\Program Files\Microsoft Visual Studio\18\Community"
             "C:\Program Files\Microsoft Visual Studio\2022\Community"
             "C:\Program Files\Microsoft Visual Studio\2022\Professional"
             "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
             "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community"
             "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools") do (
    if exist "%%~V\VC\Auxiliary\Build\vcvars64.bat" set VCVARS=%%~V\VC\Auxiliary\Build\vcvars64.bat
)
if "%VCVARS%"=="" (
    echo [FATAL] vcvars64.bat not found; needs an MSVC C++ toolchain
    exit /b 1
)
echo [info] vcvars: %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 ( echo [FATAL] vcvars64 failed & exit /b 1 )

rem --- whisper.h and the ggml headers must all be present ---
if not exist "%INCDIR%\whisper.h" (
    echo [FATAL] missing %INCDIR%\whisper.h
    echo         run  python sync_runtime.py  first
    exit /b 1
)
if not exist "%INCDIR%\ggml.h" (
    echo [FATAL] missing %INCDIR%\ggml.h  ^(whisper.h includes it^)
    exit /b 1
)
if not exist "%DLLDIR%\whisper.dll" (
    echo [FATAL] missing %DLLDIR%\whisper.dll
    exit /b 1
)

echo === 1/4 dump exports of whisper.dll ===
dumpbin /nologo /exports "%DLLDIR%\whisper.dll" > "%OUTDIR%\exports_raw.txt" 2>&1
if errorlevel 1 ( echo [FATAL] dumpbin failed & exit /b 1 )

echo === 2/4 generate .def ===
python "%HERE%make_def.py" "%OUTDIR%\exports_raw.txt" "%OUTDIR%\whisper.def"
if errorlevel 1 ( echo [FATAL] make_def failed & exit /b 1 )

echo === 3/4 build import lib whisper.lib ===
lib /nologo /def:"%OUTDIR%\whisper.def" /out:"%OUTDIR%\whisper.lib" /machine:x64
if errorlevel 1 ( echo [FATAL] lib failed & exit /b 1 )

rem  NOTE: use forward slashes for cl's /I, /Fo and /Fe values. A trailing
rem  backslash inside a quoted argument escapes the closing quote (the "\"
rem  right before " makes cl lose the source filename: "D8003 missing source
rem  file name"). MSVC accepts forward slashes on Windows, so sidestep it.
set INC_FWD=%INCDIR:\=/%
set HERE_FWD=%HERE:\=/%
set OUT_FWD=%OUTDIR:\=/%

echo === 4/4 compile asr_shim.dll ===
cl /nologo /O2 /LD /EHsc /utf-8 /DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /I"%INC_FWD%" /I"%HERE_FWD%" ^
   "%HERE%asr_shim.cpp" ^
   /Fo"%OUT_FWD%/" /Fe"%OUT_FWD%/asr_shim.dll" ^
   /link "%OUTDIR%\whisper.lib"
if errorlevel 1 ( echo [FATAL] link failed & exit /b 1 )

rem --- keep only the final DLL ---
del /q "%OUTDIR%\exports_raw.txt" "%OUTDIR%\*.obj" "%OUTDIR%\*.exp" 2>nul
del /q "%OUTDIR%\whisper.lib" "%OUTDIR%\whisper.def" "%OUTDIR%\asr_shim.lib" 2>nul

echo === DONE ===
dir /b "%OUTDIR%\asr_shim.dll"