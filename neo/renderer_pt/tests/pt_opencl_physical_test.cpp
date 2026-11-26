// OpenCL port of pt_cuda_physical_test.cu (PT_PHYSICAL builds only).
//
// The CUDA helper kernels build small scenes on the stack and hand them to the
// device functions. The OpenCL device functions take __global scene data, so
// each work-item builds the same scene in its own zero-filled ptTestScene_t
// (CUDA's "= {}"). CUDA's g_rayOffset keeps its 0.02 default in this test, which
// the helpers pass explicitly.
#include "renderer_pt/tests/pt_opencl_test_common.h"
#include "renderer_pt/pt_physical.h"

#ifndef PT_PHYSICAL
#error pt_opencl_physical_test requires PT_PHYSICAL
#endif

static const char* testKernels = R"CLC(
typedef struct {
	ptVertex_t vertices[3];
	ptTriangle_t triangle;
	ptBVHNode_t node;
	ptMaterial_t material;
	ptLight_t light;
	ptLight_t lights[4];
	int index;
} ptTestScene_t;

__kernel void CheckLightModel(ptRenderParams_t p, __global float* results, __global ptTestScene_t* scene) {
	if (get_global_id(0) != 0) return;
	__global ptLight_t* light = &scene->light;
	light->intensity = 1.0f;
	light->radius = 1.0f;
	light->boxFalloff = 1;
	light->position[2] = 100.0f;
	light->color[0] = light->color[1] = light->color[2] = 1.0f;
	light->projectedTextureIndex = light->falloffTextureIndex = -1;
	const float position[3] = { 0.0f, 0.0f, 0.0f };
	const float normal[3] = { 0.0f, 0.0f, 1.0f };
	float filter[3];
	results[0] = PT_LightFilter(light, position, 100, normal, NULL, filter, &p);
	results[1] = PT_LightFilter(light, position, 200, normal, NULL, filter, &p);
	p.physicalMetersPerUnit *= 2.0f;
	results[2] = PT_LightFilter(light, position, 100, normal, NULL, filter, &p);
	results[3] = LightImportance(light, position, normal, 100, &p) > 0.0f;
	ptRng_t rng;
	PT_RngInit(&rng, 0, 0);
	int picked = -1;
	float weight = 0.0f;
	results[4] = PT_SelectLights(light, 1, position, normal, 1, 100, &rng, &picked, &weight, &p);
	results[5] = weight;
	light->type = PT_LIGHT_DIRECTIONAL;
	results[6] = PT_LightFilter(light, position, 100, normal, NULL, filter, &p);
	light->direction[2] = -1.0f;
	results[10] = LightImportance(light, position, normal, 100, &p);
	light->type = PT_LIGHT_POINT;
	light->coneAngle = PT_PI / 4.0f;
	light->coneFalloff = 2.0f;
	light->direction[2] = -1.0f;
	p.physicalMetersPerUnit = 0.01f;
	results[7] = PT_LightFilter(light, position, 100, normal, NULL, filter, &p);
	const float away[3] = { 0.0f, 0.0f, -1.0f };
	results[8] = PT_LightFilter(light, position, 100, away, NULL, filter, &p);
	results[9] = PT_LightFilter(light, position, 100, normal, NULL, filter, NULL);
}

__kernel void CheckLens(ptRenderParams_t p, __global float* results, int count) {
	const int index = get_global_id(0);
	if (index >= count) return;
	ptRng_t rng;
	PT_RngInit(&rng, index, 1);
	float origin[3] = { 0.0f, 0.0f, 0.0f };
	float direction[3] = { 0.2f, -0.1f, 1.0f };
	normalize3(direction);
	PT_PhysicalLens(&p, &rng, origin, direction);
	const float t = p.physicalFocusDistance / direction[2];
	results[index * 4] = origin[0] + t * direction[0];
	results[index * 4 + 1] = origin[1] + t * direction[1];
	results[index * 4 + 2] = origin[0] * origin[0] + origin[1] * origin[1];
	p.physicalApertureRadius = 0.0f;
	const uint state = rng.state;
	PT_PhysicalLens(&p, &rng, origin, direction);
	results[index * 4 + 3] = rng.state == state;
}

__kernel void IntegrateBsdf(__global float* results, int count, float roughness, float metallic, float viewCosine) {
	const int index = get_global_id(0);
	if (index >= count) return;
	ptRng_t rng;
	PT_RngInit(&rng, index, 9);
	const float normal[3] = { 0.0f, 0.0f, 1.0f };
	const float wo[3] = { sqrt(1.0f - viewCosine * viewCosine), 0.0f, viewCosine };
	const float albedo[3] = { 1.0f, 0.5f, 0.1f };
	float wi[3], pdf, brdf[3], diffuse[3];
	PT_SamplePhysicalBsdf(normal, wo, albedo, metallic, roughness, 1.5f, &rng, wi, &pdf);
	PT_EvaluatePhysicalMaterial(albedo, normal, wo, wi, metallic, roughness, 1.5f, brdf, diffuse);
	for (int c = 0; c < 3; c++) {
		results[index * 4 + c] = pdf > 0.0f ? brdf[c] * fmax(wi[2], 0.0f) / pdf : 0.0f;
	}
	float reciprocal[3];
	PT_EvaluatePhysicalMaterial(albedo, normal, wi, wo, metallic, roughness, 1.5f, reciprocal, NULL);
	results[index * 4 + 3] = fabs(brdf[0] - reciprocal[0]) / fmax(1.0f, brdf[0]);
}

