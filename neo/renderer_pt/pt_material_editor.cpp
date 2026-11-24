#include "sys/platform.h"

#ifdef PT_PHYSICAL

#include "framework/Common.h"
#include "framework/CmdSystem.h"
#include "framework/FileSystem.h"
#include "framework/File.h"
#include "renderer/tr_local.h"
#include "renderer/RenderWorld_local.h"
#include "renderer_pt/pt_material_editor.h"
#include "renderer_pt/pt_material_profiles.h"
#include "renderer_pt/pt_render.h"
#include "renderer_pt/pt_ui.h"
#include "sys/sys_imgui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif

static idPTMaterialOverrides ptMaterialOverrides;
static idStr ptSelectedMaterial;
static idStr ptMaterialStatus;
static bool ptMaterialStatusError;
static bool ptMaterialPickPending;
static const char *PT_MATERIAL_OVERRIDE_FILE = "pt_materials.cfg";
static idCVar r_ptPhysicalAutoMaterials( "r_ptPhysicalAutoMaterials", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
	"automatically calibrate legacy physical materials; explicit ptMaterial blocks and editor overrides take priority" );

ptMaterialProfile_t R_PT_MaterialProfile( const idMaterial *material ) {
	if ( !r_ptPhysicalAutoMaterials.GetBool() || material->GetPhysicalMaterial().enabled ) return ptMaterialProfile_t();
	ptMaterialFacts_t facts = { material->GetName(), material->GetDescription(), material->GetSurfaceType(), false, false,
		material->Coverage() == MC_TRANSLUCENT, false };
	idStr name = material->GetName();
	name.ToLower();
	name.BackSlashesToSlashes();
	const bool pane = PT_IsWindowPane( name );
	facts.excluded = !material->IsDrawn() || material->HasGui() || material->Deform() != DFRM_NONE
		|| ( material->GetSort() == SS_DECAL && !pane )
		|| ( material->HasSubview() && name != "textures/washroom/mirror" && !pane )
		|| !name.Icmpn( "textures/decals/", 16 )
		|| ( !name.Icmpn( "textures/sfx/", 13 ) && name != "textures/sfx/chrome_mal_pipe" && !pane )
		|| !name.Icmpn( "textures/guisurfaces/", 21 ) || !name.Icmpn( "guis/", 5 );
	for ( int i = 0; i < material->GetNumStages(); i++ ) {
		const shaderStage_t *stage = material->GetStage( i );
		if ( !stage || stage->newStage ) continue;
		facts.interaction |= stage->lighting == SL_DIFFUSE || stage->lighting == SL_SPECULAR;
		facts.specular |= stage->lighting == SL_SPECULAR;
	}
	ptMaterialProfile_t profile = PT_ChooseMaterialProfile( facts );
	if ( profile.label && profile.value.transmission == 0 ) profile.value.ior = cvarSystem->GetCVarFloat( "r_ptPhysicalIor" );
	return profile;
}

static void PT_MaterialFailure( const idStr &message ) {
	ptMaterialStatus = message;
	ptMaterialStatusError = true;
	common->Warning( "Physical material editor: %s", message.c_str() );
}

static bool PT_MaterialEditable() {
	if ( RB_PT_IsCapturing() ) {
		PT_MaterialFailure( "Material editing is locked during capture; use r_ptAbort first" );
		return false;
	}
	return true;
}

static ptMaterialOverride_t PT_DefaultMaterial( const idMaterial *material ) {
	ptMaterialOverride_t value;
	const physicalMaterialInfo_t &authored = material->GetPhysicalMaterial();
	const ptMaterialProfile_t profile = R_PT_MaterialProfile( material );
	if ( profile.label ) value = profile.value;
	if ( authored.enabled ) {
		value.transmission = authored.transmission;
		value.ior = authored.ior;
		value.thin = authored.thin;
		value.thickness = authored.thickness;
		if ( authored.metallic >= 0 ) value.metallic = authored.metallic;
		if ( authored.roughness >= 0 ) value.roughness = authored.roughness;
		for ( int c = 0; c < 3; c++ ) value.absorption[c] = authored.absorption[c];
	}
	return value;
}

