// STL headers must come before idlib: idlib/Str.h redefines snprintf/vsnprintf
// (use_idStr_snPrintf), which breaks the using-declarations in MSVC's <cstdio>.

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/CVarSystem.h"
#include "renderer_pt/pt_ui.h"
#include "renderer_pt/pt_render.h"
#include "renderer_pt/pt_quality_preset.h"
#include "renderer_pt/pt_physical.h"
#include "sys/sys_imgui.h"

#ifndef IMGUI_DISABLE

static const char *ptRequestedTab = NULL;
static idList<idCVar *> ptUIVariables;
static const char *ptUIUsage = "usage: r_ptUI [quality|settings"
#ifdef PT_PHYSICAL
	"|physical"
#endif
	"]";

static void PT_CollectUIVariable( const char *name ) {
	if ( idStr::Icmpn( name, "r_pt", 4 ) != 0 ) return;
	idCVar *var = cvarSystem->Find( name );
	if ( !var ) {
		common->Warning( "path tracer UI could not find registered CVar '%s'", name );
		return;
	}
	ptUIVariables.Append( var );
}

static void PT_RefreshUIVariables() {
	ptUIVariables.Clear();
	cvarSystem->CommandCompletion( PT_CollectUIVariable );
	ptUIVariables.Sort( []( idCVar *const *a, idCVar *const *b ) {
		return idStr::Icmp( ( *a )->GetName(), ( *b )->GetName() );
	} );
}

static void PT_DrawPreset( idCVar &var ) {
	if ( ImGui::BeginCombo( "##value", var.GetString() ) ) {
		for ( int i = 0; i < PT_QUALITY_PRESET_COUNT; i++ ) {
			const char *name = ptQualityPresets[i].name;
			if ( ImGui::Selectable( name, idStr::Icmp( name, var.GetString() ) == 0 ) ) var.SetString( name );
		}
		for ( int i = 0; i < PT_CUSTOM_QUALITY_PRESET_COUNT; i++ ) {
			const char *name = ptCustomQualityPresets[i];
			if ( ImGui::Selectable( name, idStr::Icmp( name, var.GetString() ) == 0 ) ) var.SetString( name );
		}
		ImGui::EndCombo();
	}
}

static void PT_DrawUIVariable( idCVar &var ) {
	ImGui::PushID( var.GetName() );
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex( 0 );
	ImGui::TextUnformatted( var.GetName() );
	if ( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", var.GetDescription() );
	ImGui::TableSetColumnIndex( 1 );
	ImGui::SetNextItemWidth( -1 );
	const int flags = var.GetFlags();
	ImGui::BeginDisabled( ( flags & ( CVAR_ROM | CVAR_INIT ) ) != 0 );
	if ( idStr::Icmp( var.GetName(), "r_ptPreset" ) == 0 ) {
		PT_DrawPreset( var );
	} else if ( flags & CVAR_BOOL ) {
		bool value = var.GetBool();
		if ( ImGui::Checkbox( "##value", &value ) ) var.SetBool( value );
	} else if ( flags & CVAR_INTEGER ) {
		int value = var.GetInteger();
		const bool changed = var.GetMinValue() < var.GetMaxValue()
			? ImGui::SliderInt( "##value", &value, (int)var.GetMinValue(), (int)var.GetMaxValue(), "%d", ImGuiSliderFlags_AlwaysClamp )
			: ImGui::InputInt( "##value", &value );
		if ( changed ) var.SetInteger( value );
	} else if ( flags & CVAR_FLOAT ) {
		float value = var.GetFloat();
		const bool changed = var.GetMinValue() < var.GetMaxValue()
			? ImGui::SliderFloat( "##value", &value, var.GetMinValue(), var.GetMaxValue(), "%.6g", ImGuiSliderFlags_AlwaysClamp )
			: ImGui::InputFloat( "##value", &value, 0, 0, "%.6g" );
		if ( changed ) {
			if ( !FLOAT_IS_NAN( value ) ) var.SetFloat( value );	// also rejects infinities
			else common->Warning( "path tracer UI requires a finite value for '%s'", var.GetName() );
		}
	} else if ( var.GetValueStrings() ) {
		if ( ImGui::BeginCombo( "##value", var.GetString() ) ) {
			for ( const char **value = var.GetValueStrings(); *value; value++ ) {
				if ( ImGui::Selectable( *value, idStr::Icmp( *value, var.GetString() ) == 0 ) ) var.SetString( *value );
			}
			ImGui::EndCombo();
		}
	} else {
		idList<char> value;
		value.SetNum( idStr::Length( var.GetString() ) + 512 );
		idStr::Copynz( value.Ptr(), var.GetString(), value.Num() );
		if ( ImGui::InputText( "##value", value.Ptr(), (size_t)value.Num(), ImGuiInputTextFlags_EnterReturnsTrue ) ) {
			var.SetString( value.Ptr() );
		}
	}
	if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) ) {
		ImGui::SetTooltip( "%s\nCurrent: %s", var.GetDescription(), var.GetString() );
	}
	ImGui::EndDisabled();
	ImGui::PopID();
}