__kernel void CompareBsdfSampling(
	__global float* results, int count, float roughness, float metallic, float viewCosine, int orientation
) {
	const int index = get_global_id(0);
	if (index >= count) return;
	const float normal[3] = { orientation == 0 ? 0.0f : orientation == 1 ? 0.8f : 1.0f,
		0.0f, orientation == 0 ? 1.0f : orientation == 1 ? 0.6f : 0.0f };
	const float tangent[3] = { normal[2], 0.0f, -normal[0] };
	const float sine = sqrt(1.0f - viewCosine * viewCosine);
	const float wo[3] = { sine * tangent[0] + viewCosine * normal[0], 0.0f,
		sine * tangent[2] + viewCosine * normal[2] };
	const float albedo[3] = { 1.0f, 0.5f, 0.1f };
	const float probability = PT_PhysicalSpecularProbability(albedo, metallic, 1.5f);
	ptRng_t rng;
	PT_RngInit(&rng, index, 37);
	float wi[3], pdf, brdf[3];
	PT_SamplePhysicalBsdf(normal, wo, albedo, metallic, roughness, 1.5f, &rng, wi, &pdf);
	PT_EvaluatePhysicalMaterial(albedo, normal, wo, wi, metallic, roughness, 1.5f, brdf, NULL);
	results[index * 4] = pdf > 0 ? brdf[0] * fmax(dot3(normal, wi), 0.0f) / pdf : 0;
	results[index * 4 + 3] = pdf > 0 ? fabs(dot3(wi, wi) - 1.0f) : 0;

	// Keep the previous NDF sampler and its PDF as an independent statistical reference.
	PT_RngInit(&rng, index, 37);
	const float n[3] = { 0, 0, 1 };
	const float v[3] = { sine, 0, viewCosine };
	if (PT_Rand(&rng) >= probability) {
		SampleHemisphere(n, &rng, wi, &pdf);
	} else {
		const float r = PT_Rand(&rng);
		const float phi = 2 * PT_PI * PT_Rand(&rng);
		const float alpha = roughness * roughness;
		const float cosine = sqrt((1 - r) / (1 + (alpha * alpha - 1) * r));
		const float sinTheta = sqrt(fmax(0.0f, 1 - cosine * cosine));
		const float h[3] = { sinTheta * cos(phi), sinTheta * sin(phi), cosine };
		const float vh = dot3(v, h);
		for (int c = 0; c < 3; c++) wi[c] = 2 * vh * h[c] - v[c];
		if (vh <= 0) wi[2] = -1;
		normalize3(wi);
	}
	pdf = 0;
	if (wi[2] > 0) {
		float h[3] = { v[0] + wi[0], wi[1], v[2] + wi[2] };
		normalize3(h);
		const float alpha = roughness * roughness, a2 = alpha * alpha;
		const float denominator = h[0] * h[0] + h[1] * h[1] + a2 * h[2] * h[2];
		const float distribution = a2 / (PT_PI * denominator * denominator);
		const float specularPdf = distribution * h[2] / (4 * dot3(v, h));
		pdf = (1 - probability) * wi[2] / PT_PI + probability * specularPdf;
	}
	PT_EvaluatePhysicalMaterial(albedo, n, v, wi, metallic, roughness, 1.5f, brdf, NULL);
	results[index * 4 + 1] = pdf > 0 ? brdf[0] * fmax(wi[2], 0.0f) / pdf : 0;

	const float worldWi[3] = { wi[0] * tangent[0] + wi[2] * normal[0], wi[1],
		wi[0] * tangent[2] + wi[2] * normal[2] };
	results[index * 4 + 2] = pdf > 0
		? PT_PhysicalBsdfPdf(normal, wo, worldWi, roughness, probability) / pdf : 0;
}

__kernel void CheckBsdfBoundary(__global float* results) {
	if (get_global_id(0) != 0) return;
	const float n[3] = { 0, 0, 1 }, tangent[3] = { 1, 0, 0 }, back[3] = { 0, 0, -1 };
	const float albedo[3] = { 1, 1, 1 };
	results[0] = PT_PhysicalBsdfPdf(n, tangent, n, 0.6f, 0.5f);
	results[1] = PT_PhysicalBsdfPdf(n, back, n, 0.6f, 0.5f);
	results[2] = PT_PhysicalBsdfPdf(n, n, back, 0.6f, 0.5f);
	ptRng_t rng;
	PT_RngInit(&rng, 0, 0);
	const uint state = rng.state;
	float direction[3], pdf;
	PT_SamplePhysicalBsdf(n, tangent, albedo, 1, 0.6f, 1.5f, &rng, direction, &pdf);
	results[3] = pdf;
	results[4] = dot3(direction, direction);
	PT_SamplePhysicalBsdf(n, back, albedo, 1, 0.6f, 1.5f, &rng, direction, &pdf);
	results[5] = pdf;
	results[6] = dot3(direction, direction);
	results[7] = rng.state == state;
}

__kernel void CheckDirect(__global float* results, __global ptTestScene_t* scene) {
	if (get_global_id(0) != 0) return;
	const ptClGlobals_t g = { 0.0f, 0.02f };
	scene->vertices[0].position[0] = scene->vertices[0].position[1] = -1000.0f;
	scene->vertices[1].position[0] = 1000.0f;
	scene->vertices[1].position[1] = -1000.0f;
	scene->vertices[2].position[1] = 1000.0f;
	scene->triangle.vertexIndices[1] = 1;
	scene->triangle.vertexIndices[2] = 2;
	scene->triangle.flags = PT_TRI_OPAQUE;
	scene->node.bounds[0] = scene->node.bounds[1] = -1000.0f;
	scene->node.bounds[3] = scene->node.bounds[4] = 1000.0f;
	scene->node.leftChild = scene->node.escapeIndex = -1;
	scene->node.primitiveCount = 1;
	scene->index = 0;
	__global ptLight_t* light = &scene->light;
	light->intensity = 1.0f;
	light->color[0] = light->color[1] = light->color[2] = 1.0f;
	light->position[2] = 100.0f;
	light->radius = 1000.0f;
	light->projectedTextureIndex = light->textureIndex = -1;
	ptRenderParams_t p;
	for (uint i = 0; i < sizeof(p) / 4; i++) ((uint*)&p)[i] = 0;
	p.physicalMetersPerUnit = 0.01f;
	p.physicalLightLumens = 4.0f * PT_PI * 100.0f;
	p.physicalIor = 1.5f;
	const float position[3] = { 0.0f, 0.0f, 0.0f }, normal[3] = { 0.0f, 0.0f, 1.0f };
	ptRng_t rng;
	PT_RngInit(&rng, 0, 0);
	float radiance[3], wi[3];
	SampleDirectLight(light, position, normal, 0, 100, scene->vertices, &scene->triangle, &scene->node, &scene->index,
		&scene->material, NULL, &rng, radiance, wi, &p, false, &g);
	results[0] = radiance[0];
	light->position[2] = 200.0f;
	SampleDirectLight(light, position, normal, 0, 100, scene->vertices, &scene->triangle, &scene->node, &scene->index,
		&scene->material, NULL, &rng, radiance, wi, &p, false, &g);
	results[1] = radiance[0];
}

