/*
===============================================================================

	OpenCL C port of pt_cuda_lbvh.cu.

	CUB's DeviceRadixSort is replaced by a portable stable LSD radix sort
	(4 bit digits, one tile of PT_CL_SORT_WG keys per work-group). Both are
	stable sorts of the same (key, index) pairs, so the permutation and with
	it the BVH topology are identical to the CUDA backend.

===============================================================================
*/

#ifndef PT_CL_SORT_WG
#define PT_CL_SORT_WG	256
#endif

/*
================================================================================
Morton Code Utilities
================================================================================
*/

PT_INLINE uint expandBits(uint v) {
	// spread 10 bits across 30 bits with 2 gaps between each bit
	v = (v * 0x00010001u) & 0xFF0000FFu;
	v = (v * 0x00000101u) & 0x0F00F00Fu;
	v = (v * 0x00000011u) & 0xC30C30C3u;
	v = (v * 0x00000005u) & 0x49249249u;
	return v;
}

PT_INLINE uint morton3D(float x, float y, float z) {
	// quantize to 10-bit integers [0, 1023]
	uint ix = (uint)fmin(fmax(x * 1024.0f, 0.0f), 1023.0f);
	uint iy = (uint)fmin(fmax(y * 1024.0f, 0.0f), 1023.0f);
	uint iz = (uint)fmin(fmax(z * 1024.0f, 0.0f), 1023.0f);
	return (expandBits(iz) << 2) | (expandBits(iy) << 1) | expandBits(ix);
}

/*
================================================================================
Kernel 1: Compute Morton Codes
================================================================================
*/

__kernel void ComputeMortonCodesKernel(
	__global const ptTriangle_t* triangles,
	int numTriangles,
	__global uint* mortonCodes,
	__global int* indices,
	float sceneMinX, float sceneMinY, float sceneMinZ,
	float sceneInvExtX, float sceneInvExtY, float sceneInvExtZ
) {
	int idx = get_global_id(0);
	if (idx >= numTriangles) return;

	__global const ptTriangle_t* tri = &triangles[idx];

	// Centroid from precomputed bounds
	float cx = (tri->bounds[0] + tri->bounds[3]) * 0.5f;
	float cy = (tri->bounds[1] + tri->bounds[4]) * 0.5f;
	float cz = (tri->bounds[2] + tri->bounds[5]) * 0.5f;

	// Normalize to [0, 1]
	float nx = (cx - sceneMinX) * sceneInvExtX;
	float ny = (cy - sceneMinY) * sceneInvExtY;
	float nz = (cz - sceneMinZ) * sceneInvExtZ;

	mortonCodes[idx] = morton3D(nx, ny, nz);
	indices[idx] = idx;
}

/*
================================================================================
Stable LSD radix sort

PT_RadixHistogram	per tile digit counts, digit major: hist[digit * numTiles + tile]
PT_RadixScan		exclusive prefix sum of hist, in place, one work-group
PT_RadixScatter		stable local sort of each tile by digit, then scatter
================================================================================
*/

PT_INLINE uint PT_RadixDigit(uint key, int shift) {
	return (key >> shift) & (PT_CL_RADIX_DIGITS - 1);
}

__kernel __attribute__((reqd_work_group_size(PT_CL_SORT_WG, 1, 1)))
void PT_RadixHistogram(__global const uint* keys, int count, int shift, __global uint* hist, int numTiles) {
	__local uint counts[PT_CL_RADIX_DIGITS];
	const int lid = get_local_id(0);
	const int tile = get_group_id(0);
	if (lid < PT_CL_RADIX_DIGITS) counts[lid] = 0;
	barrier(CLK_LOCAL_MEM_FENCE);
	const int index = tile * PT_CL_SORT_WG + lid;
	if (index < count) atomic_inc(&counts[PT_RadixDigit(keys[index], shift)]);
	barrier(CLK_LOCAL_MEM_FENCE);
	if (lid < PT_CL_RADIX_DIGITS) hist[lid * numTiles + tile] = counts[lid];
}

__kernel __attribute__((reqd_work_group_size(PT_CL_SORT_WG, 1, 1)))
void PT_RadixScan(__global uint* data, int count) {
	__local uint sums[PT_CL_SORT_WG];
	const int lid = get_local_id(0);
	const int chunk = (count + PT_CL_SORT_WG - 1) / PT_CL_SORT_WG;
	const int begin = min(count, lid * chunk);
	const int end = min(count, begin + chunk);
	uint total = 0;
	for (int i = begin; i < end; i++) total += data[i];
	sums[lid] = total;
	barrier(CLK_LOCAL_MEM_FENCE);
	// Hillis-Steele inclusive scan of the chunk totals
	for (int offset = 1; offset < PT_CL_SORT_WG; offset <<= 1) {
		const uint add = lid >= offset ? sums[lid - offset] : 0;
		barrier(CLK_LOCAL_MEM_FENCE);
		sums[lid] += add;
		barrier(CLK_LOCAL_MEM_FENCE);
	}
	uint running = sums[lid] - total;
	for (int i = begin; i < end; i++) {
		const uint value = data[i];
		data[i] = running;
		running += value;
	}
}

