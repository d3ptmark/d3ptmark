/*
===============================================================================

	OpenCL C port of PathTracingKernel and ToneMappingKernel from
	pt_kernel_pathtracing.cuh.

===============================================================================
*/

/*
========================
PT_ZeroGuide
========================
*/
PT_INLINE void PT_ZeroGuide( ptClDenoiseGuide_t *guide ) {
	for ( int c = 0; c < 3; c++ ) {
		guide->normal[c] = 0.0f;
		guide->position[c] = 0.0f;
		guide->albedo[c] = 0.0f;
		guide->shadingNormal[c] = 0.0f;
		guide->specularAlbedo[c] = 0.0f;
	}
	guide->depth = 0.0f;
	guide->materialIndex = 0;
	guide->historyValid = 0;
	guide->roughness = 0.0f;
	guide->coverage = 0;
	guide->volumeDepth = 0.0f;
}

/*
========================
PathTracingKernel

Full PBR path tracing

The five trailing buffers are optional (NULL), like the CUDA defaults.
========================
*/
__kernel void PathTracingKernel(
	PT_SCENE_KERNEL_ARGS,
	__global float *framebuffer,
	const ptRenderParams_t params,
	__global float *radianceBuffer,
	__global ptClDenoiseGuide_t *guideBuffer,
	__global float *diffuseBuffer,
	__global float *specularBuffer,
	__global float *volumeBuffer
) {
	PT_UNPACK_RENDER_PARAMS( params );
	const ptClTextures_t textureTable = { textureDesc, texels };
	const ptClTextures_t *textures = textureDesc ? &textureTable : NULL;	// CUDA passes NULL tables through
	// LaunchPathTracingKernel stages these into g_softShadowScale / g_rayOffset
	const ptClGlobals_t globals = { params.softShadowScale, params.rayOffset };
	const ptClGlobals_t *g = &globals;
	int x = get_global_id( 0 );
	int y = get_global_id( 1 );

	if ( x >= width || y >= height ) return;

	int pixelIndex = ( y * width + x ) * 4;

	ptRng_t rng;
	PT_RngInit( &rng, (uint)( y * width + x ), params.sampleIndex );

	float color[3] = { 0.0f, 0.0f, 0.0f };
	float diffuseColor[3] = { 0.0f, 0.0f, 0.0f }, specularColor[3] = { 0.0f, 0.0f, 0.0f }, volumeColor[3] = { 0.0f, 0.0f, 0.0f };
	ptClDenoiseGuide_t guide;
	PT_ZeroGuide( &guide );

	for ( int sample = 0; sample < samplesPerPixel; sample++ ) {
		bool guideWritten = false;
		ptClDenoiseGuide_t sampleGuide;
		PT_ZeroGuide( &sampleGuide );
		// primary ray with jitter
		float jitterX = PT_Rand( &rng ) - 0.5f;
		float jitterY = PT_Rand( &rng ) - 0.5f;

		float u = ( (float)( x ) + 0.5f + jitterX ) / (float)width * 2.0f - 1.0f;
		float v = ( (float)( y ) + 0.5f + jitterY ) / (float)height * 2.0f - 1.0f;

		float tanHalfFovX = PT_TanHalfFov( fov );
		float tanHalfFovY = PT_TanHalfFov( fovY );

		ptRay_t ray;
		ray.origin[0] = cameraPos[0];
		ray.origin[1] = cameraPos[1];
		ray.origin[2] = cameraPos[2];

		ray.direction[0] = cameraForward[0] + u * cameraRight[0] * tanHalfFovX + v * cameraUp[0] * tanHalfFovY;
		ray.direction[1] = cameraForward[1] + u * cameraRight[1] * tanHalfFovX + v * cameraUp[1] * tanHalfFovY;
		ray.direction[2] = cameraForward[2] + u * cameraRight[2] * tanHalfFovX + v * cameraUp[2] * tanHalfFovY;
		normalize3( ray.direction );
#ifdef PT_PHYSICAL
		if ( PT_PhysicalActive( &params ) ) PT_PhysicalLens( &params, &rng, ray.origin, ray.direction );
#endif

		ray.tMin = rayOffset;
		ray.tMax = 1e10f;
		ray.worldTMin = 0.0f;
		ray.flags = PT_RAY_SEES_WEAPON;

		float pathThroughput[3] = { 1.0f, 1.0f, 1.0f };
		float pathRadiance[3] = { 0.0f, 0.0f, 0.0f };
		float sampleDiffuse[3] = { 0.0f, 0.0f, 0.0f }, sampleSpecular[3] = { 0.0f, 0.0f, 0.0f }, volumeRadiance[3] = { 0.0f, 0.0f, 0.0f };
		float indirectBaseline[3] = { 0.0f, 0.0f, 0.0f }, volumeBaseline[3] = { 0.0f, 0.0f, 0.0f }, diffuseFraction[3] = { 0.0f, 0.0f, 0.0f };
		bool splitIndirect = false;
		bool lastSampledLights = false;
#ifdef PT_PHYSICAL
		int mediumMaterials[16];
		int mediumCount = 0;
#endif

		int depth = 0;
		const int maxTransparentPasses = maxDepth * 8;  // generous budget for overlapping particles/decals
		for ( int iteration = 0; iteration < maxTransparentPasses && depth < maxDepth; iteration++ ) {
			ptHitInfo_t hitInfo;
			hitInfo.hit = false;
			hitInfo.t = ray.tMax;
			hitInfo.materialIndex = 0;
			hitInfo.triangleIndex = 0;
			hitInfo.barycentrics[0] = 0.0f;
			hitInfo.barycentrics[1] = 0.0f;
			hitInfo.barycentrics[2] = 0.0f;

			const bool foundHit = IntersectBVH( &ray, vertices, triangles, bvhNodes, triIndices, materials, &hitInfo );
#ifdef PT_PHYSICAL
			if ( PT_PhysicalActive( &params ) ) {
				const float segmentLength = foundHit ? hitInfo.t : ray.tMax;
				if ( mediumCount > 0 ) {
					__global const ptMaterial_t *medium = &materials[mediumMaterials[mediumCount - 1]];
					for ( int c = 0; c < 3; c++ ) pathThroughput[c] *= PT_Beer( medium->absorption[c],
						segmentLength * params.physicalMetersPerUnit );
				} else if ( params.physicalFogDensity > 0.0f ) {
					float scattering[3];
					PT_PhysicalSingleScattering( &params, &ray, segmentLength, vertices, triangles, bvhNodes,
						triIndices, materials, textures, lights, numLights, &rng, scattering, g );
					const float transmit = PT_PhysicalFogTransmit( &params, ray.origin, ray.direction, segmentLength );
					for ( int c = 0; c < 3; c++ ) {
						const float contribution = pathThroughput[c] * scattering[c];
						pathRadiance[c] += contribution;
						volumeRadiance[c] += contribution;
						pathThroughput[c] *= transmit;
					}
				}
			}
#endif
			if ( !foundHit ) {
				// sky dome - primary rays only (bounce misses return black for Doom 3 darkness)
				if ( ( depth == 0
#ifdef PT_PHYSICAL
					|| PT_PhysicalActive( &params )
#endif
					) && skyIntensity > 0.0f ) {
					float upDot = ray.direction[2];
					float skyColor[3];
					if ( upDot > 0.0f ) {
						float t = fmin( 1.0f, upDot * 2.0f );
						skyColor[0] = skyColorHorizon[0] * ( 1.0f - t ) + skyColorZenith[0] * t;
						skyColor[1] = skyColorHorizon[1] * ( 1.0f - t ) + skyColorZenith[1] * t;
						skyColor[2] = skyColorHorizon[2] * ( 1.0f - t ) + skyColorZenith[2] * t;
					} else {
						float t = fmin( 1.0f, -upDot * 3.0f );
						skyColor[0] = skyColorHorizon[0] * ( 1.0f - t ) + skyColorGround[0] * t;
						skyColor[1] = skyColorHorizon[1] * ( 1.0f - t ) + skyColorGround[1] * t;
						skyColor[2] = skyColorHorizon[2] * ( 1.0f - t ) + skyColorGround[2] * t;
					}
					pathRadiance[0] += pathThroughput[0] * skyColor[0] * skyIntensity;
					pathRadiance[1] += pathThroughput[1] * skyColor[1] * skyIntensity;
					pathRadiance[2] += pathThroughput[2] * skyColor[2] * skyIntensity;
				}
				break;
			}

			// pass-through continuations restart at the hit, keep the clearance where it was
			ray.worldTMin = fmax( 0.0f, ray.worldTMin - hitInfo.t );

			__global const ptMaterial_t *material = &materials[hitInfo.materialIndex];
			float materialIor = 0.0f;
#ifdef PT_PHYSICAL
			if ( PT_PhysicalActive( &params ) ) {
				materialIor = material->physicalMaterial == PT_PHYSICAL_AUTHORED
					|| ( material->physicalMaterial == PT_PHYSICAL_CALIBRATED && material->transmission > 0.0f )
					? material->ior : params.physicalIor;
			}
#endif

			// rays leaving the view weapon skip the world geometry it pokes into
			const bool hitWeapon = ( hitInfo.triFlags & PT_TRI_WEAPON ) != 0;
			const float shadeWorldTMin = hitWeapon ? PT_WEAPON_CLEARANCE : 0.0f;

			// ---------------------------------------------------------------
			// Apply texture transforms to UV
			// ---------------------------------------------------------------
			float tu = hitInfo.texcoord[0];
			float tv = hitInfo.texcoord[1];

			PT_ApplyTexTransform( material, &tu, &tv );

			// ---------------------------------------------------------------
			// Sample albedo texture
			// ---------------------------------------------------------------
			float albedo[4];
			if ( material->albedoTexture >= 0 && textures ) {
				SampleTextureSrgb( textures, material->albedoTexture, tu, tv, albedo );
				albedo[0] *= material->albedo[0];
				albedo[1] *= material->albedo[1];
				albedo[2] *= material->albedo[2];
				albedo[3] *= material->albedo[3];
			} else {
				albedo[0] = material->albedo[0];
				albedo[1] = material->albedo[1];
				albedo[2] = material->albedo[2];
				albedo[3] = material->albedo[3];
			}

			// ---------------------------------------------------------------
			// Normal mapping with RXGB format & degenerate tangent frame
			// ---------------------------------------------------------------
			float surfaceNormal[3];
			surfaceNormal[0] = hitInfo.normal[0];
			surfaceNormal[1] = hitInfo.normal[1];
			surfaceNormal[2] = hitInfo.normal[2];

			if ( material->normalTexture >= 0 && textures ) {
				float normalMap[4];
				SampleTexture( textures, material->normalTexture, tu, tv, normalMap );

				// RXGB format: A=X, G=Y, B=Z
				float tangentNormal[3];
				tangentNormal[0] = ( normalMap[3] * 2.0f - 1.0f ) * material->bumpScale;
				tangentNormal[1] = ( normalMap[1] * 2.0f - 1.0f ) * material->bumpScale;
				tangentNormal[2] = ( normalMap[2] * 2.0f - 1.0f );

				float T[3] = { hitInfo.tangent[0], hitInfo.tangent[1], hitInfo.tangent[2] };
				float B[3] = { hitInfo.binormal[0], hitInfo.binormal[1], hitInfo.binormal[2] };
				float N[3] = { hitInfo.normal[0], hitInfo.normal[1], hitInfo.normal[2] };

				// Fabricate tangent frame if degenerate
				float tLen = T[0] * T[0] + T[1] * T[1] + T[2] * T[2];
				float bLen = B[0] * B[0] + B[1] * B[1] + B[2] * B[2];

				if ( tLen < 0.001f || bLen < 0.001f ) {
					if ( fabs( N[2] ) < 0.999f ) {
						float invLen = 1.0f / sqrt( N[0] * N[0] + N[1] * N[1] );
						T[0] = -N[1] * invLen; T[1] = N[0] * invLen; T[2] = 0.0f;
					} else {
						T[0] = 1.0f; T[1] = 0.0f; T[2] = 0.0f;
					}
					cross3( N, T, B );
				}

				surfaceNormal[0] = T[0] * tangentNormal[0] + B[0] * tangentNormal[1] + N[0] * tangentNormal[2];
				surfaceNormal[1] = T[1] * tangentNormal[0] + B[1] * tangentNormal[1] + N[1] * tangentNormal[2];
				surfaceNormal[2] = T[2] * tangentNormal[0] + B[2] * tangentNormal[1] + N[2] * tangentNormal[2];

				float len = sqrt( surfaceNormal[0] * surfaceNormal[0] + surfaceNormal[1] * surfaceNormal[1] + surfaceNormal[2] * surfaceNormal[2] );
				if ( len > 0.001f ) {
					surfaceNormal[0] /= len;
					surfaceNormal[1] /= len;
					surfaceNormal[2] /= len;
				}
			}

			// ---------------------------------------------------------------
			// Specular map -> PBR roughness / metallic / spec color
			// ---------------------------------------------------------------
			float metallic = material->metallic;
			float roughness = material->roughness;
			float specColor[3] = { 1.0f, 1.0f, 1.0f };

			if ( material->specularTexture >= 0 && textures
#ifdef PT_PHYSICAL
				&& material->physicalMaterial != PT_PHYSICAL_AUTHORED
#endif
				) {
				float specularMap[4];
				SampleTexture( textures, material->specularTexture, tu, tv, specularMap );

				specColor[0] = specularMap[0];
				specColor[1] = specularMap[1];
				specColor[2] = specularMap[2];

				float specLum = specularMap[0] * 0.2126f + specularMap[1] * 0.7152f + specularMap[2] * 0.0722f;
				roughness *= ( 1.0f - specLum * 0.6f );
#ifdef PT_PHYSICAL
				if ( !PT_PhysicalActive( &params ) )
#endif
				metallic = fmax( metallic, specLum * 0.3f );
				metallic = fmin( 1.0f, fmax( 0.0f, metallic ) );
				roughness = fmin( 1.0f, fmax( 0.01f, roughness ) );
			}

			// ---------------------------------------------------------------
			// Vertex color
			// ---------------------------------------------------------------
			PT_ApplyVertexColor( material, vertices, &triangles[hitInfo.triangleIndex],
				hitInfo.barycentrics[0], hitInfo.barycentrics[1], hitInfo.barycentrics[2], albedo );
			if ( material->additiveUsesAlpha == PT_ADDITIVE_LOCAL_MASK ) {
				albedo[3] = PT_AdditiveMaskAlpha( material, textures, vertices, &triangles[hitInfo.triangleIndex],
					hitInfo.barycentrics[0], hitInfo.barycentrics[1], hitInfo.barycentrics[2],
					hitInfo.texcoord[0], hitInfo.texcoord[1] );
			}
#ifdef PT_PHYSICAL
			if ( PT_PhysicalActive( &params ) ) {
				for ( int c = 0; c < 3; c++ ) albedo[c] = PT_Clamp( albedo[c], 0.0f, 1.0f );
				metallic = PT_Clamp( material->metallic, 0.0f, 1.0f );
				roughness = PT_Clamp( roughness * params.physicalRoughnessScale, 0.04f, 1.0f );
			}
#endif

			// ---------------------------------------------------------------
			// Alpha test: skip transparent fragments
			// ---------------------------------------------------------------
			if ( material->alphaTest > 0.0f && albedo[3] < material->alphaTest ) {
				ray.origin[0] = hitInfo.position[0] + ray.direction[0] * rayOffset;
				ray.origin[1] = hitInfo.position[1] + ray.direction[1] * rayOffset;
				ray.origin[2] = hitInfo.position[2] + ray.direction[2] * rayOffset;
				ray.tMin = rayOffset;
				continue;  // transparent skip - does NOT increment depth
			}

			// Invisible local masks must not replace the receiver's denoising guides.
			if ( ( material->blendMode == PT_BLEND_ALPHA && albedo[3] < 0.05f )
				|| ( material->blendMode == PT_BLEND_ADD
					&& material->additiveUsesAlpha == PT_ADDITIVE_LOCAL_MASK && albedo[3] <= 0.0f ) ) {
				ray.origin[0] = hitInfo.position[0] + ray.direction[0] * rayOffset;
				ray.origin[1] = hitInfo.position[1] + ray.direction[1] * rayOffset;
				ray.origin[2] = hitInfo.position[2] + ray.direction[2] * rayOffset;
				ray.tMin = rayOffset;
				continue;  // transparent skip - does NOT increment depth
			}

			// Primary emission stays sharp; surface and volume lighting are filtered separately.
			if ( guideBuffer && !guideWritten ) {
				guideWritten = true;
				sampleGuide.coverage = material->blendMode != PT_BLEND_OPAQUE || material->transmission > 0.0f
					? PT_DENOISE_TRANSPARENT : PT_DENOISE_OVERLAY;
				float cameraDelta[3];
				for ( int c = 0; c < 3; c++ ) {
					sampleGuide.normal[c] = hitInfo.normal[c];
					sampleGuide.shadingNormal[c] = surfaceNormal[c];
					sampleGuide.position[c] = hitInfo.position[c];
					cameraDelta[c] = hitInfo.position[c] - cameraPos[c];
				}
				sampleGuide.volumeDepth = dot3( cameraDelta, cameraForward );
				if ( material->blendMode == PT_BLEND_OPAQUE && material->transmission == 0.0f
					&& !material->isAmbientOnly && ( !material->noShadows || material->alphaTest > 0.0f ) ) {
					for ( int c = 0; c < 3; c++ ) {
						sampleGuide.albedo[c] = fmax( 0.04f, albedo[c] * ( 1.0f - metallic ) );
						const float f0 = 0.04f * ( 1.0f - metallic ) + albedo[c] * metallic;
						sampleGuide.specularAlbedo[c] = fmax( 0.01f, f0 * specularBoost * specColor[c] );
#ifdef PT_PHYSICAL
						if ( PT_PhysicalActive( &params ) ) {
							sampleGuide.specularAlbedo[c] = fmax( 0.01f, PT_PhysicalF0( albedo[c], metallic, materialIor ) );
						}
#endif
					}
					sampleGuide.depth = sampleGuide.volumeDepth;
					sampleGuide.roughness = fmin( 1.0f, fmax( 0.04f, roughness ) );
					sampleGuide.materialIndex = hitInfo.materialIndex;
					sampleGuide.historyValid = ( hitInfo.triFlags & ( PT_TRI_NO_HISTORY | PT_TRI_WEAPON ) ) == 0;
#ifdef PT_PHYSICAL
					if ( params.physicalApertureRadius > 0.0f ) sampleGuide.historyValid = 0;
#endif
					sampleGuide.coverage = sampleGuide.historyValid ? PT_DENOISE_STATIC : PT_DENOISE_DYNAMIC;
				}
			}

			// ---------------------------------------------------------------
			// Lit noshadows decal overlays
			// ---------------------------------------------------------------
			if ( PT_IsLitDecal( material ) ) {
				float alpha = albedo[3];
				float lum = albedo[0] * 0.2126f + albedo[1] * 0.7152f + albedo[2] * 0.0722f;
				if ( lum < 0.02f ) alpha = 0.0f;

				if ( alpha < 0.05f ) {
					ray.origin[0] = hitInfo.position[0] + ray.direction[0] * rayOffset;
					ray.origin[1] = hitInfo.position[1] + ray.direction[1] * rayOffset;
					ray.origin[2] = hitInfo.position[2] + ray.direction[2] * rayOffset;
					ray.tMin = rayOffset;
					continue;
				}

				// light the decal overlay
				int picks[PT_MAX_LIGHT_PICKS];
				float pickWeights[PT_MAX_LIGHT_PICKS];
				const int numPicks = PT_SelectLights( lights, numLights, hitInfo.position, surfaceNormal,
					min( numLights, maxLightSamples ), emissionBoost, &rng, picks, pickWeights, &params );
				for ( int i = 0; i < numPicks; i++ ) {
					float directLight[3], wi[3];
					SampleDirectLight( &lights[picks[i]], hitInfo.position, surfaceNormal, shadeWorldTMin, emissionBoost,
						vertices, triangles, bvhNodes, triIndices, materials, textures, &rng, directLight, wi, &params, false, g );
					float w = alpha * pickWeights[i];
#ifdef PT_PHYSICAL
					if ( PT_PhysicalActive( &params ) ) w /= PT_PI;
#endif
					pathRadiance[0] += pathThroughput[0] * directLight[0] * albedo[0] * w;
					pathRadiance[1] += pathThroughput[1] * directLight[1] * albedo[1] * w;
					pathRadiance[2] += pathThroughput[2] * directLight[2] * albedo[2] * w;
				}

				float emission[3];
				PT_SampleEmission( material, textures, vertices, &triangles[hitInfo.triangleIndex],
					hitInfo.barycentrics[0], hitInfo.barycentrics[1], hitInfo.barycentrics[2],
					hitInfo.texcoord[0], hitInfo.texcoord[1], emission );
				for ( int c = 0; c < 3; c++ ) pathRadiance[c] += pathThroughput[c] * emission[c] * emissionBoost * alpha;

				pathThroughput[0] *= ( 1.0f - alpha );
				pathThroughput[1] *= ( 1.0f - alpha );
				pathThroughput[2] *= ( 1.0f - alpha );

				ray.origin[0] = hitInfo.position[0] + ray.direction[0] * rayOffset;
				ray.origin[1] = hitInfo.position[1] + ray.direction[1] * rayOffset;
				ray.origin[2] = hitInfo.position[2] + ray.direction[2] * rayOffset;
				ray.tMin = rayOffset;
				continue;  // decal overlay - does NOT increment depth
			}

			// ---------------------------------------------------------------
			// Ambient-only materials (self-illuminated, HUD, etc)
			// ---------------------------------------------------------------
			if ( material->isAmbientOnly ) {
				float alpha = albedo[3];
				float overlayScale = 1.0f;
#ifdef PT_PHYSICAL
				if ( PT_PhysicalActive( &params ) && material->emissiveOverlay ) overlayScale = emissionBoost;
#endif

				if ( material->blendMode == 2 ) {
					// additive
					const float weight = PT_AdditiveOpacity( material, alpha ) * overlayScale;
					for ( int c = 0; c < 3; c++ ) pathRadiance[c] += pathThroughput[c] * albedo[c] * weight;
				} else if ( PT_IsModulation( material->blendMode ) ) {
					// modulate
					for ( int c = 0; c < 3; c++ ) pathThroughput[c] *= PT_ModulationFactor( albedo[c], material->blendMode );
				} else {
					// default alpha-over
					pathRadiance[0] += pathThroughput[0] * albedo[0] * alpha * overlayScale;
					pathRadiance[1] += pathThroughput[1] * albedo[1] * alpha * overlayScale;
					pathRadiance[2] += pathThroughput[2] * albedo[2] * alpha * overlayScale;
					pathThroughput[0] *= ( 1.0f - alpha );
					pathThroughput[1] *= ( 1.0f - alpha );
					pathThroughput[2] *= ( 1.0f - alpha );
				}

				ray.origin[0] = hitInfo.position[0] + ray.direction[0] * rayOffset;
				ray.origin[1] = hitInfo.position[1] + ray.direction[1] * rayOffset;
				ray.origin[2] = hitInfo.position[2] + ray.direction[2] * rayOffset;
				ray.tMin = rayOffset;
				continue;  // ambient-only overlay - does NOT increment depth
			}

			// ---------------------------------------------------------------
			// Additive blending
			// ---------------------------------------------------------------
			if ( material->blendMode == 2 ) {
				const float weight = PT_AdditiveOpacity( material, albedo[3] );
				for ( int c = 0; c < 3; c++ ) pathRadiance[c] += pathThroughput[c] * albedo[c] * weight;
				ray.origin[0] = hitInfo.position[0] + ray.direction[0] * rayOffset;
				ray.origin[1] = hitInfo.position[1] + ray.direction[1] * rayOffset;
				ray.origin[2] = hitInfo.position[2] + ray.direction[2] * rayOffset;
				ray.tMin = rayOffset;
				continue;  // additive overlay - does NOT increment depth
			} else if ( PT_IsModulation( material->blendMode ) ) {
				// modulate
				for ( int c = 0; c < 3; c++ ) pathThroughput[c] *= PT_ModulationFactor( albedo[c], material->blendMode );
				ray.origin[0] = hitInfo.position[0] + ray.direction[0] * rayOffset;
				ray.origin[1] = hitInfo.position[1] + ray.direction[1] * rayOffset;
				ray.origin[2] = hitInfo.position[2] + ray.direction[2] * rayOffset;
				ray.tMin = rayOffset;
				continue;  // modulate overlay - does NOT increment depth
			}

			// ---------------------------------------------------------------
			// Transmission / glass: Fresnel reflection vs refraction
			// ---------------------------------------------------------------
#ifdef PT_PHYSICAL
			if ( PT_PhysicalActive( &params ) && material->physicalMaterial && material->transmission > 0.0f
				&& ( depth == 0 || !( hitInfo.triFlags & PT_TRI_AREA_LIGHT ) || !lastSampledLights ) ) {
				float emission[3];
				PT_SampleEmission( material, textures, vertices, &triangles[hitInfo.triangleIndex],
					hitInfo.barycentrics[0], hitInfo.barycentrics[1], hitInfo.barycentrics[2],
					hitInfo.texcoord[0], hitInfo.texcoord[1], emission );
				for ( int c = 0; c < 3; c++ ) pathRadiance[c] += pathThroughput[c] * emission[c] * emissionBoost;
			}
			if ( PT_PhysicalActive( &params ) && material->physicalMaterial && material->transmission > 0.0f
				&& PT_Rand( &rng ) < material->transmission ) {
				__global const ptTriangle_t *tri = &triangles[hitInfo.triangleIndex];
				float e1[3], e2[3], normal[3];
				for ( int c = 0; c < 3; c++ ) {
					e1[c] = vertices[tri->vertexIndices[1]].position[c] - vertices[tri->vertexIndices[0]].position[c];
					e2[c] = vertices[tri->vertexIndices[2]].position[c] - vertices[tri->vertexIndices[0]].position[c];
				}
				cross3( e1, e2, normal );
				normalize3( normal );
				const bool entering = dot3( ray.direction, normal ) < 0.0f;
				if ( !entering ) for ( int c = 0; c < 3; c++ ) normal[c] = -normal[c];
				const bool solid = material->thinTransmission == 0;
				const float etaI = mediumCount > 0 ? materials[mediumMaterials[mediumCount - 1]].ior
					: ( solid && !entering ? material->ior : 1.0f );
				const float etaT = solid && !entering
					? ( mediumCount > 1 ? materials[mediumMaterials[mediumCount - 2]].ior : 1.0f ) : material->ior;
				const float cosine = fmax( 0.0f, -dot3( ray.direction, normal ) );
				float cosineT;
				float fresnel = PT_DielectricFresnel( cosine, etaI, etaT, &cosineT );
				float sheetReflection[3], sheetTransmission[3];
				if ( !solid ) {
					float probability = 0.0f;
					for ( int c = 0; c < 3; c++ ) {
						PT_ThinDielectric( fresnel, PT_Beer( material->absorption[c],
							material->thickness / fmax( cosineT, 1e-5f ) ), &sheetReflection[c], &sheetTransmission[c] );
						probability += sheetReflection[c] / 3.0f;
					}
					fresnel = probability;
				}
				const bool reflected = PT_Rand( &rng ) < fresnel;
				float next[3];
				bool sampledTransmissionLights = false;
				if ( reflected ) {
					for ( int c = 0; c < 3; c++ ) {
						next[c] = ray.direction[c] + 2.0f * cosine * normal[c];
						if ( !solid ) pathThroughput[c] *= sheetReflection[c] / fmax( fresnel, 1e-8f );
					}
				} else if ( solid ) {
					const float eta = etaI / etaT;
					for ( int c = 0; c < 3; c++ ) {
						next[c] = eta * ray.direction[c] + ( eta * cosine - cosineT ) * normal[c];
						pathThroughput[c] *= eta * eta;
					}
					if ( entering ) {
						// Every interface consumes a bounce; maxDepth is bounded to 16.
						if ( mediumCount >= 16 ) break;
						mediumMaterials[mediumCount++] = hitInfo.materialIndex;
					} else if ( mediumCount > 0 ) {
						mediumCount--;
					} else {
						for ( int c = 0; c < 3; c++ ) pathThroughput[c] *= PT_Beer( material->absorption[c],
							hitInfo.t * params.physicalMetersPerUnit );
					}
				} else {
					for ( int c = 0; c < 3; c++ ) pathThroughput[c] *= sheetTransmission[c] / fmax( 1.0f - fresnel, 1e-8f );
					if ( material->thinTransmission == 2 ) {
						float opposite[3], pdf;
						for ( int c = 0; c < 3; c++ ) { opposite[c] = -normal[c]; pathThroughput[c] *= albedo[c]; }
						int picks[PT_MAX_LIGHT_PICKS];
						float weights[PT_MAX_LIGHT_PICKS];
						const int count = PT_SelectLights( lights, numLights, hitInfo.position, opposite,
							min( numLights, maxLightSamples ), emissionBoost, &rng, picks, weights, &params );
						sampledTransmissionLights = count > 0;
						for ( int i = 0; i < count; i++ ) {
							float direct[3], wi[3];
							SampleDirectLight( &lights[picks[i]], hitInfo.position, opposite, shadeWorldTMin, emissionBoost,
								vertices, triangles, bvhNodes, triIndices, materials, textures, &rng, direct, wi, &params, false, g );
							for ( int c = 0; c < 3; c++ ) pathRadiance[c] += pathThroughput[c] * direct[c] * weights[i] / PT_PI;
						}
						SampleHemisphere( opposite, &rng, next, &pdf );
					} else {
						for ( int c = 0; c < 3; c++ ) next[c] = ray.direction[c];
					}
				}
				normalize3( next );
				for ( int c = 0; c < 3; c++ ) {
					ray.origin[c] = hitInfo.position[c] + next[c] * rayOffset;
					ray.direction[c] = next[c];
				}
				ray.tMin = rayOffset;
				ray.tMax = 1e10f;
				ray.worldTMin = shadeWorldTMin;
				ray.flags = 0;
				lastSampledLights = sampledTransmissionLights;
				depth++;
				continue;
			}
#endif
			if ( material->transmission > 0.0f ) {
#ifdef PT_PHYSICAL
				if ( !PT_PhysicalActive( &params ) || !material->physicalMaterial ) {
#endif
				float negDir[3] = { -ray.direction[0], -ray.direction[1], -ray.direction[2] };
				float cosTheta = fabs( dot3( surfaceNormal, negDir ) );
				float r0 = ( material->ior - 1.0f ) / ( material->ior + 1.0f );
				r0 = r0 * r0;
				float fresnel = r0 + ( 1.0f - r0 ) * pow( 1.0f - cosTheta, 5.0f );
				float reflectProb = fresnel + ( 1.0f - fresnel ) * ( 1.0f - material->transmission );

				if ( PT_Rand( &rng ) > reflectProb ) {
					// transmit through
					pathThroughput[0] *= albedo[0];
					pathThroughput[1] *= albedo[1];
					pathThroughput[2] *= albedo[2];
					ray.origin[0] = hitInfo.position[0] - surfaceNormal[0] * rayOffset;
					ray.origin[1] = hitInfo.position[1] - surfaceNormal[1] * rayOffset;
					ray.origin[2] = hitInfo.position[2] - surfaceNormal[2] * rayOffset;
					ray.tMin = rayOffset;
					continue;  // glass transmission - does NOT increment depth
				}
#ifdef PT_PHYSICAL
				}
#endif
			}

			// ---------------------------------------------------------------
			// Emission
			// ---------------------------------------------------------------
			// skip emitters the previous bounce's light sampling already covered
			if ( ( depth == 0 || !( hitInfo.triFlags & PT_TRI_AREA_LIGHT ) || !lastSampledLights )
#ifdef PT_PHYSICAL
				&& !( PT_PhysicalActive( &params ) && material->physicalMaterial && material->transmission > 0.0f )
#endif
				) {
				float emission[3];
				PT_SampleEmission( material, textures, vertices, &triangles[hitInfo.triangleIndex],
					hitInfo.barycentrics[0], hitInfo.barycentrics[1], hitInfo.barycentrics[2],
					hitInfo.texcoord[0], hitInfo.texcoord[1], emission );
				for ( int c = 0; c < 3; c++ ) pathRadiance[c] += pathThroughput[c] * emission[c] * emissionBoost;
			}

			// ---------------------------------------------------------------
			// Direct light sampling with PBR BRDF
			// ---------------------------------------------------------------
			bool sampledLights = false;
			{
				int picks[PT_MAX_LIGHT_PICKS];
				float pickWeights[PT_MAX_LIGHT_PICKS];
				const int numPicks = PT_SelectLights( lights, numLights, hitInfo.position, surfaceNormal,
					min( numLights, maxLightSamples ), emissionBoost, &rng, picks, pickWeights, &params );
				sampledLights = numPicks > 0;

				for ( int i = 0; i < numPicks; i++ ) {
					float directLight[3], wi[3];
					SampleDirectLight( &lights[picks[i]], hitInfo.position, surfaceNormal, shadeWorldTMin, emissionBoost,
						vertices, triangles, bvhNodes, triIndices, materials, textures, &rng, directLight, wi, &params, false, g );
					if ( directLight[0] + directLight[1] + directLight[2] <= 0.0f ) {
						continue;
					}

					const float lightWeight = pickWeights[i];
					if ( material->blendMode == PT_BLEND_OPAQUE || material->blendMode == PT_BLEND_ALPHA ) {
						float wo[3] = { -ray.direction[0], -ray.direction[1], -ray.direction[2] };
						float directBrdf[3], diffuseBrdf[3];
						EvaluatePBRMaterial( albedo, surfaceNormal, wo, wi, metallic, roughness, specularBoost, specColor,
							directBrdf, diffuseBrdf, &params, materialIor );

						float alphaScale = ( material->blendMode == PT_BLEND_ALPHA ) ? albedo[3] : 1.0f;
						pathRadiance[0] += pathThroughput[0] * directLight[0] * directBrdf[0] * alphaScale * lightWeight;
						pathRadiance[1] += pathThroughput[1] * directLight[1] * directBrdf[1] * alphaScale * lightWeight;
						pathRadiance[2] += pathThroughput[2] * directLight[2] * directBrdf[2] * alphaScale * lightWeight;
						if ( sampleGuide.depth > 0.0f && depth == 0 ) {
							for ( int c = 0; c < 3; c++ ) {
								const float incoming = pathThroughput[c] * directLight[c] * alphaScale * lightWeight;
								sampleDiffuse[c] += incoming * diffuseBrdf[c];
								sampleSpecular[c] += incoming * ( directBrdf[c] - diffuseBrdf[c] );
							}
						}
					} else {
						// fallback Lambertian
						pathRadiance[0] += pathThroughput[0] * directLight[0] * albedo[0] * lightWeight;
						pathRadiance[1] += pathThroughput[1] * directLight[1] * albedo[1] * lightWeight;
						pathRadiance[2] += pathThroughput[2] * directLight[2] * albedo[2] * lightWeight;
					}
				}
			}

			// ---------------------------------------------------------------
			// Volumetric scattering along the segment that reached this hit.
			// Uniform light picks: importance at the surface says nothing
			// about the lights the segment passes through.
			// ---------------------------------------------------------------
			if ( depth < 2 && volumetricDensity > 0.0f && numLights > 0 ) {
				const int numSamples = min( numLights, maxLightSamples );
				const float lightWeight = (float)numLights / (float)numSamples;
				for ( int i = 0; i < numSamples; i++ ) {
					const int lightIndex = min( (int)( PT_Rand( &rng ) * numLights ), numLights - 1 );
					if ( lights[lightIndex].volumetric <= 0.0f ) {
						continue;
					}
					float volumetric[3];
					SampleVolumetricScattering( &lights[lightIndex], ray.origin, ray.direction, hitInfo.t,
						vertices, triangles, bvhNodes, triIndices, materials, textures, &rng, volumetric,
						volumetricDensity, volumetricSteps, volumetricAnisotropy, volFalloff, volMaxDist, g );

					pathRadiance[0] += pathThroughput[0] * volumetric[0] * lightWeight;
					pathRadiance[1] += pathThroughput[1] * volumetric[1] * lightWeight;
					pathRadiance[2] += pathThroughput[2] * volumetric[2] * lightWeight;
					for ( int c = 0; c < 3; c++ ) volumeRadiance[c] += pathThroughput[c] * volumetric[c] * lightWeight;
				}
			}

			// No ambient fill - Doom 3 scenes are lit exclusively by placed lights.

			// ---------------------------------------------------------------
			// Indirect lighting (one-bounce GI), only for single bounce
			// paths: deeper paths already continue into the same bounce
			// ---------------------------------------------------------------
			if ( depth == 0 && maxDepth <= 1 && indirectProb > 0.0f && PT_Rand( &rng ) < indirectProb ) {
				float indirectDir[3];
				float indirectPdf;
				SampleHemisphere( surfaceNormal, &rng, indirectDir, &indirectPdf );

				if ( indirectPdf > 1e-6f ) {
					float indirectLight[3];
					SampleIndirectLight( hitInfo.position, surfaceNormal, indirectDir, shadeWorldTMin, emissionBoost, sampledLights,
						vertices, triangles, bvhNodes, triIndices, materials, textures,
						lights, numLights, &rng, indirectLight, g );

					if ( !isfinite( indirectLight[0] ) ) indirectLight[0] = 0.0f;
					if ( !isfinite( indirectLight[1] ) ) indirectLight[1] = 0.0f;
					if ( !isfinite( indirectLight[2] ) ) indirectLight[2] = 0.0f;

					if ( material->blendMode == 0 || material->blendMode == 1 ) {
						float indirectBrdf[3], diffuseBrdf[3];
						float wo[3] = { -ray.direction[0], -ray.direction[1], -ray.direction[2] };
						float NdotL_indirect = fmax( 0.0f, dot3( surfaceNormal, indirectDir ) );

						EvaluatePBRMaterial( albedo, surfaceNormal, wo, indirectDir, metallic, roughness, specularBoost,
							specColor, indirectBrdf, diffuseBrdf, &params, 0.0f );

						float indirectScale = NdotL_indirect / ( indirectProb * indirectPdf );
						if ( indirectScale > 10.0f ) indirectScale = 10.0f;
						float alphaScale = ( material->blendMode == 1 ) ? albedo[3] : 1.0f;
						pathRadiance[0] += pathThroughput[0] * indirectLight[0] * indirectBrdf[0] * indirectScale * alphaScale;
						pathRadiance[1] += pathThroughput[1] * indirectLight[1] * indirectBrdf[1] * indirectScale * alphaScale;
						pathRadiance[2] += pathThroughput[2] * indirectLight[2] * indirectBrdf[2] * indirectScale * alphaScale;
						if ( sampleGuide.depth > 0.0f ) {
							for ( int c = 0; c < 3; c++ ) {
								const float incoming = pathThroughput[c] * indirectLight[c] * indirectScale * alphaScale;
								sampleDiffuse[c] += incoming * diffuseBrdf[c];
								sampleSpecular[c] += incoming * ( indirectBrdf[c] - diffuseBrdf[c] );
							}
						}
					} else {
						float indirectBrdf[3];
						float wo[3] = { -ray.direction[0], -ray.direction[1], -ray.direction[2] };
						EvaluateMaterial( albedo, surfaceNormal, wo, indirectDir, indirectBrdf );

						float indirectScale = ( 1.0f / indirectProb ) / indirectPdf;
						if ( indirectScale > 10.0f ) indirectScale = 10.0f;
						pathRadiance[0] += pathThroughput[0] * indirectLight[0] * indirectBrdf[0] * indirectScale;
						pathRadiance[1] += pathThroughput[1] * indirectLight[1] * indirectBrdf[1] * indirectScale;
						pathRadiance[2] += pathThroughput[2] * indirectLight[2] * indirectBrdf[2] * indirectScale;
					}
				}
			}

			// ---------------------------------------------------------------
			// Low throughput roulette (unbiased replacement for a hard cut)
			// ---------------------------------------------------------------
			if ( throughputClamp > 0.0f ) {
				const float maxThroughput = fmax( pathThroughput[0], fmax( pathThroughput[1], pathThroughput[2] ) );
				if ( maxThroughput < throughputClamp ) {
					const float survival = maxThroughput / throughputClamp;
					if ( PT_Rand( &rng ) >= survival ) break;
					pathThroughput[0] /= survival;
					pathThroughput[1] /= survival;
					pathThroughput[2] /= survival;
				}
			}

			// alpha blend pass-through for semi-transparent surfaces
			if ( material->blendMode == 1 && albedo[3] < 0.95f ) {
				float alpha = albedo[3];
				pathThroughput[0] *= ( 1.0f - alpha );
				pathThroughput[1] *= ( 1.0f - alpha );
				pathThroughput[2] *= ( 1.0f - alpha );
				ray.origin[0] = hitInfo.position[0] + ray.direction[0] * rayOffset;
				ray.origin[1] = hitInfo.position[1] + ray.direction[1] * rayOffset;
				ray.origin[2] = hitInfo.position[2] + ray.direction[2] * rayOffset;
				ray.tMin = rayOffset;
				continue;  // alpha blend pass-through - does NOT increment depth
			}

			// only real opaque/PBR bounces consume depth budget
			depth++;

			// ---------------------------------------------------------------
			// Russian roulette
			// ---------------------------------------------------------------
			if ( rrEnabled && depth >= rrMinBounces ) {
				float maxThroughput = fmax( pathThroughput[0], fmax( pathThroughput[1], pathThroughput[2] ) );
				float survivalProb = fmin( 0.95f, fmax( rrSurvivalMin, maxThroughput ) );
				if ( PT_Rand( &rng ) > survivalProb ) {
					break;
				}
				float invProb = 1.0f / survivalProb;
				pathThroughput[0] *= invProb;
				pathThroughput[1] *= invProb;
				pathThroughput[2] *= invProb;
			}

			// early termination roulette
			if ( earlyTermThreshold > 0.0f ) {
				const float maxThroughput = fmax( pathThroughput[0], fmax( pathThroughput[1], pathThroughput[2] ) );
				if ( maxThroughput < earlyTermThreshold ) {
					const float survival = maxThroughput / earlyTermThreshold;
					if ( PT_Rand( &rng ) >= survival ) break;
					pathThroughput[0] /= survival;
					pathThroughput[1] /= survival;
					pathThroughput[2] /= survival;
				}
			}

			// ---------------------------------------------------------------
			// Sample next bounce direction (cosine-weighted hemisphere)
			// ---------------------------------------------------------------
			float nextDir[3];
			float pdf;
			float wo[3] = { -ray.direction[0], -ray.direction[1], -ray.direction[2] };
#ifdef PT_PHYSICAL
			if ( PT_PhysicalActive( &params ) ) {
				PT_SamplePhysicalBsdf( surfaceNormal, wo, albedo, metallic, roughness, materialIor, &rng, nextDir, &pdf );
			} else
#endif
			SampleHemisphere( surfaceNormal, &rng, nextDir, &pdf );

#ifdef PT_PHYSICAL
			if ( PT_PhysicalActive( &params ) ? pdf <= 0.0f : pdf < 1e-6f ) break;
#else
			if ( pdf < 1e-6f ) break;
#endif

			// update throughput with PBR BRDF
			float brdf[3], diffuseBrdf[3] = { 0.0f, 0.0f, 0.0f };
			float NdotL_bounce = fmax( 0.0f, dot3( surfaceNormal, nextDir ) );

			if ( material->blendMode == 0 || material->blendMode == 1 ) {
				EvaluatePBRMaterial( albedo, surfaceNormal, wo, nextDir, metallic, roughness, specularBoost, specColor,
					brdf, diffuseBrdf, &params, materialIor );
			} else {
				EvaluateMaterial( albedo, surfaceNormal, wo, nextDir, brdf );
			}

			if ( sampleGuide.depth > 0.0f && depth == 1 ) {
				splitIndirect = true;
				for ( int c = 0; c < 3; c++ ) {
					diffuseFraction[c] = brdf[c] > 0.0f ? fmin( 1.0f, diffuseBrdf[c] / brdf[c] ) : 0.0f;
					indirectBaseline[c] = pathRadiance[c];
					volumeBaseline[c] = volumeRadiance[c];
				}
			}
			pathThroughput[0] *= brdf[0] * NdotL_bounce / pdf;
			pathThroughput[1] *= brdf[1] * NdotL_bounce / pdf;
			pathThroughput[2] *= brdf[2] * NdotL_bounce / pdf;

			// NaN/Inf guard on throughput
			if ( !isfinite( pathThroughput[0] ) || !isfinite( pathThroughput[1] ) || !isfinite( pathThroughput[2] ) ) {
				break;
			}

			lastSampledLights = sampledLights;

			// next ray from surface
			ray.origin[0] = hitInfo.position[0] + surfaceNormal[0] * rayOffset;
			ray.origin[1] = hitInfo.position[1] + surfaceNormal[1] * rayOffset;
			ray.origin[2] = hitInfo.position[2] + surfaceNormal[2] * rayOffset;
			ray.direction[0] = nextDir[0];
			ray.direction[1] = nextDir[1];
			ray.direction[2] = nextDir[2];
			ray.tMin = rayOffset;
			ray.tMax = 1e10f;
			ray.worldTMin = shadeWorldTMin;
			ray.flags = 0;

		}  // end bounce loop

		color[0] += pathRadiance[0];
		color[1] += pathRadiance[1];
		color[2] += pathRadiance[2];
		for ( int c = 0; c < 3; c++ ) {
			if ( splitIndirect ) {
				const float bounced = fmax( 0.0f, ( pathRadiance[c] - indirectBaseline[c] )
					- ( volumeRadiance[c] - volumeBaseline[c] ) );
				sampleDiffuse[c] += bounced * diffuseFraction[c];
				sampleSpecular[c] += bounced * ( 1.0f - diffuseFraction[c] );
			}
			diffuseColor[c] += sampleDiffuse[c];
			specularColor[c] += sampleSpecular[c];
			volumeColor[c] += volumeRadiance[c];
		}
		if ( guideBuffer ) {
			if ( sample == 0 ) {
				guide = sampleGuide;
			} else {
				guide.volumeDepth += ( sampleGuide.volumeDepth - guide.volumeDepth ) / ( sample + 1.0f );
				if ( guide.depth > 0.0f ) {
					if ( sampleGuide.depth <= 0.0f
						|| dot3( sampleGuide.normal, guide.normal ) < 0.9f
						|| fabs( sampleGuide.depth - guide.depth ) > fmax( 0.01f, guide.depth * 0.02f ) ) {
						guide.depth = 0.0f;
						guide.historyValid = 0;
						guide.coverage = PT_DENOISE_MIXED;
					} else {
						if ( sampleGuide.materialIndex != guide.materialIndex || guide.coverage == PT_DENOISE_MIXED ) {
							guide.coverage = PT_DENOISE_MIXED;
							guide.historyValid = 0;
						}
						for ( int c = 0; c < 3; c++ ) {
							guide.albedo[c] += sampleGuide.albedo[c];
							guide.specularAlbedo[c] += sampleGuide.specularAlbedo[c];
							guide.shadingNormal[c] += sampleGuide.shadingNormal[c];
						}
						guide.roughness += sampleGuide.roughness;
						guide.historyValid &= sampleGuide.historyValid;
					}
				} else if ( sampleGuide.depth > 0.0f || sampleGuide.coverage != guide.coverage ) guide.coverage = PT_DENOISE_MIXED;
			}
		}

	}  // end sample loop

	// average over samples
	color[0] /= (float)samplesPerPixel;
	color[1] /= (float)samplesPerPixel;
	color[2] /= (float)samplesPerPixel;
	float unclampedColor[3] = { color[0], color[1], color[2] };

	// firefly clamp
	if ( fireflyClamp > 0.0f ) {
		color[0] = fmin( color[0], fireflyClamp );
		color[1] = fmin( color[1], fireflyClamp );
		color[2] = fmin( color[2], fireflyClamp );
	}

	// NaN/Inf guard
	if ( !isfinite( color[0] ) || !isfinite( color[1] ) || !isfinite( color[2] ) ) {
		color[0] = color[1] = color[2] = 0.0f;
	}
	if ( radianceBuffer ) {
		for ( int c = 0; c < 3; c++ ) radianceBuffer[pixelIndex + c] = color[c];
		radianceBuffer[pixelIndex + 3] = 1.0f;
	}
	for ( int c = 0; c < 3; c++ ) {
		const float scale = isfinite( unclampedColor[c] ) && unclampedColor[c] > 0.0f
			? color[c] / ( unclampedColor[c] * samplesPerPixel ) : 0.0f;
		const float diffuse = fmin( color[c], fmax( 0.0f, diffuseColor[c] * scale ) );
		const float specular = fmin( fmax( 0.0f, color[c] - diffuse ), fmax( 0.0f, specularColor[c] * scale ) );
		const float volume = fmin( fmax( 0.0f, color[c] - diffuse - specular ), fmax( 0.0f, volumeColor[c] * scale ) );
		if ( diffuseBuffer ) diffuseBuffer[pixelIndex + c] = diffuse;
		if ( specularBuffer ) specularBuffer[pixelIndex + c] = specular;
		if ( volumeBuffer ) volumeBuffer[pixelIndex + c] = volume;
	}
	if ( diffuseBuffer ) diffuseBuffer[pixelIndex + 3] = 1.0f;
	if ( specularBuffer ) specularBuffer[pixelIndex + 3] = 1.0f;
	if ( volumeBuffer ) volumeBuffer[pixelIndex + 3] = 1.0f;
	if ( guideBuffer ) {
		if ( params.captureTargetFrames <= 0 ) {
			guide.volumeDepth = 0.0f;
			if ( guide.coverage == PT_DENOISE_MIXED ) guide.depth = 0.0f;
		}
		if ( guide.depth > 0.0f ) {
			for ( int c = 0; c < 3; c++ ) {
				guide.albedo[c] /= samplesPerPixel;
				guide.specularAlbedo[c] /= samplesPerPixel;
			}
			guide.roughness /= samplesPerPixel;
			if ( guide.coverage != PT_DENOISE_MIXED ) guide.coverage = guide.historyValid ? PT_DENOISE_STATIC : PT_DENOISE_DYNAMIC;
			if ( dot3( guide.shadingNormal, guide.shadingNormal ) > 1e-6f ) normalize3( guide.shadingNormal );
			else for ( int c = 0; c < 3; c++ ) guide.shadingNormal[c] = guide.normal[c];
		}
		guideBuffer[y * width + x] = guide;
	}

	// Running mean avoids the precision loss of an ever-growing frame sum.
	const float oldWeight = (float)frameIndex;
	const float newWeight = 1.0f / ( oldWeight + 1.0f );
	framebuffer[pixelIndex + 0] += ( color[0] - framebuffer[pixelIndex + 0] ) * newWeight;
	framebuffer[pixelIndex + 1] += ( color[1] - framebuffer[pixelIndex + 1] ) * newWeight;
	framebuffer[pixelIndex + 2] += ( color[2] - framebuffer[pixelIndex + 2] ) * newWeight;
	framebuffer[pixelIndex + 3] = 1.0f;
}