__kernel void CheckTransportMath(ptRenderParams_t p, __global float* results, __global ptTestScene_t* scene) {
	if (get_global_id(0) != 0) return;
	float transmittedCosine;
	results[0] = PT_DielectricFresnel(1.0f, 1.0f, 1.5f, &transmittedCosine);
	results[1] = transmittedCosine;
	results[2] = PT_DielectricFresnel(0.5f, 1.5f, 1.0f, &transmittedCosine);
	results[3] = PT_DielectricFresnel(0.8f, 1.0f, 1.5f, &transmittedCosine);
	results[4] = transmittedCosine;
	results[5] = PT_Beer(log(2.0f), 1.0f);
	const float origin[3] = { 0, 0, 0 }, direction[3] = { 0, 0, 1 };
	p.physicalFogDensity = 0.2f;
	p.physicalFogRadius = 100;
	results[6] = PT_PhysicalFogTransmit(&p, origin, direction, 1000);
	const float outside[3] = { 0, 0, -200 };
	float start;
	results[7] = PT_PhysicalFogInterval(&p, outside, direction, 1000, &start);
	results[8] = start;
	const float tangent[3] = { 200, 0, 0 };
	results[9] = PT_PhysicalFogTransmit(&p, tangent, direction, 1000);
	results[10] = HenyeyGreenstein(1, 0.5f) > HenyeyGreenstein(-1, 0.5f);
	scene->vertices[1].position[0] = scene->vertices[2].position[1] = 1;
	scene->triangle.vertexIndices[1] = 1;
	scene->triangle.vertexIndices[2] = 2;
	__global ptMaterial_t* material = &scene->material;
	material->physicalMaterial = 1;
	material->thinTransmission = 1;
	material->transmission = 0.7f;
	material->ior = 1.5f;
	material->thickness = 1;
	material->albedoTexture = -1;
	for (int c = 0; c < 3; c++) material->absorption[c] = log(2.0f);
	float attenuation[3] = { 1, 1, 1 };
	results[11] = PT_ShadowTransmit(direction, scene->vertices, &scene->triangle, 0, 0, material, NULL, attenuation);
	results[12] = attenuation[0];
	material->thinTransmission = 0;
	results[13] = PT_ShadowTransmit(direction, scene->vertices, &scene->triangle, 0, 0, material, NULL, attenuation);
	material->thinTransmission = 2;
	results[14] = PT_ShadowTransmit(direction, scene->vertices, &scene->triangle, 0, 0, material, NULL, attenuation);
}

__kernel void CheckScattering(__global float* allResults, __global ptTestScene_t* scenes) {
	const int thread = get_global_id(0);
	__global float* results = allResults + thread * 6;
	__global ptTestScene_t* scene = &scenes[thread];
	const ptClGlobals_t g = { 0.0f, 0.02f };
	ptRenderParams_t p;
	for (uint i = 0; i < sizeof(p) / 4; i++) ((uint*)&p)[i] = 0;
	p.physicalIor = 1.5f;
	p.physicalMetersPerUnit = 0.01f;
	p.physicalSunLux = 10;
	p.physicalFogDensity = 0.2f;
	p.physicalFogAlbedo = 0.8f;
	p.physicalFogRadius = 100;
	p.physicalFogSteps = 16;
	p.rayOffset = 0.001f;
	ptRay_t ray;
	for (int c = 0; c < 3; c++) ray.origin[c] = ray.direction[c] = 0.0f;
	ray.tMin = ray.tMax = ray.worldTMin = 0.0f;
	ray.flags = 0;
	ray.direction[2] = 1;
	__global ptLight_t* light = &scene->light;
	light->type = PT_LIGHT_DIRECTIONAL;
	light->direction[2] = -1;
	light->intensity = 1;
	light->color[0] = light->color[1] = light->color[2] = 1;
	scene->vertices[0].position[0] = scene->vertices[0].position[1] = -1000;
	scene->vertices[1].position[0] = 1000;
	scene->vertices[1].position[1] = -1000;
	scene->vertices[2].position[1] = 1000;
	for (int i = 0; i < 3; i++) scene->vertices[i].position[2] = 50;
	scene->triangle.vertexIndices[1] = 1; scene->triangle.vertexIndices[2] = 2;
	scene->triangle.flags = PT_TRI_OPAQUE;
	scene->node.leftChild = scene->node.escapeIndex = -1;
	scene->node.bounds[0] = scene->node.bounds[1] = scene->node.bounds[2] = -1000;
	scene->node.bounds[3] = scene->node.bounds[4] = scene->node.bounds[5] = 1000;
	scene->index = 0;
	ptRng_t rng;
	PT_RngInit(&rng, thread, 0);
	float scattering[3];
	PT_PhysicalSingleScattering(&p, &ray, 100, scene->vertices, &scene->triangle, &scene->node, &scene->index, &scene->material,
		NULL, light, 1, &rng, scattering, &g);
	results[0] = scattering[0];
	p.physicalFogAlbedo = 0;
	PT_PhysicalSingleScattering(&p, &ray, 100, scene->vertices, &scene->triangle, &scene->node, &scene->index, &scene->material,
		NULL, light, 1, &rng, scattering, &g);
	results[1] = scattering[0];
	p.physicalFogAlbedo = 0.8f;
	scene->node.primitiveCount = 1;
	PT_PhysicalSingleScattering(&p, &ray, 40, scene->vertices, &scene->triangle, &scene->node, &scene->index, &scene->material,
		NULL, light, 1, &rng, scattering, &g);
	results[2] = scattering[0];
	p.physicalFogDensity = 0;
	const uint state = rng.state;
	PT_PhysicalSingleScattering(&p, &ray, 100, scene->vertices, &scene->triangle, &scene->node, &scene->index, &scene->material,
		NULL, light, 1, &rng, scattering, &g);
	results[3] = scattering[0];
	results[4] = state == rng.state;
	p.physicalFogDensity = 0.2f;
	p.physicalFogSteps = 1;
	scene->node.primitiveCount = 0;
	for (int i = 0; i < 4; i++) { scene->lights[i] = *light; scene->lights[i].intensity = (float)(1 << i); }
	PT_PhysicalSingleScattering(&p, &ray, 100, scene->vertices, &scene->triangle, &scene->node, &scene->index, &scene->material,
		NULL, scene->lights, 4, &rng, scattering, &g);
	results[5] = scattering[0];
}
)CLC";

