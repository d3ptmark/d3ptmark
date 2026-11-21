#ifndef __PT_MATERIAL_PROFILES_H__
#define __PT_MATERIAL_PROFILES_H__

#include "renderer/Material.h"
#include "renderer_pt/pt_material_editor.h"

struct ptMaterialFacts_t {
	const char *name;
	const char *description;
	surfTypes_t surface;
	bool interaction, specular, translucent, excluded;
};

struct ptMaterialProfile_t {
	const char *label;
	ptMaterialOverride_t value;
	ptMaterialProfile_t() : label( NULL ) {}
};

inline bool PT_IsWindowPane( const idStr &name ) {
	return !name.Icmpn( "textures/glass/", 15 ) || !name.Icmpn( "textures/object/windowpane", 26 )
		|| name == "textures/base_door/glass_door" || name == "models/mapobjects/elevator/glass"
		|| name == "textures/washroom/glass01" || name == "models/mapobjects/enpro/enproplasmatubeglass"
		|| name == "textures/sfx/chiglass1" || name == "textures/sfx/chiglass1blue"
		|| name == "textures/sfx/chiglass1blue_sort_close" || name == "textures/sfx/chiglass1bluemac"
		|| name == "textures/sfx/chiglass1bluex";
}

inline ptMaterialProfile_t PT_ChooseMaterialProfile( const ptMaterialFacts_t &facts ) {
	ptMaterialProfile_t result;
	if ( facts.excluded ) return result;
	idStr name = facts.name;
	name.ToLower();
	name.BackSlashesToSlashes();
	name.StripFileExtension();
	idStr base;
	name.ExtractFileName( base );
	idStr hints = idStr( facts.description ) + " " + base;
	hints.ToLower();
	const auto has = [&]( const char *text ) { return hints.Find( text ) >= 0; };
	const auto prefix = [&]( const char *text ) { return !name.Icmpn( text, idStr::Length( text ) ); };
	const auto opaque = [&]( const char *label, float metallic, float roughness ) {
		result.label = label;
		result.value.metallic = metallic;
		result.value.roughness = roughness;
	};

	// These are actual panes, not glass-named decals, monitors or creature skins.
	const bool pane = PT_IsWindowPane( name );
	const bool water = name == "textures/washroom/water01";
	if ( pane || water ) {
		opaque( water ? "Water sheet" : "Window glass", 0, water ? 0.1f : 0.12f );
		result.value.transmission = 1;
		result.value.ior = water ? 1.333f : 1.52f;
		result.value.thin = 1;
		result.value.thickness = water ? 0.01f : 0.006f;
		result.value.absorption[0] = water ? 0.35f : 0.15f;
		result.value.absorption[1] = water ? 0.08f : 0.03f;
		result.value.absorption[2] = water ? 0.03f : 0.08f;
		return result;
	}
	if ( name == "textures/washroom/mirror" || name == "textures/sfx/chrome_mal_pipe" ) {
		opaque( name == "textures/washroom/mirror" ? "Mirror" : "Polished metal", 1,
			name == "textures/washroom/mirror" ? 0.06f : 0.18f );
		return result;
	}
	if ( !facts.interaction || facts.translucent ) return result;

	// Sound/impact surface types describe the substrate, not necessarily its coating.
	if ( has( "rubber" ) ) {
		opaque( "Rubber", 0, 0.9f );
	} else if ( has( "paint" ) || prefix( "textures/recyc_wall/shinypipe" ) || prefix( "textures/recyc_wall/dirtypipe" ) ) {
		opaque( "Painted metal", 0, has( "dirty" ) || has( "dust" ) ? 0.76f : has( "shiny" ) ? 0.36f : 0.62f );
	} else if ( facts.surface == SURFTYPE_CARDBOARD || has( "paper" ) || has( "cardboard" ) || has( "book" ) || has( "manual" ) ) {
		opaque( "Paper / cardboard", 0, 0.95f );
	} else if ( has( "cloth" ) || has( "fabric" ) || has( "carpet" ) || has( "strap" ) ) {
		opaque( "Fabric", 0, 0.94f );
	} else if ( prefix( "models/characters/eyes/" ) ) {
		opaque( "Eye surface", 0, 0.18f );
	} else if ( facts.surface == SURFTYPE_FLESH || prefix( "models/characters/" ) || prefix( "models/monsters/" ) ) {
		opaque( "Skin / organic", 0, 0.65f );
	} else if ( facts.surface == SURFTYPE_WOOD || has( "wood" ) ) {
		opaque( "Wood", 0, 0.82f );
	} else if ( facts.surface == SURFTYPE_PLASTIC || has( "plastic" ) ) {
		opaque( "Plastic", 0, 0.52f );
	} else if ( has( "glasscountertop" ) || facts.surface == SURFTYPE_GLASS ) {
		opaque( "Opaque glass / glazed surface", 0, 0.24f );
	} else if ( has( "copper" ) ) {
		opaque( "Copper", has( "very patined" ) ? 0.25f : has( "patined" ) ? 0.65f : 1.0f,
			has( "very patined" ) ? 0.76f : has( "patined" ) ? 0.56f : 0.36f );
	} else if ( has( "wet" ) && !has( "dried" ) ) {
		opaque( "Wet coated surface", 0, 0.32f );
	} else if ( has( "rust" ) || has( "corrod" ) || has( "algae" ) ) {
		opaque( "Oxidized / dirty metal", has( "slight rust" ) ? 0.35f : 0.08f, has( "slight rust" ) ? 0.72f : 0.9f );
	} else if ( has( "chrome" ) || has( "polished metal" ) || has( "polished steel" ) ) {
		opaque( "Polished metal", 1, 0.2f );
	} else if ( has( "steel" ) || has( "brass" ) || has( "alumin" ) || has( "brushed" ) ) {
		opaque( "Bare metal", 0.95f, 0.46f );
	} else if ( facts.surface == SURFTYPE_STONE || has( "concrete" ) || ( has( "rock" ) && !has( "rocket" ) ) || has( "brick" ) || has( "stone" ) ) {
		opaque( "Stone / concrete", 0, 0.92f );
	} else if ( facts.surface == SURFTYPE_METAL || facts.surface == SURFTYPE_RICOCHET || has( "grate" )
		|| has( "diamond" ) || has( "diafloor" ) || has( "girder" ) || has( "corrugated" ) || has( "wire_fence" )
		|| ( has( "chain" ) && !has( "chainsaw" ) ) ) {
		opaque( "Weathered metal", 0.65f, 0.64f );
	} else if ( prefix( "textures/base_floor/" ) || prefix( "textures/recyc_floor/" ) ) {
		opaque( "Coated industrial floor", 0, 0.76f );
	} else {
		opaque( facts.specular ? "Coated / dielectric" : "Matte dielectric", 0, facts.specular ? 0.64f : 0.9f );
	}
	return result;
}

#endif
