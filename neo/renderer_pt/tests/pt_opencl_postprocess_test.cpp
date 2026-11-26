// OpenCL port of pt_cuda_postprocess_test.cu
#include "renderer_pt/tests/pt_opencl_test_common.h"

#include <limits>

static void Check( bool condition, const char *label ) {
	if ( !condition ) {
		std::fprintf( stderr, "FAIL: %s\n", label );
		std::exit( 1 );
	}
}

struct fixture_t {
	int width, height;
	cl_mem input, history;
	ptClPostprocessBuffers_t buffers;
	std::vector<float> source, originalHistory;

	fixture_t( int w, int h ) : width( w ), height( h ), buffers(),
		source( w * h * 4, 0 ), originalHistory( w * h * 4, 0.375f ) {
		const size_t count = source.size();
		input = Allocate<float>( count );
		history = Allocate<float>( count );
		buffers.output = Allocate<float>( count );
		// Maximum scratch size for every supported radius (minimum downsample is four).
		const size_t smallCount = (size_t)( ( w + 3 ) / 4 ) * ( ( h + 3 ) / 4 ) * 4;
		buffers.highlights = Allocate<float>( smallCount );
		buffers.scratch = Allocate<float>( smallCount );
		buffers.blurred = Allocate<float>( smallCount );
		Upload( history, originalHistory.data(), count * sizeof( float ) );
		for ( size_t i = 3; i < source.size(); i += 4 ) source[i] = 0.75f;
	}

	~fixture_t() {
		Free( input ); Free( history );
		Free( buffers.output ); Free( buffers.highlights );
		Free( buffers.scratch ); Free( buffers.blurred );
	}

	std::vector<float> Run( float bloom, float flare, float exposure = 1, float threshold = 1, float radius = 0.025f ) {
		const size_t bytes = source.size() * sizeof( float );
		Upload( input, source.data(), bytes );
		CL_CHECK( PT_CL_Postprocess( clTest.program, clTest.queue, buffers, input, width, height, exposure, threshold, radius, bloom, flare ) );
		CL_CHECK( clFinish( clTest.queue ) );
		std::vector<float> result( source.size() ), unchanged( source.size() );
		Download( result.data(), buffers.output, bytes );
		Download( unchanged.data(), input, bytes );
		Check( std::memcmp( unchanged.data(), source.data(), bytes ) == 0, "HDR input untouched" );
		Download( unchanged.data(), history, bytes );
		Check( unchanged == originalHistory, "history untouched" );
		for ( float value : result ) Check( std::isfinite( value ), "finite output" );
		for ( size_t i = 3; i < source.size(); i += 4 )
			Check( result[i] == source[i], "alpha preserved" );
		return result;
	}
};

static double Energy( const std::vector<float> &image ) {
	double total = 0;
	for ( size_t i = 0; i < image.size(); i++ ) if ( i % 4 != 3 ) total += image[i];
	return total;
}

int main() {
	const int init = PT_CL_TestInit( "OpenCL postprocess test", NULL );
	if ( init ) return init;
	{
		fixture_t f( 129, 97 );
		Check( Energy( f.Run( 1, 1 ) ) == 0, "constant black stays black" );
		const size_t bright = ( 27 * f.width + 31 ) * 4;
		f.source[bright] = f.source[bright + 1] = f.source[bright + 2] = 64;
		Check( f.Run( 0, 0 ) == f.source, "disabled identity" );
		Check( f.Run( -1, -1 ) == f.source, "negative strengths disable effects" );
		const std::vector<float> bloom = f.Run( 1, 0 );
		Check( bloom[bright + 4] > 0, "isolated highlight spreads" );
		Check( Energy( bloom ) > Energy( f.source ), "bloom adds highlight energy" );
		const std::vector<float> flare = f.Run( 0, 1 );
		Check( Energy( flare ) > Energy( f.source ), "flare independently enabled" );
		const size_t reflected = ( ( f.height - 1 - 27 ) * f.width + f.width - 1 - 31 ) * 4;
		Check( flare[reflected] > 0, "ghost reaches reflected screen location" );
		Check( f.Run( 1, 1, 1, 100 ) == f.source, "below exposed threshold unchanged" );
		Check( f.Run( 1, 1, 1, 64 ) == f.source, "at exposed threshold unchanged" );
		Check( Energy( f.Run( 1, 0, 2, 100 ) ) > Energy( f.source ), "exposure changes threshold" );
		std::vector<float> scaledSource = f.source;
		for ( size_t i = 0; i < f.source.size(); i++ ) if ( i % 4 != 3 ) f.source[i] *= 2;
		const std::vector<float> doubled = f.Run( 1, 0, 0.5f );
		for ( size_t i = 0; i < bloom.size(); i++ ) if ( i % 4 != 3 )
			Check( std::fabs( doubled[i] * 0.5f - bloom[i] ) < 1e-5f, "exposure-relative equivalence" );
		f.source = scaledSource;
		f.source[0] = std::numeric_limits<float>::infinity();
		f.source[1] = std::numeric_limits<float>::quiet_NaN();
		f.source[2] = -1;
		f.Run( 4, 1, 1, 0 );
		f.Run( 1, 1, 1, 1, std::numeric_limits<float>::infinity() );
		f.Run( 1, 1, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() );
		f.source[0] = f.source[1] = f.source[2] = std::numeric_limits<float>::max();
		f.Run( 4, 1, std::numeric_limits<float>::max(), 0, 0.1f );
	}
	{
		fixture_t tiny( 1, 1 );
		tiny.source[0] = tiny.source[1] = tiny.source[2] = 16;
		tiny.Run( 1, 1 );
	}
	{
		fixture_t adaptive( 43, 513 );
		adaptive.source[0] = adaptive.source[1] = adaptive.source[2] = 64;
		adaptive.Run( 1, 1, 1, 1, 0.1f );
	}
	Check( PT_ClPostprocessScale( 1080, 0.025f ) * 2 == PT_ClPostprocessScale( 2160, 0.025f ),
		"radius scales with resolution" );
	std::puts( "OpenCL postprocess tests passed" );
	return 0;
}
