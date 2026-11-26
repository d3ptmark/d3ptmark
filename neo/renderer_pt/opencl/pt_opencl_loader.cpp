#define PT_OPENCL_LOADER_IMPLEMENTATION
#include "renderer_pt/opencl/pt_opencl_device.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define PT_CL_DEFINE_POINTER( name ) decltype( &::name ) pt_##name = NULL;
PT_CL_FUNCTIONS( PT_CL_DEFINE_POINTER )
#undef PT_CL_DEFINE_POINTER

static void *PT_CL_OpenLibrary() {
#if defined( _WIN32 )
	return (void *)LoadLibraryA( "OpenCL.dll" );
#elif defined( __APPLE__ )
	return dlopen( "/System/Library/Frameworks/OpenCL.framework/OpenCL", RTLD_NOW | RTLD_LOCAL );
#else
	static const char *names[] = { "libOpenCL.so.1", "libOpenCL.so" };
	for ( size_t i = 0; i < sizeof( names ) / sizeof( names[0] ); i++ ) {
		void *library = dlopen( names[i], RTLD_NOW | RTLD_LOCAL );
		if ( library ) return library;
	}
	return NULL;
#endif
}

static void *PT_CL_Symbol( void *library, const char *name ) {
#ifdef _WIN32
	return (void *)GetProcAddress( (HMODULE)library, name );
#else
	return dlsym( library, name );
#endif
}

/*
========================
PT_CL_Load

The library stays loaded for the lifetime of the process: ICDs commonly
register atexit handlers and unloading them early is unsafe.
========================
*/
bool PT_CL_Load() {
	static int state = 0;	// 0 untried, 1 loaded, -1 unavailable
	if ( state != 0 ) return state > 0;
	state = -1;
	void *library = PT_CL_OpenLibrary();
	if ( !library ) return false;
#define PT_CL_RESOLVE( name ) \
	pt_##name = (decltype( pt_##name ))PT_CL_Symbol( library, #name ); \
	if ( !pt_##name ) return false;
	PT_CL_FUNCTIONS( PT_CL_RESOLVE )
#undef PT_CL_RESOLVE
	state = 1;
	return true;
}
