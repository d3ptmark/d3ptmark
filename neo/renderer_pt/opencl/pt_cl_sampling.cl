/*
===============================================================================

	OpenCL C port of pt_cuda_sampling.cuh.

	CUDA reads g_softShadowScale / g_rayOffset from __device__ globals that
	LaunchPathTracingKernel stages before each launch. OpenCL 1.2 has no
	writable program scope variables, so the same two values travel as
	const ptClGlobals_t *g.

===============================================================================
*/

/*
========================
SampleHemisphere

Cosine-weighted hemisphere sampling
========================
*/
PT_INLINE void SampleHemisphere(
	const float *normal,
	ptRng_t *rng,
	float *direction,
	float *pdf
) {
	float r1 = PT_Rand( rng );
	float r2 = PT_Rand( rng );

	float sinTheta = sqrt( r1 );
	float cosTheta = sqrt( 1.0f - r1 );
	float phi = 2.0f * PT_PI * r2;

	// build orthonormal basis from normal
	float upX, upY, upZ;
	if ( fabs( normal[0] ) < 0.9f ) {
		upX = 1.0f; upY = 0.0f; upZ = 0.0f;
	} else {
		upX = 0.0f; upY = 1.0f; upZ = 0.0f;
	}

	// tangent = normalize(cross(up, normal))
	float tX = upY * normal[2] - upZ * normal[1];
	float tY = upZ * normal[0] - upX * normal[2];
	float tZ = upX * normal[1] - upY * normal[0];
	float tIL = rsqrt( tX * tX + tY * tY + tZ * tZ + 1e-12f );
	tX *= tIL; tY *= tIL; tZ *= tIL;

	// bitangent = cross(normal, tangent)
	float bX = normal[1] * tZ - normal[2] * tY;
	float bY = normal[2] * tX - normal[0] * tZ;
	float bZ = normal[0] * tY - normal[1] * tX;

	float cosP = cos( phi );
	float sinP = sin( phi );

	direction[0] = sinTheta * cosP * tX + sinTheta * sinP * bX + cosTheta * normal[0];
	direction[1] = sinTheta * cosP * tY + sinTheta * sinP * bY + cosTheta * normal[1];
	direction[2] = sinTheta * cosP * tZ + sinTheta * sinP * bZ + cosTheta * normal[2];

	// PDF for cosine-weighted sampling: cosTheta / pi
	*pdf = cosTheta / PT_PI;
}

#ifdef PT_PHYSICAL

/*
========================
PT_SamplePhysicalBsdf
========================
*/
PT_INLINE void PT_SamplePhysicalBsdf(
	const float *normal, const float *wo, const float *albedo, float metallic, float roughness,
	float ior, ptRng_t *rng, float *direction, float *pdf
) {
	if ( dot3( normal, wo ) <= 0.0f ) {
		*pdf = 0.0f;
		direction[0] = direction[1] = direction[2] = 0.0f;
		return;
	}
	const float probability = PT_PhysicalSpecularProbability( albedo, metallic, ior );
	if ( PT_Rand( rng ) >= probability ) {
		SampleHemisphere( normal, rng, direction, pdf );
	} else {
		const float radius = sqrt( PT_Rand( rng ) );
		const float phi = 2.0f * PT_PI * PT_Rand( rng );
		const float alpha = roughness * roughness;
		float axis[3] = { 0.0f, 0.0f, 0.0f };
		axis[fabs( normal[0] ) < 0.9f ? 0 : 1] = 1.0f;
		float tangent[3], bitangent[3], h[3];
		cross3( axis, normal, tangent );
		normalize3( tangent );
		cross3( normal, tangent, bitangent );

		float view[3] = { alpha * dot3( wo, tangent ), alpha * dot3( wo, bitangent ), dot3( wo, normal ) };
		normalize3( view );
		const float planarLength = view[0] * view[0] + view[1] * view[1];
		const float invLength = planarLength > 0.0f ? rsqrt( planarLength ) : 0.0f;
		const float diskTangent[3] = { planarLength > 0.0f ? -view[1] * invLength : 1.0f,
			view[0] * invLength, 0.0f };
		float diskBitangent[3];
		cross3( view, diskTangent, diskBitangent );
		const float x = radius * cos( phi );
		const float blend = 0.5f * ( 1.0f + view[2] );
		const float y = ( 1.0f - blend ) * sqrt( fmax( 0.0f, 1.0f - x * x ) ) + blend * radius * sin( phi );
		const float z = sqrt( fmax( 0.0f, 1.0f - x * x - y * y ) );
		float localNormal[3];
		for ( int c = 0; c < 3; c++ ) {
			localNormal[c] = x * diskTangent[c] + y * diskBitangent[c] + z * view[c];
		}
		localNormal[0] *= alpha;
		localNormal[1] *= alpha;
		localNormal[2] = fmax( localNormal[2], 0.0f );
		normalize3( localNormal );
		for ( int c = 0; c < 3; c++ ) {
			h[c] = localNormal[0] * tangent[c] + localNormal[1] * bitangent[c] + localNormal[2] * normal[c];
		}
		const float vh = dot3( wo, h );
		if ( vh <= 0.0f ) {
			*pdf = 0.0f;
			direction[0] = direction[1] = direction[2] = 0.0f;
			return;
		}
		for ( int c = 0; c < 3; c++ ) direction[c] = 2.0f * vh * h[c] - wo[c];
		normalize3( direction );
	}
	*pdf = PT_PhysicalBsdfPdf( normal, wo, direction, roughness, probability );
}
#endif

