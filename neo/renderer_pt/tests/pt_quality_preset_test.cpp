#include "sys/platform.h"
#include "renderer_pt/pt_quality_preset.h"

#include <stdio.h>
#include <stdlib.h>

class idCommon;
idCommon *common = NULL; // idlib links against the engine global even for string helpers

#define CHECK( condition ) do { \
	if ( !( condition ) ) { \
		fprintf( stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition ); \
		exit( 1 ); \
	} \
} while ( 0 )

int main() {
	const ptQualitySettings_t expected[] = {
		{ 0.35f, 1, 2, 1 },
		{ 0.50f, 4, 3, 2 },
		{ 0.75f, 6, 4, 3 },
		{ 1.00f, 8, 5, 4 },
		{ 0.35f, 1, 1, 1 },
		{ 0.50f, 1, 2, 1 },
		{ 0.67f, 1, 2, 2 },
		{ 0.75f, 2, 3, 2 },
		{ 1.00f, 4, 3, 2 },
		{ 1.00f, 8, 4, 4 },
		{ 1.00f, 16, 6, 6 },
		{ 1.00f, 32, 8, 8 }
	};
	const char *names[] = {
		"low", "med", "high", "ultra",
		"gameplay_low", "gameplay_med", "gameplay_high", "gameplay_ultra",
		"cinematic_low", "cinematic_med", "cinematic_high", "cinematic_ultra"
	};
	const char *customNames[] = { "custom", "gameplay_custom", "cinematic_custom" };
	CHECK( PT_QUALITY_PRESET_COUNT == 12 );
	CHECK( PT_CUSTOM_QUALITY_PRESET_COUNT == 3 );
	for ( int i = 0; i < PT_QUALITY_PRESET_COUNT; i++ ) {
		const ptQualityPreset_t *preset = PT_FindQualityPreset( names[i] );
		CHECK( preset != NULL );
		CHECK( PT_QualitySettingsEqual( preset->settings, expected[i] ) );
		CHECK( idStr::Icmp( preset->customName, customNames[i / 4] ) == 0 );
		CHECK( PT_FindCustomQualityPreset( preset->customName ) != NULL );
		CHECK( PT_FindCustomQualityPreset( preset->name ) == NULL );
		CHECK( preset->settings.renderScale >= 0.1f && preset->settings.renderScale <= 1.0f );
		CHECK( preset->settings.samplesPerPixel >= 1 && preset->settings.samplesPerPixel <= 64 );
		CHECK( preset->settings.maxDepth >= 1 && preset->settings.maxDepth <= 16 );
		CHECK( preset->settings.maxLightSamples >= 1 && preset->settings.maxLightSamples <= 8 );
		for ( int j = 0; j < i; j++ ) CHECK( idStr::Icmp( preset->name, names[j] ) != 0 );
		if ( i % 4 > 0 ) {
			const ptQualitySettings_t &previous = expected[i - 1];
			CHECK( preset->settings.renderScale >= previous.renderScale );
			CHECK( preset->settings.samplesPerPixel >= previous.samplesPerPixel );
			CHECK( preset->settings.maxDepth >= previous.maxDepth );
			CHECK( preset->settings.maxLightSamples >= previous.maxLightSamples );
			CHECK( !PT_QualitySettingsEqual( preset->settings, previous ) );
		}
	}
	CHECK( PT_FindQualityPreset( "HIGH" ) == &ptQualityPresets[2] );
	CHECK( PT_FindQualityPreset( "GAMEPLAY_MED" ) == &ptQualityPresets[5] );
	CHECK( PT_FindQualityPreset( "CINEMATIC_ULTRA" ) == &ptQualityPresets[11] );
	for ( int i = 0; i < PT_CUSTOM_QUALITY_PRESET_COUNT; i++ ) {
		CHECK( PT_FindQualityPreset( customNames[i] ) == NULL );
		CHECK( idStr::Icmp( PT_FindCustomQualityPreset( customNames[i] ), customNames[i] ) == 0 );
	}
	CHECK( idStr::Icmp( PT_FindCustomQualityPreset( "GAMEPLAY_CUSTOM" ), "gameplay_custom" ) == 0 );
	CHECK( idStr::Icmp( PT_FindCustomQualityPreset( "CINEMATIC_CUSTOM" ), "cinematic_custom" ) == 0 );
	CHECK( PT_FindCustomQualityPreset( NULL ) == NULL );
	CHECK( PT_FindCustomQualityPreset( "invalid" ) == NULL );
	CHECK( PT_FindQualityPreset( "gameplay_invalid" ) == NULL );
	CHECK( PT_FindQualityPreset( "cinematic_" ) == NULL );
	CHECK( PT_FindQualityPreset( "invalid" ) == NULL );
	CHECK( PT_FindQualityPreset( NULL ) == NULL );

	ptQualitySettings_t edited = expected[1];
	edited.renderScale = 0.6f;
	CHECK( !PT_QualitySettingsEqual( edited, expected[1] ) );
	edited = expected[1];
	edited.samplesPerPixel++;
	CHECK( !PT_QualitySettingsEqual( edited, expected[1] ) );
	edited = expected[1];
	edited.maxDepth++;
	CHECK( !PT_QualitySettingsEqual( edited, expected[1] ) );
	edited = expected[1];
	edited.maxLightSamples++;
	CHECK( !PT_QualitySettingsEqual( edited, expected[1] ) );

	puts( "quality preset regression tests passed" );
	return 0;
}
