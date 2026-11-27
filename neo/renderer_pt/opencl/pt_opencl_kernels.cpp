/*
===============================================================================

	OpenCL launch layer: program build plus the host halves of
	pt_cuda_kernels.cu, pt_cuda_postprocess.cuh and pt_cuda_lbvh.cu.

	Every launch sequence matches the CUDA one kernel for kernel, so both
	backends run the same passes on the same buffers in the same order.

===============================================================================
*/

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "renderer_pt/opencl/pt_opencl_device.h"
#include "idlib/math/Math.h"

// generated at build time from the pt_cl_*.cl sources
#include "pt_opencl_program.h"

static const char *ptClKernelNames[PT_CL_KERNEL_COUNT] = {
	"PathTracingKernel",
	"TriangleDrawKernel",
	"TriangleDrawTexturedKernel",
	"TriangleDrawRayTracedKernel",
	"ToneMappingKernel",
	"SVGFCaptureGuidesKernel",
	"SVGFCaptureKernel",
	"SVGFTemporalKernel",
	"SVGFBootstrapVolumeVarianceKernel",
	"SVGFAtrousKernel",
	"SVGFStoreSurfaceKernel",
	"SVGFResolveKernel",
	"SVGFDenoiseDebugKernel",
	"PT_PostHighlights",
	"PT_PostBlur",
	"PT_PostComposite",
	"ComputeMortonCodesKernel",
	"PT_RadixHistogram",
	"PT_RadixScan",
	"PT_RadixScatter",
	"BuildRadixTreeKernel",
	"InitLeafNodesKernel",
	"BuildEscapeLinksKernel",
	"ComputeNodeBoundsKernel",
	"InitSingleLeafKernel",
	"PT_LayoutKernel"
};

/*
========================
PT_CL_ErrorString
========================
*/
const char *PT_CL_ErrorString( cl_int error ) {
	switch ( error ) {
		case CL_SUCCESS: return "CL_SUCCESS";
		case CL_DEVICE_NOT_FOUND: return "CL_DEVICE_NOT_FOUND";
		case CL_DEVICE_NOT_AVAILABLE: return "CL_DEVICE_NOT_AVAILABLE";
		case CL_COMPILER_NOT_AVAILABLE: return "CL_COMPILER_NOT_AVAILABLE";
		case CL_MEM_OBJECT_ALLOCATION_FAILURE: return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
		case CL_OUT_OF_RESOURCES: return "CL_OUT_OF_RESOURCES";
		case CL_OUT_OF_HOST_MEMORY: return "CL_OUT_OF_HOST_MEMORY";
		case CL_PROFILING_INFO_NOT_AVAILABLE: return "CL_PROFILING_INFO_NOT_AVAILABLE";
		case CL_MEM_COPY_OVERLAP: return "CL_MEM_COPY_OVERLAP";
		case CL_BUILD_PROGRAM_FAILURE: return "CL_BUILD_PROGRAM_FAILURE";
		case CL_MAP_FAILURE: return "CL_MAP_FAILURE";
		case CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST: return "CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST";
		case CL_INVALID_VALUE: return "CL_INVALID_VALUE";
		case CL_INVALID_DEVICE_TYPE: return "CL_INVALID_DEVICE_TYPE";
		case CL_INVALID_PLATFORM: return "CL_INVALID_PLATFORM";
		case CL_INVALID_DEVICE: return "CL_INVALID_DEVICE";
		case CL_INVALID_CONTEXT: return "CL_INVALID_CONTEXT";
		case CL_INVALID_QUEUE_PROPERTIES: return "CL_INVALID_QUEUE_PROPERTIES";
		case CL_INVALID_COMMAND_QUEUE: return "CL_INVALID_COMMAND_QUEUE";
		case CL_INVALID_HOST_PTR: return "CL_INVALID_HOST_PTR";
		case CL_INVALID_MEM_OBJECT: return "CL_INVALID_MEM_OBJECT";
		case CL_INVALID_BINARY: return "CL_INVALID_BINARY";
		case CL_INVALID_BUILD_OPTIONS: return "CL_INVALID_BUILD_OPTIONS";
		case CL_INVALID_PROGRAM: return "CL_INVALID_PROGRAM";
		case CL_INVALID_PROGRAM_EXECUTABLE: return "CL_INVALID_PROGRAM_EXECUTABLE";
		case CL_INVALID_KERNEL_NAME: return "CL_INVALID_KERNEL_NAME";
		case CL_INVALID_KERNEL_DEFINITION: return "CL_INVALID_KERNEL_DEFINITION";
		case CL_INVALID_KERNEL: return "CL_INVALID_KERNEL";
		case CL_INVALID_ARG_INDEX: return "CL_INVALID_ARG_INDEX";
		case CL_INVALID_ARG_VALUE: return "CL_INVALID_ARG_VALUE";
		case CL_INVALID_ARG_SIZE: return "CL_INVALID_ARG_SIZE";
		case CL_INVALID_KERNEL_ARGS: return "CL_INVALID_KERNEL_ARGS";
		case CL_INVALID_WORK_DIMENSION: return "CL_INVALID_WORK_DIMENSION";
		case CL_INVALID_WORK_GROUP_SIZE: return "CL_INVALID_WORK_GROUP_SIZE";
		case CL_INVALID_WORK_ITEM_SIZE: return "CL_INVALID_WORK_ITEM_SIZE";
		case CL_INVALID_GLOBAL_OFFSET: return "CL_INVALID_GLOBAL_OFFSET";
		case CL_INVALID_EVENT_WAIT_LIST: return "CL_INVALID_EVENT_WAIT_LIST";
		case CL_INVALID_EVENT: return "CL_INVALID_EVENT";
		case CL_INVALID_OPERATION: return "CL_INVALID_OPERATION";
		case CL_INVALID_BUFFER_SIZE: return "CL_INVALID_BUFFER_SIZE";
		case CL_INVALID_GLOBAL_WORK_SIZE: return "CL_INVALID_GLOBAL_WORK_SIZE";
		default: return "unknown OpenCL error";
	}
}

