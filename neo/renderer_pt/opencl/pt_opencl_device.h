#ifndef __PT_OPENCL_DEVICE_H__
#define __PT_OPENCL_DEVICE_H__

/*
===============================================================================

	OpenCL backend internals, shared by pt_opencl_kernels.cpp,
	pt_backend_opencl.cpp and the OpenCL regression tests. Counterpart of
	pt_cuda_device.h: buffer bundles plus one launch function per CUDA
	Launch* entry point, all ordered on the caller's in-order queue.

	This layer deliberately has no idlib dependency.

	Nothing outside renderer_pt/opencl/ and its tests should include this.

===============================================================================
*/

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

// clXxx calls go through runtime-loaded entry points, see PT_CL_Load()
#include "renderer_pt/opencl/pt_opencl_loader.h"

#include <string>
#include <vector>

#include "renderer_pt/opencl/pt_opencl_shared.h"

enum ptClKernel_t {
	PT_CL_PATHTRACING,
	PT_CL_TRIANGLE_DRAW,
	PT_CL_TRIANGLE_TEXTURED,
	PT_CL_TRIANGLE_RAYTRACED,
	PT_CL_TONEMAP,
	PT_CL_SVGF_CAPTURE_GUIDES,
	PT_CL_SVGF_CAPTURE,
	PT_CL_SVGF_TEMPORAL,
	PT_CL_SVGF_BOOTSTRAP_VOLUME,
	PT_CL_SVGF_ATROUS,
	PT_CL_SVGF_STORE_SURFACE,
	PT_CL_SVGF_RESOLVE,
	PT_CL_SVGF_DEBUG,
	PT_CL_POST_HIGHLIGHTS,
	PT_CL_POST_BLUR,
	PT_CL_POST_COMPOSITE,
	PT_CL_MORTON,
	PT_CL_RADIX_HISTOGRAM,
	PT_CL_RADIX_SCAN,
	PT_CL_RADIX_SCATTER,
	PT_CL_RADIX_TREE,
	PT_CL_LEAF_NODES,
	PT_CL_ESCAPE_LINKS,
	PT_CL_NODE_BOUNDS,
	PT_CL_SINGLE_LEAF,
	PT_CL_LAYOUT,
	PT_CL_KERNEL_COUNT
};

struct ptClProgram_t {
	cl_context				context;
	cl_device_id			device;
	cl_program				program;
	cl_kernel				kernels[PT_CL_KERNEL_COUNT];
	size_t					maxWorkGroup[PT_CL_KERNEL_COUNT];
	size_t					sortWorkGroup;		// PT_CL_SORT_WG the program was built with
	std::vector<cl_event> *	events;				// when set, every enqueued command appends its event
};

struct ptClSceneView_t {
	cl_mem	vertices;
	cl_mem	triangles;
	cl_mem	bvhNodes;
	cl_mem	triIndices;
	cl_mem	materials;
	cl_mem	textureDesc;		// ptClTexture_t[PT_MAX_TEXTURES]
	cl_mem	texels;				// RGBA8 pool, may be NULL when no texture is resident
	cl_mem	lights;
	int		numLights;
};

struct ptClDenoiseSignal_t {
	cl_mem	radiance;
	cl_mem	history[2];			// realtime: illumination + variance; capture: raw mean RGB + E[luminance^2]
	cl_mem	moments[2];			// float4: luminance, squared luminance, history length, rejection reason
};

struct ptClDenoiseBuffers_t {
	cl_mem				radiance;			// full fresh HDR, including emission and volume
	ptClDenoiseSignal_t	signals[3];			// diffuse, specular, volume
	cl_mem				guides[2];
	cl_mem				scratch;			// float4 illumination + variance
	cl_mem				filteredDiffuse;
	cl_mem				filteredSpecular;
};

// Dedicated display-only storage: never aliases accumulation or denoiser history.
struct ptClPostprocessBuffers_t {
	cl_mem	highlights;
	cl_mem	scratch;
	cl_mem	blurred;
	cl_mem	output;
};

const char *	PT_CL_ErrorString( cl_int error );

// The embedded OpenCL C program (pt_types.h, pt_opencl_shared.h and the pt_cl_*.cl files).
const char *	PT_CL_ProgramSource();

// Build options derived from the device and the host's compile time switches (PT_PHYSICAL).
std::string		PT_CL_BuildOptions( cl_device_id device );

