/*
===============================================================================

	OpenCL C port of pt_kernel_triangles.cuh, pt_kernel_triangles_textured.cuh
	and pt_kernel_triangles_raytraced.cuh: the BVH / texture debug views and
	the Whitted ray tracer.

===============================================================================
*/

// Expands ptRenderParams_t into the local names the kernels were written with.
#define PT_UNPACK_RENDER_PARAMS( p ) \
	const int width = ( p ).width; \
	const int height = ( p ).height; \
	const float *cameraPos = ( p ).camPos; \
	const float *cameraForward = ( p ).camForward; \
	const float *cameraRight = ( p ).camRight; \
	const float *cameraUp = ( p ).camUp; \
	const float fov = ( p ).fovX; \
	const float fovY = ( p ).fovY; \
	const int samplesPerPixel = ( p ).samplesPerPixel; \
	const int maxDepth = ( p ).maxDepth; \
	const int maxLightSamples = ( p ).maxLightSamples; \
	const int frameIndex = ( p ).frameIndex; \
	const float emissionBoost = ( p ).emissionBoost; \
	const float indirectProb = ( p ).indirectProb; \
	const int rrEnabled = ( p ).rrEnabled; \
	const int rrMinBounces = ( p ).rrMinBounces; \
	const float rrSurvivalMin = ( p ).rrSurvivalMin; \
	const float earlyTermThreshold = ( p ).earlyTermThreshold; \
	const float fireflyClamp = ( p ).fireflyClamp; \
	const float throughputClamp = ( p ).throughputClamp; \
	const float rayOffset = ( p ).rayOffset; \
	const float specularBoost = ( p ).specularBoost; \
	const float skyIntensity = ( p ).skyIntensity; \
	const float *skyColorZenith = ( p ).skyZenith; \
	const float *skyColorHorizon = ( p ).skyHorizon; \
	const float *skyColorGround = ( p ).skyGround; \
	const float volumetricDensity = ( p ).volDensity; \
	const int volumetricSteps = ( p ).volSteps; \
	const float volumetricAnisotropy = ( p ).volAnisotropy; \
	const float volFalloff = ( p ).volFalloff; \
	const float volMaxDist = ( p ).volMaxDist

// the scene arguments every render kernel takes, CUDA passed the texture table as one pointer
#define PT_SCENE_KERNEL_ARGS \
	__global const ptVertex_t *vertices, \
	__global const ptTriangle_t *triangles, \
	__global const ptBVHNode_t *bvhNodes, \
	__global const int *triIndices, \
	__global const ptMaterial_t *materials, \
	__global const ptClTexture_t *textureDesc, \
	__global const uchar4 *texels, \
	__global const ptLight_t *lights, \
	int numLights

/*
========================
IntersectAABB_Simple
Ray-AABB test for BVH traversal
========================
*/
PT_INLINE bool IntersectAABB_Simple(
	float origX, float origY, float origZ,
	float invDirX, float invDirY, float invDirZ,
	__global const float *bounds,
	float maxT
) {
	float t1 = ( bounds[0] - origX ) * invDirX;
	float t2 = ( bounds[3] - origX ) * invDirX;
	float tmin = fmin( t1, t2 );
	float tmax = fmax( t1, t2 );

	t1 = ( bounds[1] - origY ) * invDirY;
	t2 = ( bounds[4] - origY ) * invDirY;
	tmin = fmax( tmin, fmin( t1, t2 ) );
	tmax = fmin( tmax, fmax( t1, t2 ) );

	t1 = ( bounds[2] - origZ ) * invDirZ;
	t2 = ( bounds[5] - origZ ) * invDirZ;
	tmin = fmax( tmin, fmin( t1, t2 ) );
	tmax = fmin( tmax, fmax( t1, t2 ) );

	return tmax >= fmax( tmin, 0.0f ) && tmin < maxT;
}

/*
========================
IntersectTriangle_Simple
Moller-Trumbore intersection, returns t (or -1 on miss)
========================
*/
PT_INLINE float IntersectTriangle_Simple(
	float origX, float origY, float origZ,
	float dirX, float dirY, float dirZ,
	__global const ptVertex_t *vertices,
	__global const ptTriangle_t *tri
) {
	__global const float *v0 = vertices[tri->vertexIndices[0]].position;
	__global const float *v1 = vertices[tri->vertexIndices[1]].position;
	__global const float *v2 = vertices[tri->vertexIndices[2]].position;

	float e1[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
	float e2[3] = { v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2] };

	float h[3];
	float d[3] = { dirX, dirY, dirZ };
	cross3( d, e2, h );
	float a = dot3( e1, h );

	if ( fabs( a ) < 1e-7f ) return -1.0f;

	float f = 1.0f / a;
	float s[3] = { origX - v0[0], origY - v0[1], origZ - v0[2] };
	float u = f * dot3( s, h );
	if ( u < 0.0f || u > 1.0f ) return -1.0f;

	float q[3];
	cross3( s, e1, q );
	float v = f * dot3( d, q );
	if ( v < 0.0f || u + v > 1.0f ) return -1.0f;

	float t = f * dot3( e2, q );
	return ( t > 1e-4f ) ? t : -1.0f;
}

