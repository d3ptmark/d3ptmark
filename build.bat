@echo off
rem Builds d3ptmark-le (legacy renderer) and d3ptmark-pt (CUDA path tracer) into build\.
rem Extra arguments are passed to CMake, e.g.:
rem   build.bat -DPATHTRACED=OFF
rem   build.bat -DDHEWM3LIBS=C:\path\to\dhewm3-libs\x86_64-w64-mingw32

setlocal
cd /d "%~dp0"

cmake -S neo -B build %*
if errorlevel 1 exit /b 1

cmake --build build --config RelWithDebInfo --parallel
if errorlevel 1 exit /b 1

endlocal