/*
========================
PT_CL_ProgramSource
========================
*/
const char *PT_CL_ProgramSource() {
	return ptOpenCLProgram;
}

static idStr PT_CL_DeviceString( cl_device_id device, cl_device_info param ) {
	size_t size = 0;
	if ( clGetDeviceInfo( device, param, 0, NULL, &size ) != CL_SUCCESS || size == 0 ) return "";
	idList<char> value;
	value.SetNum( (int)size + 1 );
	value[(int)size] = '\0';
	if ( clGetDeviceInfo( device, param, size, value.Ptr(), NULL ) != CL_SUCCESS ) return "";
	return value.Ptr();	// stops at the terminating NUL
}

static size_t PT_CL_SortWorkGroup( cl_device_id device ) {
	size_t maxGroup = 256;
	clGetDeviceInfo( device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof( maxGroup ), &maxGroup, NULL );
	size_t group = 256;
	while ( group > 16 && group > maxGroup ) group >>= 1;
	return group;
}

#define PT_CL_INTEL_LARGE_GRF	"-cl-intel-256-GRF-per-thread"

static bool PT_CL_IsIntel( cl_device_id device ) {
	const idStr vendor = PT_CL_DeviceString( device, CL_DEVICE_VENDOR );
	return vendor.Find( "Intel" ) >= 0 || vendor.Find( "intel" ) >= 0;
}

/*
========================
PT_CL_BuildOptions

-cl-fp32-correctly-rounded-divide-sqrt matches nvcc's default IEEE division
and square root (-prec-div=true -prec-sqrt=true). No fast math, exactly like
the CUDA build.
========================
*/
idStr PT_CL_BuildOptions( cl_device_id device ) {
	idStr options = "-cl-std=CL1.2";
	char group[64];
	idStr::snPrintf( group, sizeof( group ), " -DPT_CL_SORT_WG=%u", (unsigned int)PT_CL_SortWorkGroup( device ) );
	options += group;
#ifdef PT_PHYSICAL
	options += " -DPT_PHYSICAL";
#endif
	cl_device_fp_config fp = 0;
	if ( clGetDeviceInfo( device, CL_DEVICE_SINGLE_FP_CONFIG, sizeof( fp ), &fp, NULL ) == CL_SUCCESS
		&& ( fp & CL_FP_CORRECTLY_ROUNDED_DIVIDE_SQRT ) ) {
		options += " -cl-fp32-correctly-rounded-divide-sqrt";
	}
	if ( PT_CL_IsIntel( device ) ) {
		// The path tracing kernel spills heavily in Intel's default 128 GRF mode, and
		// that configuration faulted nondeterministically (CL_OUT_OF_RESOURCES, about
		// one launch in a hundred on identical inputs) with compute-runtime 26.22.
		// 256 GRF avoids the spills; PT_CL_CreateProgram retries without it if the
		// driver rejects the option.
		options += " " PT_CL_INTEL_LARGE_GRF;
	}
	return options;
}