/*
========================
HenyeyGreenstein

Henyey-Greenstein phase function for volumetric scattering
========================
*/
PT_INLINE float HenyeyGreenstein( float cosTheta, float g ) {
	float g2 = g * g;
	float denom = 1.0f + g2 - 2.0f * g * cosTheta;
	denom = fmax( denom, 1e-6f );
	return ( 1.0f - g2 ) / ( 4.0f * PT_PI * pow( denom, 1.5f ) );
}

/*
========================
SpotlightAttenuation

Spotlight attenuation (smoothstep cone)
========================
*/
PT_INLINE float SpotlightAttenuation( __global const ptLight_t *light, const float *toSurface ) {
	// compute angle between the vector from light to surface and the light direction
	float cosAngle = dot3g( toSurface, light->direction );

	float cosCone = cos( light->coneAngle );
	float cosInner = cos( light->coneAngle * 0.8f );  // Inner cone = 80% of outer

	if ( cosAngle < cosCone ) {
		return 0.0f;  // Outside cone
	}
	if ( cosAngle > cosInner ) {
		return 1.0f;  // Inside inner cone (full brightness)
	}

	// smoothstep between inner and outer cone
	float t = ( cosAngle - cosCone ) / ( cosInner - cosCone );
	t = fmax( 0.0f, fmin( 1.0f, t ) );
	// smooth hermite interpolation
	float atten = t * t * ( 3.0f - 2.0f * t );

	// apply falloff exponent for sharper/softer edges
	if ( light->coneFalloff > 0.0f ) {
		atten = pow( atten, light->coneFalloff );
	}

	return atten;
}

/*
===============================================================================

	SampleLightProjectionUV

	Compute projected texture UV coordinates using Doom 3's lightProject planes.

===============================================================================
*/
PT_INLINE bool SampleLightProjectionUV(
	__global const ptLight_t *light,
	const float *worldPos,
	float *u,
	float *v,
	float *falloff
) {
	float s = light->lightProject[0][0] * worldPos[0] +
			  light->lightProject[0][1] * worldPos[1] +
			  light->lightProject[0][2] * worldPos[2] +
			  light->lightProject[0][3];

	float t = light->lightProject[1][0] * worldPos[0] +
			  light->lightProject[1][1] * worldPos[1] +
			  light->lightProject[1][2] * worldPos[2] +
			  light->lightProject[1][3];

	float q = light->lightProject[2][0] * worldPos[0] +
			  light->lightProject[2][1] * worldPos[1] +
			  light->lightProject[2][2] * worldPos[2] +
			  light->lightProject[2][3];

	float f = light->lightProject[3][0] * worldPos[0] +
			  light->lightProject[3][1] * worldPos[1] +
			  light->lightProject[3][2] * worldPos[2] +
			  light->lightProject[3][3];

	if ( q <= 0.0001f ) {
		return false;
	}

	float invQ = 1.0f / q;
	*u = s * invQ;
	*v = t * invQ;
	*falloff = f;

	if ( *u < 0.0f || *u > 1.0f || *v < 0.0f || *v > 1.0f ) {
		return false;
	}
	if ( *falloff < 0.0f || *falloff > 1.0f ) {
		return false;
	}

	return true;
}

/*
========================
JitterLightPosition

Area light sampling for soft shadows
========================
*/
PT_INLINE void JitterLightPosition(
	__global const ptLight_t *light,
	ptRng_t *rng,
	float *jitteredPos,
	const ptClGlobals_t *g
) {
	jitteredPos[0] = light->position[0];
	jitteredPos[1] = light->position[1];
	jitteredPos[2] = light->position[2];

	float jitterScale = g->softShadowScale;
	if ( jitterScale <= 0.0f ) {
		return;
	}

	if ( light->type == PT_LIGHT_DIRECTIONAL ) {
		return;  // directional light
	}

	if ( light->coneAngle > 0.0f ) {
		float r1 = ( PT_Rand( rng ) - 0.5f ) * 2.0f * jitterScale;
		float r2 = ( PT_Rand( rng ) - 0.5f ) * 2.0f * jitterScale;

		float rightLen = light->right[0] * light->right[0] + light->right[1] * light->right[1] + light->right[2] * light->right[2];
		if ( rightLen > 0.001f ) {
			float extent = light->radius * jitterScale;
			jitteredPos[0] += light->right[0] * r1 * extent + light->up[0] * r2 * extent;
			jitteredPos[1] += light->right[1] * r1 * extent + light->up[1] * r2 * extent;
			jitteredPos[2] += light->right[2] * r1 * extent + light->up[2] * r2 * extent;
		}
	} else {
		float r1 = ( PT_Rand( rng ) - 0.5f ) * 2.0f;
		float r2 = ( PT_Rand( rng ) - 0.5f ) * 2.0f;
		float r3 = ( PT_Rand( rng ) - 0.5f ) * 2.0f;

		jitteredPos[0] += r1 * light->lightRadius3[0] * jitterScale;
		jitteredPos[1] += r2 * light->lightRadius3[1] * jitterScale;
		jitteredPos[2] += r3 * light->lightRadius3[2] * jitterScale;
	}
}

