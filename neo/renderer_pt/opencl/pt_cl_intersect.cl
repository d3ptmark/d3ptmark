/*
===============================================================================

	OpenCL C port of the intersection half of pt_kernel_pathtracing.cuh
	(everything above its #include of the sampling code).

===============================================================================
*/

/*
=================================================================================
IntersectTriangleUV

Moller-Trumbore, returns the ray distance and barycentrics only. The hit
attributes are interpolated once for the closest hit by PT_FillHitAttributes.
=================================================================================
*/
PT_INLINE bool IntersectTriangleUV(
	const float* orig,
	const float* dir,
	__global const ptVertex_t* vertices,
	__global const ptTriangle_t* tri,
	float* t,
	float* u,
	float* v
) {
	__global const float* p0 = vertices[tri->vertexIndices[0]].position;
	__global const float* p1 = vertices[tri->vertexIndices[1]].position;
	__global const float* p2 = vertices[tri->vertexIndices[2]].position;

	const float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
	const float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };

	float h[3];
	cross3(dir, e2, h);
	const float a = dot3(e1, h);
	if (fabs(a) < 1e-8f) return false;

	const float f = 1.0f / a;
	const float s[3] = { orig[0] - p0[0], orig[1] - p0[1], orig[2] - p0[2] };
	*u = f * dot3(s, h);
	if (*u < 0.0f || *u > 1.0f) return false;

	float q[3];
	cross3(s, e1, q);
	*v = f * dot3(dir, q);
	if (*v < 0.0f || *u + *v > 1.0f) return false;

	*t = f * dot3(e2, q);
	return true;
}

/*
=================================================================================
PT_FillHitAttributes
=================================================================================
*/
PT_INLINE void PT_FillHitAttributes(
	const ptRay_t* ray,
	__global const ptVertex_t* vertices,
	__global const ptTriangle_t* triangles,
	int triIdx,
	float t,
	float u,
	float v,
	ptHitInfo_t* hitInfo
) {
	__global const ptTriangle_t* tri = &triangles[triIdx];
	__global const ptVertex_t* v0 = &vertices[tri->vertexIndices[0]];
	__global const ptVertex_t* v1 = &vertices[tri->vertexIndices[1]];
	__global const ptVertex_t* v2 = &vertices[tri->vertexIndices[2]];
	const float w = 1.0f - u - v;

	hitInfo->hit = true;
	hitInfo->t = t;
	for (int i = 0; i < 3; i++) {
		hitInfo->position[i] = ray->origin[i] + t * ray->direction[i];
		hitInfo->normal[i] = w * v0->normal[i] + u * v1->normal[i] + v * v2->normal[i];
		hitInfo->tangent[i] = w * v0->tangent[i] + u * v1->tangent[i] + v * v2->tangent[i];
		hitInfo->binormal[i] = w * v0->binormal[i] + u * v1->binormal[i] + v * v2->binormal[i];
	}
	normalize3(hitInfo->normal);
	normalize3(hitInfo->tangent);
	normalize3(hitInfo->binormal);

	hitInfo->texcoord[0] = w * v0->texcoord[0] + u * v1->texcoord[0] + v * v2->texcoord[0];
	hitInfo->texcoord[1] = w * v0->texcoord[1] + u * v1->texcoord[1] + v * v2->texcoord[1];

	hitInfo->barycentrics[0] = w;
	hitInfo->barycentrics[1] = u;
	hitInfo->barycentrics[2] = v;

	hitInfo->materialIndex = tri->materialIndex;
	hitInfo->triangleIndex = triIdx;
	hitInfo->triFlags = tri->flags;
}

/*
=================================================================================
Slab test helpers
=================================================================================
*/
PT_INLINE void PT_InvDir(const float* dir, float* invD) {
	for (int i = 0; i < 3; i++) {
		// a huge finite value keeps (bound - origin) * invD free of 0 * inf NaNs
		invD[i] = fabs(dir[i]) > 1e-12f ? 1.0f / dir[i] : copysign(1e30f, dir[i]);
	}
}

PT_INLINE bool PT_RayBox(const float* orig, const float* invD, __global const float* b, float tmin, float tmax) {
	for (int i = 0; i < 3; i++) {
		const float t0 = (b[i] - orig[i]) * invD[i];
		const float t1 = (b[i + 3] - orig[i]) * invD[i];
		tmin = fmax(tmin, fmin(t0, t1));
		tmax = fmin(tmax, fmax(t0, t1));
	}
	return tmin <= tmax;
}