static unsigned long long PT_CL_Hash( unsigned long long hash, const idStr &text ) {
	for ( int i = 0; i < text.Length(); i++ ) {
		hash ^= (unsigned char)text[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

idStr PT_CL_ProgramKey( cl_device_id device, const char *extraSource ) {
	unsigned long long hash = 14695981039346656037ULL;
	hash = PT_CL_Hash( hash, PT_CL_ProgramSource() );
	hash = PT_CL_Hash( hash, extraSource ? extraSource : "" );
	hash = PT_CL_Hash( hash, PT_CL_BuildOptions( device ) );
	hash = PT_CL_Hash( hash, PT_CL_DeviceString( device, CL_DEVICE_NAME ) );
	hash = PT_CL_Hash( hash, PT_CL_DeviceString( device, CL_DEVICE_VENDOR ) );
	hash = PT_CL_Hash( hash, PT_CL_DeviceString( device, CL_DRIVER_VERSION ) );
	hash = PT_CL_Hash( hash, PT_CL_DeviceString( device, CL_DEVICE_VERSION ) );
	char key[32];
	idStr::snPrintf( key, sizeof( key ), "%016llx", hash );
	return key;
}

/*
========================
PT_CL_CreateProgram
========================
*/
static cl_int PT_CL_BuildFromSource( ptClProgram_t &prog, const char *extraSource, const idStr &options ) {
	const char *sources[2] = { PT_CL_ProgramSource(), extraSource ? extraSource : "" };
	cl_int error = CL_SUCCESS;
	prog.program = clCreateProgramWithSource( prog.context, 2, sources, NULL, &error );
	if ( error != CL_SUCCESS ) return error;
	return clBuildProgram( prog.program, 1, &prog.device, options.c_str(), NULL, NULL );
}

static void PT_CL_AppendBuildLog( const ptClProgram_t &prog, idStr &log ) {
	if ( !prog.program ) return;
	size_t size = 0;
	if ( clGetProgramBuildInfo( prog.program, prog.device, CL_PROGRAM_BUILD_LOG, 0, NULL, &size ) != CL_SUCCESS || size <= 1 ) return;
	idList<char> buffer;
	buffer.SetNum( (int)size + 1 );
	buffer[(int)size] = '\0';
	if ( clGetProgramBuildInfo( prog.program, prog.device, CL_PROGRAM_BUILD_LOG, size, buffer.Ptr(), NULL ) != CL_SUCCESS ) return;
	idStr text = buffer.Ptr();
	text.StripTrailing( '\n' );
	if ( !text.IsEmpty() ) {
		log += text;
		log += "\n";
	}
}

cl_int PT_CL_CreateProgram( ptClProgram_t &prog, cl_context context, cl_device_id device, const char *extraSource,
							const idList<byte> *cachedBinary, idList<byte> *binaryOut,
							idStr &log ) {
	memset( &prog, 0, sizeof( prog ) );
	prog.context = context;
	prog.device = device;
	prog.sortWorkGroup = PT_CL_SortWorkGroup( device );
	const idStr options = PT_CL_BuildOptions( device );

	cl_int error = CL_INVALID_BINARY;
	if ( cachedBinary && cachedBinary->Num() > 0 ) {
		const size_t size = (size_t)cachedBinary->Num();
		const unsigned char *data = cachedBinary->Ptr();
		cl_int status = CL_SUCCESS;
		prog.program = clCreateProgramWithBinary( context, 1, &device, &size, &data, &status, &error );
		if ( error == CL_SUCCESS && status == CL_SUCCESS ) {
			error = clBuildProgram( prog.program, 1, &device, options.c_str(), NULL, NULL );
		} else if ( error == CL_SUCCESS ) {
			error = status;
		}
		if ( error != CL_SUCCESS && prog.program ) {
			clReleaseProgram( prog.program );
			prog.program = NULL;
		}
	}
	if ( error != CL_SUCCESS ) {
		error = PT_CL_BuildFromSource( prog, extraSource, options );
		const int largeGrf = options.Find( PT_CL_INTEL_LARGE_GRF );
		if ( ( error == CL_INVALID_BUILD_OPTIONS || error == CL_BUILD_PROGRAM_FAILURE ) && largeGrf >= 0 ) {
			// older Intel drivers do not know the large register file option
			if ( prog.program ) clReleaseProgram( prog.program );
			prog.program = NULL;
			const idStr fallback = options.Left( largeGrf ) + options.Mid( largeGrf + idStr::Length( PT_CL_INTEL_LARGE_GRF ),
				options.Length() );
			error = PT_CL_BuildFromSource( prog, extraSource, fallback );
		}
		PT_CL_AppendBuildLog( prog, log );
		if ( error != CL_SUCCESS ) {
			PT_CL_DestroyProgram( prog );
			return error;
		}
		if ( binaryOut ) {
			size_t size = 0;
			binaryOut->Clear();
			if ( clGetProgramInfo( prog.program, CL_PROGRAM_BINARY_SIZES, sizeof( size ), &size, NULL ) == CL_SUCCESS && size > 0 ) {
				binaryOut->SetNum( (int)size );
				unsigned char *data = binaryOut->Ptr();
				if ( clGetProgramInfo( prog.program, CL_PROGRAM_BINARIES, sizeof( data ), &data, NULL ) != CL_SUCCESS ) {
					binaryOut->Clear();
				}
			}
		}
	} else if ( binaryOut ) {
		binaryOut->Clear();
	}

	for ( int i = 0; i < PT_CL_KERNEL_COUNT; i++ ) {
		prog.kernels[i] = clCreateKernel( prog.program, ptClKernelNames[i], &error );
		if ( error != CL_SUCCESS ) {
			log += idStr::Format( "clCreateKernel( %s ) failed\n", ptClKernelNames[i] );
			PT_CL_DestroyProgram( prog );
			return error;
		}
		prog.maxWorkGroup[i] = 0;
		clGetKernelWorkGroupInfo( prog.kernels[i], device, CL_KERNEL_WORK_GROUP_SIZE, sizeof( size_t ), &prog.maxWorkGroup[i], NULL );
		if ( prog.maxWorkGroup[i] == 0 ) prog.maxWorkGroup[i] = 1;
	}
	if ( prog.maxWorkGroup[PT_CL_RADIX_HISTOGRAM] < prog.sortWorkGroup
		|| prog.maxWorkGroup[PT_CL_RADIX_SCAN] < prog.sortWorkGroup
		|| prog.maxWorkGroup[PT_CL_RADIX_SCATTER] < prog.sortWorkGroup ) {
		log += "radix sort kernels can not run their required work-group size\n";
		PT_CL_DestroyProgram( prog );
		return CL_INVALID_WORK_GROUP_SIZE;
	}
	return CL_SUCCESS;
}

void PT_CL_DestroyProgram( ptClProgram_t &prog ) {
	for ( int i = 0; i < PT_CL_KERNEL_COUNT; i++ ) {
		if ( prog.kernels[i] ) clReleaseKernel( prog.kernels[i] );
		prog.kernels[i] = NULL;
	}
	if ( prog.program ) clReleaseProgram( prog.program );
	prog.program = NULL;
}

/*
===============================================================================

	Enqueue helpers

===============================================================================
*/

static cl_event *PT_CL_NextEvent( ptClProgram_t &prog ) {
	if ( !prog.events ) return NULL;
	prog.events->Append( NULL );
	return &( *prog.events )[prog.events->Num() - 1];
}

struct ptClArgs_t {
	cl_kernel	kernel;
	cl_uint		index;
	cl_int		error;

	ptClArgs_t( cl_kernel k ) : kernel( k ), index( 0 ), error( CL_SUCCESS ) {}

	template< class T >
	ptClArgs_t &operator()( const T &value ) {
		const cl_int result = clSetKernelArg( kernel, index++, sizeof( T ), &value );
		if ( error == CL_SUCCESS ) error = result;
		return *this;
	}
};

cl_int PT_CL_Launch2D( ptClProgram_t &prog, cl_command_queue queue, int kernel, int width, int height ) {
	if ( width <= 0 || height <= 0 ) return CL_SUCCESS;
	size_t local[2] = { PT_CL_TILE_SIZE, PT_CL_TILE_SIZE };
	while ( local[0] * local[1] > prog.maxWorkGroup[kernel] && local[0] * local[1] > 1 ) {
		if ( local[1] >= local[0] ) local[1] >>= 1;
		else local[0] >>= 1;
	}
	const size_t global[2] = {
		( ( (size_t)width + local[0] - 1 ) / local[0] ) * local[0],
		( ( (size_t)height + local[1] - 1 ) / local[1] ) * local[1]
	};
	return clEnqueueNDRangeKernel( queue, prog.kernels[kernel], 2, NULL, global, local, 0, NULL, PT_CL_NextEvent( prog ) );
}

static cl_int PT_CL_Launch1D( ptClProgram_t &prog, cl_command_queue queue, int kernel, size_t count, size_t group ) {
	if ( count == 0 ) return CL_SUCCESS;
	if ( group == 0 ) {
		group = 256;
		while ( group > prog.maxWorkGroup[kernel] && group > 1 ) group >>= 1;
	}
	const size_t global = ( ( count + group - 1 ) / group ) * group;
	return clEnqueueNDRangeKernel( queue, prog.kernels[kernel], 1, NULL, &global, &group, 0, NULL, PT_CL_NextEvent( prog ) );
}

cl_int PT_CL_Fill( ptClProgram_t &prog, cl_command_queue queue, cl_mem buffer, unsigned char value, size_t bytes ) {
	if ( bytes == 0 ) return CL_SUCCESS;
	return clEnqueueFillBuffer( queue, buffer, &value, 1, 0, bytes, 0, NULL, PT_CL_NextEvent( prog ) );
}

cl_int PT_CL_Copy( ptClProgram_t &prog, cl_command_queue queue, cl_mem src, cl_mem dst, size_t bytes ) {
	if ( bytes == 0 ) return CL_SUCCESS;
	return clEnqueueCopyBuffer( queue, src, dst, 0, 0, bytes, 0, NULL, PT_CL_NextEvent( prog ) );
}

#define PT_CL_RETURN_ON_ERROR( call ) \
	do { \
		const cl_int _error = ( call ); \
		if ( _error != CL_SUCCESS ) return _error; \
	} while ( 0 )

/*
========================
PT_CL_CheckLayout
========================
*/
cl_int PT_CL_CheckLayout( ptClProgram_t &prog, cl_command_queue queue, idStr &error ) {
	cl_int status = CL_SUCCESS;
	cl_mem buffer = clCreateBuffer( prog.context, CL_MEM_WRITE_ONLY, 8 * sizeof( cl_int ), NULL, &status );
	if ( status != CL_SUCCESS ) return status;
	cl_int sizes[8] = {};
	status = ptClArgs_t( prog.kernels[PT_CL_LAYOUT] )( buffer ).error;
	if ( status == CL_SUCCESS ) status = PT_CL_Launch1D( prog, queue, PT_CL_LAYOUT, 1, 1 );
	if ( status == CL_SUCCESS ) status = clEnqueueReadBuffer( queue, buffer, CL_TRUE, 0, sizeof( sizes ), sizes, 0, NULL, NULL );
	clReleaseMemObject( buffer );
	if ( status != CL_SUCCESS ) return status;
	const int expected[8] = {
		(int)sizeof( ptVertex_t ), (int)sizeof( ptTriangle_t ), (int)sizeof( ptMaterial_t ), (int)sizeof( ptLight_t ),
		(int)sizeof( ptBVHNode_t ), (int)sizeof( ptRenderParams_t ), (int)sizeof( ptClDenoiseGuide_t ), (int)sizeof( ptClTexture_t )
	};
	static const char *names[8] = {
		"ptVertex_t", "ptTriangle_t", "ptMaterial_t", "ptLight_t", "ptBVHNode_t", "ptRenderParams_t",
		"ptClDenoiseGuide_t", "ptClTexture_t"
	};
	for ( int i = 0; i < 8; i++ ) {
		if ( sizes[i] != expected[i] ) {
			char text[128];
			idStr::snPrintf( text, sizeof( text ), "%s is %d bytes on the device but %d on the host", names[i], sizes[i], expected[i] );
			error = text;
			return CL_INVALID_KERNEL_DEFINITION;
		}
	}
	return CL_SUCCESS;
}

/*
========================
PT_CL_LaunchPathTracing

All render parameters are passed by value. The CUDA backend stages
softShadowScale / rayOffset into __device__ globals here; the OpenCL kernel
reads them from the same params.
========================
*/
cl_int PT_CL_LaunchPathTracing( ptClProgram_t &prog, cl_command_queue queue, const ptClSceneView_t &s,
								cl_mem accumBuffer, cl_mem radianceBuffer, cl_mem guideBuffer,
								cl_mem diffuseBuffer, cl_mem specularBuffer, cl_mem volumeBuffer,
								const ptRenderParams_t &params ) {
	const ptRenderParams_t p = params;
	int kernel;
	switch ( p.renderMode ) {
		case PT_MODE_BVH_DEBUG:
			kernel = PT_CL_TRIANGLE_DRAW;
			break;
		case PT_MODE_TEX_COMBINED:
		case PT_MODE_TEX_BASECOLOR:
		case PT_MODE_TEX_ALBEDO:
		case PT_MODE_TEX_NORMAL:
		case PT_MODE_TEX_SPECULAR:
			kernel = PT_CL_TRIANGLE_TEXTURED;
			break;
		case PT_MODE_WHITTED:
			kernel = PT_CL_TRIANGLE_RAYTRACED;
			break;
		default:
			kernel = PT_CL_PATHTRACING;
			break;
	}

	ptClArgs_t args( prog.kernels[kernel] );
	args( s.vertices )( s.triangles )( s.bvhNodes )( s.triIndices )( s.materials )( s.textureDesc )( s.texels )
		( s.lights )( s.numLights )( accumBuffer );
	if ( kernel == PT_CL_TRIANGLE_TEXTURED ) {
		// 0 = combined, 1 = base color, 2 = albedo texture, 3 = normal map, 4 = specular map
		args( (cl_int)( p.renderMode - PT_MODE_TEX_COMBINED ) );
	}
	args( p );
	if ( kernel == PT_CL_PATHTRACING ) {
		args( radianceBuffer )( guideBuffer )( diffuseBuffer )( specularBuffer )( volumeBuffer );
	}
	PT_CL_RETURN_ON_ERROR( args.error );
	return PT_CL_Launch2D( prog, queue, kernel, p.width, p.height );
}

/*
========================
PT_CL_LaunchDenoising
========================
*/
cl_int PT_CL_LaunchDenoising( ptClProgram_t &prog, cl_command_queue queue, const ptClDenoiseBuffers_t &b,
							  cl_mem accumBuffer, cl_mem filteredBuffer, const ptRenderParams_t &params,
							  const ptRenderParams_t &previousParams, int frameSlot, int historyValid ) {
	const ptRenderParams_t p = params;
	const int prev = frameSlot ^ 1;
	const bool capture = p.captureTargetFrames > 0;
	if ( capture ) {
		PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_SVGF_CAPTURE_GUIDES] )( b.guides[frameSlot] )( b.guides[prev] )( p ).error );
		PT_CL_RETURN_ON_ERROR( PT_CL_Launch2D( prog, queue, PT_CL_SVGF_CAPTURE_GUIDES, p.width, p.height ) );
	}
	bool volumeHistoryValid = historyValid != 0;
	// Fog is view dependent and the physical medium moves with the camera.
	for ( int c = 0; c < 3; c++ ) {
		volumeHistoryValid &= p.camPos[c] == previousParams.camPos[c]
			&& p.camForward[c] == previousParams.camForward[c]
			&& p.camRight[c] == previousParams.camRight[c] && p.camUp[c] == previousParams.camUp[c];
	}
	volumeHistoryValid &= p.fovX == previousParams.fovX && p.fovY == previousParams.fovY;
	for ( int signal = 0; signal < 3; signal++ ) {
		const ptClDenoiseSignal_t &s = b.signals[signal];
		if ( capture ) {
			PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_SVGF_CAPTURE] )( s.radiance )( b.guides[frameSlot] )
				( s.history[prev] )( s.history[frameSlot] )( filteredBuffer )( s.moments[frameSlot] )( p )( (cl_int)signal ).error );
			PT_CL_RETURN_ON_ERROR( PT_CL_Launch2D( prog, queue, PT_CL_SVGF_CAPTURE, p.width, p.height ) );
		} else {
			const cl_int valid = signal == 2 ? ( volumeHistoryValid ? 1 : 0 ) : historyValid;
			PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_SVGF_TEMPORAL] )( s.radiance )( b.guides[frameSlot] )
				( b.guides[prev] )( s.history[prev] )( s.moments[prev] )( filteredBuffer )( s.moments[frameSlot] )
				( p )( previousParams )( valid )( (cl_int)signal ).error );
			PT_CL_RETURN_ON_ERROR( PT_CL_Launch2D( prog, queue, PT_CL_SVGF_TEMPORAL, p.width, p.height ) );
		}
		if ( signal == 2 && capture ) {
			PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_SVGF_BOOTSTRAP_VOLUME] )( filteredBuffer )
				( b.guides[frameSlot] )( s.moments[frameSlot] )( (cl_int)p.width )( (cl_int)p.height )( (cl_int)p.frameIndex ).error );
			PT_CL_RETURN_ON_ERROR( PT_CL_Launch2D( prog, queue, PT_CL_SVGF_BOOTSTRAP_VOLUME, p.width, p.height ) );
		}
		cl_mem input = filteredBuffer;
		for ( int iteration = 0; iteration < 4; iteration++ ) {
			cl_mem output = iteration == 0 ? s.history[frameSlot] : ( iteration == 2 ? filteredBuffer : b.scratch );
			if ( capture ) output = iteration % 2 == 0 ? b.scratch : filteredBuffer;
			PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_SVGF_ATROUS] )( input )( b.guides[frameSlot] )( output )
				( (cl_int)p.width )( (cl_int)p.height )( (cl_int)( 1 << iteration ) )( p.denoiseStrength )( (cl_int)signal ).error );
			PT_CL_RETURN_ON_ERROR( PT_CL_Launch2D( prog, queue, PT_CL_SVGF_ATROUS, p.width, p.height ) );
			input = output;
		}
		if ( signal < 2 ) {
			PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_SVGF_STORE_SURFACE] )( input )( b.guides[frameSlot] )
				( signal == 0 ? b.filteredDiffuse : b.filteredSpecular )( (cl_int)p.width )( (cl_int)p.height )( (cl_int)signal ).error );
			PT_CL_RETURN_ON_ERROR( PT_CL_Launch2D( prog, queue, PT_CL_SVGF_STORE_SURFACE, p.width, p.height ) );
		} else {
			PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_SVGF_RESOLVE] )( input )( b.filteredDiffuse )( b.filteredSpecular )
				( capture ? accumBuffer : b.radiance )
				( capture ? b.signals[0].history[frameSlot] : b.signals[0].radiance )
				( capture ? b.signals[1].history[frameSlot] : b.signals[1].radiance )
				( capture ? b.signals[2].history[frameSlot] : b.signals[2].radiance )
				( accumBuffer )( b.guides[frameSlot] )( filteredBuffer )( (cl_int)p.width )( (cl_int)p.height )
				( (cl_int)p.denoiseDebug )( (cl_int)( capture ? 1 : 0 ) ).error );
			PT_CL_RETURN_ON_ERROR( PT_CL_Launch2D( prog, queue, PT_CL_SVGF_RESOLVE, p.width, p.height ) );
		}
	}
	return CL_SUCCESS;
}

