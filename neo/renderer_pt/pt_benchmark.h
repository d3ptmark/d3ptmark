#ifndef __PT_BENCHMARK_H__
#define __PT_BENCHMARK_H__

#include <algorithm>
#include <vector>
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
	int batch = std::min( samples, std::max( 1, presetBatch ) );
	while ( samples % batch ) --batch;
	return batch;
}

struct ptBenchmarkStatistics_t {
	std::vector<double> frameMilliseconds;
	double gpuMilliseconds = 0;
	double renderMilliseconds = 0;
	int dispatches = 0;
	int imagesWritten = 0;
	double imageMilliseconds = 0;

	void AddImage( double milliseconds ) {
		imagesWritten++;
		imageMilliseconds += milliseconds;
	}

	void AddFrame( double milliseconds, double gpu, double render, int batches ) {
		frameMilliseconds.push_back( milliseconds );
		gpuMilliseconds += gpu;
		renderMilliseconds += render;
		dispatches += batches;
	}
	double TotalMilliseconds() const {
		double total = 0;
		for ( double value : frameMilliseconds ) total += value;
		return total;
	}
	double Percentile( double fraction ) const {
		if ( frameMilliseconds.empty() ) return 0;
		std::vector<double> sorted = frameMilliseconds;
		std::sort( sorted.begin(), sorted.end() );
		const double index = fraction * ( sorted.size() - 1 );
		const size_t lower = (size_t)index;
		const size_t upper = std::min( lower + 1, sorted.size() - 1 );
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

#ifdef HAVE_PATHTRACER
class idCmdArgs;
void R_PT_Benchmark_f( const idCmdArgs &args );
bool R_PT_BenchmarkActive();
bool R_PT_BenchmarkAdvance();
void R_PT_BenchmarkNewFrame();
int R_PT_BenchmarkTargetFrames();
int R_PT_BenchmarkBatch();
void R_PT_BenchmarkRendered( int accumulatedFrames, float gpuMilliseconds, int width, int height );
void R_PT_BenchmarkScreenBegin();
void R_PT_BenchmarkScreenComplete();
void R_PT_EndBenchmark( bool completed );
#endif

#endif