static const idMaterial *PT_SelectedMaterial() {
	if ( ptSelectedMaterial.IsEmpty() ) return NULL;
	const idMaterial *material = declManager->FindMaterial( ptSelectedMaterial, false );
	if ( !material || material->TestMaterialFlag( MF_DEFAULTED ) ) {
		PT_MaterialFailure( va( "Material '%s' is unavailable", ptSelectedMaterial.c_str() ) );
		ptSelectedMaterial.Clear();
		return NULL;
	}
	return material;
}

static ptMaterialOverride_t PT_CurrentMaterial( const idMaterial *material ) {
	const ptMaterialOverride_t *preview = ptMaterialOverrides.Preview( material->GetName() );
	return preview ? *preview : PT_DefaultMaterial( material );
}

static void PT_PreviewMaterial( const ptMaterialOverride_t &value ) {
	if ( !ptMaterialOverrides.Set( ptSelectedMaterial, value ) ) {
		PT_MaterialFailure( "Invalid material values; preview was not changed" );
		return;
	}
	ptMaterialStatus = "Live preview applied. Use Save material to keep it after restart.";
	ptMaterialStatusError = false;
}

static void PT_LoadMaterialOverrides() {
	const idStr path = fileSystem->RelativePathToOSPath( PT_MATERIAL_OVERRIDE_FILE, "fs_configpath" );
	errno = 0;
	idFile *file = fileSystem->OpenExplicitFileRead( path );
	if ( !file ) {
		if ( errno != ENOENT ) PT_MaterialFailure( va( "Cannot read %s: %s", path.c_str(), strerror( errno ) ) );
		return;
	}
	const int length = file->Length();
	if ( length < 0 || length > 1024 * 1024 ) {
		fileSystem->CloseFile( file );
		PT_MaterialFailure( "Material override file exceeds the 1 MiB limit" );
		return;
	}
	idList<char> text;
	text.SetNum( length + 1 );
	const int read = file->Read( text.Ptr(), length );
	fileSystem->CloseFile( file );
	text[length] = '\0';
	idStr error;
	if ( read != length ) {
		PT_MaterialFailure( va( "Short read from %s", path.c_str() ) );
	} else if ( !ptMaterialOverrides.Load( text.Ptr(), length, error ) ) {
		PT_MaterialFailure( error );
	} else {
		ptMaterialStatus = "Saved physical material overrides loaded.";
		ptMaterialStatusError = false;
		common->Printf( "Physical material editor: loaded %s\n", path.c_str() );
	}
}

static void PT_SaveSelectedMaterial() {
	const idMaterial *material = PT_SelectedMaterial();
	if ( !material || !PT_MaterialEditable() ) return;
	const idStr text = ptMaterialOverrides.SerializeForSave( ptSelectedMaterial );
	if ( text.Length() > 1024 * 1024 ) {
		PT_MaterialFailure( "Saved material overrides would exceed the 1 MiB limit" );
		return;
	}
	const idStr temporary = idStr( PT_MATERIAL_OVERRIDE_FILE ) + ".tmp";
	idFile *file = fileSystem->OpenFileWrite( temporary, "fs_configpath" );
	if ( !file ) {
		PT_MaterialFailure( "Cannot create temporary material override file" );
		return;
	}
	// Flush has no error result; unbuffered writes surface disk failures through Write.
	file->ForceFlush();
	const int written = file->Write( text.c_str(), text.Length() );
	file->Flush();
	fileSystem->CloseFile( file );
	const idStr source = fileSystem->RelativePathToOSPath( temporary, "fs_configpath" );
	const idStr destination = fileSystem->RelativePathToOSPath( PT_MATERIAL_OVERRIDE_FILE, "fs_configpath" );
	if ( written != text.Length() ) {
		remove( source.c_str() );
		PT_MaterialFailure( "Short write: previous saved material overrides were preserved" );
		return;
	}
#ifdef _WIN32
	const bool replaced = MoveFileExA( source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) != 0;
#else
	const bool replaced = rename( source.c_str(), destination.c_str() ) == 0;
#endif
	if ( !replaced ) {
		remove( source.c_str() );
		PT_MaterialFailure( va( "Cannot replace %s; previous saved overrides were preserved", destination.c_str() ) );
		return;
	}
	ptMaterialOverrides.CommitSave( ptSelectedMaterial );
	ptMaterialStatus = va( "Saved %s to %s", material->GetName(), destination.c_str() );
	ptMaterialStatusError = false;
	common->Printf( "Physical material editor: %s\n", ptMaterialStatus.c_str() );
}

