// System, STL and OpenCL headers must come before idlib: idlib/Str.h redefines
// snprintf/vsnprintf (use_idStr_snPrintf), which breaks MSVC's <string>/<stdio.h>.
#include <stdio.h>
#include <stdlib.h>
#include "renderer_pt/opencl/pt_opencl_device.h"

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/CVarSystem.h"
#include "framework/FileSystem.h"
#include "idlib/math/Math.h"

#include "renderer_pt/pt_backend.h"
#include "renderer_pt/pt_scene.h"

/*
===============================================================================

	idPathTracerBackendOpenCL

	OpenCL port of idPathTracerBackendCUDA. All device work is ordered on a
	single in-order queue. The only host synchronization point is the end of
	Render(), so ReadPixels() and the timing queries see finished results.
	Scene and texture uploads are blocking, like CUDA's copies from pageable
	memory, so the scene's host arrays may change as soon as UploadScene()
	returns.

	OpenCL 1.2 buffers can not hold pointers, so the per-texture device
	allocations of the CUDA backend become slices of one texel pool that is
	addressed through the ptClTexture_t descriptor table.

===============================================================================
*/

static idCVar r_ptOpenCLPlatform( "r_ptOpenCLPlatform", "-1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER,
	"OpenCL path tracer platform index, -1 picks the best GPU (applies when the backend starts)" );
static idCVar r_ptOpenCLDevice( "r_ptOpenCLDevice", "-1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER,
	"OpenCL path tracer device index within r_ptOpenCLPlatform, -1 picks the best GPU (applies when the backend starts)" );
static idCVar r_ptOpenCLCache( "r_ptOpenCLCache", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
	"cache compiled OpenCL path tracer programs in <fs_savepath>/<game>/ptcache" );