// host mirror of ptTestScene_t, only used for its size
struct ptTestScene_t {
	ptVertex_t vertices[3];
	ptTriangle_t triangle;
	ptBVHNode_t node;
	ptMaterial_t material;
	ptLight_t light;
	ptLight_t lights[4];
	int index;
};

static const float PI = 3.14159265358979f;
static const cl_mem nullBuffer = NULL;

static void CheckNear(const char* label, double actual, double expected, double tolerance) {
	if (!std::isfinite(actual) || std::fabs(actual - expected) > tolerance) {
		std::fprintf(stderr, "%s: got %.9f, expected %.9f (+/- %.9f)\n", label, actual, expected, tolerance);
		std::exit(1);
	}
}

static ptRenderParams_t PhysicalParams() {
	ptRenderParams_t p = {};
	p.physicalMetersPerUnit = 0.01f;
	p.physicalLightLumens = 4.0f * PI * 100.0f;
	p.physicalSunLux = 50000.0f;
	p.physicalIor = 1.5f;
	p.physicalRoughnessScale = 1.0f;
	p.physicalFocusDistance = 300.0f;
	p.physicalApertureRadius = 0.625f;
	p.camForward[2] = 1.0f;
	p.camRight[0] = p.camUp[1] = 1.0f;
	return p;
}

// the scene pointers of a CUDA PathTracingKernel launch, each in its own buffer
struct sceneBuffers_t {
	cl_mem vertices, triangles, node, indices, materials, lights;
};

static sceneBuffers_t CreateScene(const ptVertex_t* vertices, int numVertices, const ptTriangle_t* triangles, int numTriangles,
	const ptBVHNode_t& node, const int* indices, const ptMaterial_t* materials, int numMaterials, const ptLight_t& light) {
	sceneBuffers_t s;
	s.vertices = Allocate<ptVertex_t>(numVertices, 0);
	s.triangles = Allocate<ptTriangle_t>(numTriangles, 0);
	s.node = Allocate<ptBVHNode_t>(1, 0);
	s.indices = Allocate<int>(numTriangles, 0);
	s.materials = Allocate<ptMaterial_t>(numMaterials, 0);
	s.lights = Allocate<ptLight_t>(1, 0);
	Upload(s.vertices, vertices, numVertices * sizeof(ptVertex_t));
	Upload(s.triangles, triangles, numTriangles * sizeof(ptTriangle_t));
	Upload(s.node, &node, sizeof(node));
	Upload(s.indices, indices, numTriangles * sizeof(int));
	Upload(s.materials, materials, numMaterials * sizeof(ptMaterial_t));
	Upload(s.lights, &light, sizeof(light));
	return s;
}

static void FreeScene(sceneBuffers_t& s) {
	Free(s.vertices); Free(s.triangles); Free(s.node); Free(s.indices); Free(s.materials); Free(s.lights);
}

// PathTracingKernel( vertices, triangles, node, indices, materials, NULL textures, lights, numLights, ... )
static void PathTrace(const sceneBuffers_t& s, int numLights, cl_mem accumulation, const ptRenderParams_t& p,
	cl_mem radiance = NULL, cl_mem guide = NULL, cl_mem diffuse = NULL, cl_mem specular = NULL, cl_mem volume = NULL) {
	SetArgs(clTest.program.kernels[PT_CL_PATHTRACING])(s.vertices)(s.triangles)(s.node)(s.indices)(s.materials)
		(nullBuffer)(nullBuffer)(s.lights)((cl_int)numLights)(accumulation)(p)(radiance)(guide)(diffuse)(specular)(volume);
	CL_CHECK(PT_CL_Launch2D(clTest.program, clTest.queue, PT_CL_PATHTRACING, p.width, p.height));
	CL_CHECK(clFinish(clTest.queue));
}