// exclusive scan of one flag per work-item, returns the total through *sum
PT_INLINE uint PT_LocalExclusiveScan(__local uint* scratch, uint value, uint* sum) {
	const int lid = get_local_id(0);
	scratch[lid] = value;
	barrier(CLK_LOCAL_MEM_FENCE);
	for (int offset = 1; offset < PT_CL_SORT_WG; offset <<= 1) {
		const uint add = lid >= offset ? scratch[lid - offset] : 0;
		barrier(CLK_LOCAL_MEM_FENCE);
		scratch[lid] += add;
		barrier(CLK_LOCAL_MEM_FENCE);
	}
	const uint inclusive = scratch[lid];
	*sum = scratch[PT_CL_SORT_WG - 1];
	barrier(CLK_LOCAL_MEM_FENCE);
	return inclusive - value;
}

__kernel __attribute__((reqd_work_group_size(PT_CL_SORT_WG, 1, 1)))
void PT_RadixScatter(
	__global const uint* keysIn, __global const int* valuesIn, __global uint* keysOut, __global int* valuesOut,
	int count, int shift, __global const uint* offsets, int numTiles
) {
	__local uint scan[PT_CL_SORT_WG];
	__local uint localKeys[PT_CL_SORT_WG];
	__local int localValues[PT_CL_SORT_WG];
	__local uint localDigits[PT_CL_SORT_WG];
	__local int digitStart[PT_CL_RADIX_DIGITS];

	const int lid = get_local_id(0);
	const int tile = get_group_id(0);
	const int index = tile * PT_CL_SORT_WG + lid;
	const bool valid = index < count;

	uint key = valid ? keysIn[index] : 0xFFFFFFFFu;
	int value = valid ? valuesIn[index] : -1;
	// out of range slots sort last (they are the tile's last items), so they
	// never change the rank of a valid key
	uint digit = valid ? PT_RadixDigit(key, shift) : (uint)(PT_CL_RADIX_DIGITS - 1);

	// stable sort of the tile by digit: four 1-bit splits, LSB first
	for (int bit = 0; bit < PT_CL_RADIX_BITS; bit++) {
		const uint isZero = ((digit >> bit) & 1u) ? 0u : 1u;
		uint zeros;
		const uint zeroRank = PT_LocalExclusiveScan(scan, isZero, &zeros);
		const uint target = isZero ? zeroRank : zeros + (uint)lid - zeroRank;
		localKeys[target] = key;
		localValues[target] = value;
		localDigits[target] = digit;
		barrier(CLK_LOCAL_MEM_FENCE);
		key = localKeys[lid];
		value = localValues[lid];
		digit = localDigits[lid];
		barrier(CLK_LOCAL_MEM_FENCE);
	}

	// first position of every digit in the sorted tile
	if (lid == 0 || localDigits[lid - 1] != digit) digitStart[digit] = lid;
	barrier(CLK_LOCAL_MEM_FENCE);

	const int localCount = min(PT_CL_SORT_WG, count - tile * PT_CL_SORT_WG);
	if (lid < localCount) {
		const uint position = offsets[digit * numTiles + tile] + (uint)(lid - digitStart[digit]);
		keysOut[position] = key;
		valuesOut[position] = value;
	}
}

/*
================================================================================
Delta Function
================================================================================
*/

PT_INLINE int lbvhDelta(
	__global const uint* sortedCodes,
	int numPrimitives,
	int i,
	int j
) {
	if (j < 0 || j >= numPrimitives) return -1;
	uint ci = sortedCodes[i];
	uint cj = sortedCodes[j];
	if (ci == cj) {
		// tiebreaker: use index XOR for duplicate Morton codes
		return 32 + clz(i ^ j);
	}
	return (int)clz(ci ^ cj);
}

/*
================================================================================
Kernel 2: Build Radix Tree
================================================================================
*/