cl_int PT_CL_LaunchDenoisingDebug( ptClProgram_t &prog, cl_command_queue queue, const ptClDenoiseBuffers_t &buffers,
								   cl_mem output, const ptRenderParams_t &p, int frameSlot ) {
	const ptClDenoiseSignal_t &s = buffers.signals[p.denoiseDebugSignal == 2 ? 2 : ( p.denoiseDebugSignal == 1 ? 1 : 0 )];
	PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_SVGF_DEBUG] )( buffers.guides[frameSlot] )( s.moments[frameSlot] )
		( s.history[frameSlot] )( output )( (cl_int)p.width )( (cl_int)p.height )( (cl_int)p.denoiseDebug )
		( (cl_int)( p.captureTargetFrames > 0 ? 1 : 0 ) ).error );
	return PT_CL_Launch2D( prog, queue, PT_CL_SVGF_DEBUG, p.width, p.height );
}

/*
========================
PT_CL_Postprocess

Host half of PT_Postprocess, identical clamping and pass order.
========================
*/
static bool PT_CL_IsFinite( float value ) {
	return !FLOAT_IS_NAN( value );	// idlib: all-ones exponent, so NaN and both infinities
}

cl_int PT_CL_Postprocess( ptClProgram_t &prog, cl_command_queue queue, const ptClPostprocessBuffers_t &buffers,
						  cl_mem hdr, int width, int height, float exposure, float threshold, float radius,
						  float bloom, float flare ) {
	if ( width < 1 || height < 1 || !hdr || !buffers.output ) return CL_INVALID_VALUE;
	bloom = PT_CL_IsFinite( bloom ) ? fminf( fmaxf( bloom, 0.0f ), 4.0f ) : 0.0f;
	flare = PT_CL_IsFinite( flare ) ? fminf( fmaxf( flare, 0.0f ), 1.0f ) : 0.0f;
	if ( bloom == 0 && flare == 0 ) {
		return PT_CL_Copy( prog, queue, hdr, buffers.output, (size_t)width * height * 4 * sizeof( float ) );
	}
	if ( !buffers.highlights || !buffers.scratch || !buffers.blurred || buffers.output == hdr ) return CL_INVALID_VALUE;
	radius = PT_CL_IsFinite( radius ) && radius > 0 ? fminf( radius, 0.1f ) : 0.025f;
	const int scale = PT_ClPostprocessScale( height, radius );
	const int sw = ( width + scale - 1 ) / scale, sh = ( height + scale - 1 ) / scale;
	exposure = PT_CL_IsFinite( exposure ) ? fminf( fmaxf( exposure, 1.0e-6f ), 1.0e12f ) : 1.0f;
	threshold = PT_CL_IsFinite( threshold ) ? fminf( fmaxf( threshold, 0.0f ), 1.0e12f ) : 1.0f;
	const float sigma = fminf( fmaxf( radius * height / scale, 0.5f ), 3.0f );
	const float sceneThreshold = threshold / exposure;
	PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_POST_HIGHLIGHTS] )( hdr )( buffers.highlights )
		( (cl_int)width )( (cl_int)height )( (cl_int)sw )( (cl_int)sh )( (cl_int)scale )( sceneThreshold ).error );
	PT_CL_RETURN_ON_ERROR( PT_CL_Launch2D( prog, queue, PT_CL_POST_HIGHLIGHTS, sw, sh ) );
	PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_POST_BLUR] )( buffers.highlights )( buffers.scratch )
		( (cl_int)sw )( (cl_int)sh )( sigma )( (cl_int)1 ).error );
	PT_CL_RETURN_ON_ERROR( PT_CL_Launch2D( prog, queue, PT_CL_POST_BLUR, sw, sh ) );
	PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_POST_BLUR] )( buffers.scratch )( buffers.blurred )
		( (cl_int)sw )( (cl_int)sh )( sigma )( (cl_int)0 ).error );
	PT_CL_RETURN_ON_ERROR( PT_CL_Launch2D( prog, queue, PT_CL_POST_BLUR, sw, sh ) );
	PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_POST_COMPOSITE] )( hdr )( buffers.blurred )( buffers.output )
		( (cl_int)width )( (cl_int)height )( (cl_int)sw )( (cl_int)sh )( (cl_int)scale )( bloom )( flare ).error );
	return PT_CL_Launch2D( prog, queue, PT_CL_POST_COMPOSITE, width, height );
}