static bool PT_RequestMaterialPick() {
	if ( !PT_MaterialEditable() ) return false;
	ptMaterialPickPending = true;
	ptMaterialStatus = "Waiting for a main 3D view to pick its center surface.";
	ptMaterialStatusError = false;
	return true;
}

void R_PT_UpdateMaterialPick( const viewDef_t *view ) {
	if ( !ptMaterialPickPending || view->isSubview || !view->renderWorld ) return;
	ptMaterialPickPending = false;
	if ( !PT_MaterialEditable() ) return;
	// Match r_showSurfaceInfo's geometry trace, including translucent materials.
	const idVec3 direction = view->renderView.viewaxis[0];
	idVec3 start = view->renderView.vieworg + direction * 16;
	const idVec3 end = start + direction * 4096;
	for ( int pass = 0; pass < 16; pass++ ) {
		modelTrace_t hit;
		if ( !view->renderWorld->Trace( hit, start, end, 0, false, true ) ) break;
		if ( hit.material && hit.material->IsDrawn() && !( hit.entity && hit.entity->weaponDepthHack ) ) {
			ptSelectedMaterial = hit.material->GetName();
			ptMaterialStatus = "Selection locked. Edits affect every instance of this material.";
			ptMaterialStatusError = false;
			common->Printf( "Physical material editor: selected %s\n", ptSelectedMaterial.c_str() );
			return;
		}
		start = hit.point + direction * 0.1f;
	}
	PT_MaterialFailure( "No drawable material at the view center; previous selection was kept" );
}

static void PT_MaterialPick_f( const idCmdArgs &args ) {
	if ( !PT_RequestMaterialPick() ) return;
	using namespace D3::ImGuiHooks;
	if ( IsImguiEnabled() ) R_PT_OpenUI( "physical" );
}

