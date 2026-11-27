#!/usr/bin/env bash
# Headless benchmark of both binaries (d3ptmark-pt, d3ptmark-le): an OpenGL baseline,
# then every quality preset on every available backend. PRESETS lists preset:spp pairs (spp per demo frame).
#
#   ./benchmark.sh [demo]
#
# The window is always 1280x1024, the classic Doom 3 benchmark resolution; the presets
# scale the traced resolution from it (r_ptRenderScale).
#
# D3_BASEPATH points at the Doom 3 data (directory containing base/), default ./d3data.
# Results: results/<timestamp>/summary.csv and one directory per run with
#   console.log
#   ptreports|glreports/benchmark/run_00001/   metadata.json, frames.csv, settings.csv, telemetry.csv
#   ptbench|glbench/<demo>/.../frame_*.png    every benchmarked frame

cd "$(dirname "$0")"
DEMO="${1:-demo1.demo}"
RES="1280x1024"
BASEPATH="${D3_BASEPATH:-$PWD/d3data}"
OUT="$PWD/results/$(date +%Y%m%d_%H%M%S)"
PRESETS="low:1 gameplay_low:1 cinematic_low:1"	# preset:spp per demo frame

mkdir -p "$OUT"
echo "run,exit_code,fps" > "$OUT/summary.csv"

run() {	# name binary engine-args...
	local name="$1" binary="$2"
	shift 2
	echo "== $name"
	mkdir -p "$OUT/$name"
	# savedata is shared so the OpenCL kernel cache is reused; the run's screenshots and
	# reports are moved into its own directory afterwards
	"build/$binary" --headless +set fs_basepath "$BASEPATH" +set fs_savepath "$OUT/savedata" \
		+set fs_configpath "$OUT/config/$name" +set com_skipIntroVideos 1 +set r_screenshotFormat 2 \
		+set r_mode -1 +set r_customWidth "${RES%x*}" +set r_customHeight "${RES#*x}" \
		"$@" > "$OUT/$name/console.log" 2>&1
	local code=$?
	if [ -d "$OUT/savedata/base/screenshots" ]; then
		mv "$OUT/savedata/base/screenshots"/* "$OUT/$name/" 2>/dev/null
		rmdir "$OUT/savedata/base/screenshots"
	fi
	local fps=$(sed -n 's/.*Effective playback: \([0-9.]*\).*/\1/p' "$OUT/$name/console.log")
	echo "$name,$code,$fps" | tee -a "$OUT/summary.csv"
	return $code
}

for binary in d3ptmark-pt d3ptmark-le; do
	if [ ! -x "build/$binary" ]; then
		echo "build/$binary not found, skipping it"
		continue
	fi
	run "${binary#d3ptmark-}_opengl" "$binary" +r_glBenchmark "$DEMO" uncapped save_image 1
	for backend in cuda opencl; do
		# warm-up on a one-frame demo: compiles/caches the OpenCL kernels outside the measured
		# runs, and skips backends this binary or machine cannot use
		run "${binary#d3ptmark-}_${backend}_warmup" "$binary" +set r_ptBackend "$backend" \
			+r_ptBenchmark shot_demo001 1 uncapped save_image 0 || continue
		for entry in $PRESETS; do
			preset="${entry%:*}" spp="${entry#*:}"
			run "${binary#d3ptmark-}_${backend}_${preset}" "$binary" +set r_ptBackend "$backend" \
				+set r_ptPreset "$preset" +r_ptBenchmark "$DEMO" "$spp" uncapped save_image 1
		done
	done
done

echo "summary: $OUT/summary.csv"