static void PT_DrawQualityTab( bool capturing ) {
	ImGui::TextWrapped( "Presets change quality budgets only. Editing a budget selects the family's custom mode." );
	ImGui::BeginDisabled( capturing );
	if ( ImGui::BeginTable( "quality", 2, ImGuiTableFlags_SizingStretchProp ) ) {
		const char *names[] = { "r_ptDraw", "r_ptMode", "r_ptPreset", "r_ptRenderScale",
			"r_ptSamplesPerPixel", "r_ptMaxDepth", "r_ptMaxLightSamples", "r_ptDenoise", "r_ptAccumulation" };
		for ( const char *name : names ) {
			idCVar *var = cvarSystem->Find( name );
			if ( var ) PT_DrawUIVariable( *var );
			else common->Warning( "path tracer UI could not find quality CVar '%s'", name );
		}
		ImGui::EndTable();
	}
	ImGui::EndDisabled();
}

static void PT_DrawSettingsTab( bool capturing ) {
	static ImGuiTextFilter filter;
	filter.Draw( "Search name or description" );
	ImGui::TextWrapped( "Hover for descriptions. Ctrl-click sliders for exact values; press Enter to apply text values." );
	ImGui::BeginDisabled( capturing );
	if ( ImGui::BeginChild( "variables" ) ) {
		if ( ImGui::BeginTable( "settings", 2, ImGuiTableFlags_SizingStretchProp ) ) {
			for ( int v = 0; v < ptUIVariables.Num(); v++ ) {
				idCVar *var = ptUIVariables[v];
				const idStr searchable = idStr( var->GetName() ) + " " + var->GetDescription();
				if ( filter.PassFilter( searchable.c_str() ) ) PT_DrawUIVariable( *var );
			}
			ImGui::EndTable();
		}
	}
	ImGui::EndChild();
	ImGui::EndDisabled();
}

void R_PT_DrawUI() {
	using namespace D3::ImGuiHooks;
	bool open = true;
	ImGui::SetNextWindowSize( ImVec2( 720, 650 ), ImGuiCond_FirstUseEver );
	if ( ptRequestedTab ) ImGui::SetNextWindowCollapsed( false );
	if ( ImGui::Begin( "Path Tracer", &open ) ) {
		const bool capturing = RB_PT_IsCapturing();
		if ( capturing ) ImGui::TextWrapped( "Capture in progress: tuning is locked until capture completes or r_ptAbort." );
		if ( ImGui::BeginTabBar( "ptTabs" ) ) {
			if ( ImGui::BeginTabItem( "Quality", NULL, ptRequestedTab && idStr::Icmp( ptRequestedTab, "quality" ) == 0 ? ImGuiTabItemFlags_SetSelected : 0 ) ) {
				PT_DrawQualityTab( capturing );
				ImGui::EndTabItem();
			}
#ifdef PT_PHYSICAL
			if ( ImGui::BeginTabItem( "Physical", NULL, ptRequestedTab && idStr::Icmp( ptRequestedTab, "physical" ) == 0 ? ImGuiTabItemFlags_SetSelected : 0 ) ) {
				R_PT_DrawPhysicalControls();
				ImGui::EndTabItem();
			}
#endif
			if ( ImGui::BeginTabItem( "All CVars", NULL, ptRequestedTab && idStr::Icmp( ptRequestedTab, "settings" ) == 0 ? ImGuiTabItemFlags_SetSelected : 0 ) ) {
				PT_DrawSettingsTab( capturing );
				ImGui::EndTabItem();
			}
			ptRequestedTab = NULL;
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
	if ( !open ) CloseWindow( D3_ImGuiWin_PathTracer );
}

void R_PT_OpenUI( const char *tab ) {
	using namespace D3::ImGuiHooks;
	const char *requested = NULL;
	if ( tab ) {
		if ( idStr::Icmp( tab, "quality" ) == 0 ) requested = "quality";
		else if ( idStr::Icmp( tab, "settings" ) == 0 ) requested = "settings";
#ifdef PT_PHYSICAL
		else if ( idStr::Icmp( tab, "physical" ) == 0 ) requested = "physical";
#endif
		else {
			common->Warning( "%s (unknown tab '%s')", ptUIUsage, tab );
			return;
		}
	}
	ptRequestedTab = requested;
	PT_RefreshUIVariables();
	OpenWindow( D3_ImGuiWin_PathTracer );
	common->Printf( "Path tracer UI opened%s%s (%d CVars)\n", tab ? ": " : "", tab ? tab : "", ptUIVariables.Num() );
}
#endif

static void PT_UI_f( const idCmdArgs &args ) {
	using namespace D3::ImGuiHooks;
	if ( !IsImguiEnabled() ) {
		common->Warning( "r_ptUI requires an initialized ImGui context (build with IMGUI=ON)" );
		return;
	}
#ifndef IMGUI_DISABLE
	const char *tab = args.Argc() == 2 ? args.Argv( 1 ) : NULL;
	if ( args.Argc() > 2 ) {
		common->Warning( "%s", ptUIUsage );
		return;
	}
	if ( !tab && ( GetOpenWindowsMask() & D3_ImGuiWin_PathTracer ) ) {
		CloseWindow( D3_ImGuiWin_PathTracer );
		common->Printf( "Path tracer UI closed\n" );
		return;
	}
	R_PT_OpenUI( tab );
#endif
}

void R_PT_InitUI() {
	cmdSystem->AddCommand( "r_ptUI", PT_UI_f, CMD_FL_RENDERER, "toggle path tracer controls, or open a quality/settings"
#ifdef PT_PHYSICAL
		"/physical"
#endif
		" tab" );
}