/*
===============================================================================

	PT_LightFilter

	Everything about a light's reach that does not depend on visibility: the
	light color filtered by the light image, times the returned scalar
	attenuation. Returns 0 when the point is outside the light.

	boxFalloff lights reproduce the GL interaction: lightProject gives (s, t)
	for the light image and the falloff coordinate for the falloff image, both
	zero outside the light volume. Other non-area lights use a spherical window
	and optionally a spot cone.

	CUDA default: params = NULL

===============================================================================
*/
PT_INLINE float PT_LightFilter(
	__global const ptLight_t *light,
	const float *pos,
	float dist,
	const float *toLight,
	const ptClTextures_t *textures,
	float *filter,
	const ptRenderParams_t *params
) {
	filter[0] = light->color[0];
	filter[1] = light->color[1];
	filter[2] = light->color[2];

#ifdef PT_PHYSICAL
	if ( PT_PhysicalActive( params ) ) {
		if ( light->type == PT_LIGHT_DIRECTIONAL ) return params->physicalSunLux;
		const float distanceMeters = dist * params->physicalMetersPerUnit;
		float solidAngle = 4.0f * PT_PI;
		float angular = 1.0f;
		if ( light->coneAngle > 0.0f ) {
			const float outer = cos( light->coneAngle );
			const float cosine = -dot3g( toLight, light->direction );
			const float exponent = fmax( light->coneFalloff, 0.0f );
			angular = pow( PT_Clamp( ( cosine - outer ) / fmax( 1.0f - outer, 1e-6f ), 0.0f, 1.0f ), exponent );
			if ( cosine <= outer ) return 0.0f;
			solidAngle = 2.0f * PT_PI * ( 1.0f - outer ) / ( exponent + 1.0f );
			if ( light->projectedTextureIndex >= 0 ) {
				__global const float ( *lp )[4] = light->lightProject;
				const float q = lp[2][0] * pos[0] + lp[2][1] * pos[1] + lp[2][2] * pos[2] + lp[2][3];
				if ( q <= 0.0f ) return 0.0f;
				const float s = ( lp[0][0] * pos[0] + lp[0][1] * pos[1] + lp[0][2] * pos[2] + lp[0][3] ) / q;
				const float t = ( lp[1][0] * pos[0] + lp[1][1] * pos[1] + lp[1][2] * pos[2] + lp[1][3] ) / q;
				if ( s < 0.0f || s > 1.0f || t < 0.0f || t > 1.0f ) return 0.0f;
				if ( textures ) {
					float tex[4];
					__global const float *m = light->projectedTextureTransform;
					SampleLightTextureSrgb( textures, light->projectedTextureIndex,
						m[0] * s + m[1] * t + m[2], m[3] * s + m[4] * t + m[5], light->projectedTextureAddress, tex );
					for ( int c = 0; c < 3; c++ ) filter[c] *= tex[c];
				}
			}
		}
		return params->physicalLightLumens * angular / fmax( solidAngle * distanceMeters * distanceMeters, 1e-8f );
	}
#endif

	if ( light->type == PT_LIGHT_DIRECTIONAL ) {
		return 1.0f;
	}

	if ( light->boxFalloff ) {
		float s, t, f;
		if ( !SampleLightProjectionUV( light, pos, &s, &t, &f ) ) {
			return 0.0f;
		}
		float tex[4];
		if ( light->projectedTextureIndex >= 0 && textures ) {
			__global const float *m = light->projectedTextureTransform;
			const float u = m[0] * s + m[1] * t + m[2];
			const float v = m[3] * s + m[4] * t + m[5];
			SampleLightTextureSrgb( textures, light->projectedTextureIndex, u, v, light->projectedTextureAddress, tex );
			filter[0] *= tex[0];
			filter[1] *= tex[1];
			filter[2] *= tex[2];
		}
		if ( light->falloffTextureIndex >= 0 && textures ) {
			// clamp instead of wrapping into the other end of the falloff ramp
			const int ftWidth = textures->desc[light->falloffTextureIndex].width;
			const float halfTexel = ftWidth > 0 ? 0.5f / (float)ftWidth : 0.0f;
			SampleTexture( textures, light->falloffTextureIndex, fmin( fmax( f, halfTexel ), 1.0f - halfTexel ), 0.5f, tex );
			return PT_SrgbToLinear( tex[0] );
		}
		return 1.0f;
	}

	float atten = 1.0f;
	const float lightRadius = fmax( light->radius, 1.0f );
	const float lt = dist / lightRadius;
	if ( lt >= 1.0f ) {
		return 0.0f;
	}
	const float window = 1.0f - lt * lt;
	atten = window * window;

	if ( light->projectedTextureIndex >= 0 ) {
		float s, t, f;
		if ( !SampleLightProjectionUV( light, pos, &s, &t, &f ) ) {
			return 0.0f;
		}
		if ( textures ) {
			float tex[4];
			__global const float *m = light->projectedTextureTransform;
			const float u = m[0] * s + m[1] * t + m[2];
			const float v = m[3] * s + m[4] * t + m[5];
			SampleLightTextureSrgb( textures, light->projectedTextureIndex, u, v, light->projectedTextureAddress, tex );
			filter[0] *= tex[0];
			filter[1] *= tex[1];
			filter[2] *= tex[2];
		}
		const float ff = fmax( 0.0f, 1.0f - f );
		atten *= ff * ff;
	} else if ( light->coneAngle > 0.0f ) {
		const float toSurface[3] = { -toLight[0], -toLight[1], -toLight[2] };
		atten *= SpotlightAttenuation( light, toSurface );
	} else if ( light->textureIndex >= 0 && textures ) {
		float tex[4];
		SampleTextureSrgb( textures, light->textureIndex, 0.5f, 0.5f, tex );
		filter[0] *= fmin( tex[0], 1.0f );
		filter[1] *= fmin( tex[1], 1.0f );
		filter[2] *= fmin( tex[2], 1.0f );
	}
	return atten;
}

