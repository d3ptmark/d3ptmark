#include "renderer_pt/pt_benchmark.h"
#include <stdio.h>
#include <cmath>
#include <cstring>

#define CHECK( x ) do { if ( !( x ) ) { fprintf( stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x ); return 1; } } while ( 0 )

/*
========================
main
========================
*/
int main() {
	CHECK( !PT_BenchmarkCanAdvance( false, 1000 ) );
	CHECK( !PT_BenchmarkCanAdvance( true, 0 ) );
	CHECK( !PT_BenchmarkCanAdvance( true, 1000.0 / 30.0 - 0.0001 ) );
	CHECK( PT_BenchmarkCanAdvance( true, 1000.0 / 30.0 ) );
	CHECK( PT_BenchmarkCanAdvance( true, 1000 ) );
	int samples = 17;
	const char *bad[] = { "", "0", "-1", "32x", "1.5", "nan", "100001", "999999999999999999999999", " 32", "+32" };
	for ( const char *text : bad ) {
		CHECK( !PT_ParseBenchmarkSamples( text, samples ) );
		CHECK( samples == 17 );
	}
	CHECK( !PT_ParseBenchmarkSamples( NULL, samples ) );
	CHECK( PT_ParseBenchmarkSamples( "100000", samples ) && samples == 100000 );
	CHECK( PT_ParseBenchmarkSamples( "32", samples ) && samples == 32 );
	for ( int target = 1; target <= 1000; ++target ) {
		for ( int preset = 1; preset <= 64; ++preset ) {
			const int batch = PT_BenchmarkBatchSize( target, preset );
			CHECK( batch > 0 && batch <= preset && target % batch == 0 );
			CHECK( ( target / batch ) * batch == target );
		}
	}
	CHECK( PT_BenchmarkBatchSize( 32, 4 ) == 4 );
	CHECK( PT_BenchmarkBatchSize( 5, 4 ) == 1 );
	CHECK( PT_BenchmarkBatchSize( 10, 4 ) == 2 );
	CHECK( PT_BenchmarkBatchSize( 1, 64 ) == 1 );
	ptBenchmarkStatistics_t stats;
	CHECK( stats.TotalMilliseconds() == 0 && stats.Percentile( 0.95 ) == 0 );
	stats.AddFrame( 40, 10, 20, 8 );
	stats.AddFrame( 20, 5, 10, 8 );
	stats.AddFrame( 30, 8, 15, 8 );
	CHECK( stats.TotalMilliseconds() == 90 );
	CHECK( stats.gpuMilliseconds == 23 && stats.renderMilliseconds == 45 && stats.dispatches == 24 );
	CHECK( stats.Percentile( 0 ) == 20 && stats.Percentile( 0.5 ) == 30 && stats.Percentile( 1 ) == 40 );
	CHECK( std::fabs( stats.Percentile( 0.95 ) - 39 ) < 1e-9 );
	CHECK( stats.frameMilliseconds[0] == 40 );
	CHECK( stats.imagesWritten == 0 && stats.imageMilliseconds == 0 );
	stats.AddImage( 12 );
	stats.AddImage( 8 );
	CHECK( stats.imagesWritten == 2 && stats.imageMilliseconds == 20 );
	CHECK( stats.renderMilliseconds == 45 && stats.gpuMilliseconds == 23 );
	ptBenchmarkStatistics_t glStats;
	glStats.AddFrame( 40, 0, 20, 0 );
	glStats.AddImage( 5 );
	CHECK( glStats.TotalMilliseconds() == 40 && glStats.renderMilliseconds == 20 );
	CHECK( glStats.gpuMilliseconds == 0 && glStats.dispatches == 0 );
	CHECK( glStats.imagesWritten == 1 && glStats.imageMilliseconds == 5 );
	CHECK( !strcmp( PT_BenchmarkImageExtension( 0 ), "tga" ) );
	CHECK( !strcmp( PT_BenchmarkImageExtension( 1 ), "bmp" ) );
	CHECK( !strcmp( PT_BenchmarkImageExtension( 2 ), "png" ) );
	CHECK( !strcmp( PT_BenchmarkImageExtension( 3 ), "jpg" ) );
	CHECK( !strcmp( PT_BenchmarkImageExtension( 4 ), "tga" ) );
	puts( "Benchmark sample budgets and timing statistics passed" );
	return 0;
}