/*
========================
TriangleDrawKernel
Simple BVH debug visualization - random color per triangle.
========================
*/
__kernel void TriangleDrawKernel(
	PT_SCENE_KERNEL_ARGS,
	__global float *framebuffer,
	const ptRenderParams_t params
) {
	PT_UNPACK_RENDER_PARAMS( params );
	int x = get_global_id( 0 );
	int y = get_global_id( 1 );

	if ( x >= width || y >= height ) {
		return;
	}

	int pixelIndex = ( y * width + x ) * 4;

	// generate primary ray
	float halfTanX = PT_TanHalfFov( fov );
	float halfTanY = PT_TanHalfFov( fovY );

	float u = ( 2.0f * ( (float)x + 0.5f ) / (float)width  - 1.0f ) * halfTanX;
	float v = ( 2.0f * ( (float)y + 0.5f ) / (float)height - 1.0f ) * halfTanY;

	float dirX = cameraForward[0] + u * cameraRight[0] + v * cameraUp[0];
	float dirY = cameraForward[1] + u * cameraRight[1] + v * cameraUp[1];
	float dirZ = cameraForward[2] + u * cameraRight[2] + v * cameraUp[2];

	float invLen = rsqrt( dirX * dirX + dirY * dirY + dirZ * dirZ );
	dirX *= invLen;
	dirY *= invLen;
	dirZ *= invLen;

	float origX = cameraPos[0];
	float origY = cameraPos[1];
	float origZ = cameraPos[2];

	float invDirX = 1.0f / ( fabs( dirX ) > 1e-8f ? dirX : copysign( 1e-8f, dirX ) );
	float invDirY = 1.0f / ( fabs( dirY ) > 1e-8f ? dirY : copysign( 1e-8f, dirY ) );
	float invDirZ = 1.0f / ( fabs( dirZ ) > 1e-8f ? dirZ : copysign( 1e-8f, dirZ ) );

	// BVH traversal - find nearest triangle
	float nearestT = 1e30f;
	int hitTriIdx = -1;

	for ( int nodeIdx = 0; nodeIdx >= 0; ) {
		__global const ptBVHNode_t *node = &bvhNodes[nodeIdx];

		if ( !IntersectAABB_Simple( origX, origY, origZ, invDirX, invDirY, invDirZ,
								  node->bounds, nearestT ) ) {
			nodeIdx = node->escapeIndex;
			continue;
		}

		if ( node->leftChild == -1 ) {
			// leaf node
			for ( int i = 0; i < node->primitiveCount; i++ ) {
				int triIdx = triIndices[node->firstPrimitive + i];
				float t = IntersectTriangle_Simple( origX, origY, origZ,
												   dirX, dirY, dirZ,
												   vertices, &triangles[triIdx] );
				if ( t > 0.0f && t < nearestT ) {
					nearestT = t;
					hitTriIdx = triIdx;
				}
			}
			nodeIdx = node->escapeIndex;
		} else {
			nodeIdx = node->leftChild;
		}
	}

	// color output - random color per triangle
	float r, g, b;

	if ( hitTriIdx >= 0 ) {
		uint hash = cupt_hash( (uint)hitTriIdx * 7919u + 1u );
		r = (float)( ( hash >>  0 ) & 0xFF ) / 255.0f;
		g = (float)( ( hash >>  8 ) & 0xFF ) / 255.0f;
		b = (float)( ( hash >> 16 ) & 0xFF ) / 255.0f;

		r = r * 0.7f + 0.3f;
		g = g * 0.7f + 0.3f;
		b = b * 0.7f + 0.3f;
	} else {
		r = 0.0f;
		g = 0.0f;
		b = 0.0f;
	}

	// write HDR framebuffer (float4 RGBA)
	framebuffer[pixelIndex + 0] = r;
	framebuffer[pixelIndex + 1] = g;
	framebuffer[pixelIndex + 2] = b;
	framebuffer[pixelIndex + 3] = 1.0f;
}

/*
========================
IntersectTriangleUV_Simple
Moller-Trumbore with barycentric output, returns t (or -1 on miss)
========================
*/
PT_INLINE float IntersectTriangleUV_Simple(
	float origX, float origY, float origZ,
	float dirX, float dirY, float dirZ,
	__global const ptVertex_t *vertices,
	__global const ptTriangle_t *tri,
	float *outU, float *outV
) {
	__global const float *v0 = vertices[tri->vertexIndices[0]].position;
	__global const float *v1 = vertices[tri->vertexIndices[1]].position;
	__global const float *v2 = vertices[tri->vertexIndices[2]].position;

	float e1[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
	float e2[3] = { v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2] };

	float d[3] = { dirX, dirY, dirZ };
	float h[3];
	cross3( d, e2, h );
	float a = dot3( e1, h );

	if ( fabs( a ) < 1e-7f ) return -1.0f;

	float f = 1.0f / a;
	float s[3] = { origX - v0[0], origY - v0[1], origZ - v0[2] };
	float u = f * dot3( s, h );
	if ( u < 0.0f || u > 1.0f ) return -1.0f;

	float q[3];
	cross3( s, e1, q );
	float v = f * dot3( d, q );
	if ( v < 0.0f || u + v > 1.0f ) return -1.0f;

	float t = f * dot3( e2, q );
	if ( t > 1e-4f ) {
		*outU = u;
		*outV = v;
		return t;
	}
	return -1.0f;
}