static void PT_Material_f( const idCmdArgs &args ) {
	if ( args.Argc() < 2 ) {
		common->Printf( "r_ptMaterial select <name> | set <property> <value(s)> | reset | revert | save | status\n"
			"r_ptMaterialPick selects the center surface and opens the Physical tab.\n" );
		return;
	}
	if ( !idStr::Icmp( args.Argv( 1 ), "status" ) && args.Argc() == 2 ) {
		const idMaterial *material = PT_SelectedMaterial();
		if ( !material ) { common->Printf( "Physical material editor: no material selected\n" ); return; }
		const ptMaterialOverride_t value = PT_CurrentMaterial( material );
		const idPTMaterialOverrides::entry_t *entry = ptMaterialOverrides.Find( ptSelectedMaterial );
		common->Printf( "Physical material %s: override=%d dirty=%d metallic=%g roughness=%g transmission=%g ior=%g thin=%g thickness=%g tint=(%g %g %g) absorption=(%g %g %g)\n",
			ptSelectedMaterial.c_str(), ptMaterialOverrides.Preview( ptSelectedMaterial ) != NULL, entry && entry->Dirty(),
			value.metallic, value.roughness, value.transmission, value.ior, value.thin, value.thickness,
			value.tint[0], value.tint[1], value.tint[2], value.absorption[0], value.absorption[1], value.absorption[2] );
		const ptMaterialProfile_t profile = R_PT_MaterialProfile( material );
		common->Printf( "Material defaults: %s\n", profile.label ? profile.label :
			material->GetPhysicalMaterial().enabled ? "Authored ptMaterial" : "Legacy translation" );
		return;
	}
	if ( !PT_MaterialEditable() ) return;
	if ( !idStr::Icmp( args.Argv( 1 ), "select" ) && args.Argc() == 3 ) {
		if ( !PT_ValidMaterialOverrideName( args.Argv( 2 ) ) ) { PT_MaterialFailure( "Invalid material name" ); return; }
		const idMaterial *material = declManager->FindMaterial( args.Argv( 2 ), false );
		if ( !material || material->TestMaterialFlag( MF_DEFAULTED ) ) { PT_MaterialFailure( "Requested material was not found" ); return; }
		ptSelectedMaterial = material->GetName();
		ptMaterialPickPending = false;
		ptMaterialStatus = "Material selected by name.";
		ptMaterialStatusError = false;
		return;
	}
	const idMaterial *material = PT_SelectedMaterial();
	if ( !material ) { PT_MaterialFailure( "Select a material first" ); return; }
	if ( !idStr::Icmp( args.Argv( 1 ), "set" ) && args.Argc() >= 4 ) {
		ptMaterialOverride_t value = PT_CurrentMaterial( material );
		int count;
		float *values = PT_MaterialField( value, args.Argv( 2 ), count );
		if ( !values || args.Argc() != count + 3 ) { PT_MaterialFailure( "Unknown property or wrong number of values" ); return; }
		for ( int c = 0; c < count; c++ ) {
			char *end;
			values[c] = strtof( args.Argv( c + 3 ), &end );
			if ( end == args.Argv( c + 3 ) || *end ) { PT_MaterialFailure( "Material values must be finite numbers in range" ); return; }
		}
		PT_PreviewMaterial( value );
	} else if ( !idStr::Icmp( args.Argv( 1 ), "reset" ) && args.Argc() == 2 ) {
		ptMaterialOverrides.Reset( ptSelectedMaterial );
		ptMaterialStatus = "Material defaults restored. Save to remove a previously saved override.";
		ptMaterialStatusError = false;
	} else if ( !idStr::Icmp( args.Argv( 1 ), "revert" ) && args.Argc() == 2 ) {
		ptMaterialOverrides.Revert( ptSelectedMaterial );
		ptMaterialStatus = "Unsaved changes discarded.";
		ptMaterialStatusError = false;
	} else if ( !idStr::Icmp( args.Argv( 1 ), "save" ) && args.Argc() == 2 ) {
		PT_SaveSelectedMaterial();
	} else {
		PT_MaterialFailure( "Invalid r_ptMaterial arguments; run r_ptMaterial for usage" );
	}
}

void R_PT_InitMaterialEditor() {
	PT_LoadMaterialOverrides();
	cmdSystem->AddCommand( "r_ptMaterialPick", PT_MaterialPick_f, CMD_FL_RENDERER, "pick the center material for physical tuning" );
	cmdSystem->AddCommand( "r_ptMaterial", PT_Material_f, CMD_FL_RENDERER, "preview, reset, revert, save or inspect physical material overrides" );
}

unsigned int R_PT_MaterialOverrideRevision() {
	return ptMaterialOverrides.Revision() * 2u + ( r_ptPhysicalAutoMaterials.GetBool() ? 1u : 0u );
}
const ptMaterialOverride_t *R_PT_MaterialOverride( const char *name ) { return ptMaterialOverrides.Preview( name ); }