cl_int PT_CL_LaunchPostprocess( ptClProgram_t &prog, cl_command_queue queue, const ptClPostprocessBuffers_t &buffers,
								cl_mem hdr, const ptRenderParams_t &p ) {
	return PT_CL_Postprocess( prog, queue, buffers, hdr, p.width, p.height, p.exposure, p.bloomThreshold,
		p.bloomRadius, p.bloomStrength, p.lensFlareStrength );
}

/*
========================
PT_CL_LaunchToneMapping
========================
*/
cl_int PT_CL_LaunchToneMapping( ptClProgram_t &prog, cl_command_queue queue, cl_mem hdrBuffer, cl_mem ldrBuffer,
								int width, int height, float exposure, float gamma, int toneMapMode ) {
	PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_TONEMAP] )( hdrBuffer )( ldrBuffer )( (cl_int)width )
		( (cl_int)height )( exposure )( gamma )( (cl_int)toneMapMode ).error );
	return PT_CL_Launch2D( prog, queue, PT_CL_TONEMAP, width, height );
}

/*
========================
PT_CL_LBVHSortTempBytes
========================
*/
size_t PT_CL_LBVHSortTempBytes( const ptClProgram_t &prog, int numTriangles ) {
	if ( numTriangles <= 1 ) return 0;
	const size_t tiles = ( (size_t)numTriangles + prog.sortWorkGroup - 1 ) / prog.sortWorkGroup;
	return tiles * PT_CL_RADIX_DIGITS * sizeof( cl_uint );
}