/*
===============================================================================

	LightImportance

	Cheap, texture free estimate of a light's contribution at a surface point,
	used to pick which lights get shadow rays. Zero only where the light can
	not contribute at all, so importance sampling stays unbiased.

	CUDA default: params = NULL

===============================================================================
*/
PT_INLINE float LightImportance( __global const ptLight_t *light, const float *pos, const float *normal, float emissionBoost,
	const ptRenderParams_t *params ) {
	if ( light->type == PT_LIGHT_AREA ) {
		const float power = ( 0.2126f * light->color[0] + 0.7152f * light->color[1] + 0.0722f * light->color[2] )
			* light->area[0] * emissionBoost;
		if ( power <= 0.0f ) {
			return 0.0f;
		}
		const float d[3] = { light->position[0] - pos[0], light->position[1] - pos[1], light->position[2] - pos[2] };
		const float normalDistance = dot3( normal, d );
		// The bounding sphere must be entirely below the receiver's hemisphere.
		if ( normalDistance + light->areaBoundRadius < -1e-3f ) {
			return 0.0f;
		}
		const float dist2 = dot3( d, d );
		const float cosTerm = fmax( normalDistance * rsqrt( fmax( dist2, 1e-8f ) ), 0.0f ) + 0.02f;
		return power * cosTerm / ( dist2 + light->area[0] );
	}

	const float power = ( 0.2126f * light->color[0] + 0.7152f * light->color[1] + 0.0722f * light->color[2] ) * light->intensity;
	if ( power <= 0.0f ) {
		return 0.0f;
	}

	if ( light->type == PT_LIGHT_DIRECTIONAL ) {
#ifdef PT_PHYSICAL
		if ( PT_PhysicalActive( params ) ) {
			return power * params->physicalSunLux * ( fmax( -dot3g( normal, light->direction ), 0.0f ) + 0.02f );
		}
#endif
		return power * ( fmax( -dot3g( normal, light->direction ), 0.0f ) + 0.02f );
	}

	float d[3] = { light->position[0] - pos[0], light->position[1] - pos[1], light->position[2] - pos[2] };
	const float dist = length3( d );
	const float ndotl = dist > 1e-4f ? dot3( normal, d ) / dist : 1.0f;
	// soft lights may still reach slightly back facing points
	const float cosTerm = fmax( ndotl, 0.0f ) + 0.02f;

#ifdef PT_PHYSICAL
	if ( PT_PhysicalActive( params ) ) {
		const float wi[3] = { d[0] / fmax( dist, 1e-6f ), d[1] / fmax( dist, 1e-6f ), d[2] / fmax( dist, 1e-6f ) };
		float filter[3];
		return power * cosTerm * PT_LightFilter( light, pos, dist, wi, NULL, filter, params );
	}
#endif

	float shape;
	if ( light->boxFalloff ) {
		__global const float ( *lp )[4] = light->lightProject;
		const float q = lp[2][0] * pos[0] + lp[2][1] * pos[1] + lp[2][2] * pos[2] + lp[2][3];
		if ( q <= 0.0001f ) return 0.0f;
		const float s = ( lp[0][0] * pos[0] + lp[0][1] * pos[1] + lp[0][2] * pos[2] + lp[0][3] ) / q;
		const float t = ( lp[1][0] * pos[0] + lp[1][1] * pos[1] + lp[1][2] * pos[2] + lp[1][3] ) / q;
		const float f = lp[3][0] * pos[0] + lp[3][1] * pos[1] + lp[3][2] * pos[2] + lp[3][3];
		if ( s < 0.0f || s > 1.0f || t < 0.0f || t > 1.0f || f < 0.0f || f > 1.0f ) return 0.0f;
		const float ds = 2.0f * s - 1.0f, dt = 2.0f * t - 1.0f;
		if ( light->coneAngle > 0.0f ) {
			shape = 1.0f - ( ds * ds + dt * dt ) * 0.5f;
		} else {
			const float df = 2.0f * f - 1.0f;
			shape = 1.0f - ( ds * ds + dt * dt + df * df ) * ( 1.0f / 3.0f );
		}
		shape = fmax( shape, 0.05f );
	} else {
		const float lt = dist / fmax( light->radius, 1.0f );
		if ( lt >= 1.0f ) return 0.0f;
		shape = ( 1.0f - lt * lt );
		shape = fmax( shape * shape, 0.01f );
		if ( light->projectedTextureIndex < 0 && light->coneAngle > 0.0f && dist > 1e-4f ) {
			const float toSurface[3] = { -d[0] / dist, -d[1] / dist, -d[2] / dist };
			shape *= SpotlightAttenuation( light, toSurface );
		}
	}
	return power * cosTerm * shape;
}

/*
===============================================================================

	PT_SelectLights

	Picks numPicks lights (with replacement) proportionally to LightImportance,
	one weighted reservoir per pick, in a single pass over the lights. invPdf
	receives 1 / (pmf * numPicks) for each pick so that the sum of
	contribution * invPdf is an unbiased estimate of the sum over all lights.
	Returns the number of picks, 0 when no light reaches the point.

	CUDA default: params = NULL

===============================================================================
*/
#define PT_MAX_LIGHT_PICKS	8

