#include "sys/platform.h"

#ifdef HAVE_PATHTRACER

#include "framework/Console.h"
#include "framework/Session.h"
#include "renderer/tr_local.h"
#include "renderer_pt/pt_render.h"
#include "renderer_pt/pt_benchmark.h"

extern idCVar r_ptDraw;
extern idCVar r_ptMode;
extern idCVar r_ptAccumulation;
extern idCVar r_ptSamplesPerPixel;

static struct {
	bool active = false;
	bool ready = false;
	bool rendered = false;
	bool savedDraw = false, savedAccumulation = false;
	int savedMode = 0;
	int samples = 0, batch = 0, batches = 0;
	int width = 0, height = 0;
	int frameBatches = 0;
	int imageFormat = 0;
	double start = 0, frameStart = 0, frameGpu = 0, frameRender = 0, screenStart = 0;
	idStr demo;
	idStr imageDirectory;
	ptBenchmarkStatistics_t stats;
} benchmark;

bool R_PT_BenchmarkActive() { return benchmark.active; }
int R_PT_BenchmarkTargetFrames() { return benchmark.active ? benchmark.batches : 0; }
int R_PT_BenchmarkBatch() { return benchmark.batch; }

void R_PT_EndBenchmark( bool completed ) {
	if ( !benchmark.active ) return;
	if ( benchmark.ready ) {
		benchmark.stats.AddFrame( Sys_MillisecondsPrecise() - benchmark.frameStart, benchmark.frameGpu,
			benchmark.frameRender, benchmark.frameBatches );
		benchmark.ready = false;
	}
	benchmark.active = false;
	const double seconds = ( Sys_MillisecondsPrecise() - benchmark.start ) * 0.001;
	const size_t frames = benchmark.stats.frameMilliseconds.size();
	common->Printf( "\n=== Path Tracer Benchmark %s ===\n", completed ? "Complete" : "Aborted (partial results)" );
	common->Printf( "  Demo: %s\n  Completed demo frames: %zu\n  Samples/pixel/frame: %d (batch %d)\n",
		benchmark.demo.c_str(), frames, benchmark.samples, benchmark.batch );
	common->Printf( "  Trace resolution: %dx%d\n  Accumulation dispatches: %d\n",
		benchmark.width, benchmark.height, benchmark.stats.dispatches );
	common->Printf( "  Measured wall time: %.3f s (excludes initial demo loading)\n", seconds );
	common->Printf( "  Images saved: %d in %s/\n  Image encoding/writing total: %.3f s\n",
		benchmark.stats.imagesWritten, benchmark.imageDirectory.c_str(), benchmark.stats.imageMilliseconds * 0.001 );
	if ( frames ) {
		const double total = benchmark.stats.TotalMilliseconds();
		common->Printf( "  Frame time ms: mean %.3f, min %.3f, median %.3f, p95 %.3f, p99 %.3f, max %.3f\n",
			total / frames, benchmark.stats.Percentile( 0 ), benchmark.stats.Percentile( 0.5 ),
			benchmark.stats.Percentile( 0.95 ), benchmark.stats.Percentile( 0.99 ), benchmark.stats.Percentile( 1 ) );
		common->Printf( "  Effective playback: %.3f fps; recorded duration %.3f s at 30 Hz\n",
			seconds > 0 ? frames / seconds : 0, frames / 30.0 );
		common->Printf( "  GPU render total: %.3f s; mean %.3f ms/demo frame\n",
			benchmark.stats.gpuMilliseconds * 0.001, benchmark.stats.gpuMilliseconds / frames );
		common->Printf( "  Screen rendering total: %.3f s; mean %.3f ms/demo frame (includes readback/presentation)\n",
			benchmark.stats.renderMilliseconds * 0.001, benchmark.stats.renderMilliseconds / frames );
	}
	common->Printf( "  Wall/frame times include playback pacing and CPU work. GPU times include tracing, denoising and tone mapping,\n"
		"  but exclude scene uploads/BVH construction and readback. Wall/frame times include image encoding/writing;\n"
		"  GPU/screen rendering times exclude image encoding/writing.\n"
		"====================================\n" );
	r_ptDraw.SetBool( benchmark.savedDraw );
	r_ptMode.SetInteger( benchmark.savedMode );
	r_ptAccumulation.SetBool( benchmark.savedAccumulation );
	if ( RB_PT_GetBackend() ) RB_PT_GetBackend()->ResetAccumulation();
}