/*
========================
TriangleDrawTexturedKernel
========================
*/
__kernel void TriangleDrawTexturedKernel(
	PT_SCENE_KERNEL_ARGS,
	__global float *framebuffer,
	int texSubMode,
	const ptRenderParams_t params
) {
	PT_UNPACK_RENDER_PARAMS( params );
	const ptClTextures_t textureTable = { textureDesc, texels };
	const ptClTextures_t *textures = textureDesc ? &textureTable : NULL;	// CUDA passes NULL tables through
	int x = get_global_id( 0 );
	int y = get_global_id( 1 );

	if ( x >= width || y >= height ) {
		return;
	}

	int pixelIndex = ( y * width + x ) * 4;

	const int subMode = texSubMode;

	// generate primary ray
	float halfTanX = PT_TanHalfFov( fov );
	float halfTanY = PT_TanHalfFov( fovY );

	float u = ( 2.0f * ( (float)x + 0.5f ) / (float)width  - 1.0f ) * halfTanX;
	float v = ( 2.0f * ( (float)y + 0.5f ) / (float)height - 1.0f ) * halfTanY;

	float dirX = cameraForward[0] + u * cameraRight[0] + v * cameraUp[0];
	float dirY = cameraForward[1] + u * cameraRight[1] + v * cameraUp[1];
	float dirZ = cameraForward[2] + u * cameraRight[2] + v * cameraUp[2];

	float invLen = rsqrt( dirX * dirX + dirY * dirY + dirZ * dirZ );
	dirX *= invLen;
	dirY *= invLen;
	dirZ *= invLen;

	float origX = cameraPos[0];
	float origY = cameraPos[1];
	float origZ = cameraPos[2];

	float invDirX = 1.0f / ( fabs( dirX ) > 1e-8f ? dirX : copysign( 1e-8f, dirX ) );
	float invDirY = 1.0f / ( fabs( dirY ) > 1e-8f ? dirY : copysign( 1e-8f, dirY ) );
	float invDirZ = 1.0f / ( fabs( dirZ ) > 1e-8f ? dirZ : copysign( 1e-8f, dirZ ) );

	// BVH traversal - find nearest triangle with barycentrics
	float nearestT = 1e30f;
	int hitTriIdx = -1;
	float hitU = 0.0f;
	float hitV = 0.0f;

	for ( int nodeIdx = 0; nodeIdx >= 0; ) {
		__global const ptBVHNode_t *node = &bvhNodes[nodeIdx];

		if ( !IntersectAABB_Simple( origX, origY, origZ, invDirX, invDirY, invDirZ,
								  node->bounds, nearestT ) ) {
			nodeIdx = node->escapeIndex;
			continue;
		}

		if ( node->leftChild == -1 ) {
			// Leaf node
			for ( int i = 0; i < node->primitiveCount; i++ ) {
				int triIdx = triIndices[node->firstPrimitive + i];
				float bu, bv;
				float t = IntersectTriangleUV_Simple( origX, origY, origZ,
													  dirX, dirY, dirZ,
													  vertices, &triangles[triIdx],
													  &bu, &bv );
				if ( t > 0.0f && t < nearestT ) {
					nearestT = t;
					hitTriIdx = triIdx;
					hitU = bu;
					hitV = bv;
				}
			}
			nodeIdx = node->escapeIndex;
		} else {
			nodeIdx = node->leftChild;
		}
	}

	// shade hit point
	float r, g, b;

	if ( hitTriIdx >= 0 ) {
		__global const ptTriangle_t *tri = &triangles[hitTriIdx];
		int i0 = tri->vertexIndices[0];
		int i1 = tri->vertexIndices[1];
		int i2 = tri->vertexIndices[2];

		float w0 = 1.0f - hitU - hitV;
		float w1 = hitU;
		float w2 = hitV;

		// interpolate texcoords
		float tu = w0 * vertices[i0].texcoord[0] + w1 * vertices[i1].texcoord[0] + w2 * vertices[i2].texcoord[0];
		float tv = w0 * vertices[i0].texcoord[1] + w1 * vertices[i1].texcoord[1] + w2 * vertices[i2].texcoord[1];

		__global const ptMaterial_t *mat = &materials[tri->materialIndex];

		if ( subMode == 1 ) {
			// base color only
			r = mat->albedo[0];
			g = mat->albedo[1];
			b = mat->albedo[2];

		} else if ( subMode == 2 ) {
			// albedo texture only
			if ( mat->albedoTexture >= 0 ) {
				float color[4];
				SampleTexture( textures, mat->albedoTexture, tu, tv, color );
				r = color[0];
				g = color[1];
				b = color[2];
			} else {
				r = 1.0f; g = 0.0f; b = 1.0f; // magenta = no texture
			}

		} else if ( subMode == 3 ) {
			// normal texture
			if ( mat->normalTexture >= 0 ) {
				float color[4];
				SampleTexture( textures, mat->normalTexture, tu, tv, color );
				// normal maps are stored as tangent-space XYZ in RGB
				r = color[0];
				g = color[1];
				b = color[2];
			} else {
				// no normal map - show interpolated geometric normal as color
				float nx = w0 * vertices[i0].normal[0] + w1 * vertices[i1].normal[0] + w2 * vertices[i2].normal[0];
				float ny = w0 * vertices[i0].normal[1] + w1 * vertices[i1].normal[1] + w2 * vertices[i2].normal[1];
				float nz = w0 * vertices[i0].normal[2] + w1 * vertices[i1].normal[2] + w2 * vertices[i2].normal[2];
				float nLen = rsqrt( nx * nx + ny * ny + nz * nz + 1e-12f );
				r = nx * nLen * 0.5f + 0.5f;
				g = ny * nLen * 0.5f + 0.5f;
				b = nz * nLen * 0.5f + 0.5f;
			}

		} else if ( subMode == 4 ) {
			// specular texture
			if ( mat->specularTexture >= 0 ) {
				float color[4];
				SampleTexture( textures, mat->specularTexture, tu, tv, color );
				r = color[0];
				g = color[1];
				b = color[2];
			} else {
				// no specular map - show roughness as grayscale
				float rough = mat->roughness;
				r = rough;
				g = rough;
				b = rough;
			}

		} else {
			// subMode 0 (default): Combined (albedo * base color, Lambertian lit)
			float albedoR = mat->albedo[0];
			float albedoG = mat->albedo[1];
			float albedoB = mat->albedo[2];

			if ( mat->albedoTexture >= 0 ) {
				float color[4];
				SampleTexture( textures, mat->albedoTexture, tu, tv, color );
				albedoR *= color[0];
				albedoG *= color[1];
				albedoB *= color[2];
			}

			// interpolate normal for Lambertian shading
			float nx = w0 * vertices[i0].normal[0] + w1 * vertices[i1].normal[0] + w2 * vertices[i2].normal[0];
			float ny = w0 * vertices[i0].normal[1] + w1 * vertices[i1].normal[1] + w2 * vertices[i2].normal[1];
			float nz = w0 * vertices[i0].normal[2] + w1 * vertices[i1].normal[2] + w2 * vertices[i2].normal[2];
			float nInvLen = rsqrt( nx * nx + ny * ny + nz * nz + 1e-12f );
			nx *= nInvLen;
			ny *= nInvLen;
			nz *= nInvLen;

			// fixed directional light for simple shading
			float lightDirX = 0.577f, lightDirY = 0.577f, lightDirZ = 0.577f;
			float NdotL = fmax( 0.0f, nx * lightDirX + ny * lightDirY + nz * lightDirZ );

			float lighting = NdotL;

			albedoR *= lighting;
			albedoG *= lighting;
			albedoB *= lighting;

			r = fmin( albedoR, 1.0f );
			g = fmin( albedoG, 1.0f );
			b = fmin( albedoB, 1.0f );
		}
	} else {
		// Sky background scaled by the configured environment intensity.
		r = skyColorHorizon[0] * skyIntensity;
		g = skyColorHorizon[1] * skyIntensity;
		b = skyColorHorizon[2] * skyIntensity;
	}

	// write HDR framebuffer (float4 RGBA)
	framebuffer[pixelIndex + 0] = r;
	framebuffer[pixelIndex + 1] = g;
	framebuffer[pixelIndex + 2] = b;
	framebuffer[pixelIndex + 3] = 1.0f;
}

