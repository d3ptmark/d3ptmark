#include "renderer_pt/pt_capture_report.h"
#include <cstdio>
#include <cstdlib>

static void Check( const std::string &actual, const std::string &expected ) {
	if ( actual != expected ) {
		std::fprintf( stderr, "Unexpected report encoding: %s, expected %s\n", actual.c_str(), expected.c_str() );
		std::exit( 1 );
	}
}

int main() {
	int frame = 17;
	for ( const char *bad : { "", "-1", "1x", " 1", "+1", "2147483647", "999999999999999999999999" } ) {
		if ( PT_ParseCaptureFrame( bad, frame ) || frame != 17 ) return 1;
	}
	if ( !PT_ParseCaptureFrame( "0", frame ) || frame != 0
		|| !PT_ParseCaptureFrame( "2147483646", frame ) || frame != 2147483646 ) return 1;
	Check( PT_ReportJsonString( "" ), "\"\"" );
	Check( PT_ReportJsonString( "a\"b\\c\n\t" ), "\"a\\\"b\\\\c\\u000a\\u0009\"" );
	Check( PT_ReportJsonString( std::string( "\0\1", 2 ) ), "\"\\u0000\\u0001\"" );
	Check( PT_ReportJsonString( "\xc3\xa9" ), "\"\xc3\xa9\"" );
	Check( PT_ReportCsvString( "image, \"quoted\"\nnext" ), "\"image, \"\"quoted\"\"\nnext\"" );
	Check( PT_ReportCsvString( "" ), "\"\"" );
	Check( PT_ReportRedactArgument( "--Password=example" ), "--Password=[redacted]" );
	Check( PT_ReportRedactArgument( "r_ptSamplesPerPixel=4" ), "r_ptSamplesPerPixel=4" );
	if ( !PT_ReportSensitiveName( "net_serverRemoteConsolePassword" ) || PT_ReportSensitiveName( "r_ptRenderScale" ) ) return 1;
	std::puts( "Capture report encoding checks passed" );
	return 0;
}
