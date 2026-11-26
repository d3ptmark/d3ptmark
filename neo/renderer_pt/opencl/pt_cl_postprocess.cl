/*
===============================================================================

	OpenCL C port of the kernels in pt_cuda_postprocess.cuh. The host side
	of PT_Postprocess lives in pt_opencl_kernels.cpp.

===============================================================================
*/

PT_INLINE float PT_PostFinite( float value ) {
	return isfinite( value ) ? fmin( fmax( value, 0.0f ), 1.0e12f ) : 0.0f;
}

PT_INLINE float4 PT_PostSample( __global const float4 *image, int width, int height, float x, float y ) {
	x = fmin( fmax( x, 0.0f ), (float)( width - 1 ) );
	y = fmin( fmax( y, 0.0f ), (float)( height - 1 ) );
	const int x0 = (int)x, y0 = (int)y;
	const int x1 = min( x0 + 1, width - 1 ), y1 = min( y0 + 1, height - 1 );
	const float tx = x - x0, ty = y - y0;
	const float4 a = image[y0 * width + x0], b = image[y0 * width + x1];
	const float4 c = image[y1 * width + x0], d = image[y1 * width + x1];
	return (float4)(
		( a.x * ( 1 - tx ) + b.x * tx ) * ( 1 - ty ) + ( c.x * ( 1 - tx ) + d.x * tx ) * ty,
		( a.y * ( 1 - tx ) + b.y * tx ) * ( 1 - ty ) + ( c.y * ( 1 - tx ) + d.y * tx ) * ty,
		( a.z * ( 1 - tx ) + b.z * tx ) * ( 1 - ty ) + ( c.z * ( 1 - tx ) + d.z * tx ) * ty, 0 );
}

__kernel void PT_PostHighlights( __global const float4 *hdr, __global float4 *highlights, int width, int height,
								 int smallWidth, int smallHeight, int scale, float sceneThreshold ) {
	const int x = get_global_id( 0 ), y = get_global_id( 1 );
	if ( x >= smallWidth || y >= smallHeight ) return;
	float4 sum = (float4)( 0, 0, 0, 0 );
	int count = 0;
	for ( int sy = y * scale; sy < min( ( y + 1 ) * scale, height ); sy++ ) {
		for ( int sx = x * scale; sx < min( ( x + 1 ) * scale, width ); sx++ ) {
			const float4 raw = hdr[sy * width + sx];
			const float r = PT_PostFinite( raw.x ), g = PT_PostFinite( raw.y ), b = PT_PostFinite( raw.z );
			const float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
			const float amount = fmax( 0.0f, 1.0f - sceneThreshold / fmax( luminance, 1.0e-20f ) );
			sum.x += r * amount; sum.y += g * amount; sum.z += b * amount;
			count++;
		}
	}
	const float inverse = 1.0f / count;
	highlights[y * smallWidth + x] = (float4)( sum.x * inverse, sum.y * inverse, sum.z * inverse, 0 );
}

__kernel void PT_PostBlur( __global const float4 *input, __global float4 *output, int width, int height, float sigma, int horizontal ) {
	const int x = get_global_id( 0 ), y = get_global_id( 1 );
	if ( x >= width || y >= height ) return;
	float4 sum = (float4)( 0, 0, 0, 0 );
	float total = 0;
	const int radius = (int)ceil( 3.0f * sigma );
	for ( int tap = -radius; tap <= radius; tap++ ) {
		const float weight = exp( -0.5f * tap * tap / ( sigma * sigma ) );
		const int sx = min( max( x + ( horizontal ? tap : 0 ), 0 ), width - 1 );
		const int sy = min( max( y + ( horizontal ? 0 : tap ), 0 ), height - 1 );
		const float4 value = input[sy * width + sx];
		sum.x += value.x * weight; sum.y += value.y * weight; sum.z += value.z * weight;
		total += weight;
	}
	output[y * width + x] = (float4)( sum.x / total, sum.y / total, sum.z / total, 0 );
}

__kernel void PT_PostComposite( __global const float4 *hdr, __global const float4 *blurred, __global float4 *output,
								int width, int height, int smallWidth, int smallHeight, int scale,
								float bloomStrength, float flareStrength ) {
	const int x = get_global_id( 0 ), y = get_global_id( 1 );
	if ( x >= width || y >= height ) return;
	const float4 base = hdr[y * width + x];
	const float4 bloom = PT_PostSample( blurred, smallWidth, smallHeight,
		( x + 0.5f ) / scale - 0.5f, ( y + 0.5f ) / scale - 0.5f );
	float r = PT_PostFinite( base.x ) + bloom.x * bloomStrength;
	float g = PT_PostFinite( base.y ) + bloom.y * bloomStrength;
	float b = PT_PostFinite( base.z ) + bloom.z * bloomStrength;
	if ( flareStrength > 0 ) {
		const float u = ( x + 0.5f ) / width - 0.5f, v = ( y + 0.5f ) / height - 0.5f;
		const float vignette = fmax( 0.0f, 1.0f - 2.0f * ( u * u + v * v ) );
		// Restrained screen-space ghosts from reflected highlights, not an optical lens simulation.
		for ( int ghost = 0; ghost < 3; ghost++ ) {
			const float magnification = 0.55f + ghost * 0.35f;
			const float su = 0.5f - u * magnification, sv = 0.5f - v * magnification;
			if ( su < 0 || su > 1 || sv < 0 || sv > 1 ) continue;
			const float4 light = PT_PostSample( blurred, smallWidth, smallHeight,
				su * width / scale - 0.5f, sv * height / scale - 0.5f );
			const float weight = flareStrength * vignette * ( 0.08f / ( ghost + 1 ) );
			r += light.x * weight * ( ghost == 0 ? 1.0f : 0.8f );
			g += light.y * weight * 0.9f;
			b += light.z * weight * ( ghost == 2 ? 1.0f : 0.8f );
		}
	}
	output[y * width + x] = (float4)( r, g, b, isfinite( base.w ) ? base.w : 0.0f );
}