/*
========================
TraceShadowRay_RT
BVH shadow ray test
========================
*/
PT_INLINE bool TraceShadowRay_RT(
	float oX, float oY, float oZ,
	float dX, float dY, float dZ,
	float maxDist,
	__global const ptVertex_t *vertices,
	__global const ptTriangle_t *triangles,
	__global const int *triIndices,
	__global const ptBVHNode_t *bvhNodes
) {
	float idX = 1.0f / ( fabs( dX ) > 1e-8f ? dX : copysign( 1e-8f, dX ) );
	float idY = 1.0f / ( fabs( dY ) > 1e-8f ? dY : copysign( 1e-8f, dY ) );
	float idZ = 1.0f / ( fabs( dZ ) > 1e-8f ? dZ : copysign( 1e-8f, dZ ) );

	for ( int ni = 0; ni >= 0; ) {
		__global const ptBVHNode_t *nd = &bvhNodes[ni];

		if ( !IntersectAABB_Simple( oX, oY, oZ, idX, idY, idZ, nd->bounds, maxDist ) ) {
			ni = nd->escapeIndex;
			continue;
		}

		if ( nd->leftChild == -1 ) {
			// Leaf
			for ( int i = 0; i < nd->primitiveCount; i++ ) {
				int ti = triIndices[nd->firstPrimitive + i];
				float t = IntersectTriangle_Simple( oX, oY, oZ, dX, dY, dZ,
												   vertices, &triangles[ti] );
				if ( t > 0.001f && t < maxDist ) {
					return true;
				}
			}
			ni = nd->escapeIndex;
		} else {
			ni = nd->leftChild;
		}
	}
	return false;
}

