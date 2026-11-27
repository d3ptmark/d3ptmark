# DOOM3 GPU PATH TRACING RENDERER

d3ptmark adds a **GPU path tracer** (`neo/renderer_pt/`) that can replace the
classic OpenGL rasterizer for 3D views. The front end is compute-API agnostic, with two
backends: **CUDA** (`neo/renderer_pt/cuda/`) and **OpenCL** (`neo/renderer_pt/opencl/`), a
port of the CUDA kernels to OpenCL 1.2 for AMD, Intel and NVIDIA GPUs. Every backend whose
toolchain is found is compiled into both binaries: `d3ptmark-pt` with the experimental physical
renderer (`PT_PHYSICAL`, see below), `d3ptmark-le` without it, next to the stock OpenGL
renderer. Without any backend toolchain `d3ptmark-le` is built with OpenGL only.

## Overview

Each frame the front end walks the view's draw surfaces and lights, builds a flat scene
(vertices, triangles, materials, lights, textures) on the CPU and hands it to the active
backend. The backend uploads it, builds a BVH on the GPU, path traces and tone maps the
image, and the front end stretches the result over the view's viewport. 2D views and
subviews (mirrors, remote cameras, portal skies) are still drawn with OpenGL, as is the HUD
and all GUI drawing on top of the 3D view.

## Source Layout

| Path | Contents |
|------|----------|
| `renderer_pt/pt_types.h` | POD types shared by host and device: `ptVertex_t`, `ptTriangle_t`, `ptMaterial_t`, `ptLight_t`, `ptBVHNode_t`, `ptRenderParams_t`, limits and enums |
| `renderer_pt/pt_backend.h` | `idPathTracerBackend` – abstract compute backend interface and factory type |
| `renderer_pt/pt_scene.h/.cpp`, `pt_scene_material.cpp` | `idPathTracerScene` – API-agnostic scene builder (geometry, Doom 3 material → PBR conversion, lights, texture readback, change hashes) |
| `renderer_pt/pt_render.h/.cpp` | Front end: cvars, backend registry / lifecycle, accumulation control, viewport blit (`RB_PT_*`) |
| `renderer_pt/pt_commands.cpp` | `r_ptCompare`, `r_ptSequence`, `r_ptAbort` capture tools |
| `renderer_pt/cuda/` | CUDA backend (`idPathTracerBackendCUDA`), kernels and the GPU LBVH builder |
| `renderer_pt/opencl/` | OpenCL backend (`idPathTracerBackendOpenCL`): `pt_cl_*.cl` ports of every CUDA kernel file, the launch layer (`pt_opencl_kernels.cpp`) and a runtime loader for the OpenCL ICD |

## Adding a Backend

1. Implement `idPathTracerBackend` (see `pt_backend.h`) in `neo/renderer_pt/<api>/`.
   The contract:
   - `Init` / `Resize` allocate the output for the maximum (window) size. `Render` may be called with any smaller trace size.
   - `UploadScene` receives an `idPathTracerScene`. Upload `PendingTextures()` into the slots given by their `index`. When `TextureGeneration()` changes, drop all cached textures.
   - `Render` accumulates samples into a float buffer until `ResetAccumulation()` is called, then tone maps it to RGBA8. Only `PT_MODE_PATHTRACE` accumulates; debug modes overwrite.
   - Report unrecoverable device errors through `IsHealthy()`, and the front end falls back to OpenGL.
   - All data types in `pt_types.h` are plain structs with no API-specific members, so they can be copied to the device as-is.
2. Export `PT_<API>_IsAvailable()`, `PT_<API>_CreateBackend()` and `PT_<API>_GetDeviceInfo()` (used by capture reports), and add them to `ptBackendFactories[]` in `pt_render.cpp`.
3. In `neo/CMakeLists.txt`, add a detection block that sets `PT_HAVE_<API>`, a `src_pt_<api>` source list, and the `HAVE_<API>` define for path-traced targets in `add_engine_binary()` (see the CUDA and OpenCL blocks).
4. Select it with `r_ptBackend <name>`. `auto` picks the first available backend.

## Rendering Modes (`r_ptMode`)

| Value | Mode |
|-------|------|
| 0 | **Path tracing** – Monte Carlo with Cook-Torrance BRDF, progressive accumulation |
| 1 | BVH debug – primary rays with random per-triangle colours |
| 2–6 | Texture debug – combined, base colour, albedo, normal map, specular map |
| 7 | **Whitted-style ray tracing** – direct lighting, Blinn-Phong specular, mirror reflections |

## Key Features

- **Linear BVH (LBVH)** built entirely on the GPU every frame using the Karras algorithm
  and a CUB radix sort, ordered on a single CUDA stream with no device-wide syncs
- **PBR materials** extracted from Doom 3 shader stages: albedo, normal map, specular,
  alpha test, blend modes, vertex colours, emission, glass (Fresnel + IOR). Time-varying shader
  registers are re-evaluated every frame, per entity. Inactive material slots are reused,
  and the cache grows when a view needs more simultaneously
- **Stochastic direct lighting** with soft shadows: every enabled light stage, its projected
  image and texture matrix, the engine-resolved falloff image, and `r_lightScale` contribute to
  the lighting. Shadow rays pass through alpha tested and translucent surfaces
- **sRGB correct shading**: textures and colours are decoded to linear before lighting
- **First person weapon** rendered as an overlay that never gets clipped by nearby walls
- **Indirect lighting** via cosine-weighted hemisphere sampling, plus textured emissive triangles
  sampled as area lights with geometric falloff and visibility
- **Volumetric scattering** with a ray-marched Henyey-Greenstein phase function
- **Progressive accumulation** that resets on camera, mode, resolution or (configurable) scene
  changes, uses a running mean to preserve precision, and ramps quality over the first frames
- **Stackless BVH traversal** with escape links, so deep LBVH trees cannot overflow a per-ray stack
- **Cache invalidation** after `reloadImages`, plus identity checks to avoid stale cache hits when
  engine pointers are reused
- **Tone mapping**: Reinhard / ACES / Uncharted 2 with exposure and gamma
- **SVGF denoising**: illumination demodulation, camera reprojection, temporal luminance
  moments, separate diffuse/specular filtering, and reflectance restoration before tone mapping
- **Comparison / sequence tools** for side-by-side GL vs. path-traced screenshots

### Denoising

