#ifndef __PT_OPENCL_SHARED_H__
#define __PT_OPENCL_SHARED_H__

/*
===============================================================================

	OpenCL backend types shared by the host (C++) and the device program
	(OpenCL C 1.2). Counterpart of the data half of pt_cuda_device.h.

	Like pt_types.h this must stay plain C with 4-byte scalars only, so the
	host and every OpenCL compiler agree on the memory layout. The backend
	verifies the device-side sizes at startup.

	Nothing outside renderer_pt/opencl/ should include this.

===============================================================================
*/

#include "renderer_pt/pt_types.h"

#define PT_CL_TILE_SIZE		16

// OpenCL 1.2 has no pointers inside buffers, so every texture lives in one
// texel pool. offset is in RGBA8 texels; width == 0 means "not resident" and
// the samplers return white, exactly like a NULL cudaTexture_t::data.
typedef struct {
	int				width;
	int				height;
	unsigned int	offset;
	int				pad;
} ptClTexture_t;

// ptRay_t::flags
#define PT_RAY_SEES_WEAPON		1		// camera ray segment, PT_TRI_WEAPON triangles are visible and win

// view weapons poke into walls and floors, the GL renderer hides that with a depth hack
#define PT_WEAPON_REACH			64.0f	// camera rays look this far for a weapon hit
#define PT_WEAPON_CLEARANCE		32.0f	// rays leaving the weapon ignore world geometry this close

typedef struct {
	float	normal[3];		// unperturbed surface normal for temporal validation
	float	depth;			// <= 0: no single surface suitable for demodulation
	float	position[3];	// world-space hit position for camera reprojection
	int		materialIndex;
	float	albedo[3];		// bounded modulation color
	int		historyValid;	// dynamic entities have spatial filtering only
	float	shadingNormal[3];
	float	roughness;
	float	specularAlbedo[3];
	int		coverage;		// ptDenoiseCoverage_t
	float	volumeDepth;	// first visible layer, independent of surface eligibility
} ptClDenoiseGuide_t;

enum ptDenoiseCoverage_t {
	PT_DENOISE_MISS,
	PT_DENOISE_STATIC,
	PT_DENOISE_DYNAMIC,
	PT_DENOISE_TRANSPARENT,
	PT_DENOISE_OVERLAY,
	PT_DENOISE_MIXED
};

enum ptDenoiseReject_t {
	PT_DENOISE_REUSED,
	PT_DENOISE_RESET,
	PT_DENOISE_UNSUPPORTED,
	PT_DENOISE_NO_MOTION,
	PT_DENOISE_OFFSCREEN,
	PT_DENOISE_DEPTH,
	PT_DENOISE_NORMAL,
	PT_DENOISE_MATERIAL,
	PT_DENOISE_REFLECTANCE,
	PT_DENOISE_LIGHTING,
	PT_DENOISE_NO_SAMPLES
};

// identical to PT_CudaPostprocessScale
static inline int PT_ClPostprocessScale( int height, float radius ) {
	const float boundedRadius = radius > 0.0f && radius <= 3.402823466e38f ? ( radius < 0.1f ? radius : 0.1f ) : 0.025f;
	int scale = 4;
	while ( boundedRadius * height / scale > 3.0f ) scale *= 2;
	return scale;
}

// morton3D produces 30 bit codes; the radix sort uses 4 bit digits
#define PT_CL_RADIX_BITS		4
#define PT_CL_RADIX_DIGITS		16
#define PT_CL_RADIX_PASSES		8		// even, so keys and values end in their first buffers

#endif /* !__PT_OPENCL_SHARED_H__ */
