#ifndef __PT_CAPTURE_REPORT_H__
#define __PT_CAPTURE_REPORT_H__

#include <climits>

#include "sys/platform.h"
#include "idlib/Str.h"

inline bool PT_ParseCaptureFrame( const char *text, int &frame ) {
	if ( !text || !*text ) return false;
	int value = 0;
	for ( const char *p = text; *p; ++p ) {
		if ( *p < '0' || *p > '9' || value > ( INT_MAX - 1 - ( *p - '0' ) ) / 10 ) return false;
		value = value * 10 + ( *p - '0' );
	}
	frame = value;
	return true;
}

inline bool PT_ReportSensitiveName( const char *name ) {
	const idStr text( name );
	return text.Find( "password", false ) >= 0 || text.Find( "passwd", false ) >= 0
		|| text.Find( "token", false ) >= 0 || text.Find( "secret", false ) >= 0
		|| text.Find( "rcon", false ) >= 0;
}

inline idStr PT_ReportRedactArgument( const idStr &argument ) {
	const int equals = argument.Find( '=' );
	if ( equals >= 0 && PT_ReportSensitiveName( argument.Left( equals ).c_str() ) )
		return argument.Left( equals + 1 ) + "[redacted]";
	return argument;
}

inline idStr PT_ReportJsonString( const idStr &text ) {
	idStr result = "\"";
	const char *hex = "0123456789abcdef";
	for ( int i = 0; i < text.Length(); i++ ) {
		const unsigned char c = (unsigned char)text[i];
		if ( c == '"' || c == '\\' ) { result += '\\'; result += (char)c; }
		else if ( c < 32 ) {
			result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15];
		} else result += (char)c;
	}
	return result + '"';
}

inline idStr PT_ReportCsvString( const idStr &text ) {
	idStr result = "\"";
	for ( int i = 0; i < text.Length(); i++ ) {
		if ( text[i] == '"' ) result += '"';
		result += text[i];
	}
	return result + '"';
}

#include "renderer_pt/pt_backend.h"
class idCmdArgs;
bool R_PT_ReportBegin( const idCmdArgs &args, const char *mode, const char *source, int requestedSpp,
	int startFrame = 0, int endFrame = 0 );
void R_PT_ReportSettings();
void R_PT_ReportFrameBegin( int sourceFrame, double sourceTimeSeconds, const char *phase );
void R_PT_ReportRendered( const ptRenderParams_t &params, const ptBackendStats_t &stats,
	double sceneMilliseconds, double backendMilliseconds, double readbackMilliseconds );
bool R_PT_ReportFrameEnd( const char *image, int imageWidth, int imageHeight, double imageMilliseconds,
	double screenMilliseconds = -1 );
void R_PT_ReportPhase( const char *phase );
void R_PT_ReportEnd( bool completed, const char *reason = "" );
void R_PT_ReportTick();
bool R_PT_ReportActive();
const char *R_PT_ReportDirectory();
#endif