The CUDA denoiser implements a practical SVGF core inspired by
[original Q2VKPT's approach](https://developer.nvidia.com/blog/path-tracing-quake-ii/),
not a full port of Q2VKPT's adaptive A-SVGF pipeline:

1. Trace fresh per-frame HDR samples alongside the untouched progressive mean. Keep
   world position, unperturbed and shading normals, camera-forward depth, material identity,
   roughness, and bounded diffuse/specular reflectance as guides, without additional guide rays. Average modulation color
   across samples; bypass pixels whose samples disagree on the primary surface.
2. Separate surface-scattered diffuse and specular transport at the primary BRDF,
   including indirect bounces, without changing raw transport or random draws.
   Demodulate each signal separately, reproject static-world history through the
   previous camera, and reject incompatible depth, normals, materials, or reflectance.
   Specular history additionally checks shading normals, roughness, and view direction.
3. Accumulate luminance moments and bounded temporal history (up to 32 frames, with
   a minimum new-color weight of 0.08). Bootstrap variance from the spatial neighborhood
   for new/disoccluded pixels. Neighborhood clipping and lighting-change rejection
   limit stale history, but are not the correlated temporal gradients of A-SVGF.
   Specular history is shorter (4-24 frames depending on roughness), with a larger
   minimum new-color weight for smooth highlights.
4. Run four variance-guided atrous passes at pixel spacings 1, 2, 4, and 8. Retain the
   first-pass illumination as feedback, then restore current reflectance for display.
   Specular spacing is capped by roughness and its shading-normal guidance is stronger.
5. Filter volumetric scattering as a third HDR signal with independent moments/history,
   no surface-reflectance modulation, and depth-boundary guidance rather than surface
   normals, textures, or roughness. Bootstrap low-batch capture variance spatially,
   including single-dispatch 8-spp captures. Real-time volume history is capped at
   16 frames, with at least 0.125 new-color weight; camera movement discards it because
   scattering is view dependent and the physical fog boundary moves with the camera.
   Add only primary emission and other non-scattered residuals back unfiltered.

Enabled by default in path tracing mode only. `r_ptDenoise 0` displays the raw progressive
mean; `r_ptDenoiseStrength` adjusts the variance-guided luminance sigma. Denoising uses
fresh samples, not the already averaged raw buffer, so it remains responsive while moving.
`r_ptAccumulation` controls the raw progressive mean independently of SVGF history.
Use `r_ptDenoise 0` and `r_ptDenoiseDebug 0` for progressively converged reference captures with `r_ptCompare`
or `r_ptSequence`. These tools use an offline progressive path: full configured tracing
quality from the first frame, running diffuse/specular/volume means and variance of the mean,
averaged surface guides, and spatial denoising of the accumulated signals. They do not
use the real-time history caps or lighting-change clipping. Unfiltered emission
residuals come from the full progressive mean too, so fresh fog noise cannot overwrite
an otherwise converged capture. Each cinematic tick resets this accumulation; screenshot
redraws reuse the completed result instead of adding extra samples.
The camera, geometry, materials and lights are snapshotted for each captured tick, so
render-time animation cannot perturb or repeatedly reset the progressive estimate.
`r_ptDenoise 1` filters the progressive capture; `0` keeps it an unfiltered reference.
Both legacy volumetrics and physical single scattering use the separate volume filter.
`r_ptDenoiseStrength` also controls its variance-guided smoothing.

Camera movement resets the raw mean without automatically throwing away reprojectable
history. Explicit resets, resolution/backend/texture changes, trace-setting changes,
strict scene invalidations (`r_ptResetOnSceneChange 2`), and camera cuts invalidate denoiser history.
The default scene policy (`1`) still resets the raw mean on structure-count changes,
but preserves denoiser history subject to per-pixel validation. This prevents particles
and visibility changes from repeatedly discarding valid static-world history. Changing
exposure or gamma, or toggling denoising, does not alter the raw accumulation.

This stage has no object motion vectors: non-world/deforming surfaces and view weapons
receive spatial filtering only, never temporal reuse. Transparent, ambient-only,
no-shadow overlays, and geometrically mixed pixels bypass surface denoising. Opaque glossy,
metallic, and emissive surfaces now participate; primary emission itself is not filtered.
Adaptive gradient sampling, object motion vectors, transmission denoising,
and TAA remain future work; low-sample silhouettes can still shimmer, sharp reflections
can remain noisy, and moving objects are spatial-only. In offline captures, compatible
opaque material mixtures retain averaged reflectance guides and spatial filtering
without temporal reprojection; true depth/normal discontinuities still use raw surface
lighting. Gameplay retains the conservative mixed-material progressive fallback.
Offline captures filter volume independently using the averaged first-visible-layer
depth, including transparent/overlay/mixed pixels and sky/miss rays. The filter respects
depth and layer-class boundaries, and leaves surface lighting and primary emission
untouched where a single surface guide is unavailable. Real-time fallback pixels retain
the full raw progressive result rather than mixing fresh fog with an accumulated surface,
and do not run the additional first-visible-layer volume filtering.
Volume temporal validation ignores surface reflectance/material changes.

Denoiser storage is allocated lazily at the trace resolution: 488 additional bytes per
pixel (about 241 MiB at 960x540), rather than at full window resolution.

#### Denoising diagnostics

`r_ptDenoiseDebug` enables the denoiser even when `r_ptDenoise` is off. Return it to
`0` for normal beauty/raw comparisons. Modes 1-4 bypass exposure, gamma, and tone mapping;
modes 5-7 show only the filtered HDR signal through normal tone mapping.

| Mode | Display |
|---|---|
| `1` | Coverage: green static, yellow dynamic/spatial-only, red transparent, magenta overlay/ambient, orange mixed, black miss |
| `2` | History length: red = one frame, green = 32 frames; shorter specular caps will not reach full green |
| `3` | Relative noise: compressed standard deviation divided by luminance; brighter means noisier |
| `4` | History rejection reason (palette below) |
| `5` | Filtered diffuse only, without emission/volume |
| `6` | Filtered specular only, without emission/volume |
| `7` | Filtered volume only, without surface lighting/emission |

`r_ptDenoiseDebugSignal 0` selects diffuse, `1` specular, and `2` volume for modes 2-4.
Rejection colors: green reused, magenta global reset, yellow dynamic/no motion,
blue offscreen, red depth/disocclusion, cyan normal/view change, orange material,
purple reflectance/roughness, white lighting change, gray missing samples, black unsupported.

Generated area lights share a target budget of 512 lights with engine light stages. Engine
light stages take priority and can grow beyond that budget; any emissive triangles that do
not fit still contribute when hit by a bounce ray, and the renderer warns once when the budget
is exceeded. Eligible opaque emitters are sampled on both sides,
including their glow texture and alpha test. Transmissive emitters and no-shadow decal overlays
are not sampled as area lights. The Whitted debug mode currently omits generated area lights.

## Building with CUDA

`d3ptmark-pt` requires the NVIDIA CUDA Toolkit and a host compiler it supports; it is
built by default:

```sh
./build.sh          # or build.bat on Windows
```

By default, kernels are built for SM 75–90 (Turing → Hopper). Override this with
`-DCMAKE_CUDA_ARCHITECTURES=<list>`, for example `75` for a faster single-GPU build.

If your distribution's GCC is newer than nvcc supports (for example, GCC 16 with CUDA 13.1),
point nvcc at an older host compiler with `-DCMAKE_CUDA_HOST_COMPILER=g++-15`, or try
`CUDAFLAGS="-allow-unsupported-compiler -std=c++17"` in the environment.

With GCC 16's libstdc++, nvcc 13.1 also fails on the
`__builtin_is_virtual_base_of` trait. Disable libstdc++'s builtin-trait
optimizations for CUDA compilation using its compatibility switch:

```sh
CUDAFLAGS="-allow-unsupported-compiler -std=c++17 -D_GLIBCXX_DO_NOT_USE_BUILTIN_TRAITS" ./build.sh
```

The flags are cached for subsequent script runs (reconfigure from scratch with
`./clean.sh`). This remains an unsupported compiler combination; a CUDA-supported host
compiler is preferred.

With `-DPATHTRACED=OFF` only `d3ptmark-le` is built: the OpenGL renderer plus the path
tracer without `PT_PHYSICAL` when a backend toolchain is found, plain dhewm3 otherwise.

## Building with OpenCL

The OpenCL backend is enabled by default (`-DOPENCL=OFF` disables it) and needs only the
OpenCL headers (`CL/cl.h`, for example the `opencl-headers` package or a GPU vendor SDK).
The executable does not link against OpenCL: the ICD loader (`OpenCL.dll`, `libOpenCL.so.1`)
is loaded when the backend starts, so `d3ptmark-pt` still runs on machines without an OpenCL
driver and falls back to OpenGL. Without the CUDA toolkit, both binaries are built with the
OpenCL backend only; configuration fails only when neither backend is available and
`d3ptmark-pt` is requested (`d3ptmark-le` then falls back to OpenGL only).

The device needs OpenCL 1.2 and an online compiler. The kernels are embedded in the
executable and compiled for the selected device on first use, which can take tens of seconds.
The device binary is cached in `<fs_savepath>/<game>/ptcache/` (disable with
`r_ptOpenCLCache 0`). `r_ptBackend opencl` selects the backend explicitly; with `auto`, CUDA is
preferred when both are available. With the OpenCL backend active, `r_ptInfo` lists every
OpenCL platform and device and marks the one in use.

The OpenCL kernels follow the CUDA ones file for file (same algorithms, constants, RNG
streams, launch order and buffer layout). Deliberate differences:

- Textures live in one texel pool addressed through a descriptor table, because OpenCL 1.2
  buffers cannot contain pointers.
- The LBVH sorts Morton codes with its own stable radix sort instead of CUB. The permutation
  is identical (checked against `std::stable_sort` by `pt_opencl_lbvh_test`).
- The camera half-angle uses `tanpi()`. Some OpenCL drivers return `tan(pi/4)` one ulp high,
  which shifts every SVGF reprojection off the pixel grid.
- Intel devices compile with `-cl-intel-256-GRF-per-thread`, because the register-spilling
  default faulted intermittently on the path tracing kernel.

Configuring with `-DTESTS=ON` builds the path tracer regression tests; run them with
`ctest --output-on-failure` from the build directory. The capture report, benchmark,
material cache and quality preset tests are pure CPU tests. The GPU tests
(`pt_cuda_lighting_test`, `pt_cuda_denoising_test`, `pt_cuda_postprocess_test`,
`pt_cuda_physical_test`, built only when `PATHTRACED` is enabled) skip at runtime if no
CUDA device or driver is available. Their OpenCL ports (`pt_opencl_lighting_test`,
`pt_opencl_denoising_test`, `pt_opencl_postprocess_test`, `pt_opencl_physical_test`) and
`pt_opencl_lbvh_test` are built when the OpenCL backend is, and skip when there is no OpenCL
GPU; `PT_OPENCL_TEST_PLATFORM` / `PT_OPENCL_TEST_DEVICE` select a device by index.

The denoising test checks diffuse/specular noise reduction, temporal reuse and roughness-dependent bounded history, camera reprojection,
disocclusion/lighting-change rejection, texture restoration, edge preservation, small/odd
image sizes, diagnostic palettes, signal-only output, and tone-mapped output.
The lighting regression test also checks guide eligibility, multi-sample guide generation,
fresh radiance independent of the progressive mean, unchanged raw accumulation, and
diffuse/specular conservation for direct, indirect, emissive, clamped, and volumetric cases.

### Decal blending

Material overlays support ordinary and inverse-color multiplication in both
path tracing and Whitted modes. Doom 3 blood decals use
`blend gl_zero, gl_one_minus_src_color`: their cyan-on-black source image is a
filter, not emitted color or an alpha-over layer. The renderer complements the
gamma-encoded source before converting the filter to linear throughput, preserving
the floor behind black texels and producing the intended red stain. Texture alpha
does not control this blend; stage RGB and vertex-color modulation still apply.
The same filter is applied to the single-bounce indirect estimator. Such overlays
retain the raw progressive fallback rather than borrowing an opaque surface's
denoising history.

### Experimental physical rendering (`PT_PHYSICAL`)

This is a separate **build-time rendering model**, not another quality preset.
It is always compiled into `d3ptmark-pt`; `d3ptmark-le` contains the same path tracer
(CUDA/OpenCL backends, Dear ImGui integration) without it, so path tracing there uses
the legacy shading model.

The CUDA host-compiler compatibility notes above also apply to this build.
Game data and game libraries must be installed as for a normal engine build.
OpenGL rendering is unchanged. In `d3ptmark-pt`, path tracing mode
(`r_ptDraw 1`, `r_ptMode 0`) always uses the experimental model; other diagnostic
and Whitted modes retain their existing shading.

The model provides:

- Inverse-square point lighting in meters, lumen-based light stages, lux-based
  directional lights, and normalized spotlight angular profiles. Legacy box/radius
  attenuation and falloff textures no longer bound these lights. Spotlight gobos
  remain angular filters and can reduce the transmitted flux.
- All registered engine lights, with animated shader stages and view suppression,
  rather than only lights passing Doom 3's light-volume visibility tests.
- Reciprocal GGX specular with height-correlated Smith visibility, dielectric IOR,
  Fresnel-attenuated diffuse, bounded reflectance, and diffuse/GGX mixture sampling
  for indirect paths. The GGX lobe samples view-visible microfacet normals with
  the matching PDF, reducing wasted samples and large grazing-angle reflection
  weights without changing the BRDF or material roughness. Legacy specular
  brightness no longer invents metalness.
  The GGX distribution preserves small half-vector angles numerically on polished
  surfaces, including rotated normals.
- Emissive surface luminance in cd/m^2 (nits), shared consistently by visible
  emission and sampled emissive triangles.
- A photographic camera: `EV100 = log2(N^2 / t * 100 / ISO)` and
  `exposure = 2^compensation / (1.2 * 2^EV100)`. The camera uses millimeter focal
  length and sensor width, meter focus distance, and a uniformly sampled circular
  thin lens. Sensor height follows the actual viewport aspect ratio. Lens FOV is
  applied before engine culling, not only to traced rays.

Run `r_ptUI` in the console to toggle the unified path tracer tuning panel, then
select its **Physical** tab. `r_ptUI physical` opens that tab directly; for example,
`bind F9 r_ptUI` makes the panel accessible while playing. Existing ImGui mouse,
keyboard, focus, and close-button handling apply. Controls persist as archived
CVars and the panel locks its tuning controls during a capture; close it before
recording a clean screenshot or sequence.

| CVar | Default | Physical interpretation |
|---|---|---|
| `r_ptPhysicalMetersPerUnit` | `0.0254` | Meters per engine unit; assumes inches |
| `r_ptPhysicalLightLumens` | `1500` | Flux represented by a unit-white engine light stage |
| `r_ptPhysicalSunLux` | `50000` | Illuminance represented by a unit-white directional light |
| `r_ptPhysicalEmissionNits` | `100` | Luminance represented by unit-white material emission |
| `r_ptPhysicalIor` | `1.5` | Opaque dielectric IOR, giving F0 = 0.04 at the default |
| `r_ptPhysicalRoughnessScale` | `1` | Scale for approximate legacy-map roughness |
| `r_ptPhysicalISO` | `800` | Camera sensitivity |
| `r_ptPhysicalShutter` | `0.03333333` | Exposure time in seconds (1/30 s) |
| `r_ptPhysicalFNumber` | `2.8` | Aperture for both exposure and lens blur |
| `r_ptPhysicalExposureCompensation` | `0` | Exposure compensation in stops |
| `r_ptPhysicalFocalLength` | `35` | Focal length in mm |
| `r_ptPhysicalSensorWidth` | `36` | Sensor width in mm |
| `r_ptPhysicalFocusDistance` | `3` | Forward focus-plane distance in meters |
| `r_ptPhysicalDOF` | `1` | Thin-lens blur; `0` selects a pinhole without changing exposure |

Exposure/ISO/shutter changes preserve the scene-linear accumulated estimate.
Focus, lens/aperture, material calibration, world scale, and lighting changes
invalidate affected accumulation/history. DOF disables real-time temporal history:
a pinhole reprojection cannot correctly reuse a stochastic lens sample. Spatial
denoising remains available, and cinematic captures still accumulate and denoise
all configured samples with the existing offline progressive path.

This is **not a conversion to measured PBR assets**: the source materials and
light stages have no calibrated units. Their RGB values and specular maps remain
approximations; the calibration controls set their assumed physical scale.
Placed point lights are ideal point sources (hard shadows); actual emissive
triangles provide area-light soft shadows. Existing transparent/additive overlays,
straight-through glass, geometry visibility/portal extraction, output gamma and
tone mappers remain approximations inherited from the renderer unless explicitly
overridden by a `ptMaterial` block (below). Surfaces outside
the engine's extracted scene cannot reflect or occlude light. The physical lens
overrides gameplay FOV, so this mode is especially suited to cinematic work.
Shutter time does not provide motion blur. Legacy volumetrics are disabled,
as are artistic light/specular/emission boosts,
the legacy single-bounce GI shortcut and biased radiance/firefly clamps.
`r_ptSkyIntensity`, if enabled, is interpreted as environment luminance and
illuminates secondary misses too; it is no longer only a backdrop.

Physical builds include `pt_cuda_physical_test`; run
`ctest --test-dir build-physical -R pt_cuda_physical --output-on-failure`.
It checks exposure stops, lens units/FOV/focus and aperture distribution,
inverse-square lighting and selection beyond old light volumes, unclamped direct
light, spotlight normalization, BRDF reciprocity, and Monte Carlo energy bounds
for diffuse/metallic materials from polished to rough and normal to grazing views.

#### HDR bloom and lens ghosts

These display effects work in both CUDA builds and default **off**. They operate on
the denoised scene-linear HDR image before exposure/tone mapping, never on the raw
accumulation or denoiser history. Changing their controls does not restart convergence.
All denoiser diagnostic views bypass them.

| CVar | Default | Meaning |
|---|---|---|
| `r_ptBloom` | `0` | Bloom strength, 0 to 1 |
| `r_ptBloomThreshold` | `1` | Highlight threshold in exposed linear units |
| `r_ptBloomRadius` | `0.025` | Blur radius as a fraction of image height |
| `r_ptLensFlare` | `0` | Screen-space lens ghost strength, 0 to 1 |

Try `r_ptBloom 0.08` and optionally `r_ptLensFlare 0.02`. The threshold follows
photographic exposure. A downsampled separable filter keeps the effect inexpensive.
Lens ghosts use visible highlights only; they are an artistic approximation, not
an optical lens simulation, and do not invent flares from hidden lights.
The physical ImGui panel exposes these controls.

#### Physical single scattering

`PT_PHYSICAL` adds a separate homogeneous participating medium. It includes
Beer-Lambert extinction on view/bounce/light segments, colored visibility shadows,
Henyey-Greenstein scattering, photometric engine lights, and sampled emissive lights.
The finite volume is explicitly a **camera-centered sphere**, not map-authored fog;
moving the camera moves its boundary. There is no multiple scattering.

| CVar | Default | Meaning |
|---|---|---|
| `r_ptPhysicalFogDensity` | `0` | Extinction in inverse meters; 0 disables the medium |
| `r_ptPhysicalFogAlbedo` | `0.9` | Scattering/extinction ratio; 0 gives pure absorption |
| `r_ptPhysicalFogAnisotropy` | `0.3` | Phase asymmetry; positive values scatter forward |
| `r_ptPhysicalFogRadius` | `30` | Volume radius in meters |
| `r_ptPhysicalFogSteps` | `16` | Stratified integration samples per ray segment |

Start with `r_ptPhysicalFogDensity 0.01`. More integration steps or accumulated
samples reduce noise. Volume light has separate variance-guided denoising and
converges through its full progressive mean in captures, including `r_ptSequence`
and `r_ptBenchmark`. Primary emission remains unfiltered. Fog changes
invalidate accumulation/history; legacy `r_ptVolumetric` does not control this medium.

#### Explicit physical material transport

No material is guessed to be glass from its name, alpha, or screen-space shader.
Add an optional top-level block to an authored `.mtr` material:

```
ptMaterial {
    transmission 1
    ior 1.5
    thin 0
    absorption 0.1 0.4 1.2
}
```

In physical path tracing, `transmission` (0..1, default 0) mixes dielectric transport
with the opaque GGX material. `ior` defaults to 1.5 (range 1..3).
`absorption` is linear RGB extinction in inverse meters (default zero), not an
sRGB tint. Solid glass uses actual traveled thickness, exact dielectric Fresnel,
Snell refraction, total internal reflection, and radiance-mode IOR weighting.
Closed, consistently wound meshes are required; nested closed dielectric shells
are supported within the configured bounce budget. Increase `r_ptMaxDepth` for
multiple glass interfaces; every interface consumes a bounce.

`thin 1` models a zero-geometric-thickness parallel glass sheet, including internal
interface reflection and absorption. `thickness` (default 0.005 meters) controls
its optical thickness. `thin 2` instead transmits diffusely into the opposite
hemisphere, tinted by the material's linear albedo; use this for thin paper or
fabric. This is not subsurface multiple scattering. The block also accepts
`metallic` (0..1) and `roughness` (0.04..1) overrides for opaque GGX reflection.
Explicit physical materials use these values rather than deriving them from
legacy specular maps. Glass interfaces are currently ideal smooth dielectrics,
not rough microfacet transmission.

An explicit block replaces legacy overlay blending/self-illumination for that
material in physical builds. Existing material textures remain available.
Default builds parse the block but ignore its physical settings. Call `r_ptReset`
after reloading changed materials to rebuild the path tracer's material cache.
Transparent primary pixels retain the raw progressive fallback: no false opaque
temporal history is reused. Straight next-event shadow rays transmit through thin
clear sheets but cannot represent refracted solid-glass or diffuse-sheet caustics;
those surfaces block direct shadow rays. Offscreen/portal-excluded geometry
remains an extraction limitation, and no point-light caustic solver is provided.

The physical GPU tests cover Fresnel/TIR/Snell math, spectrally absorbed solid and
thin transport over successive progressive frames, diffuse-sheet transport,
bounded fog extinction, analytic single scattering, and volumetric occlusion.
With `TESTS`, `pt_cuda_postprocess_test` additionally checks HDR
highlight spreading, exposure thresholds, disabled identity, and immutable inputs.

#### Live material inspector

Weapon bodies marked `noShadows` remain opaque: shadow suppression alone does
not make a surface a decal. Lit decal overlays require authored decal/offset
or translucent coverage. Active shader-stage conditions are evaluated when
translating animated materials, including muzzle-flash timing.

Particle geometry already generated by Doom's particle/deform system is traced.
When PT is enabled, deformed flare/sprite vertices are retained in frame memory
instead of being available only through OpenGL's vertex cache. Polygon-offset
decals are separated from their receivers so coplanar blood overlays remain visible.
`blend add` (`ONE, ONE`) preserves RGB even when texture alpha is zero;
`SRC_ALPHA, ONE` retains its authored alpha weighting. Selected-stage vertex
colors (including inverse vertex color), fade and UV animation are preserved.
`DST_ALPHA, ONE` overlays use a preceding supported alpha-only overwrite stage,
including its independent UV transform, alpha and vertex-alpha modulation. This
preserves the stock flashlight beam mask without turning its dust mesh into a
bright cone. Missing or unsupported local masks produce a warning and omit the
overlay; arbitrary framebuffer-alpha feedback is not implemented.
Fully masked beam pixels retain the receiver's surface/volume denoising guides.
In physical builds, additive unlit FX and alpha-blended flame/spark/muzzle-flash
materials use `r_ptPhysicalEmissionNits` for HDR radiance, visible to camera and
bounce rays. Alpha-over smoke and filter effects retain their compositing.
Lit surfaces' glow stages retain their own UV animation, vertex/inverse-vertex
color and `SRC_ALPHA, ONE` alpha weighting, independently of diffuse maps.
Camera hits, bounce hits and sampled emissive-triangle lighting evaluate the same
glow stage; Whitted mode also uses the textured glow instead of a uniform emitter.
Fully stage-faded glows do not consume emissive-light slots; partial fades also
reduce their light-selection weight without changing the estimator's energy.
This does not add volumetric particle simulation or direct-light sampling of
individual particle billboards.
OpenGL-only heat-haze/distortion shaders (including explosion shockwaves) and
alpha-write-only stages do not supply visible PT color and are omitted rather
than rendered as opaque white polygons. Supported color stages in the same
material remain visible. Authored/calibrated physical surfaces still use their
physical transport; screen-space heat-haze distortion is not implemented.

Physical builds automatically calibrate legacy materials by default
(`r_ptPhysicalAutoMaterials 1`). Stock window panes receive thin-glass transport
(IOR 1.52), including Enpro's chi-glass family and plasma-tube glass. Known panes
retain physical transport even when their GL materials use decal sorting,
subviews or effects-namespace names; actual glass decals and FX remain excluded.
The washroom water sheet uses IOR 1.333. Recognizable copper, polished
and weathered metals, paint, rust, rubber, plastic, skin, paper, wood and stone
receive separate metallic/roughness defaults. Artist descriptions take precedence
over substrate sound tags: painted pipes remain dielectric, and heavily oxidized
copper is not treated as a polished conductor. Other lit surfaces use conservative
dielectric defaults. Existing specular maps continue to vary automatic roughness
per texel, while opaque diffuse, normal, emission, alpha-test and texture animation
remain. The stock cube-map-only chrome pipe and washroom mirror use traced
reflections instead of baked environment maps.

These are approximate legacy-asset conversions, not measured PBR textures or a
per-texture remaster. Glass/liquid names alone never turn decals, monitor effects,
particles or creature skins into clear refractors. Only known stock pane/water
families receive automatic transmission; thin sheets avoid assuming watertight
meshes and do not simulate pool depth, dirty-glass layering or rough refraction.
Legacy pane overlays are replaced by clear dielectric transport. Explicit authored
`ptMaterial` blocks and saved/live inspector overrides always win.

The inspector shows the selected default profile and offers **Automatic material
calibration** to switch the global pass off for comparison. Toggling it rebuilds
materials and resets history without flushing textures or deleting saved overrides.
No asset or material-override file is generated by automatic calibration.

In a `PT_PHYSICAL` build, aim at a surface, open `r_ptPhysicalUI`, and press
**Pick center material** in the Material inspector. Alternatively, bind
`r_ptMaterialPick` to a key: it picks the center surface on the next main 3D view
and opens the panel. Selection stays locked while you move the sliders.
Picking reuses the engine's geometry trace, including translucent surfaces and
excluding the player/view weapon; it does not test individual alpha-mask texels.

Controls include metallic, roughness, transmission, IOR, solid/thin/diffuse
transmission, sheet thickness, RGB absorption, and linear base-color tint.
Edits preview immediately on **all instances sharing that material name**,
including animated instances. An override replaces legacy specular-map estimates
with the chosen metallic/roughness values. Global roughness calibration still
applies. Glass is colored through absorption; base tint affects reflected metallic
and diffuse color, not clear-glass transmission.

- **Reset to defaults** removes the live override and restores authored settings
  or automatic calibration (legacy translation when calibration is off).
- **Revert unsaved** restores the last saved override, or material defaults
  if none was saved.
- **Save material** persists only the selected material. Other unsaved previews
  remain unsaved. Reset followed by Save removes a previously saved override.

Saved overrides live in `pt_materials.cfg` in the active game's **config directory**
(`fs_configpath`, normally `~/.config/dhewm3/base/` on Linux). They are loaded on
startup; no `.mtr`, packed asset, or ordinary engine config is rewritten.
Saving replaces this sidecar through a temporary file; errors are shown in the
panel and console without marking the preview as saved. Unsaved previews last
for this engine session only.

Material changes rebuild the material cache without re-reading every texture,
and reset accumulation and denoising history even with scene-change resets
disabled. Saving an unchanged preview does not restart accumulation. Editing,
picking, resetting, reverting, and saving are locked throughout a comparison or
cinematic capture, including its preparation and stepping phases.

Console equivalents are available for precise values and scripting:

```
r_ptMaterial select textures/example/metal
r_ptMaterial set metallic 1
r_ptMaterial set roughness 0.12
r_ptMaterial set tint 0.9 0.8 0.6
r_ptMaterial set absorption 0.1 0.4 1.2
r_ptMaterial status
r_ptMaterial save
```

`r_ptMaterial reset` and `r_ptMaterial revert` match the panel buttons. Settings
are validated before previewing; unknown fields, non-finite/out-of-range values,
and malformed override files produce explicit errors. Physical builds also include
the CPU-only `pt_material_editor_test` regression target for preview isolation,
reset/revert, selected-only persistence, exact float round trips, invalid input,
and automatic material profile selection.

## Console Commands

| Command | Description |
|---------|-------------|
| `r_ptInfo` | List compiled backends and print device / memory / timing info for the active one |
| `r_ptBenchmark <demo> <spp>` | Converge and save every recorded render-demo frame at exact spp, with 30 Hz maximum playback and timing statistics including image-write overhead |
| `r_glBenchmark <demo>` | Replay the same recorded demo with OpenGL and save every frame for image-by-image PT comparison (also available without CUDA) |
| `r_ptReset` | Restart accumulation and drop cached materials and textures |
| `r_ptCompare [spp]` | Save an OpenGL and a path-traced screenshot at exact samples per pixel (default 64) |
| `r_ptSequence <start> <end> [spp]` | Path trace an inclusive range of live game ticks at exact samples per pixel (default 256) |
| `r_ptAbort` | Abort `r_ptCompare` / `r_ptSequence` / `r_ptBenchmark`, retaining completed images and report rows |
| `r_glSequence` | OpenGL counterpart of `r_ptSequence` (always available) |
| `r_glAbort` | Abort `r_glSequence` or `r_glBenchmark`, retaining completed benchmark images and report rows |

### OpenGL/PT benchmark frame pairs

Run the same **recorded render demo** through each renderer, waiting for each run
to finish before starting the other:

```
r_screenshotFormat 2
r_glBenchmark demo1
// After OpenGL Benchmark Complete:
r_ptRenderScale 1
r_ptBenchmark demo1 64
```

`r_glBenchmark` takes only the demo name (with optional `demos/` prefix and `.demo`
extension); OpenGL has no PT sample budget. Both commands replay every recorded
frame without skipping and advance at a maximum of 30 Hz, slowing down as needed
for rendering and image writes. A single-frame `demoShot` is captured once and exits.
Do not append an immediate `quit` when launching an asynchronous benchmark from
the terminal; it would abort the run.

OpenGL images are saved at viewport resolution, before buffer swap, without demo HUD,
console, or ImGui overlays:

```
screenshots/glbench/demo1/run_NNNNN/frame_000001.png
screenshots/ptbench/demo1/64spp_run_NNNNN/frame_000001_64spp.png
```

Pair images by their six-digit **frame number**, not by run-directory number.
Runs have independent numbering. The extension follows `r_screenshotFormat`.
Frame 1 represents the same first recorded view in both renderers; reports also
record matching source-frame numbers, 30 Hz source timestamps, and a demo fingerprint.
Keep viewport sizes equal and use `r_ptRenderScale 1` for equal image dimensions.
In physical builds, match the camera's focal length/sensor width to the demo's recorded
FOV and disable DOF if you want matching pinhole framing. Physical materials, exposure,
and lighting intentionally differ from stock GL.

GL benchmarks temporarily select OpenGL even if `r_ptDraw 1`, then restore the
previous renderer/mode/accumulation settings on completion or `r_glAbort`. Overlapping
GL/PT captures are rejected. GL output remains separate; no automatic diff or composite
image is generated. Existing OpenGL renderer errors are logged separately before
readback and do not by themselves abort the benchmark. Actual capture or GL-state
restoration errors abort the run, retaining previously completed frames.

### Local capture reports

The existing three PT capture commands automatically print their starting PT
settings and write versioned local reports under
`screenshots/ptreports/<compare|sequence|benchmark>/run_NNNNN/` in `fs_savepath`.
There is no new capture command and no automatic upload.

`r_glBenchmark` shares this report format under
`screenshots/glreports/benchmark/run_NNNNN/`. It records the OpenGL renderer name,
GL rendering settings, received invocation, demo fingerprint, per-frame wall and
render durations (including readback), image-write durations, FPS, and image paths. Its mode is `benchmark`
and rendering model is `OpenGL`. GPU-stage timings, CUDA throughput, and PT sample
targets are unavailable (blank CSV fields or JSON `null`), not invented GL measurements.
`screen_render_ms` includes GL readback; `image_save_ms` covers encoding/writing.
GPU telemetry for GL captures matches the device from the GL vendor/renderer strings
(the only GPU of that vendor, or a matching name), since no PCI address is known.
The existing `r_ptCaptureReport` and `r_ptCaptureTelemetry` switches also control
GL benchmark reports. Reports and GL benchmarking work in CUDA-free builds too.

* `metadata.json`: received startup arguments, operation command, original and
  effective settings, source/demo fingerprint, engine revision/build, CPU/GPU
  information, totals, and completed/aborted/report-error status. Metadata is
  replaced atomically; interrupted runs keep their last metadata and CSV rows.
* `frames.csv`: one row per completed image, including phase/source tick,
  actual spp/dispatch count, trace/image dimensions, GPU tracing/denoising/
  postprocessing/output timings, CPU scene/upload and readback timings, image
  save time, wall time, FPS, primary samples/s, scene counts, memory and image path.
* `settings.csv`: initial effective settings and subsequent changes with time/phase.
* `telemetry.csv`: approximately 1 Hz GPU temperature, power, clocks, utilization,
  memory, fan and clock-limiting reasons, plus CPU temperature, utilization and
  package power (`cpu_temperature_c`, `cpu_utilization_pct`, `cpu_power_w`, the last
  columns before `sensor_errors`), for CUDA, OpenCL and GL captures. Sources
  (`pt_gpu_telemetry.cpp`, all loaded at runtime, nothing extra needed at build time):
  * `nvml`: NVIDIA on Windows and Linux (driver's NVML library).
  * `amdgpu_sysfs`, `i915_sysfs`, `xe_sysfs`: AMD and Intel on Linux (hwmon and driver
    attributes). i915 and xe have no device-wide utilization or memory attribute, so
    these come from this process's DRM clients (`/proc/self/fdinfo`, source suffix
    `+fdinfo`): busiest engine and resident memory of the benchmark itself (local
    memory on discrete GPUs, system memory on integrated ones). Integrated Intel GPUs
    have no temperature sensor of their own (use `cpu_temperature_c`); their power
    (RAPL `uncore`) is readable by root only.
  * `d3dkmt`: any GPU on Windows 10 1803+ (WDDM 2.4): temperature, fan rpm, clocks and
    power as a percentage of the board limit (`gpu_power_pct`; Windows has no
    vendor-neutral GPU watts) from D3DKMT, utilization and memory from the
    "GPU Engine"/"GPU Adapter Memory" counters (Task Manager's numbers).
  * `linux_cpu`: `/proc/stat`, the `zenpower`/`k10temp` (Tdie, else Tctl) or `coretemp`
    (Package id 0) hwmon, else the `x86_pkg_temp` thermal zone, and the powercap RAPL
    package energy (`intel-rapl:N`, also used for AMD Zen). Kernels since the PLATYPUS
    fix make `energy_uj` readable by root only; to sample CPU power as a user, run
    `sudo chmod o+r /sys/class/powercap/intel-rapl:*/energy_uj` (until reboot; this
    re-exposes the side channel the restriction closes).
  * `windows_cpu`: `GetSystemTimes`, the "Thermal Zone Information" counters (ACPI
    zones: firmware defined, usually near the package temperature, static on some
    boards) and the "Energy Meter" `RAPL_Package*_PKG` counters (Windows 11, or an EMI
    driver). No administrator rights needed.

  The GPU is matched by PCI address (CUDA, or the OpenCL `cl_khr_pci_bus_info`,
  NVIDIA and AMD attribute queries), then by name, then as the only GPU of its vendor;
  the address is not exported. `gpu_sm_clock_mhz` is the graphics/shader clock on
  non-NVIDIA GPUs. Sensors a source does not provide are blank without an error;
  sensors that fail to read are blank and listed in `sensor_errors`, never zero-filled.
  Rates (power from energy counters, CPU and Windows GPU utilization) are blank in the
  first row. `metadata.json` records `telemetry_source` (e.g. `i915_sysfs+fdinfo+linux_cpu`),
  `telemetry_sensors`, `telemetry_unavailable_sensors` (each blank column with the
  reason, e.g. `cpu_power:/sys/class/powercap/intel-rapl:0/energy_uj is readable by
  root only`) and `telemetry_unavailable_reason` when the GPU or CPU source could not
  be opened at all.

Compare/sequence images are kept inside their unique report directory so repeat
runs cannot overwrite earlier report images. Benchmark images retain their
existing numbered benchmark directory; `image_file` is relative to `fs_savepath`.
Set `r_ptCaptureTelemetry 0` to disable sensor sampling, or `r_ptCaptureReport 0`
to disable all reports and retain legacy compare/sequence image paths.

All durations use milliseconds or explicitly labeled seconds. GPU timings exclude
scene uploads/BVH construction and readback; `backend_wall_ms` includes them.
`screen_render_ms` is currently available only for benchmark. Output timing includes
diagnostic display instead of tone mapping when diagnostics are selected.
Backend memory is an allocation estimate; telemetry memory is device-wide usage.
Compute FPS excludes playback pacing; primary samples/s is **not** rays/s.
Results include local reporting overhead and are not an isolated GPU microbenchmark.

Startup arguments exclude the executable and cannot recover shell quoting,
expansions or redirections. Known credential values are redacted, but local paths
can remain: review reports before publishing. CUDA driver version denotes API
compatibility, not the NVIDIA graphics-driver release.
Compare and sequence now interpret their budget as actual **spp**, not dispatch
count, choose an exact-dividing batch, and temporarily force accumulation/path-trace
mode before restoring previous controls.

Normal gameplay should use `com_fixedTic 0`. A saved `com_fixedTic 1` forces one
simulation tick per rendered frame and produces slow motion below 60 FPS; enter
`com_fixedTic 0` in the console if an interrupted capture left it enabled.

To capture Enpro from startup, append `+map game/enpro +r_ptSequence 200 1460 128`
to the engine command line. The sequence waits for the main 3D view, advances 200
game ticks from the loaded state using OpenGL, then freezes each tick while tracing.
Loading screens, GUI/subviews, and screenshot redraws do not advance the PT sequence.
Frames are relative to the state where capture starts; the command does not reload or
rewind an already running map. Images are written to `screenshots/ptseq_00200.*` through
`ptseq_01460.*` under the game's save path. `r_ptAbort` cancels and restores capture settings.

The comparison and sequence targets count accumulated frames, not individual samples per pixel;
the `_spp` suffix in screenshot filenames is historical.

## Path Tracing Quality Presets

### Unified tuning panel

With Dear ImGui enabled, `r_ptUI` toggles one **Path Tracer** window:

- **Quality** edits rendering mode, gameplay/cinematic presets, quality budgets,
  denoising, and accumulation.
- **Physical** contains the existing photographic camera, asset calibration,
  material inspector, fog, and lens-effect controls. It exists only in `PT_PHYSICAL`
  builds.
- **All CVars** exposes registered `r_pt*` settings, searchable by name or description,
  with type-aware controls and the CVar's declared numeric limits. Hover for descriptions;
  Ctrl-click sliders for exact input and press Enter to commit text values.

`r_ptUI quality`, `r_ptUI settings`, and (in physical builds) `r_ptUI physical` open
the corresponding tab directly. Changes use the real CVars, so console edits and
preset/custom transitions stay synchronized. Opening the panel does not change settings.
Tuning controls are locked while a compare, sequence, or benchmark capture is active;
close the panel for clean screenshots. Capture commands remain console actions.
ImGui-disabled builds report that the panel is unavailable.

The standalone `r_ptPhysicalUI` command has been removed, without an alias. Update
existing bindings and scripts to `r_ptUI` or `r_ptUI physical`.

### Preset budgets

Use `r_ptPreset gameplay_med` for a lower-cost gameplay starting point, or
`r_ptPreset cinematic_med` for full-resolution, higher-quality rendering.
Presets update these quality variables immediately:

| Preset | `r_ptRenderScale` | `r_ptSamplesPerPixel` | `r_ptMaxDepth` | `r_ptMaxLightSamples` |
|--------|-------------------|-----------------------|----------------|-----------------------|
| `gameplay_low` | 0.35 | 1 | 1 | 1 |
| `gameplay_med` | 0.5 | 1 | 2 | 1 |
| `gameplay_high` | 0.67 | 1 | 2 | 2 |
| `gameplay_ultra` | 0.75 | 2 | 3 | 2 |
| `cinematic_low` | 1.0 | 4 | 3 | 2 |
| `cinematic_med` | 1.0 | 8 | 4 | 4 |
| `cinematic_high` | 1.0 | 16 | 6 | 6 |
| `cinematic_ultra` | 1.0 | 32 | 8 | 8 |

Gameplay tiers prioritize frame cost with reduced trace resolution and 1-2 samples per
frame; keep `r_ptDenoise 1` for low-sample gameplay. Actual frame rates depend on the
GPU, viewport resolution, scene, and other settings, especially fog and DOF.
Cinematic tiers keep full trace resolution and increase sample, bounce, and light budgets;
they are intended for quality-focused rendering rather than a realtime frame-rate target.

Manual edits to these four quality variables switch to `gameplay_custom` or
`cinematic_custom`, preserving the selected family and edited values. Selecting either
custom mode directly leaves all current settings unchanged; these are mode labels, not
separately saved banks of settings.

The original names and their values remain supported for existing configurations.
The default is still `med`, matching the original defaults:

| Preset | `r_ptRenderScale` | `r_ptSamplesPerPixel` | `r_ptMaxDepth` | `r_ptMaxLightSamples` |
|--------|-------------------|-----------------------|----------------|-----------------------|
| `low` | 0.35 | 1 | 2 | 1 |
| `med` | 0.5 | 4 | 3 | 2 |
| `high` | 0.75 | 6 | 4 | 3 |
| `ultra` | 1.0 | 8 | 5 | 4 |

Editing a legacy preset still switches to `custom`; selecting `custom` directly leaves
settings unchanged. Existing custom settings are preserved when upgrading.
Presets do not change denoising, exposure, lighting colour, fog, DOF, or other artistic
controls, and do not start a capture or change gameplay simulation.
The progressive quality ramp still reduces samples and bounces for the first few frames
after movement or a reset. With `r_ptAccumulation 0`, every frame uses the first-frame limits.
Compare/sequence/benchmark captures disable this ramp and retain the selected quality
budgets, but their explicit sample/frame target remains authoritative. In particular,
`r_ptBenchmark <demo> <samples>` uses `r_ptSamplesPerPixel` as a batch-size limit, not
as a replacement for the requested total samples per recorded frame.

### Mars-inspired sky defaults

The default environment is a dusty daytime Mars-inspired gradient: muted ochre
at the zenith, a brighter butterscotch horizon, and dark rust/regolith below.
Its linear RGB defaults are zenith `(0.45, 0.28, 0.18)`, horizon
`(0.65, 0.45, 0.30)`, and ground `(0.12, 0.06, 0.03)`, with
`r_ptSkyIntensity 25` in physical builds or `1` in legacy builds. The physical
default uses a higher environment luminance scale to remain visible under the
photographic camera's default exposure. This is an artistic approximation, not a
spectral atmosphere, dust-storm, or sunset simulation.

In physical mode it contributes environment luminance to both camera and bounce
misses; legacy path tracing uses it only as a primary-ray backdrop. It does not
replace authored sky materials or add a sun, fog, or lights inside closed rooms.
Use `r_ptSkyIntensity 0` to disable it. Existing saved intensity values, including
the previous default `0`, and explicit colour overrides still take precedence;
set `r_ptSkyIntensity 25` (physical) or `1` (legacy) manually to enable it in an
existing configuration.

### Enpro lighting/look profile

For a clearer, brighter path-traced `game/enpro`, load the opt-in profile shipped
in [`neo/base/pt_enpro.cfg`](neo/base/pt_enpro.cfg):

```
exec pt_enpro.cfg
map game/enpro
```

The config must be in the game's `base` search path; copy it there if your Doom 3
data lives outside `neo/base`. It does not load a map or teleport the player itself.
The reference viewpoint is:

```
setviewpos -1177.94 3760.25 -155.75 131.5
```

This uses High quality and SVGF, raises exposure to 2.2 and emissive lighting to 2,
nudges `r_lightScale` from 2 to 2.2, and reduces specular boost to 1 to balance hot
highlights. It keeps ACES and gamma 2.2, uses gentler low-throughput roulette for
cleaner indirect estimates, and disables volumetric density to remove the noisy
haze. The profile leaves sky settings unchanged. In legacy mode `r_ptSkyIntensity`
only affects primary misses; in physical mode it also illuminates bounce misses.

The look is opt-in and does not change renderer defaults or map assets. `seta`
values persist in your saved configuration; `r_lightScale` is session-only and
also affects OpenGL, so restore it to 2 when comparing against stock lighting.
For more speed, select `r_ptPreset med` after executing the config; the artistic
settings remain. This is a lighting balance, not a relit map: fully unlit areas
still need actual lights or emissive geometry to receive illumination.

## Console Variables

| CVar | Default | Description |
|------|---------|-------------|
| `r_ptDraw` | `1` | Draw 3D views with the path tracer (0 = OpenGL) |
| `r_ptBackend` | `auto` | Compute backend: `auto`, `cuda`, `opencl` |
| `r_ptOpenCLPlatform` | `-1` | OpenCL platform index, `-1` picks the best GPU (applies when the backend starts) |
| `r_ptOpenCLDevice` | `-1` | OpenCL device index within the platform, `-1` picks the best GPU |
| `r_ptOpenCLCache` | `1` | Cache compiled OpenCL programs in `<fs_savepath>/<game>/ptcache/` |
| `r_ptMode` | `0` | Render mode, see above |
| `r_ptDebug` | `0` | Print per-frame scene / timing info |
| `r_ptPreset` | `med` | Gameplay or cinematic tiers: `gameplay_low/med/high/ultra/custom`, `cinematic_low/med/high/ultra/custom`; legacy `low/med/high/ultra/custom` retained |
| `r_ptRenderScale` | `0.5` | Trace resolution relative to the viewport (0.1–1.0) |
| `r_ptSamplesPerPixel` | `4` | Samples per pixel per frame (1–64) |
| `r_ptMaxDepth` | `3` | Maximum ray bounces (1–16) |
| `r_ptMaxLightSamples` | `2` | Lights sampled per hit, picked by estimated contribution (1–8) |
| `r_ptIndirectProb` | `0.15` | Probability of the one bounce GI estimate, only used when `r_ptMaxDepth` is 1 (0–1) |
| `r_ptRussianRoulette` | `1` | Russian roulette path termination |
| `r_ptRRMinBounces` | `2` | Bounces before Russian roulette starts (0–16) |
| `r_ptRRSurvivalMin` | `0.1` | Minimum survival probability (0.01–0.95) |
| `r_ptEarlyTermThreshold` | `0.05` | Russian roulette for paths below this throughput after a bounce (0–0.5, unbiased) |
| `r_ptThroughputClamp` | `0.25` | Russian roulette for paths below this throughput after shading (0–1, unbiased) |
| `r_ptFireflyClamp` | `5.0` | Clamp per-sample radiance (0 = off) |
| `r_ptAccumulation` | `1` | Accumulate samples while the view is static |
| `r_ptResetOnSceneChange` | `1` | Scene resets: 0 = none, 1 = raw mean on structure changes with per-pixel denoiser validation, 2 = raw mean and denoiser on any content change |
| `r_ptRayOffset` | `0.001` | Ray origin offset against self-intersection |
| `r_ptEmissionBoost` | `1.0` | Emission multiplier |
| `r_ptSpecularBoost` | `2.0` | Specular BRDF multiplier (Doom 3 uses 2×) |
| `r_ptSoftShadowScale` | `0.2` | Soft shadow jitter (0 = hard shadows) |
| `r_ptDenoise` | `1` | SVGF temporal/variance-guided denoising in path tracing mode only |
| `r_ptDenoiseStrength` | `0.5` | Variance-guided luminance sigma scale (0.05–4); higher values smooth more |
| `r_ptDenoiseDebug` | `0` | 0 beauty, 1 coverage, 2 history, 3 relative noise, 4 rejection, 5 diffuse, 6 specular, 7 volume |
| `r_ptDenoiseDebugSignal` | `0` | Signal for diagnostics 2-4: 0 diffuse, 1 specular, 2 volume |
| `r_ptExposure` | `1.0` | Exposure multiplier |
| `r_ptGamma` | `2.2` | Output gamma, textures and colors are decoded with 2.2 |
| `r_ptToneMapMode` | `1` | 0 = Reinhard, 1 = ACES, 2 = Uncharted 2 |
| `r_ptSkyIntensity` | `1.0` legacy / `25.0` physical | Environment scale; 0 disables it. Physical mode also lights bounce misses |
| `r_ptSkyColorZenithR/G/B` | `0.45 / 0.28 / 0.18` | Dusty Martian zenith, linear RGB |
| `r_ptSkyColorHorizonR/G/B` | `0.65 / 0.45 / 0.30` | Butterscotch Martian horizon, linear RGB |
| `r_ptSkyColorGroundR/G/B` | `0.12 / 0.06 / 0.03` | Dark rust/regolith ground, linear RGB |
| `r_ptVolumetric` | `0.0` | Volumetric scattering intensity (0 = off, expensive) |
| `r_ptVolumetricDensity` | `0.008` | Fog density |
| `r_ptVolumetricSteps` | `16` | Ray-march steps (1–128) |
| `r_ptVolumetricAnisotropy` | `0.6` | Henyey-Greenstein g (−1 to 1) |
| `r_ptVolFalloff` | `2.0` | Distance falloff exponent |
| `r_ptVolMaxDist` | `500.0` | Maximum sampling distance |

The old `r_cu*` cvars have been renamed to `r_pt*`. Old config entries are ignored.
