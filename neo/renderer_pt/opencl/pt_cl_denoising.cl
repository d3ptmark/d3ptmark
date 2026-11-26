/*
===============================================================================

	OpenCL C port of pt_kernel_denoising.cuh (SVGF temporal accumulation,
	a-trous filtering, resolve and diagnostics).

===============================================================================
*/

typedef __global const ptClDenoiseGuide_t* ptGuideRef_t;

PT_INLINE float PT_DenoiseLuminance(__global const float* color) {
	return color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
}

PT_INLINE float PT_DenoiseLuminanceP(const float* color) {
	return color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
}

PT_INLINE float PT_DenoiseNormalDot(ptGuideRef_t a, ptGuideRef_t b) {
	return a->normal[0] * b->normal[0] + a->normal[1] * b->normal[1] + a->normal[2] * b->normal[2];
}

PT_INLINE bool PT_DenoiseSupported(ptGuideRef_t guide, int signal) {
	return guide->depth > 0.0f || (signal == 2 && (guide->volumeDepth > 0.0f || guide->coverage == PT_DENOISE_MISS));
}

PT_INLINE float PT_DenoiseDepth(ptGuideRef_t guide, int signal) {
	return signal == 2 && guide->volumeDepth > 0.0f ? guide->volumeDepth : guide->depth;
}

PT_INLINE int PT_DenoiseVolumeLayer(ptGuideRef_t guide) {
	if (guide->coverage == PT_DENOISE_TRANSPARENT) return 1;
	if (guide->coverage == PT_DENOISE_OVERLAY) return 2;
	if (guide->coverage == PT_DENOISE_MIXED && guide->depth <= 0.0f) return 3;
	return 0;
}

// CUDA default: signal = 0
PT_INLINE bool PT_DenoiseSameSurface(ptGuideRef_t a, ptGuideRef_t b, int signal) {
	const float depthA = PT_DenoiseDepth(a, signal), depthB = PT_DenoiseDepth(b, signal);
	if (signal == 2 && depthA <= 0.0f) return depthB <= 0.0f && a->coverage == PT_DENOISE_MISS && b->coverage == PT_DENOISE_MISS;
	return depthB > 0.0f && fabs(depthA - depthB) < fmax(0.01f, depthA * 0.02f)
		&& (signal != 2 || PT_DenoiseVolumeLayer(a) == PT_DenoiseVolumeLayer(b))
		&& (signal == 2 || PT_DenoiseNormalDot(a, b) > 0.9f);
}

PT_INLINE __global const float* PT_DenoiseReflectance(ptGuideRef_t guide, int signal) {
	return signal == 0 ? guide->albedo : guide->specularAlbedo;
}

PT_INLINE void PT_DenoiseIllumination(__global const float* radiance, ptGuideRef_t guide, float* color, int signal) {
	if (signal == 2) {
		for (int c = 0; c < 3; c++) color[c] = radiance[c];
		return;
	}
	__global const float* reflectance = PT_DenoiseReflectance(guide, signal);
	for (int c = 0; c < 3; c++) color[c] = radiance[c] / reflectance[c];
}