static void CheckVisibleNormalSampling(cl_mem device, std::vector<float>& results) {
	const int count = 512 * 512;
	cl_kernel boundary = TestKernel("CheckBsdfBoundary");
	SetArgs(boundary)(device);
	Run1D(boundary, 1, 1);
	Download(results.data(), device, 8 * sizeof(float));
	for (int i = 0; i < 7; i++) CheckNear("nonpositive view/light hemisphere has no BSDF samples", results[i], 0, 0);
	CheckNear("rejected view preserves RNG", results[7], 1, 0);
	cl_kernel compare = TestKernel("CompareBsdfSampling");
	for (float roughness : { 0.04f, 0.2f, 0.6f, 1.0f }) {
		for (float metallic : { 0.0f, 1.0f }) {
			for (float cosine : { 1.0f, 0.2f, 0.05f }) {
				for (int orientation : { 0, 1, 2 }) {
					SetArgs(compare)(device)((cl_int)count)(roughness)(metallic)(cosine)((cl_int)orientation);
					Run1D(compare, count, 256);
					Download(results.data(), device, count * 4 * sizeof(float));
					double mean = 0, reference = 0, moment = 0, referenceMoment = 0, mass = 0;
					double maximum = 0;
					int accepted = 0, referenceAccepted = 0;
					for (int i = 0; i < count; i++) {
						const double value = results[i * 4], oldValue = results[i * 4 + 1];
						if (!std::isfinite(value) || value < 0 || !std::isfinite(oldValue) || oldValue < 0) {
							std::fprintf(stderr, "Invalid specular sampling weight\n");
							std::exit(1);
						}
						mean += value / count;
						reference += oldValue / count;
						moment += value * value / count;
						referenceMoment += oldValue * oldValue / count;
						mass += results[i * 4 + 2] / count;
						maximum = std::fmax(maximum, value);
						accepted += value > 0;
						referenceAccepted += oldValue > 0;
						CheckNear("BSDF sampled direction is normalized", results[i * 4 + 3], 0, 2e-6);
					}
					const double variance = std::fmax(0.0, moment - mean * mean);
					const double oldVariance = std::fmax(0.0, referenceMoment - reference * reference);
					const double standardError = std::sqrt((variance + oldVariance) / count);
					std::printf("BSDF sampling r=%.2f m=%.0f nv=%.2f orientation=%d: mean %.6f / %.6f, "
						"variance %.6f / %.6f, accepted %.4f / %.4f, max %.5f\n",
						roughness, metallic, cosine, orientation, mean, reference, variance, oldVariance,
						(double)accepted / count, (double)referenceAccepted / count, maximum);
					CheckNear("BSDF sampler preserves reflected energy", mean, reference, 6 * standardError + 0.001);
					CheckNear("BSDF PDF mass matches sampling acceptance", (double)accepted / count, mass, 0.006);
					if (metallic == 1) {
						CheckNear("visible-normal conductor weight is bounded",
							std::fmax(0.0, maximum - 1.0 / 0.95), 0, 0.0002);
						if (roughness >= 0.2f && cosine < 1.0f && variance >= 0.6 * oldVariance) {
							std::fprintf(stderr, "Grazing conductor variance did not improve by at least 40%%\n");
							std::exit(1);
						}
						if (roughness == 0.6f && cosine == 0.05f
							&& (variance >= 0.1 * oldVariance || (double)accepted / count < 0.95)) {
							std::fprintf(stderr, "Rough grazing conductor sampling regressed\n");
							std::exit(1);
						}
					}
				}
			}
		}
	}
	clReleaseKernel(boundary);
	clReleaseKernel(compare);
}