/*
=================================================================================
IntersectBVH - closest hit

View weapon handling (the GL renderer draws it with a depth hack, so its
geometry sits inside walls and floors): PT_TRI_WEAPON triangles are only
visible to rays with PT_RAY_SEES_WEAPON and then win over any world hit
within PT_WEAPON_REACH. World triangles closer than ray.worldTMin are
ignored, which lets rays leaving the weapon escape the geometry it clips.
=================================================================================
*/
PT_INLINE bool IntersectBVH(
	const ptRay_t* ray,
	__global const ptVertex_t* vertices,
	__global const ptTriangle_t* triangles,
	__global const ptBVHNode_t* nodes,
	__global const int* triIndices,
	__global const ptMaterial_t* materials,
	ptHitInfo_t* hitInfo
) {
	float invD[3];
	PT_InvDir(ray->direction, invD);

	const bool seesWeapon = (ray->flags & PT_RAY_SEES_WEAPON) != 0;
	const float worldMin = fmax(ray->tMin, ray->worldTMin);
	const float weaponMax = fmin(ray->tMax, PT_WEAPON_REACH);

	float bestT = ray->tMax;
	float bestU = 0.0f, bestV = 0.0f;
	int bestTri = -1;
	bool bestWeapon = false;

	for (int nodeIdx = 0; nodeIdx >= 0; ) {
		__global const ptBVHNode_t* node = &nodes[nodeIdx];

		const float cullT = (seesWeapon && !bestWeapon) ? fmax(bestT, weaponMax) : bestT;
		if (!PT_RayBox(ray->origin, invD, node->bounds, ray->tMin, cullT)) {
			nodeIdx = node->escapeIndex;
			continue;
		}

		if (node->leftChild != -1) {
			nodeIdx = node->leftChild;
			continue;
		}

		for (int i = node->firstPrimitive; i < node->firstPrimitive + node->primitiveCount; i++) {
			const int triIdx = triIndices[i];
			__global const ptTriangle_t* tri = &triangles[triIdx];
			const bool isWeapon = (tri->flags & PT_TRI_WEAPON) != 0;
			if (isWeapon ? !seesWeapon : bestWeapon) {
				continue;
			}

			float t, u, v;
			if (!IntersectTriangleUV(ray->origin, ray->direction, vertices, tri, &t, &u, &v)) {
				continue;
			}

			if (isWeapon) {
				if (t <= ray->tMin || t >= weaponMax || (bestWeapon && t >= bestT)) continue;
				bestWeapon = true;
			} else if (t <= worldMin || t >= bestT) {
				continue;
			}
			bestT = t;
			bestU = u;
			bestV = v;
			bestTri = triIdx;
		}
		nodeIdx = node->escapeIndex;
	}

	if (bestTri < 0) {
		return false;
	}
	PT_FillHitAttributes(ray, vertices, triangles, bestTri, bestT, bestU, bestV, hitInfo);
	return true;
}

