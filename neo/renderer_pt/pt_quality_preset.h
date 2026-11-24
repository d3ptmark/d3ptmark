#ifndef __PT_QUALITY_PRESET_H__
#define __PT_QUALITY_PRESET_H__

#include "idlib/Str.h"

struct ptQualitySettings_t {
	float renderScale;
	int samplesPerPixel;
	int maxDepth;
	int maxLightSamples;
};

struct ptQualityPreset_t {
	const char *name;
	ptQualitySettings_t settings;
	const char *customName;
};

static const ptQualityPreset_t ptQualityPresets[] = {
	{ "low",   { 0.35f, 1, 2, 1 }, "custom" },
	{ "med",   { 0.50f, 4, 3, 2 }, "custom" },
	{ "high",  { 0.75f, 6, 4, 3 }, "custom" },
	{ "ultra", { 1.00f, 8, 5, 4 }, "custom" },
	{ "gameplay_low",   { 0.35f, 1, 1, 1 }, "gameplay_custom" },
	{ "gameplay_med",   { 0.50f, 1, 2, 1 }, "gameplay_custom" },
	{ "gameplay_high",  { 0.67f, 1, 2, 2 }, "gameplay_custom" },
	{ "gameplay_ultra", { 0.75f, 2, 3, 2 }, "gameplay_custom" },
	{ "cinematic_low",   { 1.00f, 4, 3, 2 }, "cinematic_custom" },
	{ "cinematic_med",   { 1.00f, 8, 4, 4 }, "cinematic_custom" },
	{ "cinematic_high",  { 1.00f, 16, 6, 6 }, "cinematic_custom" },
	{ "cinematic_ultra", { 1.00f, 32, 8, 8 }, "cinematic_custom" }
};

static const int PT_QUALITY_PRESET_COUNT = sizeof( ptQualityPresets ) / sizeof( ptQualityPresets[0] );

static const char * const ptCustomQualityPresets[] = { "custom", "gameplay_custom", "cinematic_custom" };
static const int PT_CUSTOM_QUALITY_PRESET_COUNT = sizeof( ptCustomQualityPresets ) / sizeof( ptCustomQualityPresets[0] );

static ID_INLINE const char *PT_FindCustomQualityPreset( const char *name ) {
	if ( name ) {
		for ( int i = 0; i < PT_CUSTOM_QUALITY_PRESET_COUNT; i++ ) {
			if ( idStr::Icmp( name, ptCustomQualityPresets[i] ) == 0 ) {
				return ptCustomQualityPresets[i];
			}
		}
	}
	return NULL;
}

static ID_INLINE const ptQualityPreset_t *PT_FindQualityPreset( const char *name ) {
	if ( !name ) {
		return NULL;
	}
	for ( int i = 0; i < PT_QUALITY_PRESET_COUNT; i++ ) {
		if ( idStr::Icmp( name, ptQualityPresets[i].name ) == 0 ) {
			return &ptQualityPresets[i];
		}
	}
	return NULL;
}

static ID_INLINE bool PT_QualitySettingsEqual( const ptQualitySettings_t &a, const ptQualitySettings_t &b ) {
	return a.renderScale == b.renderScale && a.samplesPerPixel == b.samplesPerPixel
		&& a.maxDepth == b.maxDepth && a.maxLightSamples == b.maxLightSamples;
}

#endif /* !__PT_QUALITY_PRESET_H__ */