__kernel void SVGFCaptureGuidesKernel(
	__global ptClDenoiseGuide_t* guides, __global const ptClDenoiseGuide_t* previousGuides, const ptRenderParams_t params
) {
	const int x = get_global_id(0);
	const int y = get_global_id(1);
	if (x >= params.width || y >= params.height || params.frameIndex == 0) return;
	const int pixel = y * params.width + x;
	__global ptClDenoiseGuide_t* guide = &guides[pixel];
	ptGuideRef_t old = &previousGuides[pixel];
	guide->volumeDepth = old->volumeDepth + (guide->volumeDepth - old->volumeDepth) / (params.frameIndex + 1.0f);
	float planeDistance = 0.0f;
	for (int c = 0; c < 3; c++) planeDistance += (guide->position[c] - old->position[c]) * old->normal[c];
	if (guide->depth <= 0.0f || old->depth <= 0.0f
		|| PT_DenoiseNormalDot(guide, old) < 0.9f || fabs(planeDistance) > fmax(0.01f, old->depth * 0.002f)) {
		if (guide->depth > 0.0f || old->depth > 0.0f || guide->coverage != old->coverage) guide->coverage = PT_DENOISE_MIXED;
		guide->depth = 0.0f;
		guide->historyValid = 0;
		return;
	}
	if (guide->materialIndex != old->materialIndex || guide->coverage == PT_DENOISE_MIXED || old->coverage == PT_DENOISE_MIXED) {
		guide->coverage = PT_DENOISE_MIXED;
		guide->historyValid = 0;
	}
	const float alpha = 1.0f / (params.frameIndex + 1.0f);
	for (int c = 0; c < 3; c++) {
		guide->position[c] = old->position[c] + (guide->position[c] - old->position[c]) * alpha;
		guide->albedo[c] = old->albedo[c] + (guide->albedo[c] - old->albedo[c]) * alpha;
		guide->specularAlbedo[c] = old->specularAlbedo[c] + (guide->specularAlbedo[c] - old->specularAlbedo[c]) * alpha;
		guide->shadingNormal[c] = old->shadingNormal[c] + (guide->shadingNormal[c] - old->shadingNormal[c]) * alpha;
		guide->normal[c] = old->normal[c];
	}
	float normalLength = 0.0f;
	for (int c = 0; c < 3; c++) normalLength += guide->shadingNormal[c] * guide->shadingNormal[c];
	if (normalLength > 1e-12f) for (int c = 0; c < 3; c++) guide->shadingNormal[c] /= sqrt(normalLength);
	guide->depth = old->depth + (guide->depth - old->depth) * alpha;
	guide->roughness = old->roughness + (guide->roughness - old->roughness) * alpha;
}

__kernel void SVGFCaptureKernel(
	__global const float* radiance, __global const ptClDenoiseGuide_t* guides, __global const float* previousMean,
	__global float* mean, __global float* output, __global float* moments, const ptRenderParams_t params, int signal
) {
	const int x = get_global_id(0);
	const int y = get_global_id(1);
	if (x >= params.width || y >= params.height) return;
	const int pixel = y * params.width + x, index = pixel * 4;
	const float count = params.frameIndex + 1.0f, alpha = 1.0f / count;
	const float sampleLum = PT_DenoiseLuminance(radiance + index);
	for (int c = 0; c < 3; c++) {
		mean[index + c] = params.frameIndex == 0 ? radiance[index + c]
			: previousMean[index + c] + (radiance[index + c] - previousMean[index + c]) * alpha;
	}
	mean[index + 3] = params.frameIndex == 0 ? sampleLum * sampleLum
		: previousMean[index + 3] + (sampleLum * sampleLum - previousMean[index + 3]) * alpha;
	ptGuideRef_t guide = &guides[pixel];
	if (!PT_DenoiseSupported(guide, signal)) {
		for (int c = 0; c < 3; c++) output[index + c] = mean[index + c];
		output[index + 3] = 0.0f;
		moments[index] = moments[index + 1] = moments[index + 2] = 0.0f;
		moments[index + 3] = PT_DENOISE_UNSUPPORTED;
		return;
	}
	float illumination[3];
	PT_DenoiseIllumination(mean + index, guide, illumination, signal);
	for (int c = 0; c < 3; c++) output[index + c] = illumination[c];
	const float rawMean = PT_DenoiseLuminance(mean + index);
	const float reflectance = signal == 2 ? 1.0f : fmax(0.01f, PT_DenoiseLuminance(PT_DenoiseReflectance(guide, signal)));
	const float variance = fmax(0.0f, mean[index + 3] - rawMean * rawMean)
		/ (fmax(1.0f, count - 1.0f) * reflectance * reflectance);
	output[index + 3] = variance;
	const float luminance = PT_DenoiseLuminance(output + index);
	moments[index] = luminance;
	moments[index + 1] = luminance * luminance + variance;
	moments[index + 2] = count;
	moments[index + 3] = params.frameIndex == 0 ? PT_DENOISE_RESET : PT_DENOISE_REUSED;
}