// Stable 64 bit key of source + extraSource + options + device + driver, for binary caches.
std::string		PT_CL_ProgramKey( cl_device_id device, const char *extraSource );

// Builds the program (from cachedBinary when it is valid, otherwise from source),
// creates every kernel and, when binaryOut is set, returns the device binary.
// extraSource is appended to the embedded program (regression tests add kernels).
cl_int			PT_CL_CreateProgram( ptClProgram_t &prog, cl_context context, cl_device_id device, const char *extraSource,
									 const std::vector<unsigned char> *cachedBinary, std::vector<unsigned char> *binaryOut,
									 std::string &log );
void			PT_CL_DestroyProgram( ptClProgram_t &prog );

// Runs PT_LayoutKernel and compares the device struct sizes against the host's.
cl_int			PT_CL_CheckLayout( ptClProgram_t &prog, cl_command_queue queue, std::string &error );

// 2D launch over width x height with 16 x 16 tiles (smaller if the kernel can not run them).
cl_int			PT_CL_Launch2D( ptClProgram_t &prog, cl_command_queue queue, int kernel, int width, int height );

cl_int			PT_CL_Fill( ptClProgram_t &prog, cl_command_queue queue, cl_mem buffer, unsigned char value, size_t bytes );
cl_int			PT_CL_Copy( ptClProgram_t &prog, cl_command_queue queue, cl_mem src, cl_mem dst, size_t bytes );

// traces params.width x params.height into the float4 accumulation buffer (row pitch == params.width)
cl_int			PT_CL_LaunchPathTracing( ptClProgram_t &prog, cl_command_queue queue, const ptClSceneView_t &scene,
										 cl_mem accumBuffer, cl_mem radianceBuffer, cl_mem guideBuffer,
										 cl_mem diffuseBuffer, cl_mem specularBuffer, cl_mem volumeBuffer,
										 const ptRenderParams_t &params );

// Temporal reprojection/moments, four atrous passes and albedo remodulation.
// frameSlot selects current guides/history/moments; the other slot is read-only.
cl_int			PT_CL_LaunchDenoising( ptClProgram_t &prog, cl_command_queue queue, const ptClDenoiseBuffers_t &buffers,
									   cl_mem accumBuffer, cl_mem filteredBuffer, const ptRenderParams_t &params,
									   const ptRenderParams_t &previousParams, int frameSlot, int historyValid );

cl_int			PT_CL_LaunchDenoisingDebug( ptClProgram_t &prog, cl_command_queue queue, const ptClDenoiseBuffers_t &buffers,
											cl_mem output, const ptRenderParams_t &params, int frameSlot );

// PT_Postprocess: bloom / lens ghosts into buffers.output, hdr is never written
cl_int			PT_CL_Postprocess( ptClProgram_t &prog, cl_command_queue queue, const ptClPostprocessBuffers_t &buffers,
								   cl_mem hdr, int width, int height, float exposure, float threshold, float radius,
								   float bloom, float flare );
cl_int			PT_CL_LaunchPostprocess( ptClProgram_t &prog, cl_command_queue queue, const ptClPostprocessBuffers_t &buffers,
										 cl_mem hdr, const ptRenderParams_t &params );

// resolves the HDR buffer into RGBA8
cl_int			PT_CL_LaunchToneMapping( ptClProgram_t &prog, cl_command_queue queue, cl_mem hdrBuffer, cl_mem ldrBuffer,
										 int width, int height, float exposure, float gamma, int toneMapMode );

// bytes of the radix sort histogram scratch LaunchBuildLBVH needs for numTriangles triangles
size_t			PT_CL_LBVHSortTempBytes( const ptClProgram_t &prog, int numTriangles );

// GPU LBVH construction, sceneBounds is a host pointer (min xyz, max xyz). mortonCodes / mortonAlt and
// sortedIndices are numTriangles long sort buffers, parents 2n, atomicCounters n.
cl_int			PT_CL_LaunchBuildLBVH( ptClProgram_t &prog, cl_command_queue queue, cl_mem triangles, int numTriangles,
									   cl_mem bvhNodes, cl_mem triIndices, cl_mem mortonCodes, cl_mem mortonAlt,
									   cl_mem sortedIndices, cl_mem sortTemp, cl_mem parents, cl_mem atomicCounters,
									   const float *sceneBounds );

#endif /* !__PT_OPENCL_DEVICE_H__ */
