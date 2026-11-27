/*
===============================================================================

	OpenCL C port of pt_cuda_material.cuh.

===============================================================================
*/

/*
========================
PT_IsModulation
========================
*/
PT_INLINE bool PT_IsModulation( int blendMode ) {
	return blendMode == PT_BLEND_MODULATE || blendMode == PT_BLEND_MODULATE_INVERSE;
}

/*
========================
PT_ModulationFactor
========================
*/
PT_INLINE float PT_ModulationFactor( float linearSource, int blendMode ) {
	if ( blendMode != PT_BLEND_MODULATE_INVERSE ) return linearSource;
	// Legacy framebuffer blending complements the gamma-encoded source, not linear radiance.
	const float source = pow( fmax( linearSource, 0.0f ), 1.0f / 2.2f );
	return PT_SrgbToLinear( fmax( 0.0f, 1.0f - source ) );
}

/*
========================
PT_ApplyTexTransformXf

PT_ApplyTexTransform( const float *xf, ... ) overload
========================
*/
PT_INLINE void PT_ApplyTexTransformXf( __global const float *xf, float *tu, float *tv ) {
	if ( xf[0] == 1.0f && xf[1] == 0.0f && xf[2] == 0.0f
		&& xf[3] == 0.0f && xf[4] == 1.0f && xf[5] == 0.0f ) {
		return;
	}
	const float u = xf[0] * *tu + xf[1] * *tv + xf[2];
	const float v = xf[3] * *tu + xf[4] * *tv + xf[5];
	*tu = u - floor( u );
	*tv = v - floor( v );
}

/*
========================
PT_ApplyTexTransform
========================
*/
PT_INLINE void PT_ApplyTexTransform( __global const ptMaterial_t *material, float *tu, float *tv ) {
	PT_ApplyTexTransformXf( material->texTransform, tu, tv );
}

/*
========================
PT_ApplyVertexColorMode

PT_ApplyVertexColor( int mode, ... ) overload
========================
*/
PT_INLINE void PT_ApplyVertexColorMode(
	int mode, __global const ptVertex_t *vertices, __global const ptTriangle_t *tri,
	float w, float u, float v, float *color
) {
	if ( !mode ) return;
	__global const ptVertex_t *v0 = &vertices[tri->vertexIndices[0]];
	__global const ptVertex_t *v1 = &vertices[tri->vertexIndices[1]];
	__global const ptVertex_t *v2 = &vertices[tri->vertexIndices[2]];
	for ( int c = 0; c < 4; c++ ) {
		float vertexColor = w * v0->color[c] + u * v1->color[c] + v * v2->color[c];
		if ( mode == 2 ) vertexColor = 1.0f - vertexColor;
		color[c] *= c < 3 ? PT_SrgbToLinear( vertexColor ) : vertexColor;
	}
}

/*
========================
PT_ApplyVertexColor
========================
*/
PT_INLINE void PT_ApplyVertexColor(
	__global const ptMaterial_t *material, __global const ptVertex_t *vertices, __global const ptTriangle_t *tri,
	float w, float u, float v, float *color
) {
	PT_ApplyVertexColorMode( material->useVertexColor, vertices, tri, w, u, v, color );
}

/*
========================
PT_IsLitDecal
========================
*/
PT_INLINE bool PT_IsLitDecal( __global const ptMaterial_t *material ) {
	return material->decalOverlay && !material->isAmbientOnly && material->alphaTest == 0.0f
		&& material->transmission == 0.0f && material->blendMode == PT_BLEND_OPAQUE;
}

/*
========================
PT_AdditiveOpacity
========================
*/
PT_INLINE float PT_AdditiveOpacity( __global const ptMaterial_t *material, float alpha ) {
	return material->additiveUsesAlpha ? alpha : 1.0f;
}

/*
===============================================================================

	Texture Sampling (bilinear filtering)

===============================================================================
*/

/*
========================
PT_TextureResident
========================
*/
PT_INLINE bool PT_TextureResident( const ptClTextures_t *textures, int texIndex ) {
	return texIndex >= 0 && texIndex < MAX_TEXTURES
		&& textures->desc[texIndex].width > 0 && textures->desc[texIndex].height > 0;
}

/*
========================
PT_Texel
========================
*/
PT_INLINE float4 PT_Texel( const ptClTextures_t *textures, const ptClTexture_t *tex, int index ) {
	return convert_float4( textures->texels[(size_t)tex->offset + (size_t)index] );
}