/*
========================
PT_CL_LaunchBuildLBVH

All work stays on the device and is ordered on 'queue'. The host only
provides the scene bounds (computed by idPathTracerScene).
========================
*/
cl_int PT_CL_LaunchBuildLBVH( ptClProgram_t &prog, cl_command_queue queue, cl_mem triangles, int numTriangles,
							  cl_mem bvhNodes, cl_mem triIndices, cl_mem mortonCodes, cl_mem mortonAlt,
							  cl_mem sortedIndices, cl_mem sortTemp, cl_mem parents, cl_mem atomicCounters,
							  const float *sceneBounds ) {
	if ( numTriangles <= 0 ) return CL_SUCCESS;

	// single triangle
	if ( numTriangles == 1 ) {
		PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_SINGLE_LEAF] )( triangles )( bvhNodes )( triIndices ).error );
		return PT_CL_Launch1D( prog, queue, PT_CL_SINGLE_LEAF, 1, 1 );
	}

	// scene extent for normalization
	const float sceneMin[3] = { sceneBounds[0], sceneBounds[1], sceneBounds[2] };
	const float sceneMax[3] = { sceneBounds[3], sceneBounds[4], sceneBounds[5] };
	float invExt[3];
	for ( int i = 0; i < 3; i++ ) {
		const float ext = sceneMax[i] - sceneMin[i];
		invExt[i] = ( ext > 1e-6f ) ? ( 1.0f / ext ) : 0.0f;
	}

	const cl_int n = numTriangles;
	const int numInternalNodes = numTriangles - 1;
	const int numNodes = 2 * numTriangles - 1;

	// morton codes; the values start in triIndices so that after an even number
	// of passes the sorted indices end there (CUDA copies them back instead)
	PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_MORTON] )( triangles )( n )( mortonCodes )( triIndices )
		( sceneMin[0] )( sceneMin[1] )( sceneMin[2] )( invExt[0] )( invExt[1] )( invExt[2] ).error );
	PT_CL_RETURN_ON_ERROR( PT_CL_Launch1D( prog, queue, PT_CL_MORTON, (size_t)n, 0 ) );

	// stable radix sort by Morton code
	const size_t group = prog.sortWorkGroup;
	const cl_int tiles = (cl_int)( ( (size_t)n + group - 1 ) / group );
	const cl_int histogramCount = tiles * PT_CL_RADIX_DIGITS;
	cl_mem keys[2] = { mortonCodes, mortonAlt };
	cl_mem values[2] = { triIndices, sortedIndices };
	for ( int pass = 0; pass < PT_CL_RADIX_PASSES; pass++ ) {
		const cl_int shift = pass * PT_CL_RADIX_BITS;
		const int in = pass & 1, out = in ^ 1;
		PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_RADIX_HISTOGRAM] )( keys[in] )( n )( shift )( sortTemp )( tiles ).error );
		PT_CL_RETURN_ON_ERROR( PT_CL_Launch1D( prog, queue, PT_CL_RADIX_HISTOGRAM, (size_t)tiles * group, group ) );
		PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_RADIX_SCAN] )( sortTemp )( histogramCount ).error );
		PT_CL_RETURN_ON_ERROR( PT_CL_Launch1D( prog, queue, PT_CL_RADIX_SCAN, group, group ) );
		PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_RADIX_SCATTER] )( keys[in] )( values[in] )( keys[out] )
			( values[out] )( n )( shift )( sortTemp )( tiles ).error );
		PT_CL_RETURN_ON_ERROR( PT_CL_Launch1D( prog, queue, PT_CL_RADIX_SCATTER, (size_t)tiles * group, group ) );
	}
	const cl_mem sortedCodes = mortonCodes;		// PT_CL_RADIX_PASSES is even

	// initialize parent array to -1
	PT_CL_RETURN_ON_ERROR( PT_CL_Fill( prog, queue, parents, 0xFF, (size_t)numNodes * sizeof( cl_int ) ) );

	// initialize atomic counters to 0
	PT_CL_RETURN_ON_ERROR( PT_CL_Fill( prog, queue, atomicCounters, 0, (size_t)numInternalNodes * sizeof( cl_int ) ) );

	// build radix tree topology
	PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_RADIX_TREE] )( sortedCodes )( n )( bvhNodes )( parents ).error );
	PT_CL_RETURN_ON_ERROR( PT_CL_Launch1D( prog, queue, PT_CL_RADIX_TREE, (size_t)numInternalNodes, 0 ) );

	// initialize leaf nodes
	PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_LEAF_NODES] )( triangles )( triIndices )( bvhNodes )( n ).error );
	PT_CL_RETURN_ON_ERROR( PT_CL_Launch1D( prog, queue, PT_CL_LEAF_NODES, (size_t)n, 0 ) );

	PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_ESCAPE_LINKS] )( bvhNodes )( parents )( (cl_int)numNodes ).error );
	PT_CL_RETURN_ON_ERROR( PT_CL_Launch1D( prog, queue, PT_CL_ESCAPE_LINKS, (size_t)numNodes, 0 ) );

	// bottom-up AABB propagation
	PT_CL_RETURN_ON_ERROR( ptClArgs_t( prog.kernels[PT_CL_NODE_BOUNDS] )( bvhNodes )( parents )( atomicCounters )( n ).error );
	return PT_CL_Launch1D( prog, queue, PT_CL_NODE_BOUNDS, (size_t)n, 0 );
}