__kernel void SVGFTemporalKernel(
	__global const float* radiance, __global const ptClDenoiseGuide_t* guides, __global const ptClDenoiseGuide_t* previousGuides,
	__global const float* previousHistory, __global const float* previousMoments, __global float* output, __global float* moments,
	const ptRenderParams_t params, const ptRenderParams_t previousParams, int historyValid, int signal
) {
	const int x = get_global_id(0);
	const int y = get_global_id(1);
	if (x >= params.width || y >= params.height) return;
	const int pixel = y * params.width + x;
	const int index = pixel * 4;
	ptGuideRef_t guide = &guides[pixel];
	if (!PT_DenoiseSupported(guide, signal)) {
		for (int c = 0; c < 3; c++) output[index + c] = radiance[index + c];
		output[index + 3] = 0.0f;
		for (int c = 0; c < 4; c++) moments[index + c] = 0.0f;
		moments[index + 3] = PT_DENOISE_UNSUPPORTED;
		return;
	}

	float current[3];
	PT_DenoiseIllumination(radiance + index, guide, current, signal);
	const float luminance = PT_DenoiseLuminanceP(current);
	float localMin[3] = { current[0], current[1], current[2] };
	float localMax[3] = { current[0], current[1], current[2] };
	float localMean = 0.0f, localSquare = 0.0f, localCount = 0.0f;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const int sx = x + dx, sy = y + dy;
			if (sx < 0 || sx >= params.width || sy < 0 || sy >= params.height) continue;
			const int neighbor = sy * params.width + sx;
			if (!PT_DenoiseSameSurface(guide, &guides[neighbor], signal)) continue;
			float color[3];
			PT_DenoiseIllumination(radiance + neighbor * 4, &guides[neighbor], color, signal);
			const float lum = PT_DenoiseLuminanceP(color);
			localMean += lum;
			localSquare += lum * lum;
			localCount += 1.0f;
			for (int c = 0; c < 3; c++) {
				localMin[c] = fmin(localMin[c], color[c]);
				localMax[c] = fmax(localMax[c], color[c]);
			}
		}
	}
	localMean /= fmax(localCount, 1.0f);
	const float localVariance = fmax(0.0f, localSquare / fmax(localCount, 1.0f) - localMean * localMean);

	float history[3] = { 0.0f, 0.0f, 0.0f };
	float historyMoments[3] = { 0.0f, 0.0f, 0.0f };
	float totalWeight = 0.0f;
	int rejection = !historyValid ? PT_DENOISE_RESET : (!guide->historyValid ? PT_DENOISE_NO_MOTION : PT_DENOISE_OFFSCREEN);
	if (historyValid && guide->historyValid) {
		float delta[3], previousDepth = 0.0f, horizontal = 0.0f, vertical = 0.0f;
		for (int c = 0; c < 3; c++) {
			delta[c] = guide->position[c] - previousParams.camPos[c];
			previousDepth += delta[c] * previousParams.camForward[c];
			horizontal += delta[c] * previousParams.camRight[c];
			vertical += delta[c] * previousParams.camUp[c];
		}
		const float tanX = PT_TanHalfFov(previousParams.fovX);
		const float tanY = PT_TanHalfFov(previousParams.fovY);
		if (previousDepth > 0.0f && tanX > 1e-6f && tanY > 1e-6f) {
			const float footprint = 2.0f * previousDepth * fmax(tanX / params.width, tanY / params.height);
			float facing = 0.0f;
			for (int c = 0; c < 3; c++) facing += guide->normal[c] * previousParams.camForward[c];
			const float depthTolerance = fmax(0.01f, 2.0f * footprint / fmax(0.1f, fabs(facing)));
			const float px = (horizontal / (previousDepth * tanX) + 1.0f) * 0.5f * params.width - 0.5f;
			const float py = (vertical / (previousDepth * tanY) + 1.0f) * 0.5f * params.height - 0.5f;
			if (px > -1.0f && px < params.width && py > -1.0f && py < params.height) {
				const int ix = (int)floor(px), iy = (int)floor(py);
				const float fx = px - ix, fy = py - iy;
				for (int dy = 0; dy < 2; dy++) {
					for (int dx = 0; dx < 2; dx++) {
						const int sx = ix + dx, sy = iy + dy;
						if (sx < 0 || sx >= params.width || sy < 0 || sy >= params.height) continue;
						const int neighbor = sy * params.width + sx;
						ptGuideRef_t oldGuide = &previousGuides[neighbor];
						if (!oldGuide->historyValid) { rejection = PT_DENOISE_NO_MOTION; continue; }
						float planeDistance = 0.0f;
						for (int c = 0; c < 3; c++) planeDistance += (guide->position[c] - oldGuide->position[c]) * oldGuide->normal[c];
						if (oldGuide->depth <= 0.0f || fabs(oldGuide->depth - previousDepth) > depthTolerance
							|| fabs(planeDistance) > fmax(0.01f, footprint)) { rejection = PT_DENOISE_DEPTH; continue; }
						if (signal != 2 && oldGuide->materialIndex != guide->materialIndex) { rejection = PT_DENOISE_MATERIAL; continue; }
						if (signal != 2 && PT_DenoiseNormalDot(guide, oldGuide) < 0.9f) { rejection = PT_DENOISE_NORMAL; continue; }
						if (signal == 1) {
							float shadingDot = 0.0f, viewDot = 0.0f, viewLength = 0.0f, oldViewLength = 0.0f;
							for (int c = 0; c < 3 && signal != 2; c++) {
								shadingDot += guide->shadingNormal[c] * oldGuide->shadingNormal[c];
								const float view = guide->position[c] - params.camPos[c];
								viewDot += view * delta[c];
								viewLength += view * view;
								oldViewLength += delta[c] * delta[c];
							}
							viewDot /= sqrt(fmax(1e-12f, viewLength * oldViewLength));
							if (shadingDot < 0.98f - 0.18f * guide->roughness * guide->roughness
								|| viewDot < 1.0f - fmax(0.0001f, 0.05f * guide->roughness * guide->roughness)) {
								rejection = PT_DENOISE_NORMAL; continue;
							}
							if (fabs(guide->roughness - oldGuide->roughness) > 0.1f) { rejection = PT_DENOISE_REFLECTANCE; continue; }
						}
						bool sameAlbedo = true;
						__global const float* reflectance = PT_DenoiseReflectance(guide, signal);
						__global const float* oldReflectance = PT_DenoiseReflectance(oldGuide, signal);
						for (int c = 0; c < 3 && signal != 2; c++) {
							sameAlbedo &= fabs(reflectance[c] - oldReflectance[c])
								<= 0.02f + 0.2f * fmax(reflectance[c], oldReflectance[c]);
						}
						if (!sameAlbedo) { rejection = PT_DENOISE_REFLECTANCE; continue; }
						if (previousMoments[neighbor * 4 + 2] < 1.0f) { rejection = PT_DENOISE_NO_SAMPLES; continue; }
						const float weight = (dx ? fx : 1.0f - fx) * (dy ? fy : 1.0f - fy);
						for (int c = 0; c < 3; c++) {
							history[c] += previousHistory[neighbor * 4 + c] * weight;
							historyMoments[c] += previousMoments[neighbor * 4 + c] * weight;
						}
						totalWeight += weight;
					}
				}
			}
		}
	}

	float historyLength = 1.0f;
	float mean = luminance, square = luminance * luminance;
	if (totalWeight > 0.001f) {
		for (int c = 0; c < 3; c++) {
			history[c] /= totalWeight;
			historyMoments[c] /= totalWeight;
		}
		// Neighborhood-based change detection is conservative, not A-SVGF's correlated gradient estimator.
		const float oldVariance = fmax(0.0f, historyMoments[1] - historyMoments[0] * historyMoments[0]);
		const float changeLimit = 3.0f * sqrt(localVariance + oldVariance)
			+ 0.1f * fmax(0.1f, localMean);
		if (fabs(localMean - historyMoments[0]) <= changeLimit) {
			rejection = PT_DENOISE_REUSED;
			const float cap = signal == 2 ? 16.0f : (signal == 0 ? 32.0f : 4.0f + 20.0f * guide->roughness * guide->roughness);
			historyLength = fmin(cap, historyMoments[2] + 1.0f);
			const float alpha = fmax(signal == 2 ? 0.125f : (signal == 0 ? 0.08f : 0.35f - 0.25f * guide->roughness), 1.0f / historyLength);
			const float momentAlpha = fmax(0.1f, 1.0f / historyLength);
			mean += (historyMoments[0] - mean) * (1.0f - momentAlpha);
			square += (historyMoments[1] - square) * (1.0f - momentAlpha);
			for (int c = 0; c < 3; c++) {
				const float margin = 0.25f * (localMax[c] - localMin[c]);
				const float clipped = fmin(localMax[c] + margin, fmax(localMin[c] - margin, history[c]));
				current[c] += (clipped - current[c]) * (1.0f - alpha);
			}
		} else rejection = PT_DENOISE_LIGHTING;
	}
	float variance = fmax(0.0f, square - mean * mean);
	if (historyLength < 4.0f) variance = fmax(variance, localVariance) * (4.0f / historyLength);
	for (int c = 0; c < 3; c++) output[index + c] = current[c];
	output[index + 3] = variance;
	moments[index] = mean;
	moments[index + 1] = square;
	moments[index + 2] = historyLength;
	moments[index + 3] = (float)rejection;
}

