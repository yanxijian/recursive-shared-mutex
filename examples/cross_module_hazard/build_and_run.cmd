@echo off
rem Build + run the cross-module hazard demo (Windows).
rem   build_and_run.cmd msvc    (uses the installed Visual Studio)
rem   build_and_run.cmd mingw   (uses g++ from PATH)
rem RSM_REPO may be set to point at the checkout; it defaults to the repo root
rem two levels up from this script (examples\cross_module_hazard -> root).
setlocal
set HERE=%~dp0
if "%RSM_REPO%"=="" (
	for %%i in ("%HERE%..\..") do set REPO=%%~fi
) else (
	set REPO=%RSM_REPO%
)
set INC=%REPO%\include
set OUT=%HERE%out
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%"

if /i "%~1"=="mingw" goto mingw
if /i "%~1"=="msvc" goto msvc
echo usage: build_and_run.cmd msvc^|mingw
exit /b 2

:msvc
where cl >nul 2>&1
if errorlevel 1 (
	echo [msvc] cl.exe not in PATH - run this from a "Developer Command Prompt"
	exit /b 1
)
cl /nologo /O2 /std:c++17 /EHsc /I"%INC%" /LD "%HERE%dll_side.cpp" /Fe:rsm_hazard.dll /link /IMPLIB:rsm_hazard.lib >nul 2>&1
if errorlevel 1 (echo [msvc] DLL build FAILED & exit /b 1)
cl /nologo /O2 /std:c++17 /EHsc /I"%INC%" "%HERE%exe_side.cpp" rsm_hazard.lib /Fe:hazard_msvc.exe >nul 2>&1
if errorlevel 1 (echo [msvc] EXE build FAILED & exit /b 1)
echo ===== MSVC (MSVC STL / SRWLOCK) =====
hazard_msvc.exe
exit /b 0

:mingw
where g++ >nul 2>&1
if errorlevel 1 (
	echo [mingw] g++ not in PATH
	exit /b 1
)
g++ -O2 -std=c++17 -I"%INC%" -shared "%HERE%dll_side.cpp" -o rsm_hazard_mingw.dll -Wl,--out-implib,librsm_hazard_mingw.a >nul 2>&1
if errorlevel 1 (echo [mingw] DLL build FAILED & exit /b 1)
g++ -O2 -std=c++17 -I"%INC%" "%HERE%exe_side.cpp" -L. -lrsm_hazard_mingw -o hazard_mingw.exe >nul 2>&1
if errorlevel 1 (echo [mingw] EXE build FAILED & exit /b 1)
echo ===== MinGW g++ (libstdc++ / winpthreads) =====
hazard_mingw.exe
exit /b 0
