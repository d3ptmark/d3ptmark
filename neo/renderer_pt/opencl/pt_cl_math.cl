/*
===============================================================================

	OpenCL C port of pt_cuda_math.cuh.

	Porting conventions used by every pt_cl_*.cl file:
	  - CUDA references become pointers; device data is __global, everything
	    else (locals, render params) is __private
	  - default arguments are spelled out at every call site, NULL where the
	    CUDA caller relied on the default
	  - fminf / sqrtf / ... are the OpenCL overloads with the same semantics
	  - PT_Clamp replaces CUDA's clamp(): OpenCL's builtin clamp orders
	    fmin / fmax differently and would return a different value for NaN

===============================================================================
*/

#define MAX_TEXTURES	PT_MAX_TEXTURES

// float constant, M_PI is a double and would promote device math to FP64
#define PT_PI			3.14159265358979f

#define PT_INLINE		static inline

typedef struct {
	float	origin[3];
	float	direction[3];
	float	tMin;
	float	tMax;
	float	worldTMin;	// non-weapon triangles closer than this are ignored
	int		flags;
} ptRay_t;

typedef struct {
	bool	hit;
	float	t;
	float	position[3];
	float	normal[3];
	float	tangent[3];
	float	binormal[3];
	float	texcoord[2];
	float	barycentrics[3];	// (w, u, v)
	int		materialIndex;
	int		triangleIndex;
	int		triFlags;			// ptTriangle_t::flags of the hit triangle
} ptHitInfo_t;

// CUDA __device__ globals g_softShadowScale / g_rayOffset, staged per launch
typedef struct {
	float	softShadowScale;
	float	rayOffset;
} ptClGlobals_t;

// texture descriptor table + texel pool, replaces const cudaTexture_t *
typedef struct {
	__global const ptClTexture_t	*desc;
	__global const uchar4			*texels;
} ptClTextures_t;

/*
========================
PT_Clamp
========================
*/
PT_INLINE float PT_Clamp( float x, float a, float b ) {
	return fmax( a, fmin( b, x ) );
}

/*
========================
PT_TanHalfFov

tan( fovDegrees / 2 ) for the camera. CUDA evaluates tanf( fov * PT_PI / 360 ),
which nvcc rounds correctly in practice (tanf(pi/4) == 1). OpenCL only
promises 4 ulp for tan() and some drivers return 1.00000012 there, which
nudges every SVGF reprojection off the pixel grid. tanpi is exact at the
common angles and agrees with the correctly rounded value elsewhere.
========================
*/
PT_INLINE float PT_TanHalfFov( float fovDegrees ) {
	return tanpi( fovDegrees / 360.0f );
}

/*
========================
dot3
========================
*/
PT_INLINE float dot3( const float *a, const float *b ) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/*
========================
dot3g

One __global operand (vertex / light / guide data)
========================
*/
PT_INLINE float dot3g( const float *a, __global const float *b ) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/*
========================
cross3
========================
*/
PT_INLINE void cross3( const float *a, const float *b, float *result ) {
	result[0] = a[1] * b[2] - a[2] * b[1];
	result[1] = a[2] * b[0] - a[0] * b[2];
	result[2] = a[0] * b[1] - a[1] * b[0];
}

/*
========================
normalize3
========================
*/
PT_INLINE void normalize3( float *v ) {
	float len = sqrt( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
	if ( len > 1e-6f ) {
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
}

/*
========================
length3
========================
*/
PT_INLINE float length3( const float *v ) {
	return sqrt( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
}

/*
========================
PCG random numbers, bit identical to the CUDA backend
========================
*/
typedef struct {
	uint	state;
} ptRng_t;

/*
========================
PT_PcgPermute
========================
*/
PT_INLINE uint PT_PcgPermute( uint state ) {
	uint word = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

/*
========================
PT_PcgHash
========================
*/
PT_INLINE uint PT_PcgHash( uint v ) {
	return PT_PcgPermute( v * 747796405u + 2891336453u );
}

/*
========================
PT_RngInit
========================
*/
PT_INLINE void PT_RngInit( ptRng_t *rng, uint pixel, uint frame ) {
	rng->state = PT_PcgHash( pixel ^ PT_PcgHash( frame + 0x9E3779B9u ) );
}

/*
========================
PT_Rand

Uniform float in [0, 1)
========================
*/
PT_INLINE float PT_Rand( ptRng_t *rng ) {
	rng->state = rng->state * 747796405u + 2891336453u;
	return (float)( PT_PcgPermute( rng->state ) >> 8 ) * ( 1.0f / 16777216.0f );
}

/*
========================
cupt_hash

Stateless hash, used for per-triangle debug colors
========================
*/
PT_INLINE uint cupt_hash( uint seed ) {
	return PT_PcgHash( seed );
}

/*
========================
PT_SrgbToLinear

Plain 2.2 power curve, the exact inverse of the default r_ptGamma.
========================
*/
PT_INLINE float PT_SrgbToLinear( float c ) {
	return c > 0.0f ? exp2( 2.2f * log2( c ) ) : 0.0f;
}