void R_PT_BenchmarkNewFrame() {
	benchmark.ready = benchmark.rendered = false;
	benchmark.frameBatches = 0;
	benchmark.frameGpu = benchmark.frameRender = 0;
	benchmark.frameStart = Sys_MillisecondsPrecise();
	if ( RB_PT_GetBackend() ) RB_PT_GetBackend()->ResetAccumulation();
}

bool R_PT_BenchmarkAdvance() {
	if ( !benchmark.ready || Sys_MillisecondsPrecise() - benchmark.frameStart < 1000.0 / 30.0 ) return false;
	benchmark.stats.AddFrame( Sys_MillisecondsPrecise() - benchmark.frameStart, benchmark.frameGpu,
		benchmark.frameRender, benchmark.frameBatches );
	if ( benchmark.stats.frameMilliseconds.size() % 30 == 0 ) {
		common->Printf( "[ptbench] %zu demo frames completed at %d spp\n", benchmark.stats.frameMilliseconds.size(), benchmark.samples );
	}
	benchmark.ready = false;
	return true;
}

void R_PT_BenchmarkRendered( int accumulatedFrames, float gpuMilliseconds, int width, int height ) {
	if ( !benchmark.active || benchmark.ready ) return;
	if ( accumulatedFrames != benchmark.frameBatches + 1 ) {
		common->Warning( "PT benchmark accumulation was reset or advanced unexpectedly; aborting" );
		R_PT_EndBenchmark( false );
		return;
	}
	benchmark.width = width;
	benchmark.height = height;
	benchmark.frameGpu += gpuMilliseconds;
	benchmark.frameBatches++;
	benchmark.rendered = true;
}

// Called around the in-sequence screen update, after EndFrame has presented.
void R_PT_BenchmarkScreenBegin() {
	benchmark.screenStart = Sys_MillisecondsPrecise();
	benchmark.rendered = false;
}

void R_PT_BenchmarkScreenComplete() {
	if ( !benchmark.active || benchmark.ready ) return;
	if ( !benchmark.rendered ) {
		common->Warning( "PT benchmark did not render a main path-traced view; aborting" );
		R_PT_EndBenchmark( false );
		return;
	}
	benchmark.frameRender += Sys_MillisecondsPrecise() - benchmark.screenStart;
	if ( benchmark.frameBatches == benchmark.batches ) {
		const idStr image = va( "%s/frame_%06d_%dspp.%s", benchmark.imageDirectory.c_str(),
			benchmark.stats.imagesWritten + 1, benchmark.samples, PT_BenchmarkImageExtension( benchmark.imageFormat ) );
		const double start = Sys_MillisecondsPrecise();
		if ( !RB_PT_SaveImage( image, benchmark.imageFormat ) ) {
			fileSystem->RemoveFile( image );
			common->Warning( "PT benchmark could not save frame %d; aborting", benchmark.stats.imagesWritten + 1 );
			R_PT_EndBenchmark( false );
			return;
		}
		benchmark.stats.AddImage( Sys_MillisecondsPrecise() - start );
		benchmark.ready = true;
	}
}

static bool PT_BenchmarkImageDirectory( const idStr &demo, int samples, idStr &directory ) {
	idStr name;
	demo.ExtractFileName( name );
	name.StripFileExtension();
	const idStr parent = "screenshots/ptbench/" + name;
	idFileList *runs = fileSystem->ListFiles( parent, "/" );
	for ( int run = 1; run <= 99999; run++ ) {
		const idStr candidate = va( "%dspp_run_%05d", samples, run );
		if ( runs->GetList().FindIndex( candidate ) < 0 ) {
			directory = parent + "/" + candidate;
			fileSystem->FreeFileList( runs );
			return true;
		}
	}
	fileSystem->FreeFileList( runs );
	common->Warning( "No free PT benchmark image directory under '%s'", parent.c_str() );
	return false;
}