/*
========================
TraceRay_RT
BVH closest-hit traversal with barycentric output
========================
*/
PT_INLINE float TraceRay_RT(
	float oX, float oY, float oZ,
	float dX, float dY, float dZ,
	__global const ptVertex_t *vertices,
	__global const ptTriangle_t *triangles,
	__global const int *triIndices,
	__global const ptBVHNode_t *bvhNodes,
	int *outTriIdx,
	float *outU,
	float *outV
) {
	float idX = 1.0f / ( fabs( dX ) > 1e-8f ? dX : copysign( 1e-8f, dX ) );
	float idY = 1.0f / ( fabs( dY ) > 1e-8f ? dY : copysign( 1e-8f, dY ) );
	float idZ = 1.0f / ( fabs( dZ ) > 1e-8f ? dZ : copysign( 1e-8f, dZ ) );

	float nearest = 1e30f;
	*outTriIdx = -1;

	for ( int ni = 0; ni >= 0; ) {
		__global const ptBVHNode_t *nd = &bvhNodes[ni];

		if ( !IntersectAABB_Simple( oX, oY, oZ, idX, idY, idZ, nd->bounds, nearest ) ) {
			ni = nd->escapeIndex;
			continue;
		}

		if ( nd->leftChild == -1 ) {
			// leaf
			for ( int i = 0; i < nd->primitiveCount; i++ ) {
				int ti = triIndices[nd->firstPrimitive + i];
				float bu, bv;
				float t = IntersectTriangleUV_Simple( oX, oY, oZ, dX, dY, dZ,
													 vertices, &triangles[ti], &bu, &bv );
				if ( t > 0.0f && t < nearest ) {
					nearest = t;
					*outTriIdx = ti;
					*outU = bu;
					*outV = bv;
				}
			}
			ni = nd->escapeIndex;
		} else {
			ni = nd->leftChild;
		}
	}
	return nearest;
}