/*
========================
PT_SelectLights
========================
*/
PT_INLINE int PT_SelectLights(
	__global const ptLight_t *lights,
	int numLights,
	const float *pos,
	const float *normal,
	int numPicks,
	float emissionBoost,
	ptRng_t *rng,
	int *picked,
	float *invPdf,
	const ptRenderParams_t *params
) {
	numPicks = min( numPicks, PT_MAX_LIGHT_PICKS );
	if ( numLights <= 0 || numPicks <= 0 ) {
		return 0;
	}

	float pickedW[PT_MAX_LIGHT_PICKS];
	float total = 0.0f;
	for ( int i = 0; i < numLights; i++ ) {
		const float w = LightImportance( &lights[i], pos, normal, emissionBoost, params );
		if ( w <= 0.0f ) {
			continue;
		}
		total += w;
		const float p = w / total;
		for ( int j = 0; j < numPicks; j++ ) {
			if ( PT_Rand( rng ) < p ) {
				picked[j] = i;
				pickedW[j] = w;
			}
		}
	}
	if ( total <= 0.0f ) {
		return 0;
	}
	for ( int j = 0; j < numPicks; j++ ) {
		invPdf[j] = total / ( pickedW[j] * (float)numPicks );
	}
	return numPicks;
}

/*
===============================================================================

	SampleVolumetricScattering

===============================================================================
*/
PT_INLINE void SampleVolumetricScattering(
	__global const ptLight_t *light,
	const float *rayOrigin,
	const float *rayDirection,
	float rayLength,
	__global const ptVertex_t *vertices,
	__global const ptTriangle_t *triangles,
	__global const ptBVHNode_t *nodes,
	__global const int *triIndices,
	__global const ptMaterial_t *materials,
	const ptClTextures_t *textures,
	ptRng_t *rng,
	float *scattering,
	float volDensity,
	int volSteps,
	float volAnisotropy,
	float volFalloff,
	float volMaxDist,
	const ptClGlobals_t *g
) {
	scattering[0] = scattering[1] = scattering[2] = 0.0f;

	if ( light->volumetric <= 0.0f ) {
		return;
	}

	int steps = max( 2, min( 64, volSteps ) );
	float density = fmax( 0.0001f, volDensity ) * light->volumetric;
	float phaseG = fmax( -0.99f, fmin( 0.99f, volAnisotropy ) );

	if ( light->coneAngle > 0.0f ) {
		phaseG = fmax( phaseG, 0.7f );
	}

	float marchLength = rayLength;
	if ( volMaxDist > 0.0f && marchLength > volMaxDist ) {
		marchLength = volMaxDist;
	}

	const float stepSize = marchLength / (float)steps;
	const float stepTransmit = exp( -density * 0.5f * stepSize );
	float transmittance = 1.0f;

	for ( int step = 0; step < steps && transmittance >= 0.01f; step++, transmittance *= stepTransmit ) {
		const float t = ( step + PT_Rand( rng ) ) * stepSize;
		const float samplePos[3] = {
			rayOrigin[0] + rayDirection[0] * t,
			rayOrigin[1] + rayDirection[1] * t,
			rayOrigin[2] + rayDirection[2] * t
		};

		float lightDir[3];
		float lightDist;
		if ( light->type == PT_LIGHT_DIRECTIONAL ) {
			lightDir[0] = -light->direction[0];
			lightDir[1] = -light->direction[1];
			lightDir[2] = -light->direction[2];
			lightDist = 1e10f;
		} else {
			lightDir[0] = light->position[0] - samplePos[0];
			lightDir[1] = light->position[1] - samplePos[1];
			lightDir[2] = light->position[2] - samplePos[2];
			lightDist = length3( lightDir );
			if ( lightDist < 1e-4f ) continue;
			lightDir[0] /= lightDist;
			lightDir[1] /= lightDist;
			lightDir[2] /= lightDist;
		}

		float volColor[3];
		const float attenuation = PT_LightFilter( light, samplePos, lightDist, lightDir, textures, volColor, NULL );
		if ( attenuation <= 0.0f ) continue;

		ptRay_t shadowRay;
		shadowRay.origin[0] = samplePos[0];
		shadowRay.origin[1] = samplePos[1];
		shadowRay.origin[2] = samplePos[2];
		shadowRay.direction[0] = lightDir[0];
		shadowRay.direction[1] = lightDir[1];
		shadowRay.direction[2] = lightDir[2];
		shadowRay.tMin = g->rayOffset;
		shadowRay.tMax = lightDist - g->rayOffset;
		shadowRay.worldTMin = 0.0f;
		shadowRay.flags = 0;

		float shadow[3];
		if ( !TraceShadow( &shadowRay, vertices, triangles, nodes, triIndices, materials, textures, shadow ) ) continue;
		const float shadowAtten = ( shadow[0] + shadow[1] + shadow[2] ) * ( 1.0f / 3.0f );

		float distFalloff = 1.0f;
		if ( volFalloff > 0.0f && marchLength > 1.0f ) {
			distFalloff = pow( 1.0f - t / marchLength, volFalloff );
		}

		const float cosTheta = -dot3( rayDirection, lightDir );
		const float phase = HenyeyGreenstein( cosTheta, phaseG );

		float contribution = light->intensity * density * stepSize * attenuation *
							 phase * shadowAtten * transmittance * distFalloff;
		contribution = fmin( contribution, 1.0f );

		scattering[0] += volColor[0] * contribution;
		scattering[1] += volColor[1] * contribution;
		scattering[2] += volColor[2] * contribution;
	}
}

