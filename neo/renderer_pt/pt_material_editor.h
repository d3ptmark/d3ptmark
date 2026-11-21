#ifndef __PT_MATERIAL_EDITOR_H__
#define __PT_MATERIAL_EDITOR_H__

#include "idlib/containers/List.h"
#include "idlib/Lexer.h"
#include "idlib/math/Math.h"
#include "renderer_pt/pt_types.h"

struct ptMaterialOverride_t {
	float metallic, roughness, transmission, ior, thin, thickness;
	float absorption[3], tint[3];

	ptMaterialOverride_t() : metallic( 0 ), roughness( 0.7f ), transmission( 0 ), ior( 1.5f ), thin( 0 ), thickness( 0.005f ) {
		for ( int c = 0; c < 3; c++ ) { absorption[c] = 0; tint[c] = 1; }
	}

	bool Valid() const {
		// The engine uses -ffinite-math-only, so arithmetic NaN checks are insufficient.
		const float scalars[] = { metallic, roughness, transmission, ior, thin, thickness };
		for ( int i = 0; i < 6; i++ ) if ( FLOAT_IS_NAN( scalars[i] ) ) return false;
		if ( !( metallic >= 0 && metallic <= 1 && roughness >= 0.04f && roughness <= 1
			&& transmission >= 0 && transmission <= 1 && ior >= 1 && ior <= 3
			&& thickness >= 0 && thickness <= 100 && ( thin == 0 || thin == 1 || thin == 2 ) ) ) return false;
		for ( int c = 0; c < 3; c++ ) {
			if ( FLOAT_IS_NAN( absorption[c] ) || FLOAT_IS_NAN( tint[c] ) ) return false;
			if ( !( absorption[c] >= 0 && absorption[c] <= 1000000 && tint[c] >= 0 && tint[c] <= 1 ) ) return false;
		}
		return true;
	}

	bool operator==( const ptMaterialOverride_t &b ) const {
		return metallic == b.metallic && roughness == b.roughness && transmission == b.transmission
			&& ior == b.ior && thin == b.thin && thickness == b.thickness
			&& absorption[0] == b.absorption[0] && absorption[1] == b.absorption[1] && absorption[2] == b.absorption[2]
			&& tint[0] == b.tint[0] && tint[1] == b.tint[1] && tint[2] == b.tint[2];
	}
};

inline float *PT_MaterialField( ptMaterialOverride_t &value, const char *name, int &count ) {
	count = 1;
	if ( !idStr::Icmp( name, "metallic" ) ) return &value.metallic;
	if ( !idStr::Icmp( name, "roughness" ) ) return &value.roughness;
	if ( !idStr::Icmp( name, "transmission" ) ) return &value.transmission;
	if ( !idStr::Icmp( name, "ior" ) ) return &value.ior;
	if ( !idStr::Icmp( name, "thin" ) ) return &value.thin;
	if ( !idStr::Icmp( name, "thickness" ) ) return &value.thickness;
	count = 3;
	if ( !idStr::Icmp( name, "absorption" ) ) return value.absorption;
	if ( !idStr::Icmp( name, "tint" ) ) return value.tint;
	return NULL;
}

inline bool PT_ValidMaterialOverrideName( const char *name ) {
	if ( !name || !name[0] || idStr::Length( name ) > 1023 ) return false;
	for ( const unsigned char *c = (const unsigned char *)name; *c; c++ ) {
		if ( *c < 32 || *c == '"' || *c == '\\' ) return false;
	}
	return true;
}

class idPTMaterialOverrides {
public:
	struct entry_t {
		idStr name;
		ptMaterialOverride_t preview, saved;
		bool previewEnabled, savedEnabled;
		entry_t() : previewEnabled( false ), savedEnabled( false ) {}
		bool Dirty() const {
			return previewEnabled != savedEnabled || ( previewEnabled && !( preview == saved ) );
		}
	};

	idPTMaterialOverrides() : revision( 1 ) {}
	unsigned int Revision() const { return revision; }
	const entry_t *Find( const char *name ) const {
		for ( int i = 0; i < entries.Num(); i++ ) if ( !entries[i].name.Icmp( name ) ) return &entries[i];
		return NULL;
	}
	const ptMaterialOverride_t *Preview( const char *name ) const {
		const entry_t *entry = Find( name );
		return entry && entry->previewEnabled ? &entry->preview : NULL;
	}
	bool Set( const char *name, const ptMaterialOverride_t &value ) {
		if ( !PT_ValidMaterialOverrideName( name ) || !value.Valid() || ( !Find( name ) && entries.Num() >= 4096 ) ) return false;
		entry_t &entry = Get( name );
		if ( !entry.previewEnabled || !( entry.preview == value ) ) {
			entry.preview = value;
			entry.previewEnabled = true;
			revision++;
		}
		return true;
	}
	void Reset( const char *name ) {
		if ( !Find( name ) ) return;
		entry_t &entry = Get( name );
		if ( entry.previewEnabled ) { entry.previewEnabled = false; revision++; }
	}
	void Revert( const char *name ) {
		if ( !Find( name ) ) return;
		entry_t &entry = Get( name );
		if ( entry.Dirty() ) {
			entry.preview = entry.saved;
			entry.previewEnabled = entry.savedEnabled;
			revision++;
		}
	}
	void CommitSave( const char *name ) {
		if ( !Find( name ) ) return;
		entry_t &entry = Get( name );
		entry.saved = entry.preview;
		entry.savedEnabled = entry.previewEnabled;
	}