/*
=================================================================================
PT_ShadowTransmit

Filters a shadow ray through a non-opaque surface. Returns false when the
surface blocks the ray.
=================================================================================
*/
PT_INLINE bool PT_ShadowTransmit(
	const float* dir,
	__global const ptVertex_t* vertices,
	__global const ptTriangle_t* tri,
	float u,
	float v,
	__global const ptMaterial_t* materials,
	const ptClTextures_t* textures,
	float* atten
) {
	__global const ptMaterial_t* m = &materials[tri->materialIndex];
	if (m->noShadows || m->isAmbientOnly || m->blendMode == PT_BLEND_ADD) {
		return true;
	}

	float tex[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	if (m->albedoTexture >= 0 && textures) {
		__global const ptVertex_t* v0 = &vertices[tri->vertexIndices[0]];
		__global const ptVertex_t* v1 = &vertices[tri->vertexIndices[1]];
		__global const ptVertex_t* v2 = &vertices[tri->vertexIndices[2]];
		const float w = 1.0f - u - v;
		float tu = w * v0->texcoord[0] + u * v1->texcoord[0] + v * v2->texcoord[0];
		float tv = w * v0->texcoord[1] + u * v1->texcoord[1] + v * v2->texcoord[1];
		PT_ApplyTexTransform(m, &tu, &tv);
		SampleTextureSrgb(textures, m->albedoTexture, tu, tv, tex);
	}
	PT_ApplyVertexColor(m, vertices, tri, 1.0f - u - v, u, v, tex);
	const float alpha = m->albedo[3] * tex[3];

	if (m->alphaTest > 0.0f && alpha < m->alphaTest) {
		return true;
	}

	if (m->transmission > 0.0f) {
#ifdef PT_PHYSICAL
		if (m->physicalMaterial) {
			// Straight next-event rays cannot represent bent solid-glass caustics.
			if (m->thinTransmission == 0) return false;
			__global const float* p0 = vertices[tri->vertexIndices[0]].position;
			__global const float* p1 = vertices[tri->vertexIndices[1]].position;
			__global const float* p2 = vertices[tri->vertexIndices[2]].position;
			float e1[3], e2[3], n[3];
			for (int c = 0; c < 3; c++) { e1[c] = p1[c] - p0[c]; e2[c] = p2[c] - p0[c]; }
			cross3(e1, e2, n);
			normalize3(n);
			const float cosine = fabs(dot3(n, dir));
			float cosineT;
			const float f = PT_DielectricFresnel(cosine, 1.0f, m->ior, &cosineT);
			// A diffuse sheet does not transmit a collimated next-event ray.
			if (m->thinTransmission == 2) return false;
			for (int c = 0; c < 3; c++) {
				float reflection, transmitted;
				PT_ThinDielectric(f, PT_Beer(m->absorption[c], m->thickness / fmax(cosineT, 1e-5f)),
					&reflection, &transmitted);
				atten[c] *= m->transmission * transmitted;
			}
			return true;
		}
#endif
		__global const float* p0 = vertices[tri->vertexIndices[0]].position;
		__global const float* p1 = vertices[tri->vertexIndices[1]].position;
		__global const float* p2 = vertices[tri->vertexIndices[2]].position;
		const float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
		const float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
		float n[3];
		cross3(e1, e2, n);
		normalize3(n);

		const float cosT = fabs(dot3(n, dir));
		float r0 = (m->ior - 1.0f) / (m->ior + 1.0f);
		r0 = r0 * r0;
		const float fresnel = r0 + (1.0f - r0) * pow(1.0f - cosT, 5.0f);
		const float transmitFrac = (1.0f - fresnel) * m->transmission;
		for (int c = 0; c < 3; c++) {
			atten[c] *= transmitFrac * m->albedo[c] * tex[c];
		}
	} else if (m->blendMode == PT_BLEND_ALPHA) {
		const float transmit = 1.0f - fmin(alpha, 1.0f);
		atten[0] *= transmit;
		atten[1] *= transmit;
		atten[2] *= transmit;
	} else {
		return false;
	}

	return atten[0] >= 0.01f || atten[1] >= 0.01f || atten[2] >= 0.01f;
}

/*
=================================================================================
TraceShadow - any hit visibility with colored transmission

Returns false when the segment [max(tMin, worldTMin), tMax] is blocked,
otherwise atten holds the product of the transmittance of every
translucent surface along it. Weapon triangles never cast shadows.
=================================================================================
*/
PT_INLINE bool TraceShadow(
	const ptRay_t* ray,
	__global const ptVertex_t* vertices,
	__global const ptTriangle_t* triangles,
	__global const ptBVHNode_t* nodes,
	__global const int* triIndices,
	__global const ptMaterial_t* materials,
	const ptClTextures_t* textures,
	float* atten
) {
	atten[0] = atten[1] = atten[2] = 1.0f;

	float invD[3];
	PT_InvDir(ray->direction, invD);
	const float tMin = fmax(ray->tMin, ray->worldTMin);

	for (int nodeIdx = 0; nodeIdx >= 0; ) {
		__global const ptBVHNode_t* node = &nodes[nodeIdx];
		if (!PT_RayBox(ray->origin, invD, node->bounds, tMin, ray->tMax)) {
			nodeIdx = node->escapeIndex;
			continue;
		}

		if (node->leftChild != -1) {
			nodeIdx = node->leftChild;
			continue;
		}

		for (int i = node->firstPrimitive; i < node->firstPrimitive + node->primitiveCount; i++) {
			__global const ptTriangle_t* tri = &triangles[triIndices[i]];
			if (tri->flags & (PT_TRI_WEAPON | PT_TRI_NO_OCCLUDE)) {
				continue;
			}

			float t, u, v;
			if (!IntersectTriangleUV(ray->origin, ray->direction, vertices, tri, &t, &u, &v) || t <= tMin || t >= ray->tMax) {
				continue;
			}
			if (tri->flags & PT_TRI_OPAQUE) {
				return false;
			}
			if (!PT_ShadowTransmit(ray->direction, vertices, tri, u, v, materials, textures, atten)) {
				return false;
			}
		}
		nodeIdx = node->escapeIndex;
	}

	return true;
}