__kernel void SVGFBootstrapVolumeVarianceKernel(
	__global float* input, __global const ptClDenoiseGuide_t* guides, __global float* moments, int width, int height, int frameIndex
) {
	const int x = get_global_id(0);
	const int y = get_global_id(1);
	if (x >= width || y >= height || frameIndex >= 3) return;
	const int pixel = y * width + x, index = pixel * 4;
	if (!PT_DenoiseSupported(&guides[pixel], 2)) return;
	float mean = 0.0f, square = 0.0f, count = 0.0f;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const int sx = x + dx, sy = y + dy;
			if (sx < 0 || sx >= width || sy < 0 || sy >= height) continue;
			const int neighbor = sy * width + sx;
			if (!PT_DenoiseSameSurface(&guides[pixel], &guides[neighbor], 2)) continue;
			const float luminance = PT_DenoiseLuminance(input + neighbor * 4);
			mean += luminance;
			square += luminance * luminance;
			count += 1.0f;
		}
	}
	mean /= fmax(1.0f, count);
	input[index + 3] = fmax(input[index + 3], fmax(0.0f, square / fmax(1.0f, count) - mean * mean));
	moments[index + 1] = moments[index] * moments[index] + input[index + 3];
}

__kernel void SVGFAtrousKernel(
	__global const float* input, __global const ptClDenoiseGuide_t* guides, __global float* output,
	int width, int height, int step, float strength, int signal
) {
	const int x = get_global_id(0);
	const int y = get_global_id(1);
	if (x >= width || y >= height) return;
	const int pixel = y * width + x;
	const int index = pixel * 4;
	ptGuideRef_t guide = &guides[pixel];
	if (!PT_DenoiseSupported(guide, signal)) {
		for (int c = 0; c < 4; c++) output[index + c] = input[index + c];
		return;
	}
	if (signal == 1) step = min(step, max(1, (int)(1.0f + 7.0f * guide->roughness * guide->roughness)));

	float variance = 0.0f, varianceWeight = 0.0f, depthSlope = 0.0f;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const int sx = x + dx, sy = y + dy;
			if (sx < 0 || sx >= width || sy < 0 || sy >= height) continue;
			const int neighbor = sy * width + sx;
			if (!PT_DenoiseSameSurface(guide, &guides[neighbor], signal)) continue;
			const float weight = (dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f);
			variance += input[neighbor * 4 + 3] * weight;
			varianceWeight += weight;
			depthSlope = fmax(depthSlope, fabs(PT_DenoiseDepth(guide, signal) - PT_DenoiseDepth(&guides[neighbor], signal)));
		}
	}
	variance /= fmax(1.0f, varianceWeight);
	const float luminance = PT_DenoiseLuminance(input + index);
	const float colorSigma = fmax(0.0001f, 8.0f * strength * sqrt(fmax(0.0f, variance)));
	float sum[3] = { 0.0f, 0.0f, 0.0f };
	float sumVariance = 0.0f, totalWeight = 0.0f;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const int sx = x + dx * step, sy = y + dy * step;
			if (sx < 0 || sx >= width || sy < 0 || sy >= height) continue;
			const int neighbor = sy * width + sx;
			ptGuideRef_t sampleGuide = &guides[neighbor];
			if (!PT_DenoiseSupported(sampleGuide, signal)) continue;
			if (signal == 2 && !PT_DenoiseSameSurface(guide, sampleGuide, signal)) continue;
			const float depthSigma = fmax(0.01f, PT_DenoiseDepth(guide, signal) * 0.002f)
				+ depthSlope * step * sqrt((float)(dx * dx + dy * dy));
			const float colorDelta = fabs(luminance - PT_DenoiseLuminance(input + neighbor * 4));
			const float normalDot = fmin(1.0f, fmax(0.0f, PT_DenoiseNormalDot(guide, sampleGuide)));
			float shadingDot = 0.0f;
			for (int c = 0; c < 3; c++) shadingDot += guide->shadingNormal[c] * sampleGuide->shadingNormal[c];
			shadingDot = fmin(1.0f, fmax(0.0f, shadingDot));
			const float normalWeight = signal == 2 || (dx == 0 && dy == 0) ? 1.0f : pow(normalDot, 32.0f)
				* pow(shadingDot, signal == 0 ? 4.0f : 128.0f - 96.0f * guide->roughness * guide->roughness);
			const float roughnessWeight = signal != 1 ? 1.0f : exp(-20.0f * fabs(guide->roughness - sampleGuide->roughness));
			const float weight = (dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f)
				* normalWeight * roughnessWeight
				* exp(-fabs(PT_DenoiseDepth(guide, signal) - PT_DenoiseDepth(sampleGuide, signal)) / depthSigma - colorDelta / colorSigma);
			for (int c = 0; c < 3; c++) sum[c] += input[neighbor * 4 + c] * weight;
			sumVariance += input[neighbor * 4 + 3] * weight * weight;
			totalWeight += weight;
		}
	}
	// The center tap always has weight four, including at image boundaries.
	for (int c = 0; c < 3; c++) output[index + c] = sum[c] / totalWeight;
	output[index + 3] = sumVariance / (totalWeight * totalWeight);
}

