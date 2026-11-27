# Doom 3 GPU Path Tracer

d3ptmark adds a GPU path tracer (`neo/renderer_pt/`) that replaces the OpenGL rasterizer for
3D views. The front end is compute-API agnostic and has two backends: **CUDA**
(`renderer_pt/cuda/`) and **OpenCL 1.2** (`renderer_pt/opencl/`, a file-for-file port of the
CUDA kernels for AMD, Intel and NVIDIA GPUs).

Every backend whose toolchain is found is compiled into both binaries:

| Binary | Path tracer |
|--------|-------------|
| `d3ptmark-pt` | with the experimental physical renderer (`PT_PHYSICAL`) |
| `d3ptmark-le` | without `PT_PHYSICAL`, next to the stock OpenGL renderer; OpenGL only when no backend toolchain is found |

`r_ptDraw 0` switches either binary to OpenGL.

## Building

The build options are listed in the [README](../README.md#building). Configuration fails
only when `d3ptmark-pt` is requested and neither backend can be built.

### CUDA

Needs the NVIDIA CUDA Toolkit and a host compiler it supports. Kernels are built for SM 75–90
(Turing to Hopper); `-DCMAKE_CUDA_ARCHITECTURES=75` builds a single generation faster.

If your GCC is newer than nvcc supports (e.g. GCC 16 with CUDA 13.1), point nvcc at an older
host compiler with `-DCMAKE_CUDA_HOST_COMPILER=g++-15`, or force it (unsupported):

```sh
CUDAFLAGS="-allow-unsupported-compiler -std=c++17 -D_GLIBCXX_DO_NOT_USE_BUILTIN_TRAITS" ./build.sh
```

The last define works around nvcc 13.1 failing on GCC 16's `__builtin_is_virtual_base_of`.
The flags are cached; reconfigure from scratch with `./clean.sh`.

### OpenCL

Enabled by default (`-DOPENCL=OFF` disables it) and needs only the OpenCL headers (`CL/cl.h`).
The ICD loader (`OpenCL.dll`, `libOpenCL.so.1`) is loaded when the backend starts, so the
binaries still run, falling back to OpenGL, on machines without an OpenCL driver.

The device needs OpenCL 1.2 and an online compiler. The embedded kernels are compiled on first
use (tens of seconds) and cached in `<fs_savepath>/<game>/ptcache/` (`r_ptOpenCLCache 0`
disables the cache). `r_ptBackend opencl` selects the backend; `auto` prefers CUDA. `r_ptInfo`
lists all OpenCL platforms and devices and marks the active one.

Differences from the CUDA kernels:

- Textures live in one texel pool behind a descriptor table (OpenCL 1.2 buffers cannot hold
  pointers).
- The LBVH sorts Morton codes with its own stable radix sort instead of CUB; the permutation
  is identical.
- The camera half-angle uses `tanpi()`, because some drivers return `tan(pi/4)` one ulp high,
  which shifts SVGF reprojection off the pixel grid.
- Intel devices compile with `-cl-intel-256-GRF-per-thread`; the default register mode
  faulted intermittently in the path tracing kernel.

### Tests

`-DTESTS=ON` builds the regression tests; run `ctest --output-on-failure` in the build
directory.

- CPU: capture report, GPU/CPU telemetry (against a fake `/sys`), benchmark, material cache,
  quality presets and, with `PATHTRACED`, the material editor.
- CUDA: `pt_cuda_lighting_test`, `pt_cuda_denoising_test`, `pt_cuda_postprocess_test`,
  `pt_cuda_physical_test`. They skip without a CUDA device.
- OpenCL: the same four as `pt_opencl_*`, plus `pt_opencl_lbvh_test`. They skip without an
  OpenCL GPU; `PT_OPENCL_TEST_PLATFORM` / `PT_OPENCL_TEST_DEVICE` pick a device by index.

They cover light transport and energy conservation, denoiser reuse/rejection/edge
preservation, postprocessing, the physical camera, BRDF, glass and fog, and LBVH ordering.

## Architecture

Each frame the front end walks the view's draw surfaces and lights, builds a flat scene
(vertices, triangles, materials, lights, textures) on the CPU and hands it to the backend.
The backend uploads it, builds a BVH on the GPU, traces, denoises and tone maps, and the
front end stretches the image over the viewport. 2D views, subviews (mirrors, remote
cameras, portal skies), the HUD and GUIs are still drawn with OpenGL.

| Path | Contents |
|------|----------|
| `pt_types.h` | Plain structs shared by host and device: vertices, triangles, materials, lights, BVH nodes, render parameters |
| `pt_backend.h` | `idPathTracerBackend`, the backend interface |
| `pt_scene.*`, `pt_scene_material.cpp` | Scene builder: geometry, Doom 3 material to PBR translation, lights, texture readback |
| `pt_render.*` | Front end: cvars, backend lifecycle, accumulation, viewport blit |
| `pt_commands.cpp` | `r_ptCompare`, `r_ptSequence`, `r_ptAbort` |
| `pt_benchmark.*`, `pt_capture_report.*`, `pt_gpu_telemetry.*` | Benchmarks, capture reports, sensor sampling |
| `pt_physical.*`, `pt_material_editor.*` | Physical renderer and material inspector (`PT_PHYSICAL`) |
| `cuda/` | CUDA backend, kernels and GPU LBVH builder |
| `opencl/` | OpenCL backend, `pt_cl_*.cl` kernel ports, launch layer and ICD loader |

### Adding a backend

1. Implement `idPathTracerBackend` in `renderer_pt/<api>/`:
   - `Init` / `Resize` allocate for the window size; `Render` may use any smaller trace size.
   - `UploadScene` uploads `PendingTextures()` into their `index` slots and drops cached
     textures when `TextureGeneration()` changes.
   - `Render` accumulates into a float buffer until `ResetAccumulation()`, then tone maps to
     RGBA8. Only `PT_MODE_PATHTRACE` accumulates.
   - Report unrecoverable device errors through `IsHealthy()`; the front end then falls back
     to OpenGL.
2. Export `PT_<API>_IsAvailable()`, `PT_<API>_CreateBackend()` and `PT_<API>_GetDeviceInfo()`
   and add them to `ptBackendFactories[]` in `pt_render.cpp`.
3. In `neo/CMakeLists.txt`, detect the API (`PT_HAVE_<API>`), add a `src_pt_<api>` list and the
   `HAVE_<API>` define in `add_engine_binary()`, following the CUDA and OpenCL blocks.
4. Select it with `r_ptBackend <name>`.

## Rendering

| `r_ptMode` | Mode |
|------------|------|
| 0 | Path tracing: Monte Carlo, Cook-Torrance BRDF, progressive accumulation |
| 1 | BVH debug: random colour per triangle |
| 2–6 | Texture debug: combined, base colour, albedo, normal, specular |
| 7 | Whitted ray tracing: direct light, Blinn-Phong, mirror reflections |

- **GPU LBVH** (Karras) rebuilt every frame, traversed stacklessly through escape links.
- **Materials** translated from Doom 3 stages: albedo, normal and specular maps, alpha test,
  blend modes, vertex colours, emission and glass (Fresnel, IOR). Shader registers are
  re-evaluated per frame and entity; textures are decoded from sRGB before shading.
- **Lighting**: stochastic direct lighting with soft shadows from every enabled light stage,
  including projected images, falloff and `r_lightScale`. Shadow rays pass through alpha
  tested and translucent surfaces. Indirect light uses cosine-weighted hemisphere sampling.
- **Area lights**: emissive triangles are sampled as lights, sharing a budget of 512 with
  engine lights (which take priority). Emitters beyond the budget still light bounce rays.
  Whitted mode does not use them.
- **Volumetric scattering**: ray marched with a Henyey-Greenstein phase function.
- **Accumulation**: a running mean that resets on camera, mode, resolution or scene changes
  (`r_ptResetOnSceneChange`) and ramps quality up over the first frames.
- **Output**: SVGF denoising, optional bloom and lens ghosts, Reinhard / ACES / Uncharted 2
  tone mapping with exposure and gamma.
- The **first-person weapon** is an overlay that is never clipped by nearby walls.

### Material and effect translation

- **Decals**: `blend gl_zero, gl_one_minus_src_color` (Doom 3 blood) acts as an inverse
  colour filter, so stains stay red; polygon-offset decals are separated from their receivers.
  `noShadows` alone does not make a surface a decal.
- **Additive and glow stages**: `blend add` keeps RGB even with zero alpha; `SRC_ALPHA, ONE`
  keeps its alpha weighting. Glow stages keep their own UV animation and vertex colour, and
  camera, bounce and light sampling all see the same glow.
- **Masked overlays**: `DST_ALPHA, ONE` overlays use a preceding alpha-only stage as their
  mask (e.g. the flashlight beam); unsupported masks are skipped with a warning.
- **Particles**: geometry generated by the particle/deform system is traced.
- **Not traced**: heat-haze and distortion shaders and alpha-write-only stages are omitted
  rather than drawn as white polygons.

### Bloom and lens ghosts

Off by default, available in both binaries. They work on the denoised HDR image before
tone mapping, so changing them never restarts convergence.

| CVar | Default | Meaning |
|------|---------|---------|
| `r_ptBloom` | `0` | Bloom strength, 0–1 (try `0.08`) |
| `r_ptBloomThreshold` | `1` | Highlight threshold in exposed linear units |
| `r_ptBloomRadius` | `0.025` | Blur radius as a fraction of image height |
| `r_ptLensFlare` | `0` | Screen-space lens ghosts from visible highlights, 0–1 (try `0.02`) |

## Denoising

A practical SVGF core inspired by [Q2VKPT](https://developer.nvidia.com/blog/path-tracing-quake-ii/),
not its full A-SVGF pipeline. It is on by default in path tracing mode (`r_ptDenoise`).

1. Each frame's fresh samples, not the progressive mean, are filtered, along with guides:
   position, normals, depth, material, roughness and reflectance. Pixels whose samples
   disagree on the primary surface bypass filtering.
2. Diffuse and specular transport are separated at the primary hit, demodulated, and
   reprojected through the previous camera. History is rejected on depth, normal, material,
   reflectance or (specular) roughness/view changes.
3. Temporal luminance moments with bounded history: up to 32 frames diffuse, 4–24 specular
   depending on roughness. New pixels bootstrap variance spatially; neighbourhood clipping
   and lighting-change rejection limit stale history.
4. Four variance-guided à-trous passes (spacing 1, 2, 4, 8), then reflectance is restored.
5. Volumetric scattering is filtered as a third signal with depth guidance only. It keeps at
   most 16 frames of history, discarded when the camera moves. Primary emission is never
   filtered.

`r_ptDenoiseStrength` scales smoothing. `r_ptAccumulation` controls the raw mean
independently of denoiser history; camera movement resets the raw mean but keeps
reprojectable history. Explicit resets, resolution/backend/texture/trace-setting changes,
camera cuts and `r_ptResetOnSceneChange 2` also discard denoiser history.

**Captures** (`r_ptCompare`, `r_ptSequence`, `r_ptBenchmark`) use an offline path instead:
full quality from the first frame, progressive means of every signal, and spatial filtering
of the accumulated result. Each captured tick is snapshotted, so animation cannot disturb it.
`r_ptDenoise 0` keeps captures as an unfiltered reference.

**Limits**: there are no object motion vectors, so moving or deforming surfaces and the view
weapon are filtered spatially only. Transparent, overlay and mixed-surface pixels keep the
raw progressive result. Silhouettes can shimmer at low sample counts, and sharp reflections
stay noisy. Storage is 488 bytes per traced pixel (about 241 MiB at 960x540).

### Diagnostics (`r_ptDenoiseDebug`)

Any non-zero mode enables the denoiser. Modes 1–4 bypass tone mapping; 5–7 show one filtered
signal through normal tone mapping. `r_ptDenoiseDebugSignal` (0 diffuse, 1 specular,
2 volume) selects the signal for modes 2–4.

| Mode | Display |
|------|---------|
| 1 | Coverage: green static, yellow spatial-only, red transparent, magenta overlay, orange mixed, black miss |
| 2 | History length: red one frame, green 32 frames |
| 3 | Relative noise (brighter is noisier) |
| 4 | Rejection: green reused, magenta reset, yellow dynamic, blue offscreen, red disocclusion, cyan normal/view, orange material, purple reflectance/roughness, white lighting, gray no samples |
| 5 / 6 / 7 | Filtered diffuse / specular / volume only |

## Physical renderer (`PT_PHYSICAL`, `d3ptmark-pt` only)

A build-time rendering model, not a preset. In `d3ptmark-pt`, path tracing mode always uses
it; debug and Whitted modes keep their shading.

- **Lights in physical units**: inverse-square point lights in meters, lumen-based light
  stages, lux-based directional lights and normalized spotlight profiles. All registered
  lights are used, not only those passing Doom 3's light-volume tests.
- **BRDF**: reciprocal GGX with height-correlated Smith visibility, dielectric IOR,
  Fresnel-weighted diffuse, and visible-normal sampling. Specular maps no longer imply
  metalness.
- **Emission** in cd/m² (nits), the same for camera and light sampling.
- **Photographic camera**: `EV100 = log2(N² / t · 100 / ISO)`,
  `exposure = 2^compensation / (1.2 · 2^EV100)`, thin-lens depth of field, and a lens FOV
  that also drives engine culling.
- **Disabled**: legacy volumetrics, light/specular/emission boosts, the single-bounce GI
  shortcut and radiance clamps. `r_ptSkyIntensity` lights bounce misses too.

Open the controls with `r_ptUI physical`.

| CVar | Default | Meaning |
|------|---------|---------|
| `r_ptPhysicalMetersPerUnit` | `0.0254` | Meters per engine unit (inches) |
| `r_ptPhysicalLightLumens` | `1500` | Flux of a unit-white light stage |
| `r_ptPhysicalSunLux` | `50000` | Illuminance of a unit-white directional light |
| `r_ptPhysicalEmissionNits` | `100` | Luminance of unit-white emission |
| `r_ptPhysicalIor` | `1.5` | Opaque dielectric IOR (F0 = 0.04) |
| `r_ptPhysicalRoughnessScale` | `1` | Scale for roughness estimated from legacy maps |
| `r_ptPhysicalISO` | `800` | Sensitivity |
| `r_ptPhysicalShutter` | `0.03333333` | Exposure time in seconds |
| `r_ptPhysicalFNumber` | `2.8` | Aperture, for exposure and blur |
| `r_ptPhysicalExposureCompensation` | `0` | Stops |
| `r_ptPhysicalFocalLength` | `35` | mm |
| `r_ptPhysicalSensorWidth` | `36` | mm; height follows the viewport aspect |
| `r_ptPhysicalFocusDistance` | `3` | Meters |
| `r_ptPhysicalDOF` | `1` | Thin-lens blur; `0` is a pinhole with the same exposure |

Exposure changes keep the accumulated estimate; lens, focus, material, scale and lighting
changes reset it. Depth of field disables temporal denoiser history (spatial filtering still
runs). This is a physical interpretation of uncalibrated assets, not measured PBR. Point lights
give hard shadows, the lens overrides the gameplay FOV, and the shutter gives no motion blur.

### Fog

A homogeneous single-scattering medium in a sphere centred on the camera. It has
Beer-Lambert extinction, coloured shadows and Henyey-Greenstein scattering, but no multiple
scattering. `r_ptVolumetric` does not affect it.

| CVar | Default | Meaning |
|------|---------|---------|
| `r_ptPhysicalFogDensity` | `0` | Extinction per meter, 0 = off (try `0.01`) |
| `r_ptPhysicalFogAlbedo` | `0.9` | Scattering / extinction |
| `r_ptPhysicalFogAnisotropy` | `0.3` | Phase asymmetry, positive scatters forward |
| `r_ptPhysicalFogRadius` | `30` | Sphere radius in meters |
| `r_ptPhysicalFogSteps` | `16` | Samples per ray segment |

### Materials

Glass is never guessed from names or alpha. An authored `.mtr` material can add:

```
ptMaterial {
    transmission 1          // 0..1, mix of dielectric transport and opaque GGX
    ior 1.5                 // 1..3
    thin 0                  // 0 solid, 1 thin glass sheet, 2 diffuse sheet (paper, fabric)
    absorption 0.1 0.4 1.2  // linear RGB extinction per meter
}
```

It also accepts `thickness` (thin sheets, default 0.005 m), `metallic` (0..1) and `roughness`
(0.04..1). Solid glass refracts with exact Fresnel and total internal reflection. It needs
closed meshes, and every interface costs a bounce (`r_ptMaxDepth`). Interfaces are smooth;
there is no rough refraction or caustics. Builds without `PT_PHYSICAL` parse and ignore the
block. Run `r_ptReset` after reloading materials.

With `r_ptPhysicalAutoMaterials 1` (the default), legacy materials are calibrated
automatically:
- Known stock window panes become thin glass (IOR 1.52), and the washroom water sheet becomes
  water (IOR 1.333).
- Recognizable metals, paint, rust, rubber, plastic, skin, paper, wood and stone get
  metallic/roughness defaults; other surfaces get conservative dielectric defaults.
- Specular maps still vary roughness per texel.

These conversions are approximate. Explicit `ptMaterial` blocks and inspector overrides take
precedence.

**Material inspector**: aim at a surface, open `r_ptUI physical` and press **Pick center
material**, or bind `r_ptMaterialPick` to a key. Edit metallic, roughness, transmission, IOR,
thin mode, thickness, absorption and tint. Changes preview live on every instance of the
material. **Save material** writes the selected material to `pt_materials.cfg` in
`fs_configpath`, which is loaded at startup. Assets and `.mtr` files are never rewritten.
**Reset to defaults** and **Revert unsaved** undo the live and the unsaved edits. Editing is
locked during captures. Console equivalents:

```
r_ptMaterial select textures/example/metal
r_ptMaterial set metallic 1
r_ptMaterial set roughness 0.12
r_ptMaterial set tint 0.9 0.8 0.6
r_ptMaterial set absorption 0.1 0.4 1.2
r_ptMaterial status | save | reset | revert
```

## Capture and benchmark tools

| Command | Description |
|---------|-------------|
| `r_ptBenchmark <demo> <spp>` | Replay a render demo, converging and saving every frame at exactly `<spp>` |
| `r_glBenchmark <demo>` | Replay the same demo with OpenGL and save every frame |
| `r_ptCompare [spp]` | Save an OpenGL and a path-traced screenshot of the current view (default 64 spp) |
| `r_ptSequence <start> <end> [spp]` | Path trace an inclusive range of live game ticks (default 256 spp) |
| `r_glSequence` | OpenGL counterpart of `r_ptSequence` |
| `r_ptAbort` / `r_glAbort` | Cancel, keeping finished images and report rows |
| `r_ptReset` | Restart accumulation and drop cached materials and textures |
| `r_ptInfo` | Backends, device, memory and timing |
| `r_ptUI [quality\|settings\|physical]` | Tuning panel (Dear ImGui builds) |

Spp targets are exact: captures split them into equal batches no larger than
`r_ptSamplesPerPixel`, disable the startup quality ramp, and restore the previous renderer
settings afterwards. Captures cannot overlap. See [Benchmark.md](../Benchmark.md) for
benchmark timing details.

### OpenGL/PT frame pairs

```
r_screenshotFormat 2
r_glBenchmark demo1
// when it has finished:
r_ptRenderScale 1
r_ptBenchmark demo1 64
```

Both replay every recorded frame at up to 30 Hz without skipping. They write

```
screenshots/glbench/demo1/run_NNNNN/frame_000001.png
screenshots/ptbench/demo1/64spp_run_NNNNN/frame_000001_64spp.png
```

Pair images by frame number, not run number. Keep the viewport size equal and use
`r_ptRenderScale 1`. In `d3ptmark-pt`, match the lens to the demo's FOV and set
`r_ptPhysicalDOF 0` for identical framing. Don't append `quit` to a command line that
starts a benchmark; it aborts the run.

### Live sequences

`+map game/enpro +r_ptSequence 200 1460 128` on the command line waits for the map,
advances 200 ticks with OpenGL, then traces each tick to `screenshots/ptseq_00200.*` onwards.
Ticks are counted from where the capture starts; the map is not reloaded.
`configs/sequence_enpro.cfg` is a complete scripted example. Use `com_fixedTic 0` for normal
play: an interrupted capture can leave `com_fixedTic 1`, which causes slow motion.

### Capture reports

Every PT capture and `r_glBenchmark` writes a local report to
`screenshots/ptreports/<compare|sequence|benchmark>/run_NNNNN/` (GL:
`screenshots/glreports/benchmark/`). Nothing is uploaded.

- `metadata.json`: arguments, settings, demo fingerprint, build, CPU/GPU, totals and status.
  It is replaced atomically, so interrupted runs keep their last state.
- `frames.csv`: per image: tick, spp, dimensions, GPU stage timings (tracing, denoising,
  postprocess, tone map), scene/upload/readback/save times, FPS, primary samples/s, scene
  counts and image path. GL reports leave PT-only fields blank.
- `settings.csv`: initial settings and every change.
- `telemetry.csv`: about 1 Hz GPU temperature, power, clocks, utilization, memory, fan and
  clock-limit reasons, plus CPU temperature, utilization and package power.

GPU timings exclude uploads, BVH builds and readback (`backend_wall_ms` includes them).
Primary samples/s is not rays/s. Reports include their own overhead. Arguments are recorded
with known credentials redacted, but local paths remain, so review reports before sharing.
`r_ptCaptureTelemetry 0` disables sensors; `r_ptCaptureReport 0` disables reports.

**Telemetry sources** are loaded at runtime, so nothing extra is needed to build:

| Source | Provides |
|--------|----------|
| `nvml` | NVIDIA, Windows and Linux |
| `amdgpu_sysfs`, `i915_sysfs`, `xe_sysfs` | AMD and Intel on Linux. Intel utilization and memory come from this process's DRM clients (`+fdinfo`); integrated Intel GPUs have no own temperature |
| `d3dkmt` | Any WDDM 2.4 GPU on Windows. Power is a share of the board limit (`gpu_power_pct`), not watts |
| `linux_cpu` | `/proc/stat`, `k10temp`/`zenpower`/`coretemp`, RAPL package energy |
| `windows_cpu` | System times, ACPI thermal zones (approximate), Energy Meter RAPL counters (Windows 11) |

The GPU is matched by PCI address, then name, then as the only GPU of its vendor. Sensors
that cannot be read stay blank, never zero. Rates are blank in the first row.
`metadata.json` lists each unavailable sensor with the reason. A common case is RAPL power,
which most kernels let only root read; `sudo chmod o+r /sys/class/powercap/intel-rapl:*/energy_uj`
enables it until reboot, at the cost of re-opening the side channel the restriction closes.

## Quality presets

`r_ptPreset` sets four quality budgets:

| Preset | Render scale | Spp / frame | Max depth | Light samples |
|--------|--------------|-------------|-----------|---------------|
| `gameplay_low` | 0.35 | 1 | 1 | 1 |
| `gameplay_med` | 0.5 | 1 | 2 | 1 |
| `gameplay_high` | 0.67 | 1 | 2 | 2 |
| `gameplay_ultra` | 0.75 | 2 | 3 | 2 |
| `cinematic_low` | 1.0 | 4 | 3 | 2 |
| `cinematic_med` | 1.0 | 8 | 4 | 4 |
| `cinematic_high` | 1.0 | 16 | 6 | 6 |
| `cinematic_ultra` | 1.0 | 32 | 8 | 8 |
| `low` / `med` (default) / `high` / `ultra` | 0.35 / 0.5 / 0.75 / 1.0 | 1 / 4 / 6 / 8 | 2 / 3 / 4 / 5 | 1 / 2 / 3 / 4 |

Gameplay tiers trade resolution for frame rate (keep `r_ptDenoise 1`); cinematic tiers are
for quality. Editing a budget switches to `gameplay_custom`, `cinematic_custom` or `custom`.
Presets never change denoising, exposure, fog, depth of field or other look settings.

`r_ptUI` opens the tuning panel: **Quality** (mode, presets, budgets, denoising),
**Physical** (`d3ptmark-pt` only) and **All CVars** (searchable `r_pt*` settings with their
limits). It edits the real cvars and is locked during captures; close it before taking
screenshots.

**Sky**: the default environment is a dusty Mars-like gradient (see the sky cvars below).
In `d3ptmark-pt` it also lights bounce misses; otherwise it is only a backdrop. It does not
replace authored skies. `r_ptSkyIntensity 0` turns it off.

## Console variables

| CVar | Default | Description |
|------|---------|-------------|
| `r_ptDraw` | `1` | Draw 3D views with the path tracer (0 = OpenGL) |
| `r_ptBackend` | `auto` | `auto`, `cuda`, `opencl` |
| `r_ptOpenCLPlatform` / `r_ptOpenCLDevice` | `-1` | OpenCL platform / device index, `-1` picks the best GPU |
| `r_ptOpenCLCache` | `1` | Cache compiled OpenCL programs |
| `r_ptMode` | `0` | Render mode, see above |
| `r_ptDebug` | `0` | Print per-frame scene and timing info |
| `r_ptPreset` | `med` | Quality preset, see above |
| `r_ptRenderScale` | `0.5` | Trace resolution relative to the viewport (0.1–1) |
| `r_ptSamplesPerPixel` | `4` | Samples per pixel per frame (1–64) |
| `r_ptMaxDepth` | `3` | Maximum bounces (1–16) |
| `r_ptMaxLightSamples` | `2` | Lights sampled per hit (1–8) |
| `r_ptIndirectProb` | `0.15` | One-bounce GI probability when `r_ptMaxDepth` is 1 |
| `r_ptRussianRoulette` | `1` | Russian roulette termination |
| `r_ptRRMinBounces` | `2` | Bounces before roulette starts |
| `r_ptRRSurvivalMin` | `0.1` | Minimum survival probability |
| `r_ptEarlyTermThreshold` | `0.05` | Roulette below this throughput after a bounce (unbiased) |
| `r_ptThroughputClamp` | `0.25` | Roulette below this throughput after shading (unbiased) |
| `r_ptFireflyClamp` | `5.0` | Per-sample radiance clamp (0 = off) |
| `r_ptAccumulation` | `1` | Accumulate while the view is static |
| `r_ptResetOnSceneChange` | `1` | 0 never, 1 raw mean on structure changes, 2 everything on any change |
| `r_ptRayOffset` | `0.001` | Self-intersection offset |
| `r_ptEmissionBoost` | `1.0` | Emission multiplier |
| `r_ptSpecularBoost` | `2.0` | Specular multiplier (Doom 3 uses 2×) |
| `r_ptSoftShadowScale` | `0.2` | Soft shadow jitter (0 = hard) |
| `r_ptDenoise` | `1` | SVGF denoising |
| `r_ptDenoiseStrength` | `0.5` | Smoothing scale (0.05–4) |
| `r_ptDenoiseDebug` / `r_ptDenoiseDebugSignal` | `0` | Denoiser diagnostics, see above |
| `r_ptExposure` | `1.0` | Exposure multiplier |
| `r_ptGamma` | `2.2` | Output gamma |
| `r_ptToneMapMode` | `1` | 0 Reinhard, 1 ACES, 2 Uncharted 2 |
| `r_ptSkyIntensity` | `1` (`25` with `PT_PHYSICAL`) | Sky scale, 0 = off |
| `r_ptSkyColorZenithR/G/B` | `0.45 0.28 0.18` | Zenith, linear RGB |
| `r_ptSkyColorHorizonR/G/B` | `0.65 0.45 0.30` | Horizon, linear RGB |
| `r_ptSkyColorGroundR/G/B` | `0.12 0.06 0.03` | Ground, linear RGB |
| `r_ptVolumetric` | `0.0` | Legacy volumetric intensity (0 = off, expensive) |
| `r_ptVolumetricDensity` | `0.008` | Fog density |
| `r_ptVolumetricSteps` | `16` | Ray-march steps (1–128) |
| `r_ptVolumetricAnisotropy` | `0.6` | Henyey-Greenstein g |
| `r_ptVolFalloff` | `2.0` | Distance falloff exponent |
| `r_ptVolMaxDist` | `500.0` | Maximum sampling distance |
| `r_ptCaptureReport` / `r_ptCaptureTelemetry` | `1` | Capture reports / sensor sampling |

Bloom, physical camera and fog cvars are listed in their sections above.
