// OpenCL LBVH regression test.
//
// The CUDA backend sorts Morton codes with CUB's stable radix sort. The OpenCL
// backend replaces it with its own stable LSD radix sort, so this test checks
// that the device permutation equals a host std::stable_sort of the same
// (code, index) pairs, that the radix tree topology equals a host rebuild of
// the CUDA algorithm, and that the finished BVH is well formed: every node's
// bounds contain its children, escape links follow a pre-order walk and every
// triangle is reachable exactly once.
#include "renderer_pt/tests/pt_opencl_test_common.h"

#include <algorithm>

/*
========================
Fail
========================
*/
static void Fail( const char *label, int index ) {
	std::fprintf( stderr, "FAIL: %s (at %d)\n", label, index );
	std::exit( 1 );
}

/*
========================
ExpandBits
========================
*/
static unsigned int ExpandBits( unsigned int v ) {
	v = ( v * 0x00010001u ) & 0xFF0000FFu;
	v = ( v * 0x00000101u ) & 0x0F00F00Fu;
	v = ( v * 0x00000011u ) & 0xC30C30C3u;
	v = ( v * 0x00000005u ) & 0x49249249u;
	return v;
}

/*
========================
Morton
========================
*/
static unsigned int Morton( float x, float y, float z ) {
	const unsigned int ix = (unsigned int)std::min( std::max( x * 1024.0f, 0.0f ), 1023.0f );
	const unsigned int iy = (unsigned int)std::min( std::max( y * 1024.0f, 0.0f ), 1023.0f );
	const unsigned int iz = (unsigned int)std::min( std::max( z * 1024.0f, 0.0f ), 1023.0f );
	return ( ExpandBits( iz ) << 2 ) | ( ExpandBits( iy ) << 1 ) | ExpandBits( ix );
}

/*
========================
Clz
========================
*/
static int Clz( unsigned int v ) {
	int n = 0;
	for ( unsigned int bit = 0x80000000u; bit && !( v & bit ); bit >>= 1 ) n++;
	return n;
}

/*
========================
Delta
========================
*/
static int Delta( const std::vector<unsigned int> &codes, int i, int j ) {
	const int n = (int)codes.size();
	if ( j < 0 || j >= n ) return -1;
	if ( codes[i] == codes[j] ) return 32 + Clz( (unsigned int)( i ^ j ) );
	return Clz( codes[i] ^ codes[j] );
}

/*
========================
HostRadixNode

Host copy of BuildRadixTreeKernel (Karras 2012)
========================
*/
static void HostRadixNode( const std::vector<unsigned int> &codes, int i, int &leftChild, int &rightChild ) {
	const int N = (int)codes.size();
	const int d = Delta( codes, i, i + 1 ) >= Delta( codes, i, i - 1 ) ? 1 : -1;
	const int deltaMin = Delta( codes, i, i - d );
	int lmax = 2;
	while ( Delta( codes, i, i + lmax * d ) > deltaMin ) lmax <<= 1;	// no clamp, see pt_cl_lbvh.cl
	int l = 0;
	for ( int t = lmax >> 1; t >= 1; t >>= 1 ) {
		if ( Delta( codes, i, i + ( l + t ) * d ) > deltaMin ) l += t;
	}
	const int j = i + l * d;
	const int deltaNode = Delta( codes, i, j );
	int s = 0;
	for ( int div = 2; ; div <<= 1 ) {
		const int t = ( l + div - 1 ) / div;
		if ( t <= 0 ) break;
		if ( Delta( codes, i, i + ( s + t ) * d ) > deltaNode ) s += t;
		if ( t == 1 ) break;
	}
	const int gamma = i + s * d + std::min( d, 0 );
	leftChild = std::min( i, j ) == gamma ? N - 1 + gamma : gamma;
	rightChild = std::max( i, j ) == gamma + 1 ? N - 1 + gamma + 1 : gamma + 1;
}

static unsigned int g_random = 12345u;

/*
========================
Random
========================
*/
static float Random() {
	g_random = g_random * 1664525u + 1013904223u;
	return ( g_random >> 8 ) * ( 1.0f / 16777216.0f );
}

