@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set WS=C:\Users\Dana\Documents\deepseek-harness\default-workspace
set RSM=D:\Codes\recursive-shared-mutex
set CATCH=%RSM%\build\_deps\catch2-src
cd /d %WS%
echo === merged tree: MSVC ===
if exist m-msvc rmdir /s /q m-msvc
cmake -S %RSM% -B m-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release -DRSM_BUILD_TESTS=ON -DRSM_BUILD_STRESS=ON -DFETCHCONTENT_SOURCE_DIR_CATCH2=%CATCH% >nul 2>&1
cmake --build m-msvc >m-msvc.log 2>&1
if errorlevel 1 (echo BUILD FAILED & type m-msvc.log & exit /b 1)
ctest --test-dir m-msvc --timeout 180 2>&1 | findstr /C:"tests passed" /C:"tests failed"
echo === merged tree: MinGW ===
if exist m-gcc rmdir /s /q m-gcc
cmake -S %RSM% -B m-gcc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=C:\Strawberry\c\bin\g++.exe -DRSM_BUILD_TESTS=ON -DFETCHCONTENT_SOURCE_DIR_CATCH2=%CATCH% >nul 2>&1
cmake --build m-gcc >m-gcc.log 2>&1
if errorlevel 1 (echo BUILD FAILED & type m-gcc.log & exit /b 1)
ctest --test-dir m-gcc --timeout 180 2>&1 | findstr /C:"tests passed" /C:"tests failed"
echo === merged tree: clang ===
if exist m-clang rmdir /s /q m-clang
cmake -S %RSM% -B m-clang -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="C:\Program Files\LLVM\bin\clang++.exe" -DRSM_BUILD_TESTS=ON -DFETCHCONTENT_SOURCE_DIR_CATCH2=%CATCH% >nul 2>&1
cmake --build m-clang >m-clang.log 2>&1
if errorlevel 1 (echo BUILD FAILED & type m-clang.log & exit /b 1)
ctest --test-dir m-clang --timeout 180 2>&1 | findstr /C:"tests passed" /C:"tests failed"
endlocal
