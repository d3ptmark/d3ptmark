#ifndef __PT_OPENCL_TEST_COMMON_H__
#define __PT_OPENCL_TEST_COMMON_H__

/*
===============================================================================

	Shared harness for the OpenCL path tracer regression tests, the
	counterpart of the CUDA_CHECK / Allocate / cudaMemcpy helpers of the
	pt_cuda_*_test.cu files. Tests exit with 77 (skipped) when there is no
	OpenCL runtime or GPU.

	PT_OPENCL_TEST_PLATFORM / PT_OPENCL_TEST_DEVICE select a device by index
	(any device type); by default the first GPU is used.

===============================================================================
*/

// standard headers before idlib (pulled in by pt_opencl_device.h): idlib/Str.h
// redirects snprintf, which breaks MSVC's <cstdio>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "renderer_pt/opencl/pt_opencl_device.h"

class idCommon;
idCommon *common = NULL;	// pt_opencl_kernels.cpp uses idlib, which links against the engine global

struct ptClTest_t {
	cl_context			context;
	cl_device_id		device;
	cl_command_queue	queue;
	ptClProgram_t		program;
};

static ptClTest_t clTest;

static inline void PT_CL_TestCheck( cl_int error, const char *call, int line ) {
	if ( error != CL_SUCCESS ) {
		std::fprintf( stderr, "%s:%d: %s: %s (%d)\n", __FILE__, line, call, PT_CL_ErrorString( error ), error );
		std::exit( 1 );
	}
}

#define CL_CHECK( call ) PT_CL_TestCheck( ( call ), #call, __LINE__ )

static inline bool PT_CL_TestPickDevice( cl_platform_id &platform, cl_device_id &device ) {
	cl_uint numPlatforms = 0;
	if ( clGetPlatformIDs( 0, NULL, &numPlatforms ) != CL_SUCCESS || numPlatforms == 0 ) return false;
	std::vector<cl_platform_id> platforms( numPlatforms );
	clGetPlatformIDs( numPlatforms, platforms.data(), NULL );
	const char *wantPlatform = std::getenv( "PT_OPENCL_TEST_PLATFORM" );
	const char *wantDevice = std::getenv( "PT_OPENCL_TEST_DEVICE" );
	const bool explicitChoice = wantPlatform && wantDevice;
	for ( cl_uint p = 0; p < numPlatforms; p++ ) {
		if ( explicitChoice && (int)p != std::atoi( wantPlatform ) ) continue;
		const cl_device_type type = explicitChoice ? CL_DEVICE_TYPE_ALL : CL_DEVICE_TYPE_GPU;
		cl_uint numDevices = 0;
		if ( clGetDeviceIDs( platforms[p], type, 0, NULL, &numDevices ) != CL_SUCCESS || numDevices == 0 ) continue;
		std::vector<cl_device_id> devices( numDevices );
		clGetDeviceIDs( platforms[p], type, numDevices, devices.data(), NULL );
		for ( cl_uint d = 0; d < numDevices; d++ ) {
			if ( explicitChoice && (int)d != std::atoi( wantDevice ) ) continue;
			platform = platforms[p];
			device = devices[d];
			return true;
		}
	}
	return false;
}

// 0 on success, 77 when the test must be skipped; exits on build failures
static inline int PT_CL_TestInit( const char *testName, const char *extraSource ) {
	cl_platform_id platform = NULL;
	if ( !PT_CL_Load() || !PT_CL_TestPickDevice( platform, clTest.device ) ) {
		std::fprintf( stderr, "Skipping %s: no OpenCL runtime or device\n", testName );
		return 77;
	}
	char name[256] = {};
	clGetDeviceInfo( clTest.device, CL_DEVICE_NAME, sizeof( name ) - 1, name, NULL );
	std::printf( "%s on %s\n", testName, name );
	cl_int error = CL_SUCCESS;
	const cl_context_properties properties[] = { CL_CONTEXT_PLATFORM, (cl_context_properties)platform, 0 };
	clTest.context = clCreateContext( properties, 1, &clTest.device, NULL, NULL, &error );
	CL_CHECK( error );
	clTest.queue = clCreateCommandQueue( clTest.context, clTest.device, 0, &error );
	CL_CHECK( error );
	idStr log;
	error = PT_CL_CreateProgram( clTest.program, clTest.context, clTest.device, extraSource, NULL, NULL, log );
	if ( error != CL_SUCCESS ) {
		std::fprintf( stderr, "program build failed: %s\n%s\n", PT_CL_ErrorString( error ), log.c_str() );
		std::exit( 1 );
	}
	idStr layout;
	error = PT_CL_CheckLayout( clTest.program, clTest.queue, layout );
	if ( error != CL_SUCCESS ) {
		std::fprintf( stderr, "layout check failed: %s %s\n", PT_CL_ErrorString( error ), layout.c_str() );
		std::exit( 1 );
	}
	return 0;
}