/*
========================
CheckScene
========================
*/
static void CheckScene( int n, bool clustered ) {
	std::vector<ptTriangle_t> triangles( n );
	float bounds[6] = { 1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f };
	for ( int i = 0; i < n; i++ ) {
		ptTriangle_t &tri = triangles[i];
		std::memset( &tri, 0, sizeof( tri ) );
		float center[3];
		for ( int c = 0; c < 3; c++ ) {
			// clustered scenes produce many duplicate Morton codes, exercising the stable tie order
			center[c] = clustered ? std::floor( Random() * 4.0f ) * 100.0f : Random() * 1000.0f - 500.0f;
		}
		for ( int c = 0; c < 3; c++ ) {
			const float extent = 0.5f + Random() * 4.0f;
			tri.bounds[c] = center[c] - extent;
			tri.bounds[c + 3] = center[c] + extent;
			bounds[c] = std::min( bounds[c], tri.bounds[c] );
			bounds[c + 3] = std::max( bounds[c + 3], tri.bounds[c + 3] );
		}
	}

	const size_t nodes = 2 * (size_t)n - 1;
	cl_mem d_triangles = Allocate<ptTriangle_t>( n );
	cl_mem d_nodes = Allocate<ptBVHNode_t>( nodes );
	cl_mem d_indices = Allocate<int>( n );
	cl_mem d_morton = Allocate<unsigned int>( n );
	cl_mem d_mortonAlt = Allocate<unsigned int>( n );
	cl_mem d_sorted = Allocate<int>( n );
	cl_mem d_sortTemp = Allocate<unsigned char>( std::max<size_t>( PT_CL_LBVHSortTempBytes( clTest.program, n ), 4 ) );
	cl_mem d_parents = Allocate<int>( 2 * n );
	cl_mem d_atomics = Allocate<int>( n );
	Upload( d_triangles, triangles.data(), n * sizeof( ptTriangle_t ) );
	CL_CHECK( PT_CL_LaunchBuildLBVH( clTest.program, clTest.queue, d_triangles, n, d_nodes, d_indices, d_morton, d_mortonAlt,
		d_sorted, d_sortTemp, d_parents, d_atomics, bounds ) );
	CL_CHECK( clFinish( clTest.queue ) );

	std::vector<ptBVHNode_t> bvh( nodes );
	std::vector<int> indices( n );
	Download( bvh.data(), d_nodes, nodes * sizeof( ptBVHNode_t ) );
	Download( indices.data(), d_indices, n * sizeof( int ) );

	if ( n > 1 ) {
		// host reference: same codes, std::stable_sort == CUB SortPairs
		float invExt[3];
		for ( int c = 0; c < 3; c++ ) {
			const float ext = bounds[c + 3] - bounds[c];
			invExt[c] = ext > 1e-6f ? 1.0f / ext : 0.0f;
		}
		std::vector<std::pair<unsigned int, int> > pairs( n );
		for ( int i = 0; i < n; i++ ) {
			const ptTriangle_t &tri = triangles[i];
			const float cx = ( tri.bounds[0] + tri.bounds[3] ) * 0.5f;
			const float cy = ( tri.bounds[1] + tri.bounds[4] ) * 0.5f;
			const float cz = ( tri.bounds[2] + tri.bounds[5] ) * 0.5f;
			pairs[i] = std::make_pair( Morton( ( cx - bounds[0] ) * invExt[0], ( cy - bounds[1] ) * invExt[1], ( cz - bounds[2] ) * invExt[2] ), i );
		}
		std::stable_sort( pairs.begin(), pairs.end(),
			[]( const std::pair<unsigned int, int> &a, const std::pair<unsigned int, int> &b ) { return a.first < b.first; } );
		std::vector<unsigned int> sortedCodes( n ), deviceCodes( n );
		Download( deviceCodes.data(), d_morton, n * sizeof( unsigned int ) );
		for ( int i = 0; i < n; i++ ) {
			sortedCodes[i] = pairs[i].first;
			if ( deviceCodes[i] != pairs[i].first ) Fail( "sorted Morton codes match host", i );
			if ( indices[i] != pairs[i].second ) Fail( "stable permutation matches std::stable_sort", i );
		}
		for ( int i = 0; i < n - 1; i++ ) {
			int left, right;
			HostRadixNode( sortedCodes, i, left, right );
			if ( bvh[i].leftChild != left || bvh[i].rightChild != right ) Fail( "radix tree topology matches host", i );
		}
	}

	// structural checks: pre-order escape walk visits each leaf once, bounds nest
	std::vector<int> seen( n, 0 );
	int visited = 0;
	for ( int node = 0; node >= 0; visited++ ) {
		if ( visited > (int)nodes ) Fail( "escape walk terminates", node );
		const ptBVHNode_t &b = bvh[node];
		if ( b.leftChild == -1 ) {
			if ( b.primitiveCount != 1 ) Fail( "leaf primitive count", node );
			const int tri = indices[b.firstPrimitive];
			seen[tri]++;
			for ( int c = 0; c < 3; c++ ) {
				if ( b.bounds[c] > triangles[tri].bounds[c] || b.bounds[c + 3] < triangles[tri].bounds[c + 3] ) Fail( "leaf bounds", node );
			}
			node = b.escapeIndex;
		} else {
			for ( int child : { b.leftChild, b.rightChild } ) {
				for ( int c = 0; c < 3; c++ ) {
					if ( bvh[child].bounds[c] < b.bounds[c] || bvh[child].bounds[c + 3] > b.bounds[c + 3] ) Fail( "child bounds nest", node );
				}
			}
			if ( bvh[b.leftChild].escapeIndex != b.rightChild ) Fail( "left escape is right sibling", node );
			if ( bvh[b.rightChild].escapeIndex != b.escapeIndex ) Fail( "right escape inherits parent", node );
			node = b.leftChild;
		}
	}
	if ( visited != (int)nodes ) {
		for ( size_t i = 0; i < nodes; i++ ) std::fprintf( stderr, "node %zu: l %d r %d esc %d first %d count %d\n", i, bvh[i].leftChild, bvh[i].rightChild, bvh[i].escapeIndex, bvh[i].firstPrimitive, bvh[i].primitiveCount );
		Fail( "every node visited", visited );
	}
	for ( int i = 0; i < n; i++ ) if ( seen[i] != 1 ) Fail( "triangle reachable exactly once", i );

	Free( d_triangles ); Free( d_nodes ); Free( d_indices ); Free( d_morton ); Free( d_mortonAlt );
	Free( d_sorted ); Free( d_sortTemp ); Free( d_parents ); Free( d_atomics );
	std::printf( "  %d triangles%s: ok\n", n, clustered ? " (clustered)" : "" );
}

/*
========================
main
========================
*/
int main() {
	const int init = PT_CL_TestInit( "OpenCL LBVH test", NULL );
	if ( init ) return init;
	const int sizes[] = { 1, 2, 3, 255, 256, 257, 1000, 4097, 100000, 1000003 };
	for ( int n : sizes ) CheckScene( n, false );
	for ( int n : { 2, 513, 70000 } ) CheckScene( n, true );
	std::puts( "OpenCL LBVH checks passed" );
	return 0;
}
