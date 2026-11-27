#ifndef __PT_BENCHMARK_H__
#define __PT_BENCHMARK_H__

#include "idlib/containers/List.h"
#include "idlib/Lib.h"
#include "idlib/Str.h"
#include <stdlib.h>
#include <errno.h>

inline bool PT_ParseBenchmarkSamples( const char *text, int &samples ) {
	if ( !text || !text[0] ) return false;
	for ( const char *p = text; *p; ++p ) if ( *p < '0' || *p > '9' ) return false;
	char *end;
	errno = 0;
	const long value = strtol( text, &end, 10 );
	if ( errno || *end || value < 1 || value > 100000 ) return false;
	samples = (int)value;
	return true;
}

// Equal-sized batches preserve the renderer's mean-of-dispatches accumulation.
inline int PT_BenchmarkBatchSize( int samples, int presetBatch ) {
	int batch = Min( samples, Max( 1, presetBatch ) );
	while ( samples % batch ) --batch;
	return batch;
}

// optional trailing benchmark arguments, in any order:
//	uncapped		no 30 Hz demo pacing, vsync off, no 60 Hz engine frame wait
//	save_image 0|1	save every finished frame (default 1)
inline bool PT_ParseBenchmarkOptions( int count, const char *const *args, bool &uncapped, bool &saveImages ) {
	uncapped = false;
	saveImages = true;
	for ( int i = 0; i < count; i++ ) {
		if ( !idStr::Icmp( args[i], "uncapped" ) ) {
			uncapped = true;
		} else if ( !idStr::Icmp( args[i], "save_image" ) && i + 1 < count
			&& ( !idStr::Cmp( args[i + 1], "0" ) || !idStr::Cmp( args[i + 1], "1" ) ) ) {
			saveImages = args[++i][0] == '1';
		} else {
			return false;
		}
	}
	return true;
}

// a finished demo frame is held for one 30 Hz recorded-frame interval unless the run is uncapped
inline bool PT_BenchmarkCanAdvance( bool frameDone, double frameMilliseconds, bool uncapped = false ) {
	return frameDone && ( uncapped || frameMilliseconds >= 1000.0 / 30.0 );
}

struct ptBenchmarkStatistics_t {
	idList<double>	frameMilliseconds;
	double			gpuMilliseconds = 0;
	double			renderMilliseconds = 0;
	int				dispatches = 0;
	int				imagesWritten = 0;
	double			imageMilliseconds = 0;

	void AddImage( double milliseconds ) {
		imagesWritten++;
		imageMilliseconds += milliseconds;
	}

	void AddFrame( double milliseconds, double gpu, double render, int batches ) {
		frameMilliseconds.Append( milliseconds );
		gpuMilliseconds += gpu;
		renderMilliseconds += render;
		dispatches += batches;
	}
	double TotalMilliseconds() const {
		double total = 0;
		for ( int i = 0; i < frameMilliseconds.Num(); i++ ) total += frameMilliseconds[i];
		return total;
	}
	double Percentile( double fraction ) const {
		if ( frameMilliseconds.Num() == 0 ) return 0;
		idList<double> sorted = frameMilliseconds;
		// idList's default comparison subtracts into an int, which truncates fractions
		sorted.Sort( []( const double *a, const double *b ) { return *a < *b ? -1 : ( *a > *b ? 1 : 0 ); } );
		const double index = fraction * ( sorted.Num() - 1 );
		const int lower = (int)index;
		const int upper = Min( lower + 1, sorted.Num() - 1 );
		return sorted[lower] + ( sorted[upper] - sorted[lower] ) * ( index - lower );
	}
};

inline const char *PT_BenchmarkImageExtension( int format ) {
	switch ( format ) {
		case 1: return "bmp";
		case 2: return "png";
		case 3: return "jpg";
		default: return "tga";
	}
}

class idCmdArgs;
void R_GL_Benchmark_f( const idCmdArgs &args );
bool R_GL_BenchmarkActive();
bool R_BenchmarkActive();
bool R_BenchmarkUncapped();		// the engine loop must not wait for the next 60 Hz tic
bool R_BenchmarkAdvance();
void R_BenchmarkNewFrame();
void R_BenchmarkScreenBegin();
void R_BenchmarkCaptureGL();
void R_BenchmarkScreenComplete();
void R_EndBenchmark( bool completed );

#ifdef HAVE_PATHTRACER
void R_PT_Benchmark_f( const idCmdArgs &args );
bool R_PT_BenchmarkActive();
int R_PT_BenchmarkTargetFrames();
int R_PT_BenchmarkBatch();
void R_PT_BenchmarkRendered( int accumulatedFrames, float gpuMilliseconds, int width, int height );
#endif

#endif