__kernel void BuildRadixTreeKernel(
	__global const uint* sortedMortonCodes,
	int numPrimitives,
	__global ptBVHNode_t* nodes,
	__global int* parents
) {
	int i = get_global_id(0);
	if (i >= numPrimitives - 1) return;

	int N = numPrimitives;

	// determine direction of the range (+1 or -1)
	int deltaRight = lbvhDelta(sortedMortonCodes, N, i, i + 1);
	int deltaLeft  = lbvhDelta(sortedMortonCodes, N, i, i - 1);
	int d = (deltaRight >= deltaLeft) ? 1 : -1;

	// compute upper bound for range length
	int deltaMin = lbvhDelta(sortedMortonCodes, N, i, i - d);
	// lmax must stay a power of two for the binary search below: clamping it to
	// N left the root unable to reach the last primitives whenever N is not a
	// power of two, dropping them from the tree. No bound is needed, lbvhDelta
	// returns -1 outside [0, N), which always ends the loop.
	int lmax = 2;
	while (lbvhDelta(sortedMortonCodes, N, i, i + lmax * d) > deltaMin) {
		lmax <<= 1;
	}

	// binary search for the actual range length
	int l = 0;
	for (int t = lmax >> 1; t >= 1; t >>= 1) {
		if (lbvhDelta(sortedMortonCodes, N, i, i + (l + t) * d) > deltaMin) {
			l += t;
		}
	}
	int j = i + l * d;  // other end of the range

	// find the split position
	int deltaNode = lbvhDelta(sortedMortonCodes, N, i, j);
	int s = 0;
	int maxLen = l;
	// binary search within the range for the highest split point
	for (int div = 2; ; div <<= 1) {
		int t = (maxLen + div - 1) / div;
		if (t <= 0) break;
		if (lbvhDelta(sortedMortonCodes, N, i, i + (s + t) * d) > deltaNode) {
			s += t;
		}
		if (t == 1) break;
	}
	int gamma = i + s * d + min(d, 0);

	// assign children
	int first = min(i, j);
	int last  = max(i, j);

	// left child: covers [first, gamma]
	int leftChild;
	if (first == gamma) {
		leftChild = N - 1 + gamma;       // single leaf -> leaf node
	} else {
		leftChild = gamma;               // multiple leaves -> internal node
	}

	// right child: covers [gamma+1, last]
	int rightChild;
	if (last == gamma + 1) {
		rightChild = N - 1 + gamma + 1;  // single leaf -> leaf node
	} else {
		rightChild = gamma + 1;          // multiple leaves -> internal node
	}

	// store internal node
	nodes[i].leftChild  = leftChild;
	nodes[i].rightChild = rightChild;
	nodes[i].primitiveCount = 0;  // internal node - no direct primitives
	nodes[i].firstPrimitive = 0;

	// store parent pointers (children -> this node)
	parents[leftChild]  = i;
	parents[rightChild] = i;
}

/*
================================================================================
Kernel 3: Initialize Leaf Nodes
================================================================================
*/

__kernel void InitLeafNodesKernel(
	__global const ptTriangle_t* triangles,
	__global const int* sortedTriIndices,
	__global ptBVHNode_t* nodes,
	int numPrimitives
) {
	int i = get_global_id(0);
	if (i >= numPrimitives) return;

	int nodeIdx = numPrimitives - 1 + i;
	int triIdx  = sortedTriIndices[i];

	// mark as leaf
	nodes[nodeIdx].leftChild  = -1;
	nodes[nodeIdx].rightChild = -1;
	nodes[nodeIdx].firstPrimitive = i;       // offset into triIndices
	nodes[nodeIdx].primitiveCount = 1;

	// copy bounds with epsilon padding
	const float EPSILON = 0.001f;
	__global const ptTriangle_t* tri = &triangles[triIdx];
	nodes[nodeIdx].bounds[0] = tri->bounds[0] - EPSILON;
	nodes[nodeIdx].bounds[1] = tri->bounds[1] - EPSILON;
	nodes[nodeIdx].bounds[2] = tri->bounds[2] - EPSILON;
	nodes[nodeIdx].bounds[3] = tri->bounds[3] + EPSILON;
	nodes[nodeIdx].bounds[4] = tri->bounds[4] + EPSILON;
	nodes[nodeIdx].bounds[5] = tri->bounds[5] + EPSILON;
}

__kernel void BuildEscapeLinksKernel(__global ptBVHNode_t* nodes, __global const int* parents, int numNodes) {
	const int idx = get_global_id(0);
	if (idx >= numNodes) return;

	int child = idx;
	int parent = parents[child];
	int escape = -1;
	while (parent >= 0) {
		if (nodes[parent].leftChild == child) {
			escape = nodes[parent].rightChild;
			break;
		}
		child = parent;
		parent = parents[child];
	}
	nodes[idx].escapeIndex = escape;
}

