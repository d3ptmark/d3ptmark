# Benchmarking

## Path-traced render demo benchmark

```
r_ptBenchmark <demoname> <spp>
r_ptBenchmark demo1.demo 32
```

From the directory containing the executable and game data:

```
./dhewm3 +r_ptPreset high +r_ptBenchmark demo1.demo 32
```

This plays a **render demo** from `demos/` (`.demo` is optional), not a command
demo or a live-game cinematic. Every recorded frame, including the first, is
held until exactly the requested samples per pixel have been traced. Playback
advances by one recorded frame with no skipping or catch-up. Doom render demos
have a 30 Hz timeline: playback is capped at that rate, and slows down when a
frame takes longer. It does not attempt to preserve wall-clock speed by dropping
frames like ordinary demo playback.

The selected preset's resolution scale, bounce depth, light sampling and other
rendering settings are retained. The progressive low-quality startup ramp is
disabled. The sample target overrides the preset's *total* per-frame sampling:
equal-sized batches no larger than the preset's samples-per-dispatch are chosen
so the target is exact (32 spp with batch 4 takes eight dispatches; 5 spp with
batch 4 takes five single-sample dispatches). Accumulation and denoiser history
reset for every recorded frame. Each fully converged frame is saved to a separate
image before playback advances:

```
screenshots/ptbench/<demo>/<spp>spp_run_NNNNN/frame_NNNNNN_<spp>spp.<ext>
```

With `r_ptDenoise 1`, diffuse, specular and volumetric scattering are filtered
separately from their full per-frame accumulated samples. Fog variance is
bootstrapped spatially for low batch counts, including a single 8-spp dispatch.
Primary emission stays sharp. `r_ptDenoiseStrength` controls smoothing;
`r_ptDenoiseDebug 7` isolates the volume signal for short diagnostic runs.
Sky/miss rays also receive volume-only filtering, leaving sky radiance untouched.
Unsupported primary rays (glass, overlays and mixed silhouettes) retain the raw
progressive result. For comparable benchmark timings, keep denoising settings
and diagnostic mode unchanged throughout a run.

Paths are relative to the active game's `fs_savepath`. Each run uses an unused
numbered directory so previous captures are preserved. Frames are numbered from
one and saved at the preset's trace resolution, without HUD/UI. The format is
selected by `r_screenshotFormat` at benchmark start (0 TGA, 1 BMP, 2 PNG, 3 JPG),
with the normal PNG compression/JPG quality settings. Saving reuses the finished
PT image: it does not redraw, advance the demo or add sampling dispatches.

On completion, the console/log reports:

- Completed recorded-frame count, exact spp, trace resolution and dispatch count.
- Wall time and effective playback FPS, plus the corresponding recorded duration.
- Mean/min/median/p95/p99/max recorded-frame time.
- Total/mean GPU render time and total/mean screen-rendering time.
- Saved-image count, output directory and total image encoding/writing time.

Wall and recorded-frame timings include pacing, CPU work and the first frame's
backend/texture initialization and image encoding/writing, but exclude initial demo-file/map loading. GPU
timings cover tracing, denoising and postprocessing, not uploads/BVH construction,
readback or presentation. Screen-rendering timings include rendering, readback
and presentation but exclude image encoding/writing, demo decoding and between-screen waits. VSync and
other engine pacing therefore affect wall/screen results; compare identical
settings and report the logged configuration. This is a converged-frame playback
benchmark, not a claim of ordinary interactive temporal-denoised FPS.

`r_ptAbort` cancels, stops the demo and prints clearly marked partial results.
Already saved images are retained; an image-write failure aborts explicitly.
Disconnecting or replacing demo playback also ends the benchmark. The previous
`r_ptDraw`, `r_ptMode` and `r_ptAccumulation` settings are restored. Captures and
benchmarks cannot overlap; physical ImGui controls stay locked during the run.

Do **not** combine `timeDemo` with `r_ptSequence`: the latter single-steps live
game tics and saves screenshots, not recorded render-demo frames. `timeDemo`
remains the normal uncapped demo-throughput benchmark.

## main intro cutscene

+map game/mars_city1

## enpro cutscene

Low quality:

./dhewm3 +r_ptPreset low +map game/enpro +r_ptSequence 200 2920 8

Medium quality:


./dhewm3 +r_ptPreset med +map game/enpro +r_ptSequence 200 2920 16


High quality:

./dhewm3 +r_ptPreset high +map game/enpro +r_ptSequence 200 2920 32

Ultra quality:

./dhewm3 +r_ptPreset ultra +map game/enpro +r_ptSequence 200 2920 64

## enpro - demonic babe

teleport player to trigger cutscene:

##