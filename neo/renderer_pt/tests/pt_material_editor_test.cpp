#include "sys/platform.h"
#include "renderer_pt/pt_material_editor.h"
#include "renderer_pt/pt_material_profiles.h"

#include <limits>
#include <stdio.h>
#include <stdlib.h>

class idCommon;
idCommon *common = NULL;

#define CHECK( condition ) do { \
	if ( !( condition ) ) { \
		fprintf( stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition ); \
		exit( 1 ); \
	} \
} while ( 0 )

static bool Load( idPTMaterialOverrides &store, const char *text ) {
	idStr error;
	const bool success = store.Load( text, idStr::Length( text ), error );
	CHECK( success == error.IsEmpty() );
	return success;
}

static void CheckProfiles() {
	ptMaterialFacts_t facts = { "textures/base_trim/a_reactorpipe_04_fin.tga", "clean copper", SURFTYPE_NONE, true, true, false, false };
	ptMaterialProfile_t profile = PT_ChooseMaterialProfile( facts );
	CHECK( profile.label && profile.value.Valid() );
	CHECK( profile.value.metallic == 1 && profile.value.transmission == 0 );
	facts.description = "very patined copper with drip line for the bottom";
	CHECK( PT_ChooseMaterialProfile( facts ).value.metallic == 0.25f );
	facts.description = "shiny, clean, bright red, painted surface for pipes";
	facts.surface = SURFTYPE_METAL;
	profile = PT_ChooseMaterialProfile( facts );
	CHECK( profile.value.metallic == 0 && profile.value.roughness == 0.36f );
	facts.description = "very dark, painted black metal fittings,rubber or metal pipe";
	CHECK( PT_ChooseMaterialProfile( facts ).value.roughness == 0.9f );
	facts.description = "dull metal, very rusty";
	CHECK( PT_ChooseMaterialProfile( facts ).value.metallic == 0.08f );
	facts.name = "TEXTURES\\GLASS\\GLASS1_NVP";
	facts.translucent = true;
	facts.interaction = false;
	profile = PT_ChooseMaterialProfile( facts );
	CHECK( profile.label && profile.value.transmission == 1 && profile.value.thin == 1 && profile.value.ior == 1.52f );
	facts.excluded = true;
	CHECK( !PT_ChooseMaterialProfile( facts ).label );
	facts.excluded = false;
	const char *panes[] = { "textures/sfx/chiglass1", "textures/sfx/chiglass1blue",
		"textures/sfx/chiglass1blue_sort_close", "textures/sfx/chiglass1bluemac", "textures/sfx/chiglass1bluex",
		"models/mapobjects/enpro/enproplasmatubeglass", "textures/glass/outdoor_glass1" };
	for ( int i = 0; i < (int)( sizeof( panes ) / sizeof( panes[0] ) ); i++ ) {
		facts.name = panes[i];
		CHECK( PT_IsWindowPane( idStr( facts.name ) ) );
		profile = PT_ChooseMaterialProfile( facts );
		CHECK( profile.label && profile.value.transmission == 1 && profile.value.thin == 1 );
		CHECK( profile.value.ior == 1.52f && profile.value.Valid() );
	}
	CHECK( !PT_IsWindowPane( idStr( "textures/decals/glass_crack1" ) ) );
	CHECK( !PT_IsWindowPane( idStr( "textures/sfx/chiglass1blue_crack_decal" ) ) );
	facts.name = "textures/washroom/water01";
	profile = PT_ChooseMaterialProfile( facts );
	CHECK( profile.value.transmission == 1 && profile.value.ior == 1.333f && profile.value.Valid() );
	facts.name = "models/monsters/vagary/sac";
	facts.surface = SURFTYPE_LIQUID;
	CHECK( !PT_ChooseMaterialProfile( facts ).label );
	facts.name = "textures/decals/glass_crack1";
	CHECK( !PT_ChooseMaterialProfile( facts ).label );
	facts.name = "textures/sfx/monitor_glass";
	CHECK( !PT_ChooseMaterialProfile( facts ).label );
	facts.name = "textures/washroom/mirror";
	profile = PT_ChooseMaterialProfile( facts );
	CHECK( profile.value.metallic == 1 && profile.value.roughness == 0.06f );
	facts.name = "textures/sfx/chrome_mal_pipe";
	profile = PT_ChooseMaterialProfile( facts );
	CHECK( profile.value.metallic == 1 && profile.value.roughness == 0.18f );
	facts.translucent = false;
	facts.interaction = true;
	facts.description = "";
	facts.surface = SURFTYPE_NONE;
	const char *names[] = { "textures/object/windowpane1", "textures/object/steelpanel", "textures/object/glasscountertop",
		"textures/base_floor/grate", "textures/base_wall/concrete01", "textures/object/plastic",
		"textures/object/paperdec2", "textures/object/wood", "textures/object/cloth",
		"textures/object/chrome", "textures/base_floor/panel", "textures/object/matte",
		"models/characters/eyes/leftgreen", "models/characters/scientist/head",
		"textures/recyc_wall/shinypipe01_01_fin" };
	for ( int i = 0; i < (int)( sizeof( names ) / sizeof( names[0] ) ); i++ ) {
		facts.name = names[i];
		profile = PT_ChooseMaterialProfile( facts );
		CHECK( profile.label && profile.value.Valid() );
	}
	facts.name = "textures/object/plain";
	const surfTypes_t surfaces[] = { SURFTYPE_METAL, SURFTYPE_STONE, SURFTYPE_FLESH, SURFTYPE_WOOD,
		SURFTYPE_CARDBOARD, SURFTYPE_GLASS, SURFTYPE_PLASTIC, SURFTYPE_RICOCHET };
	for ( int i = 0; i < (int)( sizeof( surfaces ) / sizeof( surfaces[0] ) ); i++ ) {
		facts.surface = surfaces[i];
		profile = PT_ChooseMaterialProfile( facts );
		CHECK( profile.label && profile.value.Valid() );
	}
	facts.surface = SURFTYPE_STONE;
	CHECK( PT_ChooseMaterialProfile( facts ).value.metallic == 0 );
	facts.specular = false;
	facts.surface = SURFTYPE_NONE;
	CHECK( PT_ChooseMaterialProfile( facts ).value.roughness == 0.9f );
	facts.name = "textures/object/polished_stone";
	facts.description = "polished stone";
	CHECK( PT_ChooseMaterialProfile( facts ).value.metallic == 0 );
	facts.description = "";
	facts.specular = true;
	facts.name = "models/weapons/rocketlauncher/rocketlauncher";
	CHECK( PT_ChooseMaterialProfile( facts ).value.roughness == 0.64f );
	facts.name = "models/weapons/chainsaw/chainsaw";
	CHECK( PT_ChooseMaterialProfile( facts ).value.metallic == 0 );
}