/*
===============================================================================

	SampleDirectLight

	Next event estimation towards one light. wi receives the direction of the
	(jittered) shadow ray for BRDF evaluation. worldTMin is forwarded to the
	shadow ray (PT_WEAPON_CLEARANCE when shading the view weapon).

	CUDA defaults: params = NULL, volumeSample = false

===============================================================================
*/
PT_INLINE void SampleDirectLight(
	__global const ptLight_t *light,
	const float *position,
	const float *normal,
	float worldTMin,
	float emissionBoost,
	__global const ptVertex_t *vertices,
	__global const ptTriangle_t *triangles,
	__global const ptBVHNode_t *nodes,
	__global const int *triIndices,
	__global const ptMaterial_t *materials,
	const ptClTextures_t *textures,
	ptRng_t *rng,
	float *radiance,
	float *wi,
	const ptRenderParams_t *params,
	bool volumeSample,
	const ptClGlobals_t *g
) {
	radiance[0] = radiance[1] = radiance[2] = 0.0f;

	float lightPos[3];
	float bary[3] = { 0.0f, 0.0f, 0.0f };
	if ( light->type == PT_LIGHT_AREA ) {
		if ( emissionBoost <= 0.0f ) return;
		__global const ptTriangle_t *tri = &triangles[light->areaTriangleIndex];
		const float root = sqrt( PT_Rand( rng ) );
		const float r = PT_Rand( rng );
		bary[0] = 1.0f - root;
		bary[1] = root * ( 1.0f - r );
		bary[2] = root * r;
		for ( int c = 0; c < 3; c++ ) {
			lightPos[c] = bary[0] * vertices[tri->vertexIndices[0]].position[c]
				+ bary[1] * vertices[tri->vertexIndices[1]].position[c]
				+ bary[2] * vertices[tri->vertexIndices[2]].position[c];
		}
	} else {
#ifdef PT_PHYSICAL
		if ( PT_PhysicalActive( params ) ) {
			for ( int c = 0; c < 3; c++ ) lightPos[c] = light->position[c];
		} else
#endif
		JitterLightPosition( light, rng, lightPos, g );
	}

	float distance;
	if ( light->type == PT_LIGHT_DIRECTIONAL ) {
		wi[0] = -light->direction[0];
		wi[1] = -light->direction[1];
		wi[2] = -light->direction[2];
		distance = 1e10f;
	} else {
		wi[0] = lightPos[0] - position[0];
		wi[1] = lightPos[1] - position[1];
		wi[2] = lightPos[2] - position[2];
		distance = length3( wi );
		if ( distance < 1e-4f ) return;
		wi[0] /= distance;
		wi[1] /= distance;
		wi[2] /= distance;
	}

	const float ndotl = volumeSample ? 1.0f : dot3( normal, wi );
	if ( ndotl <= 0.0f ) return;

	float lightColor[3];
	float contribution;
	if ( light->type == PT_LIGHT_AREA ) {
		const float emitterCosine = fabs( dot3g( wi, light->direction ) );
		if ( emitterCosine <= 0.0f ) return;

		__global const ptTriangle_t *tri = &triangles[light->areaTriangleIndex];
		__global const ptMaterial_t *material = &materials[tri->materialIndex];
		__global const ptVertex_t *v0 = &vertices[tri->vertexIndices[0]];
		__global const ptVertex_t *v1 = &vertices[tri->vertexIndices[1]];
		__global const ptVertex_t *v2 = &vertices[tri->vertexIndices[2]];
		float tu = bary[0] * v0->texcoord[0] + bary[1] * v1->texcoord[0] + bary[2] * v2->texcoord[0];
		float tv = bary[0] * v0->texcoord[1] + bary[1] * v1->texcoord[1] + bary[2] * v2->texcoord[1];
		if ( material->alphaTest > 0.0f ) {
			float alphaU = tu, alphaV = tv;
			PT_ApplyTexTransform( material, &alphaU, &alphaV );
			float albedo[4] = { 1, 1, 1, material->albedo[3] };
			if ( material->albedoTexture >= 0 && textures ) {
				float texel[4];
				SampleTexture( textures, material->albedoTexture, alphaU, alphaV, texel );
				albedo[3] *= texel[3];
			}
			PT_ApplyVertexColor( material, vertices, tri, bary[0], bary[1], bary[2], albedo );
			if ( albedo[3] < material->alphaTest ) return;
		}

		PT_SampleEmission( material, textures, vertices, tri, bary[0], bary[1], bary[2], tu, tv, lightColor );
		if ( lightColor[0] <= 0.0f && lightColor[1] <= 0.0f && lightColor[2] <= 0.0f ) return;
		for ( int c = 0; c < 3; c++ ) lightColor[c] *= emissionBoost;
		contribution = ndotl * emitterCosine * light->area[0] / ( distance * distance );
	} else {
		const float attenuation = PT_LightFilter( light, position, distance, wi, textures, lightColor, params );
		if ( attenuation <= 0.0f ) return;
#ifdef PT_PHYSICAL
		if ( PT_PhysicalActive( params ) ) {
			contribution = light->intensity * ndotl * attenuation;
		} else
#endif
		contribution = fmin( light->intensity * ndotl * attenuation, 10.0f );
	}

	ptRay_t shadowRay;
	shadowRay.origin[0] = position[0] + normal[0] * g->rayOffset;
	shadowRay.origin[1] = position[1] + normal[1] * g->rayOffset;
	shadowRay.origin[2] = position[2] + normal[2] * g->rayOffset;
	shadowRay.direction[0] = wi[0];
	shadowRay.direction[1] = wi[1];
	shadowRay.direction[2] = wi[2];
	shadowRay.tMin = g->rayOffset;
	shadowRay.tMax = distance - g->rayOffset;
	if ( light->type == PT_LIGHT_AREA ) {
		for ( int c = 0; c < 3; c++ ) {
			shadowRay.direction[c] = lightPos[c] - shadowRay.origin[c];
		}
		const float shadowDistance = length3( shadowRay.direction );
		if ( shadowDistance <= g->rayOffset ) return;
		for ( int c = 0; c < 3; c++ ) {
			shadowRay.direction[c] /= shadowDistance;
		}
		shadowRay.tMax = shadowDistance - g->rayOffset;
	}
	shadowRay.worldTMin = worldTMin;
	shadowRay.flags = 0;

	float shadow[3];
	if ( !TraceShadow( &shadowRay, vertices, triangles, nodes, triIndices, materials, textures, shadow ) ) {
		return;
	}

#ifdef PT_PHYSICAL
	if ( PT_PhysicalActive( params ) ) {
		const float transmittance = PT_PhysicalFogTransmit( params, position, wi, distance );
		for ( int c = 0; c < 3; c++ ) shadow[c] *= transmittance;
	}
#endif
	radiance[0] = lightColor[0] * contribution * shadow[0];
	radiance[1] = lightColor[1] * contribution * shadow[1];
	radiance[2] = lightColor[2] * contribution * shadow[2];
}