/*
========================
TriangleDrawRayTracedKernel
========================
*/
__kernel void TriangleDrawRayTracedKernel(
	PT_SCENE_KERNEL_ARGS,
	__global float *framebuffer,
	const ptRenderParams_t params
) {
	PT_UNPACK_RENDER_PARAMS( params );
	const ptClTextures_t textureTable = { textureDesc, texels };
	const ptClTextures_t *textures = textureDesc ? &textureTable : NULL;	// CUDA passes NULL tables through
	int x = get_global_id( 0 );
	int y = get_global_id( 1 );

	if ( x >= width || y >= height ) {
		return;
	}

	int pixelIndex = ( y * width + x ) * 4;

	// generate primary ray
	float halfTanX = PT_TanHalfFov( fov );
	float halfTanY = PT_TanHalfFov( fovY );

	float su = ( 2.0f * ( (float)x + 0.5f ) / (float)width  - 1.0f ) * halfTanX;
	float sv = ( 2.0f * ( (float)y + 0.5f ) / (float)height - 1.0f ) * halfTanY;

	float rayDirX = cameraForward[0] + su * cameraRight[0] + sv * cameraUp[0];
	float rayDirY = cameraForward[1] + su * cameraRight[1] + sv * cameraUp[1];
	float rayDirZ = cameraForward[2] + su * cameraRight[2] + sv * cameraUp[2];

	float il = rsqrt( rayDirX * rayDirX + rayDirY * rayDirY + rayDirZ * rayDirZ );
	rayDirX *= il;
	rayDirY *= il;
	rayDirZ *= il;

	float rayOrigX = cameraPos[0];
	float rayOrigY = cameraPos[1];
	float rayOrigZ = cameraPos[2];

	// whitted-style iterative bounce loop
	float colorR = 0.0f, colorG = 0.0f, colorB = 0.0f;
	float throughR = 1.0f, throughG = 1.0f, throughB = 1.0f;

	const int MAX_BOUNCES = 4;
	const float SPECULAR_POWER = 64.0f;
	const float REFLECTIVITY = 0.15f;
	int alphaSkips = 0;
	const int MAX_ALPHA_SKIPS = 8;
	float biasOffset = fmax( rayOffset, 0.01f );

	for ( int bounce = 0; bounce < MAX_BOUNCES; bounce++ ) {

		int hitIdx;
		float hitU, hitV;
		float hitT = TraceRay_RT( rayOrigX, rayOrigY, rayOrigZ,
								 rayDirX, rayDirY, rayDirZ,
								 vertices, triangles, triIndices, bvhNodes,
								 &hitIdx, &hitU, &hitV );

		if ( hitIdx < 0 ) {
			// sky / miss - use sky color parameters
			float upDot = rayDirY; // approximate up = Y
			float skyR, skyG, skyB;
			if ( upDot > 0.0f ) {
				float t = fmin( upDot, 1.0f );
				skyR = skyColorHorizon[0] * ( 1.0f - t ) + skyColorZenith[0] * t;
				skyG = skyColorHorizon[1] * ( 1.0f - t ) + skyColorZenith[1] * t;
				skyB = skyColorHorizon[2] * ( 1.0f - t ) + skyColorZenith[2] * t;
			} else {
				float t = fmin( -upDot, 1.0f );
				skyR = skyColorHorizon[0] * ( 1.0f - t ) + skyColorGround[0] * t;
				skyG = skyColorHorizon[1] * ( 1.0f - t ) + skyColorGround[1] * t;
				skyB = skyColorHorizon[2] * ( 1.0f - t ) + skyColorGround[2] * t;
			}
			colorR += throughR * skyR * skyIntensity;
			colorG += throughG * skyG * skyIntensity;
			colorB += throughB * skyB * skyIntensity;
			break;
		}

		__global const ptTriangle_t *tri = &triangles[hitIdx];
		int i0 = tri->vertexIndices[0];
		int i1 = tri->vertexIndices[1];
		int i2 = tri->vertexIndices[2];

		float w0 = 1.0f - hitU - hitV;
		float w1 = hitU;
		float w2 = hitV;

		// interpolate surface normal
		float nx = w0 * vertices[i0].normal[0] + w1 * vertices[i1].normal[0] + w2 * vertices[i2].normal[0];
		float ny = w0 * vertices[i0].normal[1] + w1 * vertices[i1].normal[1] + w2 * vertices[i2].normal[1];
		float nz = w0 * vertices[i0].normal[2] + w1 * vertices[i1].normal[2] + w2 * vertices[i2].normal[2];
		float nIL = rsqrt( nx * nx + ny * ny + nz * nz + 1e-12f );
		nx *= nIL;
		ny *= nIL;
		nz *= nIL;

		// interpolate texcoord
		float tu = w0 * vertices[i0].texcoord[0] + w1 * vertices[i1].texcoord[0] + w2 * vertices[i2].texcoord[0];
		float tv = w0 * vertices[i0].texcoord[1] + w1 * vertices[i1].texcoord[1] + w2 * vertices[i2].texcoord[1];
		const float rawTu = tu;
		const float rawTv = tv;

		// hit position
		float pX = rayOrigX + rayDirX * hitT;
		float pY = rayOrigY + rayDirY * hitT;
		float pZ = rayOrigZ + rayDirZ * hitT;

		// material lookup
		__global const ptMaterial_t *mat = &materials[tri->materialIndex];
		float albR = mat->albedo[0];
		float albG = mat->albedo[1];
		float albB = mat->albedo[2];
		float alpha = 1.0f;

		const bool maskedAdditive = mat->additiveUsesAlpha == PT_ADDITIVE_LOCAL_MASK;
		if ( mat->blendMode == PT_BLEND_MODULATE_INVERSE || maskedAdditive ) PT_ApplyTexTransform( mat, &tu, &tv );
		if ( mat->albedoTexture >= 0 ) {
			float texColor[4];
			if ( mat->blendMode == PT_BLEND_MODULATE_INVERSE ) {
				SampleTextureSrgb( textures, mat->albedoTexture, tu, tv, texColor );
			} else {
				SampleTexture( textures, mat->albedoTexture, tu, tv, texColor );
			}
			albR *= texColor[0];
			albG *= texColor[1];
			albB *= texColor[2];
			alpha = texColor[3];
		}
		if ( maskedAdditive ) {
			float color[4] = { albR, albG, albB, alpha };
			PT_ApplyVertexColor( mat, vertices, tri, w0, w1, w2, color );
			albR = color[0];
			albG = color[1];
			albB = color[2];
			alpha = PT_AdditiveMaskAlpha( mat, textures, vertices, tri, w0, w1, w2, rawTu, rawTv );
		}
		if ( mat->blendMode == PT_BLEND_MODULATE_INVERSE ) {
			float color[4] = { albR, albG, albB, alpha };
			PT_ApplyVertexColor( mat, vertices, tri, w0, w1, w2, color );
			throughR *= PT_ModulationFactor( color[0], mat->blendMode );
			throughG *= PT_ModulationFactor( color[1], mat->blendMode );
			throughB *= PT_ModulationFactor( color[2], mat->blendMode );
			rayOrigX = pX + rayDirX * biasOffset;
			rayOrigY = pY + rayDirY * biasOffset;
			rayOrigZ = pZ + rayDirZ * biasOffset;
			bounce--;
			alphaSkips++;
			if ( alphaSkips >= MAX_ALPHA_SKIPS ) break;
			continue;
		}

		// alpha test: perforated surface fails - skip through
		if ( mat->alphaTest > 0.0f && alpha < mat->alphaTest && alphaSkips < MAX_ALPHA_SKIPS ) {
			rayOrigX = pX + rayDirX * biasOffset;
			rayOrigY = pY + rayDirY * biasOffset;
			rayOrigZ = pZ + rayDirZ * biasOffset;
			bounce--;
			alphaSkips++;
			continue;
		}

		// nearly invisible alpha blend - skip through
		if ( mat->blendMode == 1 && alpha < 0.05f && alphaSkips < MAX_ALPHA_SKIPS ) {
			rayOrigX = pX + rayDirX * biasOffset;
			rayOrigY = pY + rayDirY * biasOffset;
			rayOrigZ = pZ + rayDirZ * biasOffset;
			bounce--;
			alphaSkips++;
			continue;
		}

		// lit noshadows decal overlays - blend over and continue
		if ( PT_IsLitDecal( mat ) && alphaSkips < MAX_ALPHA_SKIPS ) {
			float lum = albR * 0.2126f + albG * 0.7152f + albB * 0.0722f;
			float decalAlpha = ( lum < 0.02f ) ? 0.0f : alpha;

			if ( decalAlpha > 0.05f ) {
				colorR += throughR * albR * decalAlpha * 0.5f;
				colorG += throughG * albG * decalAlpha * 0.5f;
				colorB += throughB * albB * decalAlpha * 0.5f;
				throughR *= ( 1.0f - decalAlpha );
				throughG *= ( 1.0f - decalAlpha );
				throughB *= ( 1.0f - decalAlpha );
			}

			rayOrigX = pX + rayDirX * biasOffset;
			rayOrigY = pY + rayDirY * biasOffset;
			rayOrigZ = pZ + rayDirZ * biasOffset;
			bounce--;
			alphaSkips++;
			continue;
		}

		// ambient-only materials (self-illuminated overlays, particles, HUD)
		if ( mat->isAmbientOnly && alphaSkips < MAX_ALPHA_SKIPS ) {
			if ( mat->blendMode == 2 ) {
				// additive
				const float weight = PT_AdditiveOpacity( mat, alpha ) * emissionBoost;
				colorR += throughR * albR * weight;
				colorG += throughG * albG * weight;
				colorB += throughB * albB * weight;
			} else if ( mat->blendMode == 3 ) {
				// modulate
				throughR *= albR;
				throughG *= albG;
				throughB *= albB;
			} else {
				// default alpha-over
				colorR += throughR * albR * alpha;
				colorG += throughG * albG * alpha;
				colorB += throughB * albB * alpha;
				throughR *= ( 1.0f - alpha );
				throughG *= ( 1.0f - alpha );
				throughB *= ( 1.0f - alpha );
			}

			rayOrigX = pX + rayDirX * biasOffset;
			rayOrigY = pY + rayDirY * biasOffset;
			rayOrigZ = pZ + rayDirZ * biasOffset;
			bounce--;
			alphaSkips++;
			continue;
		}

		// additive blending (non-ambient)
		if ( mat->blendMode == 2 && alphaSkips < MAX_ALPHA_SKIPS ) {
			const float weight = PT_AdditiveOpacity( mat, alpha );
			colorR += throughR * albR * weight;
			colorG += throughG * albG * weight;
			colorB += throughB * albB * weight;

			rayOrigX = pX + rayDirX * biasOffset;
			rayOrigY = pY + rayDirY * biasOffset;
			rayOrigZ = pZ + rayDirZ * biasOffset;
			bounce--;
			alphaSkips++;
			continue;
		}

		// modulate blending (non-ambient)
		if ( mat->blendMode == 3 && alphaSkips < MAX_ALPHA_SKIPS ) {
			throughR *= albR;
			throughG *= albG;
			throughB *= albB;

			rayOrigX = pX + rayDirX * biasOffset;
			rayOrigY = pY + rayDirY * biasOffset;
			rayOrigZ = pZ + rayDirZ * biasOffset;
			bounce--;
			alphaSkips++;
			continue;
		}

		// alpha blend (non-ambient) - partial transparency
		if ( mat->blendMode == 1 && alpha < 0.95f && alphaSkips < MAX_ALPHA_SKIPS ) {
			colorR += throughR * albR * alpha;
			colorG += throughG * albG * alpha;
			colorB += throughB * albB * alpha;
			throughR *= ( 1.0f - alpha );
			throughG *= ( 1.0f - alpha );
			throughB *= ( 1.0f - alpha );

			rayOrigX = pX + rayDirX * biasOffset;
			rayOrigY = pY + rayDirY * biasOffset;
			rayOrigZ = pZ + rayDirZ * biasOffset;
			bounce--;
			alphaSkips++;
			continue;
		}

		// emission contribution
		float emission[3];
		PT_SampleEmission( mat, textures, vertices, tri, w0, w1, w2, rawTu, rawTv, emission );
		float emitR = emission[0] * emissionBoost;
		float emitG = emission[1] * emissionBoost;
		float emitB = emission[2] * emissionBoost;
		float emitLum = emitR * 0.2126f + emitG * 0.7152f + emitB * 0.0722f;
		if ( emitLum > 0.001f ) {
			colorR += throughR * emitR;
			colorG += throughG * emitG;
			colorB += throughB * emitB;
			if ( emitLum > 0.5f ) break;
		}

		// view direction (toward camera)
		float vX = -rayDirX;
		float vY = -rayDirY;
		float vZ = -rayDirZ;

		// direct lighting from all lights
		float diffR = 0.0f, diffG = 0.0f, diffB = 0.0f;
		float specR = 0.0f, specG = 0.0f, specB = 0.0f;

		for ( int li = 0; li < numLights; li++ ) {
			__global const ptLight_t *light = &lights[li];
			if ( light->type == PT_LIGHT_AREA ) {
				continue;  // area-light sampling is implemented in path-tracing mode
			}

			float lX, lY, lZ;
			float shadowDist;
			float atten = 0.0f;
			float lightColR = light->color[0];
			float lightColG = light->color[1];
			float lightColB = light->color[2];

			if ( light->type == 0 ) {
				// point light
				float toL_x = light->position[0] - pX;
				float toL_y = light->position[1] - pY;
				float toL_z = light->position[2] - pZ;

				float dSq = toL_x * toL_x + toL_y * toL_y + toL_z * toL_z;
				float d = sqrt( dSq + 1e-12f );
				float iD = 1.0f / d;
				lX = toL_x * iD;
				lY = toL_y * iD;
				lZ = toL_z * iD;
				shadowDist = d;

				float R = light->radius;
				if ( d >= R ) continue;

				float ratio = d / R;
				float falloff = 1.0f - ratio * ratio;
				falloff = falloff * falloff;
				atten = light->intensity * falloff;

			} else if ( light->type == 1 ) {
				// directional light
				lX = light->direction[0];
				lY = light->direction[1];
				lZ = light->direction[2];

				float dIL = rsqrt( lX * lX + lY * lY + lZ * lZ + 1e-12f );
				lX *= dIL;
				lY *= dIL;
				lZ *= dIL;

				shadowDist = 10000.0f;
				atten = light->intensity;

			} else {
				// projected / spot light (type == 2)
				float toL_x = light->position[0] - pX;
				float toL_y = light->position[1] - pY;
				float toL_z = light->position[2] - pZ;

				float dSq = toL_x * toL_x + toL_y * toL_y + toL_z * toL_z;
				float d = sqrt( dSq + 1e-12f );
				float iD = 1.0f / d;
				lX = toL_x * iD;
				lY = toL_y * iD;
				lZ = toL_z * iD;
				shadowDist = d;

				float R = light->radius;
				if ( d >= R ) continue;

				float ratio = d / R;
				float falloff = 1.0f - ratio * ratio;
				falloff = falloff * falloff;
				float distAtten = falloff;

				// cone attenuation
				float negLX = -lX, negLY = -lY, negLZ = -lZ;
				float spotCos = negLX * light->direction[0] + negLY * light->direction[1] + negLZ * light->direction[2];
				float cosHalfAngle = cos( light->coneAngle );

				float coneAtten = 0.0f;
				if ( spotCos > cosHalfAngle ) {
					float t = ( spotCos - cosHalfAngle ) / ( 1.0f - cosHalfAngle + 1e-6f );
					coneAtten = pow( t, light->coneFalloff );
				} else {
					continue;
				}

				atten = light->intensity * distAtten * coneAtten;

				// projected texture modulation
				if ( light->projectedTextureIndex >= 0 ) {
					float localX = toL_x * light->right[0] + toL_y * light->right[1] + toL_z * light->right[2];
					float localY = toL_x * light->up[0]    + toL_y * light->up[1]    + toL_z * light->up[2];
					float localZ = toL_x * light->direction[0] + toL_y * light->direction[1] + toL_z * light->direction[2];

					if ( localZ > 0.001f ) {
						float projU = fmax( 0.0f, fmin( 1.0f, ( localX / localZ ) * 0.5f + 0.5f ) );
						float projV = fmax( 0.0f, fmin( 1.0f, ( localY / localZ ) * 0.5f + 0.5f ) );

						float projColor[4];
						SampleTexture( textures, light->projectedTextureIndex, projU, projV, projColor );
						lightColR *= projColor[0];
						lightColG *= projColor[1];
						lightColB *= projColor[2];
					} else {
						continue;
					}
				}
			}

			if ( atten <= 0.0f ) continue;

			// shadow ray
			float sOx = pX + nx * biasOffset;
			float sOy = pY + ny * biasOffset;
			float sOz = pZ + nz * biasOffset;

			if ( TraceShadowRay_RT( sOx, sOy, sOz, lX, lY, lZ, shadowDist,
								  vertices, triangles, triIndices, bvhNodes ) ) {
				continue;
			}

			// Lambertian diffuse
			float NdotL = fmax( nx * lX + ny * lY + nz * lZ, 0.0f );

			diffR += lightColR * NdotL * atten;
			diffG += lightColG * NdotL * atten;
			diffB += lightColB * NdotL * atten;

			// Blinn-Phong specular
			float hX = lX + vX;
			float hY = lY + vY;
			float hZ = lZ + vZ;
			float hIL = rsqrt( hX * hX + hY * hY + hZ * hZ + 1e-12f );
			hX *= hIL;
			hY *= hIL;
			hZ *= hIL;

			float NdotH = fmax( nx * hX + ny * hY + nz * hZ, 0.0f );
			float spec = pow( NdotH, SPECULAR_POWER );

			specR += lightColR * spec * atten;
			specG += lightColG * spec * atten;
			specB += lightColB * spec * atten;
		}

		// combine: diffuse * albedo + specular (no ambient - Doom 3 pure darkness)
		float localR = albR * diffR + REFLECTIVITY * specR * specularBoost;
		float localG = albG * diffG + REFLECTIVITY * specG * specularBoost;
		float localB = albB * diffB + REFLECTIVITY * specB * specularBoost;

		// add local contribution weighted by throughput
		float oneMinusRefl = 1.0f - REFLECTIVITY;
		colorR += throughR * localR * oneMinusRefl;
		colorG += throughG * localG * oneMinusRefl;
		colorB += throughB * localB * oneMinusRefl;

		// Reflection ray for next bounce
		float RdotN = rayDirX * nx + rayDirY * ny + rayDirZ * nz;
		rayDirX = rayDirX - 2.0f * RdotN * nx;
		rayDirY = rayDirY - 2.0f * RdotN * ny;
		rayDirZ = rayDirZ - 2.0f * RdotN * nz;

		rayOrigX = pX + nx * biasOffset;
		rayOrigY = pY + ny * biasOffset;
		rayOrigZ = pZ + nz * biasOffset;

		// attenuate throughput by reflectivity
		throughR *= REFLECTIVITY * albR;
		throughG *= REFLECTIVITY * albG;
		throughB *= REFLECTIVITY * albB;

		// early out if throughput is negligible
		if ( throughR + throughG + throughB < earlyTermThreshold ) {
			break;
		}
	}

	// write HDR framebuffer (tone mapping kernel handles LDR conversion)
	framebuffer[pixelIndex + 0] = colorR;
	framebuffer[pixelIndex + 1] = colorG;
	framebuffer[pixelIndex + 2] = colorB;
	framebuffer[pixelIndex + 3] = 1.0f;
}