/*
================================================================================
Kernel 4: Bottom-Up AABB Propagation

CUDA publishes a subtree's bounds with __threadfence before the arrival
counter and reads the sibling with __ldcg (from L2). OpenCL 1.2 has no
cross work-group fence (mem_fence only orders the issuing work-item) and
volatile stores are not guaranteed to be visible to other compute units,
so internal node bounds are written and read with atomics, which every
implementation performs at the device-wide coherence point. Leaf bounds
come from InitLeafNodesKernel, a previous launch, and are plain loads.
================================================================================
*/

PT_INLINE float PT_LoadBound(__global ptBVHNode_t* nodes, int node, int k, int numInternal) {
	__global float* bound = &nodes[node].bounds[k];
	if (node >= numInternal) return *bound;
	return as_float(atomic_or((volatile __global int*)bound, 0));
}

PT_INLINE void PT_StoreBound(__global ptBVHNode_t* nodes, int node, int k, float value) {
	atomic_xchg((volatile __global int*)&nodes[node].bounds[k], as_int(value));
}

__kernel void ComputeNodeBoundsKernel(
	__global ptBVHNode_t* nodes,
	__global const int* parents,
	__global int* atomicCounters,
	int numPrimitives
) {
	int i = get_global_id(0);
	if (i >= numPrimitives) return;
	const int numInternal = numPrimitives - 1;

	// start at leaf node
	int parent = parents[numPrimitives - 1 + i];

	while (parent >= 0) {
		// first child to arrive increments 0->1 and exits,
		// the second one (1->2) merges both children; the atomic bound
		// stores of the previous iteration happen before this increment
		if (atomic_inc(&atomicCounters[parent]) == 0) {
			return;
		}

		const int left  = nodes[parent].leftChild;
		const int right = nodes[parent].rightChild;

		float lb[6], rb[6];
		for (int k = 0; k < 6; k++) {
			lb[k] = PT_LoadBound(nodes, left, k, numInternal);
			rb[k] = PT_LoadBound(nodes, right, k, numInternal);
		}

		PT_StoreBound(nodes, parent, 0, fmin(lb[0], rb[0]));
		PT_StoreBound(nodes, parent, 1, fmin(lb[1], rb[1]));
		PT_StoreBound(nodes, parent, 2, fmin(lb[2], rb[2]));
		PT_StoreBound(nodes, parent, 3, fmax(lb[3], rb[3]));
		PT_StoreBound(nodes, parent, 4, fmax(lb[4], rb[4]));
		PT_StoreBound(nodes, parent, 5, fmax(lb[5], rb[5]));

		parent = parents[parent];
	}
}

/*
================================================================================
Kernel 5: Single-Triangle Fallback
================================================================================
*/

__kernel void InitSingleLeafKernel(
	__global const ptTriangle_t* triangles,
	__global ptBVHNode_t* nodes,
	__global int* triIndices
) {
	if (get_global_id(0) != 0) return;

	triIndices[0] = 0;

	nodes[0].leftChild  = -1;
	nodes[0].rightChild = -1;
	nodes[0].firstPrimitive = 0;
	nodes[0].primitiveCount = 1;
	nodes[0].escapeIndex = -1;

	const float EPSILON = 0.001f;
	nodes[0].bounds[0] = triangles[0].bounds[0] - EPSILON;
	nodes[0].bounds[1] = triangles[0].bounds[1] - EPSILON;
	nodes[0].bounds[2] = triangles[0].bounds[2] - EPSILON;
	nodes[0].bounds[3] = triangles[0].bounds[3] + EPSILON;
	nodes[0].bounds[4] = triangles[0].bounds[4] + EPSILON;
	nodes[0].bounds[5] = triangles[0].bounds[5] + EPSILON;
}

/*
================================================================================
PT_LayoutKernel

Writes the device-side sizes of every shared struct so the host can refuse
to run with a mismatched layout.
================================================================================
*/
__kernel void PT_LayoutKernel(__global int* sizes) {
	if (get_global_id(0) != 0) return;
	sizes[0] = (int)sizeof(ptVertex_t);
	sizes[1] = (int)sizeof(ptTriangle_t);
	sizes[2] = (int)sizeof(ptMaterial_t);
	sizes[3] = (int)sizeof(ptLight_t);
	sizes[4] = (int)sizeof(ptBVHNode_t);
	sizes[5] = (int)sizeof(ptRenderParams_t);
	sizes[6] = (int)sizeof(ptClDenoiseGuide_t);
	sizes[7] = (int)sizeof(ptClTexture_t);
}
