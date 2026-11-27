#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "renderer_pt/pt_capture_report.h"

class idCommon;
idCommon *common = NULL;	// idlib links against the engine global

/*
========================
Check
========================
*/
static void Check( const idStr &actual, const char *expected ) {
	if ( actual != expected ) {
		std::fprintf( stderr, "Unexpected report encoding: %s, expected %s\n", actual.c_str(), expected );
		std::exit( 1 );
	}
}

/*
========================
main
========================
*/
int main() {
	int frame = 17;
	for ( const char *bad : { "", "-1", "1x", " 1", "+1", "2147483647", "999999999999999999999999" } ) {
		if ( PT_ParseCaptureFrame( bad, frame ) || frame != 17 ) return 1;
	}
	if ( !PT_ParseCaptureFrame( "0", frame ) || frame != 0
		|| !PT_ParseCaptureFrame( "2147483646", frame ) || frame != 2147483646 ) return 1;
	Check( PT_ReportJsonString( "" ), "\"\"" );
	Check( PT_ReportJsonString( "a\"b\\c\n\t" ), "\"a\\\"b\\\\c\\u000a\\u0009\"" );
	Check( PT_ReportJsonString( "\1\37" ), "\"\\u0001\\u001f\"" );	// idStr holds C strings, so no NUL case
	Check( PT_ReportJsonString( "\xc3\xa9" ), "\"\xc3\xa9\"" );
	Check( PT_ReportCsvString( "image, \"quoted\"\nnext" ), "\"image, \"\"quoted\"\"\nnext\"" );
	Check( PT_ReportCsvString( "" ), "\"\"" );
	Check( PT_ReportRedactArgument( "--Password=example" ), "--Password=[redacted]" );
	Check( PT_ReportRedactArgument( "r_ptSamplesPerPixel=4" ), "r_ptSamplesPerPixel=4" );
	if ( !PT_ReportSensitiveName( "net_serverRemoteConsolePassword" ) || PT_ReportSensitiveName( "r_ptRenderScale" ) ) return 1;
	std::puts( "Capture report encoding checks passed" );
	return 0;
}
