@echo off
rem Build + run the cross-module hazard demo.
rem   build_and_run.cmd msvc    (uses the installed Visual Studio)
rem   build_and_run.cmd mingw   (uses g++ from PATH / Strawberry)
setlocal
set HERE=%~dp0
set REPO=D:\Codes\recursive-shared-mutex
set INC=%REPO%\include
set OUT=%HERE%out
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%"

if /i "%~1"=="mingw" goto mingw
if /i "%~1"=="msvc" goto msvc
echo usage: build_and_run.cmd msvc^|mingw
exit /b 2

:msvc
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /std:c++17 /EHsc /I"%INC%" /LD "%HERE%dll_side.cpp" /Fe:rsm_hazard.dll /link /IMPLIB:rsm_hazard.lib >nul 2>&1
if errorlevel 1 (echo [msvc] DLL build FAILED & exit /b 1)
cl /nologo /O2 /std:c++17 /EHsc /I"%INC%" "%HERE%exe_side.cpp" rsm_hazard.lib /Fe:hazard_msvc.exe >nul 2>&1
if errorlevel 1 (echo [msvc] EXE build FAILED & exit /b 1)
echo ===== MSVC (MSVC STL / SRWLOCK) =====
hazard_msvc.exe
exit /b 0

:mingw
set GXX=C:\Strawberry\c\bin\g++.exe
"%GXX%" -O2 -std=c++17 -I"%INC%" -shared "%HERE%dll_side.cpp" -o rsm_hazard_mingw.dll -Wl,--out-implib,librsm_hazard_mingw.a >nul 2>&1
if errorlevel 1 (echo [mingw] DLL build FAILED & exit /b 1)
"%GXX%" -O2 -std=c++17 -I"%INC%" "%HERE%exe_side.cpp" -L. -lrsm_hazard_mingw -o hazard_mingw.exe >nul 2>&1
if errorlevel 1 (echo [mingw] EXE build FAILED & exit /b 1)
echo ===== MinGW g++ (libstdc++ / winpthreads) =====
hazard_mingw.exe
exit /b 0