#define PT_CL_CHECK( call ) \
	do { \
		const cl_int _err = ( call ); \
		if ( _err != CL_SUCCESS ) { \
			OnError( #call, _err ); \
			return false; \
		} \
	} while ( 0 )

/*
========================
Device selection, shared by the backend, the availability probe and the
capture report.
========================
*/
struct ptClDeviceChoice_t {
	cl_platform_id	platform;
	cl_device_id	device;
	int				platformIndex;
	int				deviceIndex;
};

static bool PT_CL_DeviceUsable( cl_device_id device ) {
	cl_bool available = CL_FALSE, compiler = CL_FALSE;
	char version[128] = {};
	if ( clGetDeviceInfo( device, CL_DEVICE_AVAILABLE, sizeof( available ), &available, NULL ) != CL_SUCCESS
		|| clGetDeviceInfo( device, CL_DEVICE_COMPILER_AVAILABLE, sizeof( compiler ), &compiler, NULL ) != CL_SUCCESS
		|| clGetDeviceInfo( device, CL_DEVICE_VERSION, sizeof( version ) - 1, version, NULL ) != CL_SUCCESS ) {
		return false;
	}
	int major = 0, minor = 0;
	if ( sscanf( version, "OpenCL %d.%d", &major, &minor ) != 2 ) return false;
	return available && compiler && ( major > 1 || ( major == 1 && minor >= 2 ) );
}

static bool PT_CL_SelectDevice( ptClDeviceChoice_t &choice ) {
	memset( &choice, 0, sizeof( choice ) );
	if ( !PT_CL_Load() ) return false;
	cl_uint numPlatforms = 0;
	if ( clGetPlatformIDs( 0, NULL, &numPlatforms ) != CL_SUCCESS || numPlatforms == 0 ) return false;
	idList<cl_platform_id> platforms;
	platforms.SetNum( numPlatforms );
	if ( clGetPlatformIDs( numPlatforms, platforms.Ptr(), NULL ) != CL_SUCCESS ) return false;

	const int wantPlatform = r_ptOpenCLPlatform.GetInteger();
	const int wantDevice = r_ptOpenCLDevice.GetInteger();
	const bool explicitChoice = wantPlatform >= 0 && wantDevice >= 0;

	double bestScore = -1.0;
	for ( int p = 0; p < (int)numPlatforms; p++ ) {
		if ( explicitChoice && p != wantPlatform ) continue;
		// an explicit choice may name any device type (CPU runtimes for debugging), auto only GPUs
		const cl_device_type type = explicitChoice ? CL_DEVICE_TYPE_ALL : CL_DEVICE_TYPE_GPU;
		cl_uint numDevices = 0;
		if ( clGetDeviceIDs( platforms[p], type, 0, NULL, &numDevices ) != CL_SUCCESS || numDevices == 0 ) continue;
		idList<cl_device_id> devices;
		devices.SetNum( numDevices );
		if ( clGetDeviceIDs( platforms[p], type, numDevices, devices.Ptr(), NULL ) != CL_SUCCESS ) continue;
		for ( int d = 0; d < (int)numDevices; d++ ) {
			if ( explicitChoice && d != wantDevice ) continue;
			if ( !PT_CL_DeviceUsable( devices[d] ) ) continue;
			cl_bool unified = CL_FALSE;
			cl_ulong memory = 0;
			cl_uint units = 0;
			clGetDeviceInfo( devices[d], CL_DEVICE_HOST_UNIFIED_MEMORY, sizeof( unified ), &unified, NULL );
			clGetDeviceInfo( devices[d], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof( memory ), &memory, NULL );
			clGetDeviceInfo( devices[d], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof( units ), &units, NULL );
			// discrete before integrated, then the bigger device
			const double score = ( unified ? 0.0 : 1.0e6 ) + units + (double)memory / ( 1024.0 * 1024.0 * 1024.0 * 1024.0 );
			if ( score > bestScore ) {
				bestScore = score;
				choice.platform = platforms[p];
				choice.device = devices[d];
				choice.platformIndex = p;
				choice.deviceIndex = d;
			}
		}
	}
	return choice.device != NULL;
}

static idStr PT_CL_DeviceString( cl_device_id device, cl_device_info param ) {
	char text[1024] = {};
	if ( clGetDeviceInfo( device, param, sizeof( text ) - 1, text, NULL ) != CL_SUCCESS ) return "";
	idStr result = text;
	result.StripTrailingWhitespace();
	result.StripLeading( ' ' );
	return result;
}

static idStr PT_CL_PlatformString( cl_platform_id platform, cl_platform_info param ) {
	char text[1024] = {};
	if ( clGetPlatformInfo( platform, param, sizeof( text ) - 1, text, NULL ) != CL_SUCCESS ) return "";
	idStr result = text;
	result.StripTrailingWhitespace();
	return result;
}

class idPathTracerBackendOpenCL : public idPathTracerBackend {
public:
							idPathTracerBackendOpenCL();
	virtual					~idPathTracerBackendOpenCL();

	static bool				IsAvailable();

	virtual const char *	Name() const { return "opencl"; }
	virtual void			PrintInfo() const;

	virtual bool			Init( int maxWidth, int maxHeight );
	virtual void			Shutdown();
	virtual bool			Resize( int maxWidth, int maxHeight );
	virtual bool			IsHealthy() const { return healthy; }

	virtual bool			UploadScene( const idPathTracerScene &scene );
	virtual bool			Render( const ptRenderParams_t &params );
	virtual bool			ReadPixels( unsigned char *dest, int width, int height );

	virtual void			ResetAccumulation( bool resetDenoising = true ) {
		accumFrames = 0;
		if ( resetDenoising ) denoiseHistoryValid = false;
	}
	virtual int				GetAccumulatedFrames() const { return accumFrames; }
	virtual void			GetStats( ptBackendStats_t &stats ) const;

private:
	bool					Allocate( cl_mem &buffer, size_t bytes );
	void					Release( cl_mem &buffer );
	bool					Reserve( cl_mem &buffer, int &capacity, int needed, size_t elementSize, const char *what );
	bool					BuildProgram();
	bool					UploadTextures( const idPathTracerScene &scene );
	bool					ReserveTexturePool( size_t texels );
	void					FreeTextures();
	bool					BuildBVH( const idPathTracerScene &scene );
	void					OnError( const char *what, cl_int err );
	bool					ResizeDenoiser( int width, int height );
	void					FreeDenoiser();
	bool					ResizePostprocess( int width, int height, float radius );
	void					FreePostprocess();
	float					ElapsedMs( size_t firstEvent, size_t endEvent ) const;
	void					ReleaseEvents();

	bool					initialized;
	bool					healthy;
	ptClDeviceChoice_t		choice;
	cl_context				context;
	cl_command_queue		queue;
	ptClProgram_t			program;
	idList<cl_event>		events;			// profiling events of the current Render()
	cl_ulong				maxAllocation;

	// output
	int						maxWidth;
	int						maxHeight;
	cl_mem					d_accum;			// float4 HDR accumulation, row pitch == trace width
	cl_mem					d_filtered;			// float4 denoised HDR, never fed back into accumulation
	cl_mem					d_output;			// RGBA8
	int						lastWidth;
	int						lastHeight;
	int						accumFrames;
	float					traceMs;
	float					pathMs, denoiseMs, postprocessMs, toneMapMs;
	ptClDenoiseBuffers_t	denoiser;
	ptClPostprocessBuffers_t postprocess;
	int						postWidth, postHeight, postScale;
	int						denoiseWidth;
	int						denoiseHeight;
	int						denoiseFrameSlot;
	bool					denoiseHistoryValid;
	ptRenderParams_t		previousDenoiseParams;
	unsigned int			sampleSequence;

	// scene, all grown on demand
	cl_mem					d_vertices;		int capVertices;
	cl_mem					d_triangles;	int capTriangles;
	cl_mem					d_bvhNodes;		int capBVHNodes;
	cl_mem					d_triIndices;	int capTriIndices;
	cl_mem					d_mortonCodes;	int capMorton;
	cl_mem					d_mortonAlt;	int capMortonAlt;
	cl_mem					d_sortTemp;		int capSortTemp;	// radix sort histogram, bytes
	cl_mem					d_sortedIndices;int capSorted;
	cl_mem					d_parents;		int capParents;
	cl_mem					d_atomics;		int capAtomics;
	cl_mem					d_materials;	int capMaterials;
	cl_mem					d_lights;		int capLights;

	int						numVertices;
	int						numTriangles;
	int						numBVHNodes;
	int						numMaterials;
	int						numLights;

	// textures: host mirror of the device descriptor table plus the texel pool
	ptClTexture_t			h_texDesc[PT_MAX_TEXTURES];
	size_t					h_texCapacity[PT_MAX_TEXTURES];	// texels reserved for the slot in the pool
	cl_mem					d_texDesc;
	cl_mem					d_texPool;
	size_t					texPoolCapacity;	// texels
	size_t					texPoolUsed;		// texels
	int						numTextures;
	int						textureGeneration;
	size_t					textureBytes;
};

/*
========================
idPathTracerBackendOpenCL::idPathTracerBackendOpenCL
========================
*/
idPathTracerBackendOpenCL::idPathTracerBackendOpenCL() {
	initialized = false;
	healthy = false;
	memset( &choice, 0, sizeof( choice ) );
	context = NULL;
	queue = NULL;
	memset( &program, 0, sizeof( program ) );
	maxAllocation = 0;
	pathMs = denoiseMs = postprocessMs = toneMapMs = 0;

	maxWidth = maxHeight = 0;
	d_accum = NULL;
	d_filtered = NULL;
	d_output = NULL;
	lastWidth = lastHeight = 0;
	accumFrames = 0;
	traceMs = 0.0f;
	memset( &denoiser, 0, sizeof( denoiser ) );
	memset( &postprocess, 0, sizeof( postprocess ) );
	postWidth = postHeight = postScale = 0;
	memset( &previousDenoiseParams, 0, sizeof( previousDenoiseParams ) );
	denoiseWidth = denoiseHeight = 0;
	denoiseFrameSlot = 0;
	denoiseHistoryValid = false;
	sampleSequence = 0;

	d_vertices = NULL;		capVertices = 0;
	d_triangles = NULL;		capTriangles = 0;
	d_bvhNodes = NULL;		capBVHNodes = 0;
	d_triIndices = NULL;	capTriIndices = 0;
	d_mortonCodes = NULL;	capMorton = 0;
	d_mortonAlt = NULL;		capMortonAlt = 0;
	d_sortTemp = NULL;		capSortTemp = 0;
	d_sortedIndices = NULL;	capSorted = 0;
	d_parents = NULL;		capParents = 0;
	d_atomics = NULL;		capAtomics = 0;
	d_materials = NULL;		capMaterials = 0;
	d_lights = NULL;		capLights = 0;

	numVertices = numTriangles = numBVHNodes = numMaterials = numLights = 0;

	memset( h_texDesc, 0, sizeof( h_texDesc ) );
	memset( h_texCapacity, 0, sizeof( h_texCapacity ) );
	d_texDesc = NULL;
	d_texPool = NULL;
	texPoolCapacity = texPoolUsed = 0;
	numTextures = 0;
	textureGeneration = -1;
	textureBytes = 0;
}

/*
========================
idPathTracerBackendOpenCL::~idPathTracerBackendOpenCL
========================
*/
idPathTracerBackendOpenCL::~idPathTracerBackendOpenCL() {
	Shutdown();
}

/*
========================
idPathTracerBackendOpenCL::IsAvailable
========================
*/
bool idPathTracerBackendOpenCL::IsAvailable() {
	ptClDeviceChoice_t probe;
	return PT_CL_SelectDevice( probe );
}

/*
========================
idPathTracerBackendOpenCL::OnError
========================
*/
void idPathTracerBackendOpenCL::OnError( const char *what, cl_int err ) {
	common->Warning( "OpenCL path tracer: %s failed: %s (%d)\n", what, PT_CL_ErrorString( err ), err );
	healthy = false;
}

/*
========================
idPathTracerBackendOpenCL::PrintInfo
========================
*/
void idPathTracerBackendOpenCL::PrintInfo() const {
	if ( !PT_CL_Load() ) {
		common->Printf( "OpenCL runtime not found\n" );
		return;
	}
	cl_uint numPlatforms = 0;
	clGetPlatformIDs( 0, NULL, &numPlatforms );
	idList<cl_platform_id> platforms;
	platforms.SetNum( numPlatforms );
	if ( numPlatforms > 0 ) clGetPlatformIDs( numPlatforms, platforms.Ptr(), NULL );
	common->Printf( "OpenCL: %u platform(s)\n", numPlatforms );
	for ( int p = 0; p < (int)numPlatforms; p++ ) {
		common->Printf( "  platform %d: %s (%s)\n", p, PT_CL_PlatformString( platforms[p], CL_PLATFORM_NAME ).c_str(),
			PT_CL_PlatformString( platforms[p], CL_PLATFORM_VERSION ).c_str() );
		cl_uint numDevices = 0;
		if ( clGetDeviceIDs( platforms[p], CL_DEVICE_TYPE_ALL, 0, NULL, &numDevices ) != CL_SUCCESS ) continue;
		idList<cl_device_id> devices;
		devices.SetNum( numDevices );
		if ( numDevices == 0 || clGetDeviceIDs( platforms[p], CL_DEVICE_TYPE_ALL, numDevices, devices.Ptr(), NULL ) != CL_SUCCESS ) continue;
		for ( int d = 0; d < (int)numDevices; d++ ) {
			cl_uint units = 0;
			cl_ulong memory = 0;
			clGetDeviceInfo( devices[d], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof( units ), &units, NULL );
			clGetDeviceInfo( devices[d], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof( memory ), &memory, NULL );
			common->Printf( "    %d: %s, %s, driver %s, %u CUs, %.2f GB%s%s\n", d,
				PT_CL_DeviceString( devices[d], CL_DEVICE_NAME ).c_str(),
				PT_CL_DeviceString( devices[d], CL_DEVICE_VERSION ).c_str(),
				PT_CL_DeviceString( devices[d], CL_DRIVER_VERSION ).c_str(),
				units, memory / ( 1024.0 * 1024.0 * 1024.0 ),
				PT_CL_DeviceUsable( devices[d] ) ? "" : " (unusable)",
				devices[d] == choice.device ? " (active)" : "" );
		}
	}

	ptBackendStats_t stats;
	GetStats( stats );
	common->Printf( "  output %dx%d, %d verts, %d tris, %d BVH nodes, %d materials, %d lights, %d textures\n",
		maxWidth, maxHeight, stats.numVertices, stats.numTriangles, stats.numBVHNodes,
		stats.numMaterials, stats.numLights, stats.numTextures );
	common->Printf( "  device memory %.1f MB, last trace %.2f ms, %d accumulated frames\n",
		stats.deviceBytes / ( 1024.0 * 1024.0 ), traceMs, accumFrames );
}

/*
========================
idPathTracerBackendOpenCL::Allocate / Release
========================
*/
bool idPathTracerBackendOpenCL::Allocate( cl_mem &buffer, size_t bytes ) {
	Release( buffer );
	cl_int err = CL_SUCCESS;
	buffer = clCreateBuffer( context, CL_MEM_READ_WRITE, Max( bytes, (size_t)4 ), NULL, &err );
	if ( err != CL_SUCCESS ) {
		buffer = NULL;
		OnError( "clCreateBuffer", err );
		return false;
	}
	return true;
}

void idPathTracerBackendOpenCL::Release( cl_mem &buffer ) {
	if ( buffer ) clReleaseMemObject( buffer );
	buffer = NULL;
}

/*
========================
idPathTracerBackendOpenCL::BuildProgram

Compiling the program takes seconds, so the device binary is cached per
source / options / device / driver key.
========================
*/
bool idPathTracerBackendOpenCL::BuildProgram() {
	const bool useCache = r_ptOpenCLCache.GetBool() && fileSystem != NULL;
	const idStr cachePath = va( "ptcache/opencl_%s.bin", PT_CL_ProgramKey( choice.device, NULL ).c_str() );
	idList<byte> cached, binary;
	if ( useCache ) {
		void *buffer = NULL;
		const int size = fileSystem->ReadFile( cachePath.c_str(), &buffer );
		if ( size > 0 && buffer ) {
			cached.SetNum( size );
			memcpy( cached.Ptr(), buffer, size );
		}
		if ( buffer ) fileSystem->FreeFile( buffer );
	}

	const int start = Sys_Milliseconds();
	idStr log;
	const cl_int err = PT_CL_CreateProgram( program, context, choice.device, NULL, cached.Num() == 0 ? NULL : &cached,
		useCache ? &binary : NULL, log );
	if ( err != CL_SUCCESS ) {
		common->Warning( "OpenCL path tracer: program build failed: %s\n%s", PT_CL_ErrorString( err ), log.c_str() );
		healthy = false;
		return false;
	}
	if ( binary.Num() > 0 ) {
		common->Printf( "OpenCL path tracer: compiled kernels in %.1f s\n", ( Sys_Milliseconds() - start ) * 0.001f );
		fileSystem->WriteFile( cachePath.c_str(), binary.Ptr(), binary.Num(), "fs_savepath" );
	}

	idStr layoutError;
	const cl_int layout = PT_CL_CheckLayout( program, queue, layoutError );
	if ( layout != CL_SUCCESS ) {
		common->Warning( "OpenCL path tracer: device struct layout check failed: %s %s\n",
			PT_CL_ErrorString( layout ), layoutError.c_str() );
		healthy = false;
		return false;
	}
	return true;
}

/*
========================
idPathTracerBackendOpenCL::Init
========================
*/
bool idPathTracerBackendOpenCL::Init( int width, int height ) {
	Shutdown();

	if ( !PT_CL_SelectDevice( choice ) ) {
		common->Warning( "OpenCL path tracer: no compatible OpenCL 1.2 device found\n" );
		return false;
	}

	healthy = true;
	initialized = true;

	cl_int err = CL_SUCCESS;
	const cl_context_properties properties[] = { CL_CONTEXT_PLATFORM, (cl_context_properties)choice.platform, 0 };
	context = clCreateContext( properties, 1, &choice.device, NULL, NULL, &err );
	PT_CL_CHECK( err );
	queue = clCreateCommandQueue( context, choice.device, CL_QUEUE_PROFILING_ENABLE, &err );
	PT_CL_CHECK( err );
	clGetDeviceInfo( choice.device, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof( maxAllocation ), &maxAllocation, NULL );

	if ( !BuildProgram() ) {
		return false;
	}

	if ( !Allocate( d_texDesc, PT_MAX_TEXTURES * sizeof( ptClTexture_t ) ) ) {
		return false;
	}
	PT_CL_CHECK( clEnqueueWriteBuffer( queue, d_texDesc, CL_TRUE, 0, sizeof( h_texDesc ), h_texDesc, 0, NULL, NULL ) );

	if ( !Resize( width, height ) ) {
		return false;
	}

	common->Printf( "OpenCL path tracer: %s (%s), %dx%d\n", PT_CL_DeviceString( choice.device, CL_DEVICE_NAME ).c_str(),
		PT_CL_DeviceString( choice.device, CL_DEVICE_VERSION ).c_str(), width, height );
	return true;
}

/*
========================
idPathTracerBackendOpenCL::Shutdown

Frees only what this backend owns.
========================
*/
void idPathTracerBackendOpenCL::Shutdown() {
	if ( !initialized ) {
		return;
	}
	if ( queue ) {
		clFinish( queue );
	}
	ReleaseEvents();

	FreeTextures();

	Release( d_texDesc );
	Release( d_accum );
	FreeDenoiser();
	FreePostprocess();
	Release( d_output );
	Release( d_vertices );		capVertices = 0;
	Release( d_triangles );		capTriangles = 0;
	Release( d_bvhNodes );		capBVHNodes = 0;
	Release( d_triIndices );	capTriIndices = 0;
	Release( d_mortonCodes );	capMorton = 0;
	Release( d_mortonAlt );		capMortonAlt = 0;
	Release( d_sortTemp );		capSortTemp = 0;
	Release( d_sortedIndices );	capSorted = 0;
	Release( d_parents );		capParents = 0;
	Release( d_atomics );		capAtomics = 0;
	Release( d_materials );		capMaterials = 0;
	Release( d_lights );		capLights = 0;

	PT_CL_DestroyProgram( program );
	if ( queue ) {
		clReleaseCommandQueue( queue );
		queue = NULL;
	}
	if ( context ) {
		clReleaseContext( context );
		context = NULL;
	}

	numVertices = numTriangles = numBVHNodes = numMaterials = numLights = 0;
	maxWidth = maxHeight = lastWidth = lastHeight = 0;
	accumFrames = 0;
	textureGeneration = -1;
	initialized = false;
	healthy = false;
}

/*
========================
idPathTracerBackendOpenCL::Resize
========================
*/
bool idPathTracerBackendOpenCL::Resize( int width, int height ) {
	if ( !initialized ) {
		return false;
	}
	width = Max( width, 1 );
	height = Max( height, 1 );
	if ( width == maxWidth && height == maxHeight && d_accum && d_output ) {
		return true;
	}
	if ( queue ) {
		clFinish( queue );
	}

	Release( d_accum );
	FreeDenoiser();
	FreePostprocess();
	Release( d_output );
	maxWidth = maxHeight = 0;

	const size_t pixels = (size_t)width * height;
	if ( !Allocate( d_accum, pixels * 4 * sizeof( float ) ) || !Allocate( d_output, pixels * 4 ) ) {
		return false;
	}

	maxWidth = width;
	maxHeight = height;
	lastWidth = lastHeight = 0;
	accumFrames = 0;
	return true;
}

void idPathTracerBackendOpenCL::FreeDenoiser() {
	Release( denoiser.radiance );
	Release( denoiser.scratch );
	Release( denoiser.filteredDiffuse );
	Release( denoiser.filteredSpecular );
	for ( int i = 0; i < 2; i++ ) {
		Release( denoiser.guides[i] );
	}
	for ( int i = 0; i < 3; i++ ) {
		Release( denoiser.signals[i].radiance );
		for ( int slot = 0; slot < 2; slot++ ) {
			Release( denoiser.signals[i].history[slot] );
			Release( denoiser.signals[i].moments[slot] );
		}
	}
	Release( d_filtered );
	memset( &denoiser, 0, sizeof( denoiser ) );
	denoiseWidth = denoiseHeight = 0;
	denoiseFrameSlot = 0;
	denoiseHistoryValid = false;
}

bool idPathTracerBackendOpenCL::ResizeDenoiser( int width, int height ) {
	if ( width == denoiseWidth && height == denoiseHeight ) return true;
	PT_CL_CHECK( clFinish( queue ) );
	FreeDenoiser();
	const size_t pixels = (size_t)width * height;
	const size_t colorBytes = pixels * 4 * sizeof( float );
	if ( !Allocate( denoiser.radiance, colorBytes ) || !Allocate( denoiser.scratch, colorBytes )
		|| !Allocate( denoiser.filteredDiffuse, colorBytes ) || !Allocate( denoiser.filteredSpecular, colorBytes )
		|| !Allocate( d_filtered, colorBytes ) ) {
		return false;
	}
	for ( int i = 0; i < 2; i++ ) {
		if ( !Allocate( denoiser.guides[i], pixels * sizeof( ptClDenoiseGuide_t ) ) ) return false;
	}
	for ( int i = 0; i < 3; i++ ) {
		if ( !Allocate( denoiser.signals[i].radiance, colorBytes ) ) return false;
		for ( int slot = 0; slot < 2; slot++ ) {
			if ( !Allocate( denoiser.signals[i].history[slot], colorBytes )
				|| !Allocate( denoiser.signals[i].moments[slot], colorBytes ) ) {
				return false;
			}
		}
	}
	denoiseWidth = width;
	denoiseHeight = height;
	return true;
}

void idPathTracerBackendOpenCL::FreePostprocess() {
	Release( postprocess.highlights );
	Release( postprocess.scratch );
	Release( postprocess.blurred );
	Release( postprocess.output );
	memset( &postprocess, 0, sizeof( postprocess ) );
	postWidth = postHeight = postScale = 0;
}

bool idPathTracerBackendOpenCL::ResizePostprocess( int width, int height, float radius ) {
	const int scale = PT_ClPostprocessScale( height, radius );
	if ( width == postWidth && height == postHeight && scale == postScale ) return true;
	PT_CL_CHECK( clFinish( queue ) );
	FreePostprocess();
	const size_t smallBytes = (size_t)( ( width + scale - 1 ) / scale ) * ( ( height + scale - 1 ) / scale ) * 4 * sizeof( float );
	if ( !Allocate( postprocess.highlights, smallBytes ) || !Allocate( postprocess.scratch, smallBytes )
		|| !Allocate( postprocess.blurred, smallBytes ) || !Allocate( postprocess.output, (size_t)width * height * 4 * sizeof( float ) ) ) {
		return false;
	}
	postWidth = width;
	postHeight = height;
	postScale = scale;
	return true;
}

/*
========================
idPathTracerBackendOpenCL::Reserve
========================
*/
bool idPathTracerBackendOpenCL::Reserve( cl_mem &buffer, int &capacity, int needed, size_t elementSize, const char *what ) {
	if ( needed <= capacity ) {
		return true;
	}
	const int newCapacity = Max( needed, capacity + capacity / 2 );

	Release( buffer );
	capacity = 0;

	cl_int err = CL_SUCCESS;
	buffer = clCreateBuffer( context, CL_MEM_READ_WRITE, (size_t)newCapacity * elementSize, NULL, &err );
	if ( err != CL_SUCCESS ) {
		buffer = NULL;
		common->Warning( "OpenCL path tracer: failed to allocate %d %s (%.1f MB): %s\n",
			newCapacity, what, (double)newCapacity * elementSize / ( 1024.0 * 1024.0 ), PT_CL_ErrorString( err ) );
		healthy = false;
		return false;
	}
	capacity = newCapacity;
	return true;
}

/*
========================
idPathTracerBackendOpenCL::FreeTextures
========================
*/
void idPathTracerBackendOpenCL::FreeTextures() {
	Release( d_texPool );
	texPoolCapacity = texPoolUsed = 0;
	memset( h_texDesc, 0, sizeof( h_texDesc ) );
	memset( h_texCapacity, 0, sizeof( h_texCapacity ) );
	numTextures = 0;
	textureBytes = 0;
}

/*
========================
idPathTracerBackendOpenCL::ReserveTexturePool

Grows the texel pool to hold at least 'texels', preserving its contents.
Returns false (not fatal) when the device can not hold a bigger pool.
========================
*/
bool idPathTracerBackendOpenCL::ReserveTexturePool( size_t texels ) {
	if ( texels <= texPoolCapacity ) {
		return true;
	}
	// texel offsets are 32 bit
	const size_t maxTexels = Min( (size_t)( maxAllocation / 4 ), (size_t)0xFFFFFFFFu );
	if ( texels > maxTexels ) {
		return false;
	}
	size_t capacity = Max( texels, texPoolCapacity + texPoolCapacity / 2 );
	capacity = Max( capacity, (size_t)( 16 * 1024 * 1024 ) );		// 64 MB minimum, avoids early regrowth
	capacity = Min( capacity, maxTexels );

	cl_int err = CL_SUCCESS;
	cl_mem pool = clCreateBuffer( context, CL_MEM_READ_ONLY, capacity * 4, NULL, &err );
	if ( err != CL_SUCCESS && capacity > texels ) {
		capacity = texels;
		pool = clCreateBuffer( context, CL_MEM_READ_ONLY, capacity * 4, NULL, &err );
	}
	if ( err != CL_SUCCESS ) {
		return false;
	}
	if ( d_texPool && texPoolUsed > 0 ) {
		// in-order queue: kernels already enqueued keep the old pool alive until they finish
		err = clEnqueueCopyBuffer( queue, d_texPool, pool, 0, 0, texPoolUsed * 4, 0, NULL, NULL );
		if ( err != CL_SUCCESS ) {
			clReleaseMemObject( pool );
			OnError( "clEnqueueCopyBuffer", err );
			return false;
		}
	}
	Release( d_texPool );
	d_texPool = pool;
	texPoolCapacity = capacity;
	return true;
}

/*
========================
idPathTracerBackendOpenCL::UploadTextures
========================
*/
bool idPathTracerBackendOpenCL::UploadTextures( const idPathTracerScene &scene ) {
	if ( scene.TextureGeneration() != textureGeneration ) {
		clFinish( queue );	// kernels may still sample the old textures
		FreeTextures();
		textureGeneration = scene.TextureGeneration();
	}

	const idList<ptTextureUpload_t> &pending = scene.PendingTextures();
	for ( int i = 0; i < pending.Num(); i++ ) {
		const ptTextureUpload_t &up = pending[i];
		if ( up.index < 0 || up.index >= PT_MAX_TEXTURES || !up.rgba ) {
			continue;
		}
		ptClTexture_t &desc = h_texDesc[up.index];
		const size_t texels = (size_t)up.width * up.height;

		if ( desc.width > 0 ) {
			textureBytes -= (size_t)desc.width * desc.height * 4;
			desc.width = desc.height = 0;
		}
		if ( h_texCapacity[up.index] < texels ) {
			// a resized slot moves to the end of the pool, the old slice is reclaimed with the next generation
			if ( !ReserveTexturePool( texPoolUsed + texels ) ) {
				if ( !healthy ) return false;
				// not fatal, the sampler returns white for missing textures
				common->Warning( "OpenCL path tracer: texture %d (%dx%d) allocation failed\n", up.index, up.width, up.height );
				h_texCapacity[up.index] = 0;
				continue;
			}
			desc.offset = (unsigned int)texPoolUsed;
			h_texCapacity[up.index] = texels;
			texPoolUsed += texels;
		}
		PT_CL_CHECK( clEnqueueWriteBuffer( queue, d_texPool, CL_TRUE, (size_t)desc.offset * 4, texels * 4, up.rgba, 0, NULL, NULL ) );
		desc.width = up.width;
		desc.height = up.height;
		textureBytes += texels * 4;
	}

	numTextures = Min( scene.Textures().Num(), PT_MAX_TEXTURES );
	if ( pending.Num() > 0 || numTextures == 0 ) {
		// the whole table, so flushed slots are cleared on the device as well
		PT_CL_CHECK( clEnqueueWriteBuffer( queue, d_texDesc, CL_TRUE, 0, sizeof( h_texDesc ), h_texDesc, 0, NULL, NULL ) );
	}
	return true;
}

/*
========================
idPathTracerBackendOpenCL::BuildBVH
========================
*/
bool idPathTracerBackendOpenCL::BuildBVH( const idPathTracerScene &scene ) {
	const int n = numTriangles;
	numBVHNodes = 0;
	if ( n == 0 ) {
		return true;
	}

	if ( !Reserve( d_bvhNodes, capBVHNodes, 2 * n - 1, sizeof( ptBVHNode_t ), "BVH nodes" ) ||
		 !Reserve( d_triIndices, capTriIndices, n, sizeof( int ), "BVH indices" ) ||
		 !Reserve( d_mortonCodes, capMorton, n, sizeof( unsigned int ), "morton codes" ) ||
		 !Reserve( d_mortonAlt, capMortonAlt, n, sizeof( unsigned int ), "morton codes" ) ||
		 !Reserve( d_sortedIndices, capSorted, n, sizeof( int ), "sort indices" ) ||
		 !Reserve( d_sortTemp, capSortTemp, Max( (int)PT_CL_LBVHSortTempBytes( program, n ), 4 ), 1, "sort scratch bytes" ) ||
		 !Reserve( d_parents, capParents, 2 * n, sizeof( int ), "BVH parents" ) ||
		 !Reserve( d_atomics, capAtomics, n, sizeof( int ), "BVH counters" ) ) {
		return false;
	}

	float bounds[6];
	memcpy( bounds, scene.Bounds(), sizeof( bounds ) );
	for ( int a = 0; a < 3; a++ ) {
		if ( bounds[a + 3] - bounds[a] < 0.01f ) {
			bounds[a] -= 0.5f;
			bounds[a + 3] += 0.5f;
		}
	}

	PT_CL_CHECK( PT_CL_LaunchBuildLBVH( program, queue, d_triangles, n, d_bvhNodes, d_triIndices, d_mortonCodes, d_mortonAlt,
		d_sortedIndices, d_sortTemp, d_parents, d_atomics, bounds ) );

	numBVHNodes = 2 * n - 1;
	return true;
}

/*
========================
idPathTracerBackendOpenCL::UploadScene
========================
*/
bool idPathTracerBackendOpenCL::UploadScene( const idPathTracerScene &scene ) {
	if ( !healthy ) {
		return false;
	}
	if ( scene.TextureGeneration() != textureGeneration ) denoiseHistoryValid = false;

	numVertices = scene.Vertices().Num();
	numTriangles = scene.Triangles().Num();
	numMaterials = scene.Materials().Num();
	numLights = scene.Lights().Num();

	if ( !Reserve( d_vertices, capVertices, Max( numVertices, 1 ), sizeof( ptVertex_t ), "vertices" ) ||
		 !Reserve( d_triangles, capTriangles, Max( numTriangles, 1 ), sizeof( ptTriangle_t ), "triangles" ) ||
		 !Reserve( d_materials, capMaterials, Max( numMaterials, 1 ), sizeof( ptMaterial_t ), "materials" ) ||
		 !Reserve( d_lights, capLights, Max( numLights, 1 ), sizeof( ptLight_t ), "lights" ) ) {
		numVertices = numTriangles = numMaterials = numLights = 0;
		return false;
	}

	if ( numVertices > 0 ) {
		PT_CL_CHECK( clEnqueueWriteBuffer( queue, d_vertices, CL_TRUE, 0, numVertices * sizeof( ptVertex_t ), scene.Vertices().Ptr(), 0, NULL, NULL ) );
	}
	if ( numTriangles > 0 ) {
		PT_CL_CHECK( clEnqueueWriteBuffer( queue, d_triangles, CL_TRUE, 0, numTriangles * sizeof( ptTriangle_t ), scene.Triangles().Ptr(), 0, NULL, NULL ) );
	}
	if ( numMaterials > 0 ) {
		PT_CL_CHECK( clEnqueueWriteBuffer( queue, d_materials, CL_TRUE, 0, numMaterials * sizeof( ptMaterial_t ), scene.Materials().Ptr(), 0, NULL, NULL ) );
	}
	if ( numLights > 0 ) {
		PT_CL_CHECK( clEnqueueWriteBuffer( queue, d_lights, CL_TRUE, 0, numLights * sizeof( ptLight_t ), scene.Lights().Ptr(), 0, NULL, NULL ) );
	}

	if ( !UploadTextures( scene ) ) {
		return false;
	}
	return BuildBVH( scene );
}

/*
========================
idPathTracerBackendOpenCL::ElapsedMs

GPU time between the end of event firstEvent - 1 (or the start of event 0)
and the end of event endEvent - 1: the same spans the CUDA backend measures
with events recorded between its stages.
========================
*/
float idPathTracerBackendOpenCL::ElapsedMs( size_t firstEvent, size_t endEvent ) const {
	if ( endEvent <= firstEvent || endEvent > (size_t)events.Num() ) {
		return 0.0f;
	}
	cl_ulong begin = 0, end = 0;
	if ( firstEvent == 0 ) {
		if ( clGetEventProfilingInfo( events[0], CL_PROFILING_COMMAND_START, sizeof( begin ), &begin, NULL ) != CL_SUCCESS ) return 0.0f;
	} else if ( clGetEventProfilingInfo( events[firstEvent - 1], CL_PROFILING_COMMAND_END, sizeof( begin ), &begin, NULL ) != CL_SUCCESS ) {
		return 0.0f;
	}
	if ( clGetEventProfilingInfo( events[endEvent - 1], CL_PROFILING_COMMAND_END, sizeof( end ), &end, NULL ) != CL_SUCCESS ) return 0.0f;
	return end > begin ? (float)( ( end - begin ) * 1.0e-6 ) : 0.0f;
}

void idPathTracerBackendOpenCL::ReleaseEvents() {
	for ( int i = 0; i < events.Num(); i++ ) {
		if ( events[i] ) clReleaseEvent( events[i] );
	}
	events.Clear();
	program.events = NULL;
}

/*
========================
idPathTracerBackendOpenCL::Render
========================
*/
bool idPathTracerBackendOpenCL::Render( const ptRenderParams_t &params ) {
	if ( !healthy || !d_accum || !d_output ) {
		return false;
	}

	ptRenderParams_t p = params;
	p.width = idMath::ClampInt( 1, maxWidth, p.width );
	p.height = idMath::ClampInt( 1, maxHeight, p.height );

	if ( p.width != lastWidth || p.height != lastHeight ) {
		accumFrames = 0;
		denoiseHistoryValid = false;
	}
	lastWidth = p.width;
	lastHeight = p.height;

	const size_t pixels = (size_t)p.width * p.height;

	if ( numTriangles == 0 ) {
		accumFrames = 0;
		denoiseHistoryValid = false;
		PT_CL_CHECK( PT_CL_Fill( program, queue, d_output, 0, pixels * 4 ) );
		PT_CL_CHECK( clFinish( queue ) );
		return true;
	}

	// debug modes overwrite the buffer every frame, only path tracing accumulates
	const bool accumulate = ( p.renderMode == PT_MODE_PATHTRACE );
	if ( accumulate && p.captureTargetFrames > 0 && accumFrames >= p.captureTargetFrames ) return true;
	if ( !accumulate ) {
		accumFrames = 0;
	}
	if ( accumFrames == 0 ) {
		PT_CL_CHECK( PT_CL_Fill( program, queue, d_accum, 0, pixels * 4 * sizeof( float ) ) );
	}
	p.frameIndex = accumFrames;
	p.sampleIndex = sampleSequence++;
	p.denoiseStrength = idMath::ClampFloat( 0.05f, 4.0f, p.denoiseStrength );

	ptClSceneView_t view;
	view.vertices = d_vertices;
	view.triangles = d_triangles;
	view.bvhNodes = d_bvhNodes;
	view.triIndices = d_triIndices;
	view.materials = d_materials;
	view.textureDesc = d_texDesc;
	view.texels = d_texPool;
	view.lights = d_lights;
	view.numLights = numLights;

	// everything enqueued from here on is timed (CUDA: timerStart)
	ReleaseEvents();
	program.events = &events;

	const bool denoise = accumulate && ( p.denoiseEnabled != 0 || p.denoiseDebug != 0 );
	if ( denoise ) {
		if ( !ResizeDenoiser( p.width, p.height ) ) { ReleaseEvents(); return false; }
		float forwardDot = 0.0f, cameraDistanceSquared = 0.0f;
		for ( int c = 0; c < 3; c++ ) {
			forwardDot += p.camForward[c] * previousDenoiseParams.camForward[c];
			const float delta = p.camPos[c] - previousDenoiseParams.camPos[c];
			cameraDistanceSquared += delta * delta;
		}
		if ( forwardDot < 0.5f || cameraDistanceSquared > 256.0f * 256.0f ) denoiseHistoryValid = false;
	} else {
		denoiseHistoryValid = false;
	}
	cl_int err = PT_CL_LaunchPathTracing( program, queue, view, d_accum, denoise ? denoiser.radiance : NULL,
		denoise ? denoiser.guides[denoiseFrameSlot] : NULL,
		denoise ? denoiser.signals[0].radiance : NULL, denoise ? denoiser.signals[1].radiance : NULL,
		denoise ? denoiser.signals[2].radiance : NULL, p );
	if ( err != CL_SUCCESS ) { ReleaseEvents(); OnError( "PT_CL_LaunchPathTracing", err ); return false; }

	const size_t markPath = (size_t)events.Num();		// CUDA: timerPath
	accumFrames++;
	cl_mem hdrBuffer = d_accum;
	if ( denoise ) {
		err = PT_CL_LaunchDenoising( program, queue, denoiser, d_accum, d_filtered, p, previousDenoiseParams,
			denoiseFrameSlot, denoiseHistoryValid ? 1 : 0 );
		if ( err != CL_SUCCESS ) { ReleaseEvents(); OnError( "PT_CL_LaunchDenoising", err ); return false; }
		hdrBuffer = d_filtered;
	}
	const size_t markDenoise = (size_t)events.Num();	// CUDA: timerDenoise
	size_t markPostprocess = markDenoise;		// CUDA: timerPostprocess
	const bool hasPostprocess = accumulate && p.denoiseDebug == 0 && ( p.bloomStrength > 0.0f || p.lensFlareStrength > 0.0f );
	if ( denoise && p.denoiseDebug >= 1 && p.denoiseDebug <= 4 ) {
		err = PT_CL_LaunchDenoisingDebug( program, queue, denoiser, d_output, p, denoiseFrameSlot );
		if ( err != CL_SUCCESS ) { ReleaseEvents(); OnError( "PT_CL_LaunchDenoisingDebug", err ); return false; }
	} else {
		if ( hasPostprocess ) {
			if ( !ResizePostprocess( p.width, p.height, p.bloomRadius ) ) { ReleaseEvents(); return false; }
			err = PT_CL_LaunchPostprocess( program, queue, postprocess, hdrBuffer, p );
			if ( err != CL_SUCCESS ) { ReleaseEvents(); OnError( "PT_CL_LaunchPostprocess", err ); return false; }
			hdrBuffer = postprocess.output;
		}
		markPostprocess = (size_t)events.Num();
		err = PT_CL_LaunchToneMapping( program, queue, hdrBuffer, d_output, p.width, p.height, p.exposure, p.gamma, p.toneMapMode );
		if ( err != CL_SUCCESS ) { ReleaseEvents(); OnError( "PT_CL_LaunchToneMapping", err ); return false; }
	}

	err = clFinish( queue );
	if ( err != CL_SUCCESS ) { ReleaseEvents(); OnError( "clFinish", err ); return false; }
	const size_t markEnd = (size_t)events.Num();		// CUDA: timerStop
	traceMs = ElapsedMs( 0, markEnd );
	pathMs = ElapsedMs( 0, markPath );
	denoiseMs = ElapsedMs( markPath, markDenoise );
	postprocessMs = ElapsedMs( markDenoise, markPostprocess );
	toneMapMs = ElapsedMs( markPostprocess, markEnd );
	ReleaseEvents();
	if ( !denoise ) denoiseMs = 0;
	if ( !hasPostprocess ) postprocessMs = 0;
	if ( denoise ) {
		previousDenoiseParams = p;
		denoiseHistoryValid = true;
		denoiseFrameSlot ^= 1;
	}
	return true;
}

/*
========================
idPathTracerBackendOpenCL::ReadPixels
========================
*/
bool idPathTracerBackendOpenCL::ReadPixels( unsigned char *dest, int width, int height ) {
	if ( !healthy || !dest || width != lastWidth || height != lastHeight ) {
		return false;
	}
	PT_CL_CHECK( clEnqueueReadBuffer( queue, d_output, CL_TRUE, 0, (size_t)width * height * 4, dest, 0, NULL, NULL ) );
	return true;
}

/*
========================
idPathTracerBackendOpenCL::GetStats
========================
*/
void idPathTracerBackendOpenCL::GetStats( ptBackendStats_t &stats ) const {
	stats.traceMs = traceMs;
	stats.pathMs = pathMs;
	stats.denoiseMs = denoiseMs;
	stats.postprocessMs = postprocessMs;
	stats.toneMapMs = toneMapMs;
	stats.numVertices = numVertices;
	stats.numTriangles = numTriangles;
	stats.numBVHNodes = numBVHNodes;
	stats.numMaterials = numMaterials;
	stats.numLights = numLights;
	stats.numTextures = numTextures;
	stats.deviceBytes = texPoolCapacity * 4
		+ (size_t)maxWidth * maxHeight * 4 * ( sizeof( float ) + 1 )
		+ (size_t)denoiseWidth * denoiseHeight * ( 20 * 4 * sizeof( float ) + 2 * sizeof( ptClDenoiseGuide_t ) )
		+ (size_t)postWidth * postHeight * 4 * sizeof( float )
		+ ( postScale > 0 ? (size_t)( ( postWidth + postScale - 1 ) / postScale ) *
			( ( postHeight + postScale - 1 ) / postScale ) * 3 * 4 * sizeof( float ) : 0 )
		+ (size_t)capVertices * sizeof( ptVertex_t )
		+ (size_t)capTriangles * sizeof( ptTriangle_t )
		+ (size_t)capBVHNodes * sizeof( ptBVHNode_t )
		+ (size_t)( capTriIndices + capSorted + capParents + capAtomics ) * sizeof( int )
		+ (size_t)( capMorton + capMortonAlt ) * sizeof( unsigned int )
		+ (size_t)capSortTemp
		+ (size_t)capMaterials * sizeof( ptMaterial_t )
		+ (size_t)capLights * sizeof( ptLight_t )
		+ PT_MAX_TEXTURES * sizeof( ptClTexture_t );
}

/*
========================
Factory
========================
*/
bool PT_OpenCL_IsAvailable() {
	return idPathTracerBackendOpenCL::IsAvailable();
}

#ifndef CL_DEVICE_PCI_BUS_INFO_KHR
#define CL_DEVICE_PCI_BUS_INFO_KHR	0x410F
typedef struct {
	cl_uint pci_domain;
	cl_uint pci_bus;
	cl_uint pci_device;
	cl_uint pci_function;
} cl_device_pci_bus_info_khr;
#endif
#define PT_CL_DEVICE_PCI_BUS_ID_NV		0x4008
#define PT_CL_DEVICE_PCI_SLOT_ID_NV		0x4009
#define PT_CL_DEVICE_PCI_DOMAIN_ID_NV	0x400A
#define PT_CL_DEVICE_TOPOLOGY_AMD		0x4037
#define PT_CL_DEVICE_TOPOLOGY_TYPE_PCIE_AMD	1

// cl_device_topology_amd from cl_ext.h (cl_amd_device_attribute_query)
typedef union {
	struct { cl_uint type; cl_uint data[5]; } raw;
	struct { cl_uint type; cl_char unused[17]; cl_char bus; cl_char device; cl_char function; } pcie;
} ptClDeviceTopologyAmd_t;

/*
========================
PT_OpenCL_GetDeviceInfo

PCI bus ids use the domain:bus:device.function form, so the capture
report's GPU telemetry (NVML, Linux sysfs, Windows D3DKMT) samples the
device driven through OpenCL.
========================
*/
bool PT_OpenCL_GetDeviceInfo( ptBackendDeviceInfo_t &info ) {
	memset( &info, 0, sizeof( info ) );
	ptClDeviceChoice_t device;
	if ( !PT_CL_SelectDevice( device ) ) return false;
	const cl_device_id id = device.device;
	idStr::Copynz( info.api, "opencl", sizeof( info.api ) );
	idStr::Copynz( info.name, PT_CL_DeviceString( id, CL_DEVICE_NAME ).c_str(), sizeof( info.name ) );
	idStr::Copynz( info.driverString, PT_CL_DeviceString( id, CL_DRIVER_VERSION ).c_str(), sizeof( info.driverString ) );
	idStr::Copynz( info.apiVersion, PT_CL_DeviceString( id, CL_DEVICE_VERSION ).c_str(), sizeof( info.apiVersion ) );
	cl_ulong memory = 0;
	cl_uint units = 0, vendor = 0;
	clGetDeviceInfo( id, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof( memory ), &memory, NULL );
	clGetDeviceInfo( id, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof( units ), &units, NULL );
	// the PCI vendor id for GPUs on AMD, Intel and NVIDIA platforms
	if ( clGetDeviceInfo( id, CL_DEVICE_VENDOR_ID, sizeof( vendor ), &vendor, NULL ) == CL_SUCCESS && vendor <= 0xffff ) info.vendorId = vendor;
	info.totalMemory = (size_t)memory;
	info.multiprocessors = (int)units;
	int major = 0, minor = 0;
	if ( sscanf( info.apiVersion, "OpenCL %d.%d", &major, &minor ) == 2 ) info.runtimeVersion = major * 1000 + minor * 10;
	const idStr language = PT_CL_DeviceString( id, CL_DEVICE_OPENCL_C_VERSION );
	sscanf( language.c_str(), "OpenCL C %d.%d", &info.computeMajor, &info.computeMinor );
	info.driverVersion = atoi( info.driverString );

	const idStr extensions = PT_CL_DeviceString( id, CL_DEVICE_EXTENSIONS );
	cl_device_pci_bus_info_khr pci;
	if ( extensions.Find( "cl_khr_pci_bus_info" ) >= 0
		&& clGetDeviceInfo( id, CL_DEVICE_PCI_BUS_INFO_KHR, sizeof( pci ), &pci, NULL ) == CL_SUCCESS ) {
		idStr::snPrintf( info.pciBusId, sizeof( info.pciBusId ), "%08x:%02x:%02x.%x",
			pci.pci_domain, pci.pci_bus, pci.pci_device, pci.pci_function );
	} else if ( extensions.Find( "cl_nv_device_attribute_query" ) >= 0 ) {
		cl_uint bus = 0, slot = 0, domain = 0;
		if ( clGetDeviceInfo( id, PT_CL_DEVICE_PCI_BUS_ID_NV, sizeof( bus ), &bus, NULL ) == CL_SUCCESS
			&& clGetDeviceInfo( id, PT_CL_DEVICE_PCI_SLOT_ID_NV, sizeof( slot ), &slot, NULL ) == CL_SUCCESS ) {
			clGetDeviceInfo( id, PT_CL_DEVICE_PCI_DOMAIN_ID_NV, sizeof( domain ), &domain, NULL );
			idStr::snPrintf( info.pciBusId, sizeof( info.pciBusId ), "%08x:%02x:%02x.%x", domain, bus, slot >> 3, slot & 7 );
		}
	} else if ( extensions.Find( "cl_amd_device_attribute_query" ) >= 0 ) {
		ptClDeviceTopologyAmd_t topology;
		memset( &topology, 0, sizeof( topology ) );
		if ( clGetDeviceInfo( id, PT_CL_DEVICE_TOPOLOGY_AMD, sizeof( topology ), &topology, NULL ) == CL_SUCCESS
			&& topology.raw.type == PT_CL_DEVICE_TOPOLOGY_TYPE_PCIE_AMD ) {
			idStr::snPrintf( info.pciBusId, sizeof( info.pciBusId ), "%08x:%02x:%02x.%x", 0,
				(unsigned char)topology.pcie.bus, (unsigned char)topology.pcie.device, (unsigned char)topology.pcie.function );
		}
	}
	return true;
}

idPathTracerBackend *PT_OpenCL_CreateBackend() {
	return new idPathTracerBackendOpenCL();
}