void R_PT_DrawMaterialEditor() {
	if ( !ImGui::CollapsingHeader( "Material inspector", ImGuiTreeNodeFlags_DefaultOpen ) ) return;
	bool automatic = r_ptPhysicalAutoMaterials.GetBool();
	if ( ImGui::Checkbox( "Automatic material calibration", &automatic ) ) r_ptPhysicalAutoMaterials.SetBool( automatic );
	if ( ImGui::Button( "Pick center material" ) ) PT_RequestMaterialPick();
	ImGui::TextWrapped( "Aim first, then pick. Selection stays locked while editing. All instances sharing the material are affected." );
	const idMaterial *material = PT_SelectedMaterial();
	if ( material ) {
		ImGui::TextWrapped( "%s", material->GetName() );
		const idPTMaterialOverrides::entry_t *entry = ptMaterialOverrides.Find( ptSelectedMaterial );
		ImGui::Text( "%s%s", ptMaterialOverrides.Preview( ptSelectedMaterial ) ? "Physical override" : "Material defaults",
			entry && entry->Dirty() ? " (unsaved)" : "" );
		const ptMaterialProfile_t profile = R_PT_MaterialProfile( material );
		ImGui::Text( "Default profile: %s", profile.label ? profile.label :
			material->GetPhysicalMaterial().enabled ? "Authored ptMaterial" : "Legacy translation" );
		ptMaterialOverride_t value = PT_CurrentMaterial( material );
		bool changed = ImGui::SliderFloat( "Metallic", &value.metallic, 0, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp );
		changed |= ImGui::SliderFloat( "Material roughness", &value.roughness, 0.04f, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp );
		changed |= ImGui::SliderFloat( "Transmission", &value.transmission, 0, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp );
		changed |= ImGui::SliderFloat( "Material IOR", &value.ior, 1, 3, "%.3f", ImGuiSliderFlags_AlwaysClamp );
		int thin = (int)value.thin;
		if ( ImGui::Combo( "Transmission model", &thin, "Solid dielectric\0Thin glass\0Diffuse sheet\0" ) ) { value.thin = thin; changed = true; }
		changed |= ImGui::DragFloat( "Sheet thickness (m)", &value.thickness, 0.0001f, 0, 100, "%.4f", ImGuiSliderFlags_AlwaysClamp );
		changed |= ImGui::DragFloat3( "Absorption RGB (/m)", value.absorption, 0.05f, 0, 1000000, "%.3f", ImGuiSliderFlags_AlwaysClamp );
		changed |= ImGui::ColorEdit3( "Linear base tint", value.tint, ImGuiColorEditFlags_Float );
		if ( changed ) PT_PreviewMaterial( value );
		if ( ImGui::Button( "Reset to defaults" ) ) {
			ptMaterialOverrides.Reset( ptSelectedMaterial );
			ptMaterialStatus = "Material defaults restored. Save to remove a previously saved override.";
			ptMaterialStatusError = false;
		}
		ImGui::SameLine();
		if ( ImGui::Button( "Revert unsaved" ) ) {
			ptMaterialOverrides.Revert( ptSelectedMaterial );
			ptMaterialStatus = "Unsaved changes discarded.";
			ptMaterialStatusError = false;
		}
		if ( ImGui::Button( "Save material" ) ) PT_SaveSelectedMaterial();
		ImGui::TextWrapped( "Save writes only this material to your config-side pt_materials.cfg, never to .mtr assets. Other unsaved previews are not saved." );
		ImGui::TextWrapped( "Metallic/roughness replace legacy specular-map estimates. Tint modulates linear base color; color glass with absorption. Solid glass needs closed meshes. Picking is geometric, not alpha-texel aware." );
	}
	if ( !ptMaterialStatus.IsEmpty() ) {
		if ( ptMaterialStatusError ) ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1, 0.35f, 0.25f, 1 ) );
		ImGui::TextWrapped( "%s", ptMaterialStatus.c_str() );
		if ( ptMaterialStatusError ) ImGui::PopStyleColor();
	}
	ImGui::Separator();
}

#endif