// like the CUDA tests' Allocate: device memory filled with a byte pattern
template< class T >
static inline cl_mem Allocate( size_t count, unsigned char fill = 0xff ) {
	cl_int error = CL_SUCCESS;
	const size_t bytes = count * sizeof( T ) > 0 ? count * sizeof( T ) : 4;
	cl_mem buffer = clCreateBuffer( clTest.context, CL_MEM_READ_WRITE, bytes, NULL, &error );
	CL_CHECK( error );
	CL_CHECK( clEnqueueFillBuffer( clTest.queue, buffer, &fill, 1, 0, bytes, 0, NULL, NULL ) );
	return buffer;
}

static inline void Free( cl_mem buffer ) {
	if ( buffer ) CL_CHECK( clReleaseMemObject( buffer ) );
}

static inline void Upload( cl_mem buffer, const void *data, size_t bytes, size_t offset = 0 ) {
	CL_CHECK( clEnqueueWriteBuffer( clTest.queue, buffer, CL_TRUE, offset, bytes, data, 0, NULL, NULL ) );
}

static inline void Download( void *data, cl_mem buffer, size_t bytes, size_t offset = 0 ) {
	CL_CHECK( clEnqueueReadBuffer( clTest.queue, buffer, CL_TRUE, offset, bytes, data, 0, NULL, NULL ) );
}

static inline void Memset( cl_mem buffer, unsigned char value, size_t bytes ) {
	CL_CHECK( clEnqueueFillBuffer( clTest.queue, buffer, &value, 1, 0, bytes, 0, NULL, NULL ) );
}

// sets kernel arguments in order: SetArgs( kernel )( a )( b )...
struct Args {
	cl_kernel	kernel;
	cl_uint		index;

	explicit Args( cl_kernel k ) : kernel( k ), index( 0 ) {}

	template< class T >
	Args &operator()( const T &value ) {
		CL_CHECK( clSetKernelArg( kernel, index++, sizeof( T ), &value ) );
		return *this;
	}
};

// a function, so "SetArgs( k )( a );" can never parse as a declaration of k
static inline Args SetArgs( cl_kernel kernel ) {
	return Args( kernel );
}

static inline cl_kernel TestKernel( const char *name ) {
	cl_int error = CL_SUCCESS;
	cl_kernel kernel = clCreateKernel( clTest.program.program, name, &error );
	CL_CHECK( error );
	return kernel;
}

// launches a kernel over count work-items in 1D (the CUDA tests' <<<grid, block>>>)
static inline void Run1D( cl_kernel kernel, size_t count, size_t group = 0 ) {
	size_t local = group;
	if ( local == 0 ) {
		size_t maxGroup = 1;
		clGetKernelWorkGroupInfo( kernel, clTest.device, CL_KERNEL_WORK_GROUP_SIZE, sizeof( maxGroup ), &maxGroup, NULL );
		local = 64;
		while ( local > maxGroup && local > 1 ) local >>= 1;
		if ( count < local ) local = 1;
	}
	const size_t global = ( ( count + local - 1 ) / local ) * local;
	CL_CHECK( clEnqueueNDRangeKernel( clTest.queue, kernel, 1, NULL, &global, &local, 0, NULL, NULL ) );
	CL_CHECK( clFinish( clTest.queue ) );
}

#endif /* !__PT_OPENCL_TEST_COMMON_H__ */