/*
========================
SampleTexture
========================
*/
PT_INLINE void SampleTexture( const ptClTextures_t *textures, int texIndex, float u, float v, float *color ) {
	if ( !PT_TextureResident( textures, texIndex ) ) {
		color[0] = color[1] = color[2] = color[3] = 1.0f;
		return;
	}

	const ptClTexture_t tex = textures->desc[texIndex];

	// wrap texture coordinates
	u = u - floor( u );
	v = v - floor( v );

	// Normalized GL sampling places texel centers at (i + 0.5) / size.
	float x = u * (float)tex.width - 0.5f;
	float y = v * (float)tex.height - 0.5f;
	const int ix = (int)floor( x );
	const int iy = (int)floor( y );
	int x0 = ( ix % tex.width + tex.width ) % tex.width;
	int y0 = ( iy % tex.height + tex.height ) % tex.height;
	int x1 = ( x0 + 1 ) % tex.width;
	int y1 = ( y0 + 1 ) % tex.height;

	float fx = x - floor( x );
	float fy = y - floor( y );

	// bilinear interpolation
	const float4 t00 = PT_Texel( textures, &tex, y0 * tex.width + x0 );
	const float4 t10 = PT_Texel( textures, &tex, y0 * tex.width + x1 );
	const float4 t01 = PT_Texel( textures, &tex, y1 * tex.width + x0 );
	const float4 t11 = PT_Texel( textures, &tex, y1 * tex.width + x1 );
	const float d00[4] = { t00.x, t00.y, t00.z, t00.w };
	const float d10[4] = { t10.x, t10.y, t10.z, t10.w };
	const float d01[4] = { t01.x, t01.y, t01.z, t01.w };
	const float d11[4] = { t11.x, t11.y, t11.z, t11.w };

	for ( int c = 0; c < 4; c++ ) {
		float v0 = d00[c] * ( 1.0f - fx ) + d10[c] * fx;
		float v1 = d01[c] * ( 1.0f - fx ) + d11[c] * fx;
		color[c] = ( v0 * ( 1.0f - fy ) + v1 * fy ) / 255.0f;
	}
}

/*
========================
SampleTextureSrgb

SampleTexture with the color channels decoded from sRGB (albedo, glow and light images)
========================
*/
PT_INLINE void SampleTextureSrgb( const ptClTextures_t *textures, int texIndex, float u, float v, float *color ) {
	SampleTexture( textures, texIndex, u, v, color );
	color[0] = PT_SrgbToLinear( color[0] );
	color[1] = PT_SrgbToLinear( color[1] );
	color[2] = PT_SrgbToLinear( color[2] );
}

