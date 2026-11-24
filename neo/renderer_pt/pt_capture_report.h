#ifndef __PT_CAPTURE_REPORT_H__
#define __PT_CAPTURE_REPORT_H__

#include <string>
#include <climits>

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

inline bool PT_ReportSensitiveName( std::string name ) {
	for ( char &c : name ) if ( c >= 'A' && c <= 'Z' ) c += 'a' - 'A';
	return name.find( "password" ) != std::string::npos || name.find( "passwd" ) != std::string::npos
		|| name.find( "token" ) != std::string::npos || name.find( "secret" ) != std::string::npos
		|| name.find( "rcon" ) != std::string::npos;
}

inline std::string PT_ReportRedactArgument( const std::string &argument ) {
	const size_t equals = argument.find( '=' );
	if ( equals != std::string::npos && PT_ReportSensitiveName( argument.substr( 0, equals ) ) )
		return argument.substr( 0, equals + 1 ) + "[redacted]";
	return argument;
}

inline std::string PT_ReportJsonString( const std::string &text ) {
	std::string result = "\"";
	const char *hex = "0123456789abcdef";
	for ( unsigned char c : text ) {
		if ( c == '"' || c == '\\' ) { result += '\\'; result += c; }
		else if ( c < 32 ) {
			result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15];
		} else result += c;
	}
	return result + '"';
}

inline std::string PT_ReportCsvString( const std::string &text ) {
	std::string result = "\"";
	for ( char c : text ) {
		if ( c == '"' ) result += '"';
		result += c;
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