int main() {
	CheckProfiles();
	ptMaterialOverride_t metal;
	CHECK( metal.Valid() );
	metal.metallic = 1;
	metal.roughness = 0.123456789f;
	metal.tint[0] = 0.923456789f;
	CHECK( metal.Valid() );
	idPTMaterialOverrides store;
	const unsigned int initial = store.Revision();
	CHECK( store.Set( "textures/metal", metal ) );
	CHECK( store.Revision() != initial );
	CHECK( *store.Preview( "TEXTURES/METAL" ) == metal );
	const unsigned int previewRevision = store.Revision();
	CHECK( store.Set( "textures/metal", metal ) );
	CHECK( store.Revision() == previewRevision );
	CHECK( store.Find( "textures/metal" )->Dirty() );

	ptMaterialOverride_t glass;
	glass.transmission = 1;
	glass.thin = 1;
	glass.absorption[2] = 7.123456789f;
	CHECK( store.Set( "textures/glass", glass ) );
	const idStr saveMetal = store.SerializeForSave( "textures/metal" );
	CHECK( saveMetal.Find( "textures/glass" ) < 0 );
	idPTMaterialOverrides reloaded;
	CHECK( Load( reloaded, saveMetal ) );
	CHECK( *reloaded.Preview( "textures/metal" ) == metal );
	CHECK( !reloaded.Preview( "textures/glass" ) );
	CHECK( !reloaded.Find( "textures/metal" )->Dirty() );
	// Producing a file does not mark a failed disk save as successful.
	CHECK( store.Find( "textures/metal" )->Dirty() );
	store.CommitSave( "textures/metal" );
	CHECK( !store.Find( "textures/metal" )->Dirty() );
	CHECK( store.Find( "textures/glass" )->Dirty() );
	const idStr saveGlass = store.SerializeForSave( "textures/glass" );
	CHECK( Load( reloaded, saveGlass ) );
	CHECK( *reloaded.Preview( "textures/metal" ) == metal );
	CHECK( *reloaded.Preview( "textures/glass" ) == glass );

	metal.roughness = 0.5f;
	CHECK( store.Set( "textures/metal", metal ) );
	CHECK( Load( reloaded, store.SerializeForSave( "textures/glass" ) ) );
	CHECK( reloaded.Preview( "textures/metal" )->roughness == 0.123456789f );
	CHECK( store.Find( "textures/metal" )->Dirty() );
	store.Revert( "textures/metal" );
	CHECK( store.Preview( "textures/metal" )->roughness == 0.123456789f );
	store.Reset( "textures/metal" );
	CHECK( !store.Preview( "textures/metal" ) );
	CHECK( store.Find( "textures/metal" )->Dirty() );
	CHECK( Load( reloaded, store.SerializeForSave( "textures/metal" ) ) );
	CHECK( !reloaded.Preview( "textures/metal" ) );
	store.Revert( "textures/metal" );
	CHECK( store.Preview( "textures/metal" ) );
	store.Reset( "textures/metal" );
	store.CommitSave( "textures/metal" );
	CHECK( !store.Find( "textures/metal" )->Dirty() );
	CHECK( !store.Preview( "textures/metal" ) );

	const char *invalid[] = {
		"", "ptMaterialOverrides 2 {}", "ptMaterialOverrides 1 {", "ptMaterialOverrides 1 {} trailing",
		"ptMaterialOverrides 1 { \"a\" { unknown 1 } }",
		"ptMaterialOverrides 1 { \"a\" { ior -1 } }",
		"ptMaterialOverrides 1 { \"a\" { thin 0.5 } }",
		"ptMaterialOverrides 1 { \"a\" { metallic 2 } }",
		"ptMaterialOverrides 1 { \"a\" { tint 1 1 2 } }",
		"ptMaterialOverrides 1 { \"a\" { absorption 0 -1 0 } }",
		"ptMaterialOverrides 1 { \"a\" { transmission garbage } }",
		"ptMaterialOverrides 1 { \"a\" {} \"A\" {} }",
		"ptMaterialOverrides 1 { \"a\" {"
	};
	CHECK( Load( reloaded, saveGlass ) );
	const unsigned int loadedRevision = reloaded.Revision();
	for ( int i = 0; i < (int)( sizeof( invalid ) / sizeof( invalid[0] ) ); i++ ) {
		CHECK( !Load( reloaded, invalid[i] ) );
		CHECK( reloaded.Revision() == loadedRevision );
		CHECK( *reloaded.Preview( "textures/glass" ) == glass );
	}
	ptMaterialOverride_t bad = glass;
	bad.roughness = std::numeric_limits<float>::quiet_NaN();
	CHECK( !store.Set( "bad", bad ) );
	bad = glass; bad.ior = std::numeric_limits<float>::infinity();
	CHECK( !store.Set( "bad", bad ) );
	bad = glass; bad.thin = 3;
	CHECK( !store.Set( "bad", bad ) );
	CHECK( !store.Set( "bad\"name", glass ) );
	CHECK( !store.Set( "bad\nname", glass ) );
	CHECK( !store.Set( "", glass ) );
	CHECK( Load( reloaded, "ptMaterialOverrides 1 {}" ) );
	CHECK( !reloaded.Preview( "textures/glass" ) );
	puts( "Physical material preview, isolation, reset, persistence and validation tests passed" );
	return 0;
}
