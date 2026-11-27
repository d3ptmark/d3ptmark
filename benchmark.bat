@echo off
rem Headless benchmark of both binaries (d3ptmark-pt, d3ptmark-le): an OpenGL baseline,
rem then every quality preset on every available backend. PRESETS lists preset:spp pairs (spp per demo frame).
rem
rem   benchmark.bat [demo]
rem
rem The window is always 1280x1024, the classic Doom 3 benchmark resolution; the presets
rem scale the traced resolution from it (r_ptRenderScale).
rem
rem D3_BASEPATH points at the Doom 3 data (directory containing base\), default .\d3data.
rem Results: results\<timestamp>\summary.csv and one directory per run with
rem   console.log
rem   ptreports or glreports\benchmark\run_00001\   metadata.json, frames.csv, settings.csv, telemetry.csv
rem   ptbench or glbench\<demo>\...\frame_*.png    every benchmarked frame
rem On Windows headless mode falls back to a hidden window, so a desktop session is needed.

setlocal enabledelayedexpansion
cd /d "%~dp0"
set "DEMO=%~1"
if "%DEMO%"=="" set "DEMO=demo1.demo"
set "RES=1280x1024"
for /f "tokens=1,2 delims=x" %%a in ("%RES%") do (set "W=%%a" & set "H=%%b")
if "%D3_BASEPATH%"=="" (set "BASEPATH=%CD%\d3data") else (set "BASEPATH=%D3_BASEPATH%")
set "BIN=%CD%\build\RelWithDebInfo"
set "PRESETS=low:1 gameplay_low:1 cinematic_low:1"

for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "OUT=%CD%\results\%%t"
rem the engine writes its console output here, not to stdout
for /f "delims=" %%d in ('powershell -NoProfile -Command "[Environment]::GetFolderPath('MyDocuments')"') do set "ENGINELOG=%%d\My Games\dhewm3\dhewm3log.txt"
mkdir "%OUT%"
echo run,exit_code,fps> "%OUT%\summary.csv"

for %%b in (pt le) do if not exist "%BIN%\d3ptmark-%%b.exe" (
	echo %BIN%\d3ptmark-%%b.exe not found, skipping it
) else (
	call :run %%b_opengl d3ptmark-%%b +r_glBenchmark %DEMO% uncapped save_image 1
	for %%k in (cuda opencl) do (
		rem warm-up on a one-frame demo: caches the OpenCL kernels outside the measured
		rem runs, and skips backends this binary or machine cannot use
		call :run %%b_%%k_warmup d3ptmark-%%b +set r_ptBackend %%k +r_ptBenchmark shot_demo001 1 uncapped save_image 0
		if !RUNCODE! equ 0 (
			for %%p in (%PRESETS%) do for /f "tokens=1,2 delims=:" %%x in ("%%p") do (
				call :run %%b_%%k_%%x d3ptmark-%%b +set r_ptBackend %%k +set r_ptPreset %%x +r_ptBenchmark %DEMO% %%y uncapped save_image 1
			)
		)
	)
)

echo summary: %OUT%\summary.csv
exit /b 0

rem :run name binary engine-args...
:run
set "NAME=%~1"
set "EXE=%BIN%\%~2.exe"
set "ARGS="
shift
shift
:collect
if "%~1"=="" goto launch
set "ARGS=!ARGS! %1"
shift
goto collect
:launch
echo == %NAME%
mkdir "%OUT%\%NAME%"
rem savedata is shared so the OpenCL kernel cache is reused; the run's screenshots and
rem reports are moved into its own directory afterwards
start "" /wait "%EXE%" --headless +set fs_basepath "%BASEPATH%" +set fs_savepath "%OUT%\savedata" ^
	+set fs_configpath "%OUT%\config\%NAME%" +set com_skipIntroVideos 1 +set r_screenshotFormat 2 ^
	+set r_mode -1 +set r_customWidth %W% +set r_customHeight %H% !ARGS!
set "RUNCODE=!errorlevel!"
copy /y "%ENGINELOG%" "%OUT%\%NAME%\console.log" >nul 2>&1
for /d %%d in ("%OUT%\savedata\base\screenshots\*") do move "%%d" "%OUT%\%NAME%\" >nul
set "FPS="
for /f "tokens=3" %%f in ('findstr /c:"Effective playback:" "%OUT%\%NAME%\console.log" 2^>nul') do set "FPS=%%f"
echo %NAME%,!RUNCODE!,!FPS!>> "%OUT%\summary.csv"
echo %NAME%,!RUNCODE!,!FPS!
exit /b !RUNCODE!
