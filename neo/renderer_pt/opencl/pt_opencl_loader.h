#ifndef __PT_OPENCL_LOADER_H__
#define __PT_OPENCL_LOADER_H__

/*
===============================================================================

	Runtime loading of the OpenCL ICD loader (OpenCL.dll / libOpenCL.so.1 /
	OpenCL.framework).

	The executable never links against OpenCL, so an OPENCL_PATHTRACER build
	still starts on machines without an OpenCL runtime; the backend simply
	reports itself unavailable, like the CUDA backend without a driver.

	Every OpenCL entry point used by the backend is listed in
	PT_CL_FUNCTIONS. After this header the usual clXxx names resolve to the
	loaded function pointers. Call PT_CL_Load() before any of them.

===============================================================================
*/

#define PT_CL_FUNCTIONS( X ) \
	X( clGetPlatformIDs ) \
	X( clGetPlatformInfo ) \
	X( clGetDeviceIDs ) \
	X( clGetDeviceInfo ) \
	X( clCreateContext ) \
	X( clReleaseContext ) \
	X( clCreateCommandQueue ) \
	X( clReleaseCommandQueue ) \
	X( clCreateBuffer ) \
	X( clReleaseMemObject ) \
	X( clCreateProgramWithSource ) \
	X( clCreateProgramWithBinary ) \
	X( clBuildProgram ) \
	X( clGetProgramBuildInfo ) \
	X( clGetProgramInfo ) \
	X( clReleaseProgram ) \
	X( clCreateKernel ) \
	X( clReleaseKernel ) \
	X( clSetKernelArg ) \
	X( clGetKernelWorkGroupInfo ) \
	X( clEnqueueNDRangeKernel ) \
	X( clEnqueueFillBuffer ) \
	X( clEnqueueCopyBuffer ) \
	X( clEnqueueReadBuffer ) \
	X( clEnqueueWriteBuffer ) \
	X( clFinish ) \
	X( clGetEventProfilingInfo ) \
	X( clReleaseEvent )

#define PT_CL_DECLARE_POINTER( name ) extern decltype( &::name ) pt_##name;
PT_CL_FUNCTIONS( PT_CL_DECLARE_POINTER )
#undef PT_CL_DECLARE_POINTER

// true once the runtime library is loaded and every entry point resolved
bool PT_CL_Load();

#ifndef PT_OPENCL_LOADER_IMPLEMENTATION
#define clGetPlatformIDs			pt_clGetPlatformIDs
#define clGetPlatformInfo			pt_clGetPlatformInfo
#define clGetDeviceIDs				pt_clGetDeviceIDs
#define clGetDeviceInfo				pt_clGetDeviceInfo
#define clCreateContext				pt_clCreateContext
#define clReleaseContext			pt_clReleaseContext
#define clCreateCommandQueue		pt_clCreateCommandQueue
#define clReleaseCommandQueue		pt_clReleaseCommandQueue
#define clCreateBuffer				pt_clCreateBuffer
#define clReleaseMemObject			pt_clReleaseMemObject
#define clCreateProgramWithSource	pt_clCreateProgramWithSource
#define clCreateProgramWithBinary	pt_clCreateProgramWithBinary
#define clBuildProgram				pt_clBuildProgram
#define clGetProgramBuildInfo		pt_clGetProgramBuildInfo
#define clGetProgramInfo			pt_clGetProgramInfo
#define clReleaseProgram			pt_clReleaseProgram
#define clCreateKernel				pt_clCreateKernel
#define clReleaseKernel				pt_clReleaseKernel
#define clSetKernelArg				pt_clSetKernelArg
#define clGetKernelWorkGroupInfo	pt_clGetKernelWorkGroupInfo
#define clEnqueueNDRangeKernel		pt_clEnqueueNDRangeKernel
#define clEnqueueFillBuffer			pt_clEnqueueFillBuffer
#define clEnqueueCopyBuffer			pt_clEnqueueCopyBuffer
#define clEnqueueReadBuffer			pt_clEnqueueReadBuffer
#define clEnqueueWriteBuffer		pt_clEnqueueWriteBuffer
#define clFinish					pt_clFinish
#define clGetEventProfilingInfo		pt_clGetEventProfilingInfo
#define clReleaseEvent				pt_clReleaseEvent
#endif

#endif /* !__PT_OPENCL_LOADER_H__ */