static void CheckPathIntegration() {
	ptVertex_t vertices[3] = {};
	ptTriangle_t triangle = {};
	ptBVHNode_t node = {};
	ptMaterial_t material = {};
	ptLight_t light = {};
	const int index = 0;
	vertices[0].position[0] = vertices[0].position[1] = -1000;
	vertices[1].position[0] = 1000;
	vertices[1].position[1] = -1000;
	vertices[2].position[1] = 1000;
	for (int i = 0; i < 3; i++) {
		vertices[i].position[2] = 100;
		vertices[i].normal[2] = -1;
	}
	triangle.vertexIndices[1] = 1;
	triangle.vertexIndices[2] = 2;
	triangle.flags = PT_TRI_OPAQUE;
	node.bounds[0] = node.bounds[1] = -1000;
	node.bounds[2] = node.bounds[5] = 100;
	node.bounds[3] = node.bounds[4] = 1000;
	node.leftChild = node.escapeIndex = -1;
	node.primitiveCount = 1;
	material.albedoTexture = material.emissionTexture = material.specularTexture = material.normalTexture = -1;
	material.albedo[3] = 1;
	material.roughness = 0.5f;
	material.texTransform[0] = material.texTransform[4] = 1;
	light.intensity = 1;
	light.textureIndex = light.projectedTextureIndex = -1;
	for (int c = 0; c < 3; c++) {
		material.albedo[c] = 1;
		material.emission[c] = 1.0f / (1 << c);
		light.color[c] = 1;
	}
	sceneBuffers_t device = CreateScene(vertices, 3, &triangle, 1, node, &index, &material, 1, light);
	cl_mem accumulation = Allocate<float>(4, 0);
	cl_mem diffuse = Allocate<float>(4, 0), specular = Allocate<float>(4, 0);
	cl_mem volume = Allocate<float>(4, 0), radiance = Allocate<float>(4, 0);
	cl_mem guide = Allocate<ptClDenoiseGuide_t>(1, 0);
	ptRenderParams_t p = PhysicalParams();
	p.width = p.height = 1;
	p.maxDepth = 1;
	p.samplesPerPixel = 4;
	p.maxLightSamples = 1;
	p.rayOffset = 0.001f;
	p.emissionBoost = 100;
	p.captureTargetFrames = 2;
	float result[4], d[4], s[4];
	ptClDenoiseGuide_t g;
	for (int frame = 0; frame < 2; frame++) {
		p.frameIndex = frame;
		p.sampleIndex = frame;
		PathTrace(device, 0, accumulation, p, NULL, guide, diffuse, specular);
		Download(result, accumulation, sizeof(result));
		for (int c = 0; c < 3; c++) CheckNear("progressive physical emissive nits", result[c], 100.0f / (1 << c), 0.0001);
	}
	Download(&g, guide, sizeof(g));
	CheckNear("DOF rejects pinhole temporal history", g.historyValid, 0, 0);
	p.physicalApertureRadius = 0;
	p.frameIndex = 0;
	for (int c = 0; c < 3; c++) material.emission[c] = 0;
	Upload(device.materials, &material, sizeof(material));
	PathTrace(device, 1, accumulation, p, NULL, guide, diffuse, specular);
	Download(result, accumulation, sizeof(result));
	Download(d, diffuse, sizeof(d));
	Download(s, specular, sizeof(s));
	Download(&g, guide, sizeof(g));
	const float expectedDiffuse = 100.0f * 0.96f * 0.96f / PI;
	const float expectedSpecular = 100.0f * 0.04f / (4.0f * PI * 0.0625f);
	CheckNear("physical path diffuse", d[0], expectedDiffuse, 0.001);
	CheckNear("physical path specular", s[0], expectedSpecular, 0.001);
	CheckNear("physical path signal conservation", result[0], d[0] + s[0], 0.001);
	CheckNear("pinhole permits temporal history", g.historyValid, 1, 0);
	p.physicalFogDensity = logf(2.0f);
	p.physicalFogRadius = 1000;
	p.physicalFogSteps = 8;
	for (int c = 0; c < 3; c++) material.emission[c] = 1;
	Upload(device.materials, &material, sizeof(material));
	for (int frame = 0; frame < 2; frame++) {
		p.frameIndex = frame;
		PathTrace(device, 0, accumulation, p, NULL, guide, diffuse, specular);
		Download(result, accumulation, sizeof(result));
		CheckNear("progressive fog extinction preserves residual emission", result[0], 50, 0.001);
	}
	p.frameIndex = 0;
	p.physicalFogAlbedo = 0.8f;
	p.physicalFogAnisotropy = 0.3f;
	float v[4], fresh[4];
	PathTrace(device, 1, accumulation, p, radiance, guide, diffuse, specular, volume);
	Download(result, accumulation, sizeof(result));
	Download(fresh, radiance, sizeof(fresh));
	Download(d, diffuse, sizeof(d));
	Download(s, specular, sizeof(s));
	Download(v, volume, sizeof(v));
	if (!std::isfinite(v[0]) || v[0] <= 0.0f) {
		std::fprintf(stderr, "Physical volume decomposition must exercise nonzero scattering\n");
		std::exit(1);
	}
	for (int c = 0; c < 3; c++) {
		CheckNear("physical fog signal preserves attenuated primary emission", fresh[c] - d[c] - s[c] - v[c], 50, 0.001);
		CheckNear("physical volume output preserves raw transport", fresh[c], result[c], 0);
	}
	PathTrace(device, 1, accumulation, p);
	Download(result, accumulation, sizeof(result));
	for (int c = 0; c < 3; c++) CheckNear("physical signal extraction does not change random draws", fresh[c], result[c], 0);
	FreeScene(device);
	Free(accumulation); Free(diffuse); Free(specular); Free(volume); Free(radiance); Free(guide);
}

static void CheckTransmissionPaths() {
	ptVertex_t vertices[9] = {};
	ptTriangle_t triangles[3] = {};
	ptBVHNode_t node = {};
	ptMaterial_t materials[2] = {};
	ptLight_t light = {};
	int indices[3] = {};
	for (int plane = 0; plane < 3; plane++) {
		ptVertex_t* v = vertices + plane * 3;
		v[0].position[0] = v[0].position[1] = -100000;
		v[1].position[0] = 100000;
		v[1].position[1] = -100000;
		v[2].position[1] = 100000;
		for (int j = 0; j < 3; j++) {
			v[j].position[2] = 100.0f * (plane + 1);
			v[j].normal[2] = plane == 1 ? 1 : -1;
			triangles[plane].vertexIndices[j] = plane * 3 + j;
		}
		if (plane != 1) {
			triangles[plane].vertexIndices[1] = plane * 3 + 2;
			triangles[plane].vertexIndices[2] = plane * 3 + 1;
		}
		triangles[plane].materialIndex = plane == 2 ? 1 : 0;
		indices[plane] = plane;
	}
	node.bounds[0] = node.bounds[1] = -100000;
	node.bounds[2] = 100;
	node.bounds[3] = node.bounds[4] = 100000;
	node.bounds[5] = 300;
	node.leftChild = node.escapeIndex = -1;
	node.primitiveCount = 3;
	for (int m = 0; m < 2; m++) {
		ptMaterial_t& material = materials[m];
		material.albedoTexture = material.normalTexture = material.specularTexture = material.emissionTexture = -1;
		material.texTransform[0] = material.texTransform[4] = 1;
		material.albedo[3] = 1;
		material.roughness = 0.5f;
		for (int c = 0; c < 3; c++) material.albedo[c] = 1;
	}
	materials[0].physicalMaterial = 1;
	materials[0].transmission = 1;
	materials[0].ior = 1.5f;
	materials[1].metallic = 1;
	for (int c = 0; c < 3; c++) {
		materials[0].absorption[c] = logf(2.0f) * (c + 1);
		materials[1].albedo[c] = 0;
		materials[1].emission[c] = 1;
	}
	const int pixels = 4096;
	sceneBuffers_t device = CreateScene(vertices, 9, triangles, 3, node, indices, materials, 2, light);
	cl_mem accumulation = Allocate<float>(pixels * 4, 0);
	cl_mem diffuse = Allocate<float>(pixels * 4, 0);
	cl_mem specular = Allocate<float>(pixels * 4, 0);
	cl_mem guides = Allocate<ptClDenoiseGuide_t>(pixels, 0);
	ptRenderParams_t p = PhysicalParams();
	p.width = pixels; p.height = 1;
	p.physicalApertureRadius = 0;
	p.samplesPerPixel = 8;
	p.maxDepth = 16;
	p.rayOffset = 0.0001f;
	p.emissionBoost = 1;
	p.captureTargetFrames = 2;
	std::vector<float> results(pixels * 4);
	for (int mode = 0; mode < 3; mode++) {
		materials[0].thinTransmission = mode;
		materials[0].thickness = 1;
		node.primitiveCount = mode == 0 ? 3 : 2;
		indices[1] = mode == 0 ? 1 : 2;
		if (mode == 2) {
			// A diffuse sheet sees a constant unit environment on either side.
			node.primitiveCount = 1;
			p.skyIntensity = 1;
			for (int c = 0; c < 3; c++) p.skyZenith[c] = p.skyHorizon[c] = p.skyGround[c] = 1;
		}
		Upload(device.materials, materials, sizeof(materials));
		Upload(device.node, &node, sizeof(node));
		Upload(device.indices, indices, sizeof(indices));
		for (int frame = 0; frame < 2; frame++) {
			p.frameIndex = frame; p.sampleIndex = frame + 7;
			PathTrace(device, 0, accumulation, p, NULL, guides, diffuse, specular);
			Download(results.data(), accumulation, results.size() * sizeof(float));
			for (int c = 0; c < 3; c++) {
				double mean = 0;
				for (int i = 0; i < pixels; i++) mean += results[i * 4 + c] / pixels;
				const double absorb = std::pow(0.5, c + 1);
				const double f = 0.04;
				const double transmitted = (1 - f) * (1 - f) * absorb / (1 - f * f * absorb * absorb);
				const double reflected = f + transmitted * f * absorb;
				const double expected = mode == 2 ? reflected + transmitted : transmitted;
				CheckNear(mode == 0 ? "solid dielectric progressive absorption" :
					(mode == 1 ? "thin glass progressive transport" : "diffuse sheet progressive transport"),
					mean, expected, 0.012);
			}
		}
		ptClDenoiseGuide_t guide;
		Download(&guide, guides, sizeof(guide));
		CheckNear("transmission rejects opaque denoising history", guide.depth, 0, 0);
	}
	materials[0].transmission = 0.4f;
	for (int c = 0; c < 3; c++) materials[0].emission[c] = 0.25f;
	Upload(device.materials, materials, sizeof(materials));
	p.skyIntensity = 0;
	p.maxDepth = 1;
	p.frameIndex = 0;
	PathTrace(device, 0, accumulation, p, NULL, guides, diffuse, specular);
	Download(results.data(), accumulation, results.size() * sizeof(float));
	for (int i = 0; i < pixels; i++) CheckNear("mixed dielectric emission is counted exactly once", results[i * 4], 0.25, 1e-6);
	FreeScene(device);
	Free(accumulation); Free(diffuse); Free(specular); Free(guides);
}