	idStr SerializeForSave( const char *selected ) const {
		idStr text = "ptMaterialOverrides 1 {\n";
		const char *fields[] = { "metallic", "roughness", "transmission", "ior", "thin", "thickness", "absorption", "tint" };
		for ( int i = 0; i < entries.Num(); i++ ) {
			const entry_t &entry = entries[i];
			const bool isSelected = !entry.name.Icmp( selected );
			if ( !( isSelected ? entry.previewEnabled : entry.savedEnabled ) ) continue;
			ptMaterialOverride_t value = isSelected ? entry.preview : entry.saved;
			text += "\""; text += entry.name; text += "\" {\n";
			for ( int field = 0; field < 8; field++ ) {
				int count;
				float *values = PT_MaterialField( value, fields[field], count );
				text += "\t"; text += fields[field];
				for ( int c = 0; c < count; c++ ) {
					char number[32];
					idStr::snPrintf( number, sizeof( number ), " %.9g", values[c] );
					text += number;
				}
				text += "\n";
			}
			text += "}\n";
		}
		text += "}\n";
		return text;
	}

	bool Load( const char *text, int length, idStr &error ) {
		idLexer lexer( text, length, "pt_materials.cfg",
			LEXFL_NOERRORS | LEXFL_NOWARNINGS | LEXFL_NOFATALERRORS | LEXFL_NOSTRINGCONCAT );
		idList<entry_t> loaded;
		idToken name, token;
		bool valid = lexer.ExpectTokenString( "ptMaterialOverrides" ) && lexer.ExpectTokenString( "1" )
			&& lexer.ExpectTokenString( "{" );
		bool closed = false;
		while ( valid && lexer.ReadToken( &name ) ) {
			if ( name == "}" ) { closed = true; break; }
			valid = PT_ValidMaterialOverrideName( name.c_str() ) && lexer.ExpectTokenString( "{" ) && loaded.Num() < 4096;
			for ( int i = 0; i < loaded.Num(); i++ ) if ( !loaded[i].name.Icmp( name ) ) valid = false;
			entry_t entry;
			entry.name = name;
			bool materialClosed = false;
			while ( valid && lexer.ReadToken( &token ) ) {
				if ( token == "}" ) { materialClosed = true; break; }
				int count;
				float *values = PT_MaterialField( entry.preview, token.c_str(), count );
				if ( !values ) { valid = false; break; }
				for ( int c = 0; c < count; c++ ) values[c] = lexer.ParseFloat();
				valid = !lexer.HadError() && entry.preview.Valid();
			}
			valid &= materialClosed;
			if ( valid ) {
				entry.saved = entry.preview;
				entry.previewEnabled = entry.savedEnabled = true;
				loaded.Append( entry );
			}
		}
		if ( !valid || !closed || lexer.HadError() || lexer.ReadToken( &token ) ) {
			error = va( "Invalid material overrides near line %d (material '%s', property '%s')",
				lexer.GetLineNum(), name.c_str(), token.c_str() );
			return false;
		}
		entries = loaded;
		revision++;
		error.Clear();
		return true;
	}

private:
	entry_t &Get( const char *name ) {
		for ( int i = 0; i < entries.Num(); i++ ) if ( !entries[i].name.Icmp( name ) ) return entries[i];
		entry_t entry;
		entry.name = name;
		return entries[entries.Append( entry )];
	}
	idList<entry_t> entries;
	unsigned int revision;
};

#ifdef PT_PHYSICAL
class idMaterial;
struct ptMaterialProfile_t;
struct viewDef_s;
void R_PT_InitMaterialEditor();
void R_PT_UpdateMaterialPick( const struct viewDef_s *view );
void R_PT_DrawMaterialEditor();
unsigned int R_PT_MaterialOverrideRevision();
const ptMaterialOverride_t *R_PT_MaterialOverride( const char *name );
ptMaterialProfile_t R_PT_MaterialProfile( const idMaterial *material );
#endif

#endif