void R_PT_Benchmark_f( const idCmdArgs &args ) {
	int samples;
	if ( args.Argc() != 3 || !PT_ParseBenchmarkSamples( args.Argv( 2 ), samples ) ) {
		common->Warning( "Usage: r_ptBenchmark <render_demo_name> <spp 1..100000>" );
		return;
	}
	if ( RB_PT_IsCapturing() ) {
		common->Warning( "A path tracer capture/benchmark is active; use r_ptAbort first" );
		return;
	}
	if ( !RB_PT_IsSupported() ) {
		common->Warning( "Path tracer benchmark requires a usable r_ptBackend" );
		return;
	}
	idStr demo = args.Argv( 1 );
	demo.BackSlashesToSlashes();
	if ( demo.IsEmpty() || demo.Find( ".." ) >= 0 || demo[0] == '/' || demo.Find( ":" ) >= 0 ) {
		common->Warning( "Benchmark requires a relative render demo name inside demos/" );
		return;
	}
	if ( demo.Icmpn( "demos/", 6 ) ) demo = idStr( "demos/" ) + demo;
	demo.DefaultFileExtension( ".demo" );
	idStr imageDirectory;
	if ( !PT_BenchmarkImageDirectory( demo, samples, imageDirectory ) ) return;
	if ( !session->StartPTBenchmarkDemo( demo ) ) {
		common->Warning( "Could not start render demo '%s'; benchmark not started", demo.c_str() );
		return;
	}
	benchmark.stats = ptBenchmarkStatistics_t();
	benchmark.demo = demo;
	benchmark.imageDirectory = imageDirectory;
	benchmark.imageFormat = cvarSystem->GetCVarInteger( "r_screenshotFormat" );
	benchmark.samples = samples;
	benchmark.batch = PT_BenchmarkBatchSize( samples, r_ptSamplesPerPixel.GetInteger() );
	benchmark.batches = samples / benchmark.batch;
	benchmark.width = benchmark.height = 0;
	benchmark.savedDraw = r_ptDraw.GetBool();
	benchmark.savedMode = r_ptMode.GetInteger();
	benchmark.savedAccumulation = r_ptAccumulation.GetBool();
	r_ptDraw.SetBool( true );
	r_ptMode.SetInteger( PT_MODE_PATHTRACE );
	r_ptAccumulation.SetBool( true );
	benchmark.active = true;
	benchmark.start = Sys_MillisecondsPrecise();
	R_PT_BenchmarkNewFrame();
	console->Close();
	common->Printf( "\n[ptbench] %s: %d spp/frame, %d samples/dispatch, %d dispatches/frame\n"
		"[ptbench] Selected quality settings retained; progressive quality ramp disabled.\n"
		"[ptbench] Every recorded frame is saved after convergence; maximum 30 Hz playback, no skipping. r_ptAbort cancels.\n",
		demo.c_str(), samples, benchmark.batch, benchmark.batches );
	common->Printf( "[ptbench] Output: %s/frame_NNNNNN_%dspp.%s (trace resolution, no HUD)\n",
		benchmark.imageDirectory.c_str(), samples, PT_BenchmarkImageExtension( benchmark.imageFormat ) );
	common->Printf( "[ptbench] Backend %s, scale %.3f, depth %d, light samples %d, denoise %d, swap interval %d\n",
		cvarSystem->GetCVarString( "r_ptBackend" ), cvarSystem->GetCVarFloat( "r_ptRenderScale" ),
		cvarSystem->GetCVarInteger( "r_ptMaxDepth" ), cvarSystem->GetCVarInteger( "r_ptMaxLightSamples" ),
		cvarSystem->GetCVarBool( "r_ptDenoise" ), GLimp_GetSwapInterval() );
#ifdef PT_PHYSICAL
	common->Printf( "[ptbench] Rendering model: PT_PHYSICAL\n" );
#else
	common->Printf( "[ptbench] Rendering model: legacy path tracer\n" );
#endif
}

#endif