int main() {
	CheckNear("EV100 sunny 16", PT_PhysicalEV100(16, 1.0f / 125, 100), std::log2(32000.0), 0.00001);
	const float exposure = PT_PhysicalExposure(2.8f, 1.0f / 30, 800, 0);
	CheckNear("ISO doubles exposure", PT_PhysicalExposure(2.8f, 1.0f / 30, 1600, 0) / exposure, 2, 0.00001);
	CheckNear("shutter doubles exposure", PT_PhysicalExposure(2.8f, 1.0f / 15, 800, 0) / exposure, 2, 0.00001);
	CheckNear("stop doubles exposure", PT_PhysicalExposure(2.8f, 1.0f / 30, 800, 1) / exposure, 2, 0.00001);
	CheckNear("aperture halves exposure", PT_PhysicalExposure(2.8f * std::sqrt(2.0f), 1.0f / 30, 800, 0) / exposure, 0.5, 0.00001);
	CheckNear("50mm full-frame FOV", PT_PhysicalFov(36, 50), 39.59775, 0.0001);
	CheckNear("physical aperture radius", PT_PhysicalApertureRadius(50, 4, 0.01f), 0.625, 0.00001);
	ptRenderParams_t p = PhysicalParams(), changed = p;
	changed.exposure = 2;
	CheckNear("exposure preserves linear history", PT_PhysicalTraceChanged(p, changed), 0, 0);
	changed.physicalApertureRadius *= 2;
	CheckNear("aperture invalidates history", PT_PhysicalTraceChanged(p, changed), 1, 0);
	changed = p;
	changed.physicalFocusDistance *= 2;
	CheckNear("focus invalidates history", PT_PhysicalTraceChanged(p, changed), 1, 0);

	const int init = PT_CL_TestInit("OpenCL physical test", testKernels);
	if (init) return init;
	CheckPathIntegration();
	CheckTransmissionPaths();
	const int count = 262144;
	cl_mem device = Allocate<float>(count * 4, 0);
	const int scatterCount = 4096;
	cl_mem scenes = Allocate<ptTestScene_t>(scatterCount, 0);
	std::vector<float> results(count * 4);

	cl_kernel transport = TestKernel("CheckTransportMath");
	SetArgs(transport)(p)(device)(scenes);
	Run1D(transport, 1, 1);
	Download(results.data(), device, 15 * sizeof(float));
	CheckNear("dielectric normal Fresnel", results[0], 0.04, 1e-6);
	CheckNear("dielectric normal refraction", results[1], 1, 1e-6);
	CheckNear("total internal reflection", results[2], 1, 0);
	CheckNear("Snell law", results[4], std::sqrt(1 - 0.36 / 2.25), 1e-6);
	CheckNear("Beer absorption", results[5], 0.5, 1e-6);
	CheckNear("bounded fog transmittance", results[6], std::exp(-0.2), 1e-6);
	CheckNear("outside fog sphere interval", results[7], 200, 1e-5);
	CheckNear("outside fog sphere entry", results[8], 100, 1e-5);
	CheckNear("fog miss does not attenuate", results[9], 1, 0);
	CheckNear("forward scattering orientation", results[10], 1, 0);
	CheckNear("thin glass shadow visibility", results[11], 1, 0);
	CheckNear("absorbing sheet shadow energy", results[12], 0.7 * 0.96 * 0.96 * 0.5 / (1 - 0.04 * 0.04 * 0.25), 1e-6);
	CheckNear("solid glass does not fake straight caustics", results[13], 0, 0);
	CheckNear("diffuse sheet does not fake straight caustics", results[14], 0, 0);

	Memset(scenes, 0, scatterCount * sizeof(ptTestScene_t));
	cl_kernel scattering = TestKernel("CheckScattering");
	SetArgs(scattering)(device)(scenes);
	Run1D(scattering, scatterCount, 256);
	Download(results.data(), device, scatterCount * 6 * sizeof(float));
	CheckNear("analytic shadowed single scattering", results[0], 10 * 0.8 * 0.2 * std::exp(-0.2) / (4 * PI), 1e-6);
	CheckNear("pure absorbing fog has no scattering", results[1], 0, 0);
	CheckNear("occluder blocks volumetric illumination", results[2], 0, 0);
	CheckNear("disabled fog has no scattering", results[3], 0, 0);
	CheckNear("disabled fog preserves RNG", results[4], 1, 0);
	double scatterMean = 0;
	for (int i = 0; i < scatterCount; i++) scatterMean += results[i * 6 + 5] / scatterCount;
	CheckNear("bounded volumetric light reservoir is unbiased", scatterMean, 15 * 10 * 0.8 * 0.2 * std::exp(-0.2) / (4 * PI), 0.02);

	Memset(scenes, 0, sizeof(ptTestScene_t));
	cl_kernel lightModel = TestKernel("CheckLightModel");
	SetArgs(lightModel)(p)(device)(scenes);
	Run1D(lightModel, 1, 1);
	Download(results.data(), device, 11 * sizeof(float));
	CheckNear("100 lux at one meter", results[0], 100, 0.0001);
	CheckNear("inverse-square distance", results[1], 25, 0.0001);
	CheckNear("world scale affects illuminance", results[2], 25, 0.0001);
	CheckNear("importance outside legacy volume", results[3], 1, 0);
	CheckNear("light selection outside legacy volume", results[4], 1, 0);
	CheckNear("selection PDF", results[5], 1, 0.00001);
	CheckNear("directional lux", results[6], 50000, 0.01);
	CheckNear("directional photometric importance", results[10], 51000, 0.01);
	const float solidAngle = 2 * PI * (1 - std::cos(PI / 4)) / 3;
	CheckNear("normalized spot flux", results[7], p.physicalLightLumens / solidAngle, 0.01);
	CheckNear("spot rejects back hemisphere", results[8], 0, 0);
	CheckNear("legacy filter still clips volume", results[9], 0, 0);

	Memset(scenes, 0, sizeof(ptTestScene_t));
	cl_kernel direct = TestKernel("CheckDirect");
	SetArgs(direct)(device)(scenes);
	Run1D(direct, 1, 1);
	Download(results.data(), device, 2 * sizeof(float));
	CheckNear("unclamped physical direct light", results[0], 100, 0.001);
	CheckNear("direct inverse-square distance", results[1], 25, 0.001);

	cl_kernel lens = TestKernel("CheckLens");
	SetArgs(lens)(p)(device)((cl_int)count);
	Run1D(lens, count, 256);
	Download(results.data(), device, results.size() * sizeof(float));
	double meanRadiusSquared = 0;
	for (int i = 0; i < count; i++) {
		CheckNear("lens focus X", results[i * 4], 60, 0.0001);
		CheckNear("lens focus Y", results[i * 4 + 1], -30, 0.0001);
		CheckNear("lens support", std::fmax(0.0f, results[i * 4 + 2] - p.physicalApertureRadius * p.physicalApertureRadius), 0, 0.00001);
		CheckNear("pinhole preserves RNG", results[i * 4 + 3], 1, 0);
		meanRadiusSquared += results[i * 4 + 2] / count;
	}
	CheckNear("uniform aperture area", meanRadiusSquared, p.physicalApertureRadius * p.physicalApertureRadius * 0.5, 0.001);
	CheckVisibleNormalSampling(device, results);
	cl_kernel integrate = TestKernel("IntegrateBsdf");
	for (float roughness : { 0.04f, 0.2f, 0.6f, 1.0f }) {
		for (float metallic : { 0.0f, 1.0f }) {
			for (float cosine : { 1.0f, 0.2f }) {
				SetArgs(integrate)(device)((cl_int)count)(roughness)(metallic)(cosine);
				Run1D(integrate, count, 256);
				Download(results.data(), device, results.size() * sizeof(float));
				double energy[3] = {};
				for (int i = 0; i < count; i++) {
					for (int c = 0; c < 3; c++) {
						const float value = results[i * 4 + c];
						if (!std::isfinite(value) || value < 0) {
							std::fprintf(stderr, "Invalid physical BSDF sample\n");
							return 1;
						}
						energy[c] += value / count;
					}
					CheckNear("BSDF reciprocity", results[i * 4 + 3], 0, 0.0001);
				}
				std::printf("roughness %.2f metal %.0f view %.1f: reflected energy %.5f %.5f %.5f\n",
					roughness, metallic, cosine, energy[0], energy[1], energy[2]);
				for (double e : energy) {
					if (e < 0.001 || e > 1.02) {
						std::fprintf(stderr, "BSDF failed energy bounds: %.8f\n", e);
						return 1;
					}
				}
			}
		}
	}
	clReleaseKernel(transport); clReleaseKernel(scattering); clReleaseKernel(lightModel);
	clReleaseKernel(direct); clReleaseKernel(lens); clReleaseKernel(integrate);
	Free(device);
	Free(scenes);
	std::puts("OpenCL physical lighting, camera, sampling and energy-conservation tests passed");
	return 0;
}