#ifdef PT_PHYSICAL

/*
========================
PT_PhysicalSingleScattering
========================
*/
PT_INLINE void PT_PhysicalSingleScattering(
	const ptRenderParams_t *p, const ptRay_t *ray, float distance,
	__global const ptVertex_t *vertices, __global const ptTriangle_t *triangles, __global const ptBVHNode_t *nodes,
	__global const int *triIndices, __global const ptMaterial_t *materials, const ptClTextures_t *textures,
	__global const ptLight_t *lights, int numLights, ptRng_t *rng, float *scattering, const ptClGlobals_t *g
) {
	scattering[0] = scattering[1] = scattering[2] = 0.0f;
	if ( numLights <= 0 || p->physicalFogAlbedo <= 0.0f ) return;
	float start;
	const float length = PT_PhysicalFogInterval( p, ray->origin, ray->direction, distance, &start );
	if ( length <= 0.0f ) return;
	const int steps = max( 1, min( 128, p->physicalFogSteps ) );
	const float step = length / steps;
	for ( int i = 0; i < steps; i++ ) {
		const float t = start + ( i + PT_Rand( rng ) ) * step;
		float position[3];
		for ( int c = 0; c < 3; c++ ) position[c] = ray->origin[c] + ray->direction[c] * t;
		int light = 0;
		float lightWeight = 1.0f;
		const float noOffset[3] = { 0.0f, 0.0f, 0.0f };
		if ( numLights > 1 ) {
			// A bounded reservoir avoids scanning every light at every fog sample.
			// Its inverse weight remains unbiased over the uniform candidate set.
			float total = 0.0f, chosenWeight = 0.0f;
			const int candidates = min( numLights, 8 );
			for ( int candidate = 0; candidate < candidates; candidate++ ) {
				const int index = min( (int)( PT_Rand( rng ) * numLights ), numLights - 1 );
				const float weight = fmax( LightImportance( &lights[index], position, noOffset, p->emissionBoost, p ), 1e-12f );
				total += weight;
				if ( candidate == 0 || PT_Rand( rng ) * total < weight ) { light = index; chosenWeight = weight; }
			}
			lightWeight = total * numLights / ( chosenWeight * candidates );
		}
		float incoming[3], wi[3] = { 0.0f, 0.0f, 0.0f };
		SampleDirectLight( &lights[light], position, noOffset, fmax( 0.0f, ray->worldTMin - t ), p->emissionBoost,
			vertices, triangles, nodes, triIndices, materials, textures, rng, incoming, wi, p, true, g );
		if ( incoming[0] + incoming[1] + incoming[2] <= 0.0f ) continue;
		// wi points toward the source, so forward scattering is wi == ray.direction.
		const float phase = HenyeyGreenstein( dot3( ray->direction, wi ), p->physicalFogAnisotropy );
		const float weight = PT_Beer( p->physicalFogDensity, ( t - start ) * p->physicalMetersPerUnit )
			* p->physicalFogDensity * p->physicalFogAlbedo * step * p->physicalMetersPerUnit * lightWeight * phase;
		for ( int c = 0; c < 3; c++ ) scattering[c] += incoming[c] * weight;
	}
}
#endif