/*
========================
ToneMappingKernel

Converts HDR accumulated buffer to LDR output
========================
*/
__kernel void ToneMappingKernel(
	__global const float *hdrBuffer,
	__global uchar *ldrBuffer,
	int width,
	int height,
	float exposure,
	float gamma,
	int toneMapMode
) {
	int x = get_global_id( 0 );
	int y = get_global_id( 1 );

	if ( x >= width || y >= height ) return;

	int pixelIndex = ( y * width + x ) * 4;

	float r = hdrBuffer[pixelIndex + 0];
	float g = hdrBuffer[pixelIndex + 1];
	float b = hdrBuffer[pixelIndex + 2];

	// apply exposure
	r *= exposure;
	g *= exposure;
	b *= exposure;

	// tone mapping
	if ( toneMapMode == 1 ) {
		// ACES approximation
		float a = 2.51f;
		float bp = 0.03f;
		float c = 2.43f;
		float d = 0.59f;
		float e = 0.14f;
		r = ( r * ( a * r + bp ) ) / ( r * ( c * r + d ) + e );
		g = ( g * ( a * g + bp ) ) / ( g * ( c * g + d ) + e );
		b = ( b * ( a * b + bp ) ) / ( b * ( c * b + d ) + e );
	} else if ( toneMapMode == 2 ) {
		// uncharted 2
		float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
		float W = 11.2f;
		float mapW = ( ( W * ( A * W + C * B ) + D * E ) / ( W * ( A * W + B ) + D * F ) ) - E / F;
		float whiteScale = 1.0f / mapW;
		r = ( ( ( r * ( A * r + C * B ) + D * E ) / ( r * ( A * r + B ) + D * F ) ) - E / F ) * whiteScale;
		g = ( ( ( g * ( A * g + C * B ) + D * E ) / ( g * ( A * g + B ) + D * F ) ) - E / F ) * whiteScale;
		b = ( ( ( b * ( A * b + C * B ) + D * E ) / ( b * ( A * b + B ) + D * F ) ) - E / F ) * whiteScale;
	} else {
		// reinhard (default, mode 0)
		r = r / ( 1.0f + r );
		g = g / ( 1.0f + g );
		b = b / ( 1.0f + b );
	}

	// gamma correction
	float invGamma = 1.0f / gamma;
	r = pow( fmax( r, 0.0f ), invGamma );
	g = pow( fmax( g, 0.0f ), invGamma );
	b = pow( fmax( b, 0.0f ), invGamma );

	// convert to 8-bit LDR
	ldrBuffer[pixelIndex + 0] = (uchar)( PT_Clamp( r, 0.0f, 1.0f ) * 255.0f );
	ldrBuffer[pixelIndex + 1] = (uchar)( PT_Clamp( g, 0.0f, 1.0f ) * 255.0f );
	ldrBuffer[pixelIndex + 2] = (uchar)( PT_Clamp( b, 0.0f, 1.0f ) * 255.0f );
	ldrBuffer[pixelIndex + 3] = 255;
}