__kernel void SVGFStoreSurfaceKernel(
	__global const float* illumination, __global const ptClDenoiseGuide_t* guides, __global float* output, int width, int height, int signal
) {
	const int x = get_global_id(0);
	const int y = get_global_id(1);
	if (x >= width || y >= height) return;
	const int pixel = y * width + x;
	__global const float* reflectance = PT_DenoiseReflectance(&guides[pixel], signal);
	for (int c = 0; c < 3; c++) output[pixel * 4 + c] = illumination[pixel * 4 + c] * reflectance[c];
	output[pixel * 4 + 3] = 0.0f;
}

__kernel void SVGFResolveKernel(
	__global const float* volume, __global const float* filteredDiffuse, __global const float* filteredSpecular, __global const float* radiance,
	__global const float* diffuseRadiance, __global const float* specularRadiance, __global const float* volumeRadiance,
	__global const float* accumulation, __global const ptClDenoiseGuide_t* guides,
	__global float* output, int width, int height, int debug, int capture
) {
	const int x = get_global_id(0);
	const int y = get_global_id(1);
	if (x >= width || y >= height) return;
	const int pixel = y * width + x;
	const int index = pixel * 4;
	for (int c = 0; c < 3; c++) {
		if (guides[pixel].depth > 0.0f) {
			const float residual = fmax(0.0f, radiance[index + c] - diffuseRadiance[index + c]
				- specularRadiance[index + c] - volumeRadiance[index + c]);
			output[index + c] = debug == 5 ? filteredDiffuse[index + c] : (debug == 6 ? filteredSpecular[index + c]
				: (debug == 7 ? volume[index + c]
				: filteredDiffuse[index + c] + filteredSpecular[index + c] + volume[index + c] + residual));
		} else {
			const float raw = capture && PT_DenoiseSupported(&guides[pixel], 2)
				? fmax(0.0f, accumulation[index + c] - volumeRadiance[index + c]) + volume[index + c]
				: accumulation[index + c];
			output[index + c] = debug == 7 ? volume[index + c] : (debug >= 5 ? 0.0f : raw);
		}
	}
	output[index + 3] = accumulation[index + 3];
}