/*
========================
SampleIndirectLight

One diffuse bounce with direct lighting at the hit

Only used when r_ptMaxDepth is 1, deeper paths already continue the bounce.
========================
*/
PT_INLINE void SampleIndirectLight(
	const float *position,
	const float *normal,
	const float *direction,
	float worldTMin,
	float emissionBoost,
	bool callerSampledLights,
	__global const ptVertex_t *vertices,
	__global const ptTriangle_t *triangles,
	__global const ptBVHNode_t *bvhNodes,
	__global const int *triIndices,
	__global const ptMaterial_t *materials,
	const ptClTextures_t *textures,
	__global const ptLight_t *lights,
	int numLights,
	ptRng_t *rng,
	float *radiance,
	const ptClGlobals_t *g
) {
	radiance[0] = radiance[1] = radiance[2] = 0.0f;

	ptRay_t ray;
	ray.origin[0] = position[0] + normal[0] * g->rayOffset;
	ray.origin[1] = position[1] + normal[1] * g->rayOffset;
	ray.origin[2] = position[2] + normal[2] * g->rayOffset;
	ray.direction[0] = direction[0];
	ray.direction[1] = direction[1];
	ray.direction[2] = direction[2];
	ray.tMin = g->rayOffset;
	ray.tMax = 1e10f;
	ray.worldTMin = worldTMin;
	ray.flags = 0;

	ptHitInfo_t hitInfo;
	float albedo[4];
	float modulation[3] = { 1.0f, 1.0f, 1.0f };
	float tu = 0.0f, tv = 0.0f;
	bool foundOpaque = false;

	const int MAX_INDIRECT_STEPS = 4;
	for ( int step = 0; step < MAX_INDIRECT_STEPS; step++ ) {
		if ( !IntersectBVH( &ray, vertices, triangles, bvhNodes, triIndices, materials, &hitInfo ) ) {
			return;
		}

		__global const ptMaterial_t *mat = &materials[hitInfo.materialIndex];
		tu = hitInfo.texcoord[0];
		tv = hitInfo.texcoord[1];
		PT_ApplyTexTransform( mat, &tu, &tv );
		albedo[0] = albedo[1] = albedo[2] = albedo[3] = 1.0f;
		if ( mat->albedoTexture >= 0 && textures ) {
			SampleTextureSrgb( textures, mat->albedoTexture, tu, tv, albedo );
		}
		for ( int c = 0; c < 4; c++ ) {
			albedo[c] *= mat->albedo[c];
		}
		PT_ApplyVertexColor( mat, vertices, &triangles[hitInfo.triangleIndex],
			hitInfo.barycentrics[0], hitInfo.barycentrics[1], hitInfo.barycentrics[2], albedo );

		const bool decalOverlay = PT_IsLitDecal( mat );
		const bool passThrough = decalOverlay || mat->isAmbientOnly || mat->transmission > 0.5f
			|| mat->blendMode == PT_BLEND_ADD || PT_IsModulation( mat->blendMode )
			|| ( mat->alphaTest > 0.0f && albedo[3] < mat->alphaTest );
		if ( PT_IsModulation( mat->blendMode ) && !( mat->alphaTest > 0.0f && albedo[3] < mat->alphaTest ) ) {
			for ( int c = 0; c < 3; c++ ) modulation[c] *= PT_ModulationFactor( albedo[c], mat->blendMode );
		}
		if ( !passThrough ) {
			foundOpaque = true;
			break;
		}
		ray.origin[0] = hitInfo.position[0] + ray.direction[0] * g->rayOffset;
		ray.origin[1] = hitInfo.position[1] + ray.direction[1] * g->rayOffset;
		ray.origin[2] = hitInfo.position[2] + ray.direction[2] * g->rayOffset;
		ray.worldTMin = fmax( 0.0f, ray.worldTMin - hitInfo.t );
	}
	if ( !foundOpaque ) {
		return;
	}

	__global const ptMaterial_t *material = &materials[hitInfo.materialIndex];

	// skip emitters the caller's light sampling already covered
	if ( !( hitInfo.triFlags & PT_TRI_AREA_LIGHT ) || !callerSampledLights ) {
		float emission[3];
		PT_SampleEmission( material, textures, vertices, &triangles[hitInfo.triangleIndex],
			hitInfo.barycentrics[0], hitInfo.barycentrics[1], hitInfo.barycentrics[2],
			hitInfo.texcoord[0], hitInfo.texcoord[1], emission );
		for ( int c = 0; c < 3; c++ ) radiance[c] += emission[c] * emissionBoost;
	}

	int pick;
	float invPdf;
	if ( PT_SelectLights( lights, numLights, hitInfo.position, hitInfo.normal, 1, emissionBoost, rng, &pick, &invPdf, NULL ) > 0 ) {
		float directLight[3], wi[3];
		SampleDirectLight( &lights[pick], hitInfo.position, hitInfo.normal, 0.0f, emissionBoost,
			vertices, triangles, bvhNodes, triIndices, materials, textures, rng, directLight, wi, NULL, false, g );

		radiance[0] += albedo[0] * directLight[0] * invPdf / PT_PI;
		radiance[1] += albedo[1] * directLight[1] * invPdf / PT_PI;
		radiance[2] += albedo[2] * directLight[2] * invPdf / PT_PI;
	}

	// clamp indirect
	const float maxIndirect = 1.5f;
	radiance[0] = fmin( radiance[0] * modulation[0], maxIndirect );
	radiance[1] = fmin( radiance[1] * modulation[1], maxIndirect );
	radiance[2] = fmin( radiance[2] * modulation[2], maxIndirect );
}