/*
========================
PT_SampleEmission
========================
*/
PT_INLINE void PT_SampleEmission(
	__global const ptMaterial_t *material, const ptClTextures_t *textures,
	__global const ptVertex_t *vertices, __global const ptTriangle_t *tri, float w, float u, float v,
	float tu, float tv, float *emission
) {
	if ( material->emission[0] == 0.0f && material->emission[1] == 0.0f && material->emission[2] == 0.0f ) {
		emission[0] = emission[1] = emission[2] = 0.0f;
		return;
	}
	float glow[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	if ( material->emissionTexture >= 0 && textures ) {
		PT_ApplyTexTransformXf( material->emissionTransform, &tu, &tv );
		SampleTextureSrgb( textures, material->emissionTexture, tu, tv, glow );
	}
	glow[3] *= material->emissionAlpha;
	PT_ApplyVertexColorMode( material->emissionVertexColor, vertices, tri, w, u, v, glow );
	const float alpha = material->emissionUsesAlpha ? PT_Clamp( glow[3], 0.0f, 1.0f ) : 1.0f;
	for ( int c = 0; c < 3; c++ ) emission[c] = material->emission[c] * glow[c] * alpha;
}

/*
========================
PT_AdditiveMaskAlpha
========================
*/
PT_INLINE float PT_AdditiveMaskAlpha(
	__global const ptMaterial_t *material, const ptClTextures_t *textures,
	__global const ptVertex_t *vertices, __global const ptTriangle_t *tri, float w, float u, float v,
	float tu, float tv
) {
	const int index = material->additiveMaskTexture;
	if ( !textures || !PT_TextureResident( textures, index ) ) return 0.0f;
	PT_ApplyTexTransformXf( material->additiveMaskTransform, &tu, &tv );
	float mask[4];
	SampleTexture( textures, index, tu, tv, mask );
	float alpha = mask[3] * material->additiveMaskAlpha;
	if ( material->additiveMaskVertexColor ) {
		float vertexAlpha = w * vertices[tri->vertexIndices[0]].color[3]
			+ u * vertices[tri->vertexIndices[1]].color[3] + v * vertices[tri->vertexIndices[2]].color[3];
		if ( material->additiveMaskVertexColor == 2 ) vertexAlpha = 1.0f - vertexAlpha;
		alpha *= vertexAlpha;
	}
	return PT_Clamp( alpha, 0.0f, 1.0f );
}

/*
========================
SampleLightTextureSrgb
========================
*/
PT_INLINE void SampleLightTextureSrgb( const ptClTextures_t *textures, int texIndex,
	float u, float v, int address, float *color ) {
	if ( !PT_TextureResident( textures, texIndex ) ) {
		color[0] = color[1] = color[2] = color[3] = 1.0f;
		return;
	}

	const ptClTexture_t tex = textures->desc[texIndex];
	if ( address == PT_LIGHT_TEXTURE_CLAMP ) {
		const float halfU = 0.5f / (float)tex.width;
		const float halfV = 0.5f / (float)tex.height;
		u = fmin( fmax( u, halfU ), 1.0f - halfU );
		v = fmin( fmax( v, halfV ), 1.0f - halfV );
		SampleTexture( textures, texIndex, u, v, color );
	} else if ( address == PT_LIGHT_TEXTURE_BORDER ) {
		const float x = u * (float)tex.width - 0.5f;
		const float y = v * (float)tex.height - 0.5f;
		color[0] = color[1] = color[2] = color[3] = 0.0f;
		if ( x > -1.0f && x < (float)tex.width && y > -1.0f && y < (float)tex.height ) {
			const int x0 = (int)floor( x );
			const int y0 = (int)floor( y );
			const float fx = x - (float)x0;
			const float fy = y - (float)y0;
			for ( int dy = 0; dy < 2; dy++ ) {
				for ( int dx = 0; dx < 2; dx++ ) {
					const int ix = x0 + dx;
					const int iy = y0 + dy;
					if ( ix < 0 || ix >= tex.width || iy < 0 || iy >= tex.height ) continue;
					const float weight = ( dx ? fx : 1.0f - fx ) * ( dy ? fy : 1.0f - fy ) / 255.0f;
					const float4 t = PT_Texel( textures, &tex, iy * tex.width + ix );
					color[0] += t.x * weight;
					color[1] += t.y * weight;
					color[2] += t.z * weight;
					color[3] += t.w * weight;
				}
			}
		}
	} else {
		SampleTexture( textures, texIndex, u, v, color );
	}
	color[0] = PT_SrgbToLinear( color[0] );
	color[1] = PT_SrgbToLinear( color[1] );
	color[2] = PT_SrgbToLinear( color[2] );
}

/*
===============================================================================

	PBR Material Evaluation - Cook-Torrance BRDF

===============================================================================
*/

/*
========================
FresnelSchlick

Fresnel-Schlick approximation
========================
*/
PT_INLINE float FresnelSchlick( float cosTheta, float F0 ) {
	float x = 1.0f - cosTheta;
	float x2 = x * x;
	return F0 + ( 1.0f - F0 ) * x2 * x2 * x;  // x^5
}

/*
========================
FresnelSchlickVec3
========================
*/
PT_INLINE void FresnelSchlickVec3( float cosTheta, const float *F0, float *result ) {
	float x = 1.0f - cosTheta;
	float x2 = x * x;
	float x5 = x2 * x2 * x;
	result[0] = F0[0] + ( 1.0f - F0[0] ) * x5;
	result[1] = F0[1] + ( 1.0f - F0[1] ) * x5;
	result[2] = F0[2] + ( 1.0f - F0[2] ) * x5;
}

/*
========================
DistributionGGX

GGX normal distribution function
========================
*/
PT_INLINE float DistributionGGX( float NdotH, float roughness ) {
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH2 = NdotH * NdotH;

	float denom = NdotH2 * ( a2 - 1.0f ) + 1.0f;
	denom = PT_PI * denom * denom;

	return a2 / fmax( denom, 0.0001f );
}

/*
========================
GeometrySchlickGGX

Smith's geometry shadowing function with GGX
========================
*/
PT_INLINE float GeometrySchlickGGX( float NdotV, float roughness ) {
	float r = roughness + 1.0f;
	float k = ( r * r ) / 8.0f;

	return NdotV / ( NdotV * ( 1.0f - k ) + k );
}

/*
========================
GeometrySmith
========================
*/
PT_INLINE float GeometrySmith( float NdotV, float NdotL, float roughness ) {
	float ggx1 = GeometrySchlickGGX( NdotV, roughness );
	float ggx2 = GeometrySchlickGGX( NdotL, roughness );
	return ggx1 * ggx2;
}

/*
========================
EvaluateMaterial

Simple Lambertian BRDF (backward compatibility)
========================
*/
PT_INLINE void EvaluateMaterial(
	const float *albedo,
	const float *normal,
	const float *wo,
	const float *wi,
	float *brdf
) {
	float ndotwi = fmax( 0.0f, dot3( normal, wi ) );
	float diffuse = ndotwi / PT_PI;

	brdf[0] = albedo[0] * diffuse;
	brdf[1] = albedo[1] * diffuse;
	brdf[2] = albedo[2] * diffuse;
}

/*
========================
EvaluatePBRMaterial

Full PBR material evaluation with metallic-roughness workflow
CUDA defaults: diffuseBrdf = NULL, params = NULL, materialIor = 0
========================
*/
PT_INLINE void EvaluatePBRMaterial(
	const float *albedo,
	const float *normal,
	const float *wo,      // view direction (from surface to camera)
	const float *wi,      // light direction (from surface to light)
	float metallic,
	float roughness,
	float specularBoost,  // Doom 3 interaction shader doubles specular
	const float *specColor,  // specular tint from specular map
	float *brdf,
	float *diffuseBrdf,
	const ptRenderParams_t *params,
	float materialIor
) {
#ifdef PT_PHYSICAL
	if ( PT_PhysicalActive( params ) ) {
		PT_EvaluatePhysicalMaterial( albedo, normal, wo, wi, metallic, roughness,
			materialIor > 0.0f ? materialIor : params->physicalIor, brdf, diffuseBrdf );
		return;
	}
#endif
	float NdotL = fmax( 0.0f, dot3( normal, wi ) );
	float NdotV = fmax( 0.0f, dot3( normal, wo ) );

	if ( NdotL <= 0.0f || NdotV <= 0.0f ) {
		brdf[0] = brdf[1] = brdf[2] = 0.0f;
		if ( diffuseBrdf ) diffuseBrdf[0] = diffuseBrdf[1] = diffuseBrdf[2] = 0.0f;
		return;
	}

	// half vector for specular
	float h[3] = {
		wo[0] + wi[0],
		wo[1] + wi[1],
		wo[2] + wi[2]
	};
	normalize3( h );

	float NdotH = fmax( 0.0f, dot3( normal, h ) );
	float HdotV = fmax( 0.0f, dot3( h, wo ) );

	// clamp roughness to prevent numerical issues
	roughness = fmax( 0.04f, roughness );

	// calculate F0 (surface reflection at zero incidence)
	float F0[3];
	F0[0] = 0.04f * ( 1.0f - metallic ) + albedo[0] * metallic;
	F0[1] = 0.04f * ( 1.0f - metallic ) + albedo[1] * metallic;
	F0[2] = 0.04f * ( 1.0f - metallic ) + albedo[2] * metallic;

	// Cook-Torrance specular BRDF: D * G * F / (4 * NdotV * NdotL)
	float D = DistributionGGX( NdotH, roughness );
	float G = GeometrySmith( NdotV, NdotL, roughness );
	float F[3];
	FresnelSchlickVec3( HdotV, F0, F );

	float denominator = 4.0f * NdotV * NdotL;
	float specular[3];
	specular[0] = ( D * G * F[0] ) / fmax( denominator, 0.001f ) * specularBoost * specColor[0];
	specular[1] = ( D * G * F[1] ) / fmax( denominator, 0.001f ) * specularBoost * specColor[1];
	specular[2] = ( D * G * F[2] ) / fmax( denominator, 0.001f ) * specularBoost * specColor[2];

	// diffuse contribution (energy conserving)
	// kD = (1 - F) * (1 - metallic) - metals have no diffuse
	float kD[3];
	kD[0] = ( 1.0f - F[0] ) * ( 1.0f - metallic );
	kD[1] = ( 1.0f - F[1] ) * ( 1.0f - metallic );
	kD[2] = ( 1.0f - F[2] ) * ( 1.0f - metallic );

	// Lambertian diffuse: albedo / pi
	float diff[3];
	diff[0] = kD[0] * albedo[0] / PT_PI;
	diff[1] = kD[1] * albedo[1] / PT_PI;
	diff[2] = kD[2] * albedo[2] / PT_PI;

	// return pure BRDF f(wo,wi) = diffuse + specular (NO NdotL here)
	brdf[0] = diff[0] + specular[0];
	brdf[1] = diff[1] + specular[1];
	brdf[2] = diff[2] + specular[2];
	if ( diffuseBrdf ) {
		for ( int c = 0; c < 3; c++ ) diffuseBrdf[c] = diff[c];
	}
}