__kernel void SVGFDenoiseDebugKernel(
	__global const ptClDenoiseGuide_t* guides, __global const float* moments, __global const float* history,
	__global uchar* output, int width, int height, int debug, int progressive
) {
	const int x = get_global_id(0);
	const int y = get_global_id(1);
	if (x >= width || y >= height) return;
	const int pixel = y * width + x, index = pixel * 4;
	float color[3] = { 0.0f, 0.0f, 0.0f };
	if (debug == 1) {
		const int coverage = guides[pixel].coverage;
		color[0] = coverage >= PT_DENOISE_DYNAMIC ? 1.0f : 0.0f;
		color[1] = coverage == PT_DENOISE_STATIC || coverage == PT_DENOISE_DYNAMIC || coverage == PT_DENOISE_MIXED ? 1.0f : 0.0f;
		color[2] = coverage == PT_DENOISE_OVERLAY ? 1.0f : 0.0f;
		if (coverage == PT_DENOISE_MIXED) color[1] = 0.4f;
	} else if (guides[pixel].depth > 0.0f) {
		if (debug == 2) {
			const float t = fmin(1.0f, fmax(0.0f, (moments[index + 2] - 1.0f) / 31.0f));
			color[0] = 1.0f - t;
			color[1] = t;
		} else if (debug == 3) {
			const float variance = sqrt(fmax(0.0f, progressive
				? moments[index + 1] - moments[index] * moments[index] : history[index + 3]));
			const float relative = variance / fmax(0.05f, progressive ? moments[index] : PT_DenoiseLuminance(history + index));
			color[0] = color[1] = color[2] = relative / (1.0f + relative);
		} else {
			const int reason = (int)moments[index + 3];
			switch (reason) {
				case PT_DENOISE_REUSED: color[1] = 1.0f; break;
				case PT_DENOISE_RESET: color[0] = color[2] = 1.0f; break;
				case PT_DENOISE_NO_MOTION: color[0] = color[1] = 1.0f; break;
				case PT_DENOISE_OFFSCREEN: color[2] = 1.0f; break;
				case PT_DENOISE_DEPTH: color[0] = 1.0f; break;
				case PT_DENOISE_NORMAL: color[1] = color[2] = 1.0f; break;
				case PT_DENOISE_MATERIAL: color[0] = 1.0f; color[1] = 0.5f; break;
				case PT_DENOISE_REFLECTANCE: color[0] = 0.5f; color[2] = 1.0f; break;
				case PT_DENOISE_LIGHTING: color[0] = color[1] = color[2] = 1.0f; break;
				case PT_DENOISE_NO_SAMPLES: color[0] = color[1] = color[2] = 0.5f; break;
			}
		}
	}
	for (int c = 0; c < 3; c++) output[index + c] = (uchar)(color[c] * 255.0f + 0.5f);
	output[index + 3] = 255;
}
