#include "sys/platform.h"

#include "framework/Console.h"
#include "framework/Session.h"
#include "renderer/tr_local.h"
#include "renderer/qgl.h"
#include "renderer_pt/pt_render.h"
#include "renderer_pt/pt_benchmark.h"
#include "renderer_pt/pt_capture_report.h"

#ifdef HAVE_PATHTRACER
extern idCVar r_ptDraw;
extern idCVar r_ptMode;
extern idCVar r_ptAccumulation;
extern idCVar r_ptSamplesPerPixel;
#endif

static struct {
	bool active = false;
	bool openGL = false;
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
	idStr lastImage;
	double lastImageMilliseconds = 0;
	ptBenchmarkStatistics_t stats;
} benchmark;

bool R_BenchmarkActive() { return benchmark.active; }
bool R_GL_BenchmarkActive() { return benchmark.active && benchmark.openGL; }
#ifdef HAVE_PATHTRACER
bool R_PT_BenchmarkActive() { return benchmark.active && !benchmark.openGL; }
int R_PT_BenchmarkTargetFrames() { return benchmark.active ? benchmark.batches : 0; }
int R_PT_BenchmarkBatch() { return benchmark.batch; }
#endif

void R_EndBenchmark( bool completed ) {
	if ( !benchmark.active ) return;
	if ( benchmark.ready ) {
		if ( !R_PT_ReportFrameEnd( benchmark.lastImage, benchmark.width, benchmark.height,
			benchmark.lastImageMilliseconds, benchmark.frameRender ) ) completed = false;
		benchmark.stats.AddFrame( Sys_MillisecondsPrecise() - benchmark.frameStart, benchmark.frameGpu,
			benchmark.frameRender, benchmark.frameBatches );
		benchmark.ready = false;
	}
	benchmark.active = false;
	R_PT_ReportEnd( completed, completed ? "" : "benchmark_aborted" );
	const double seconds = ( Sys_MillisecondsPrecise() - benchmark.start ) * 0.001;
	const int frames = benchmark.stats.frameMilliseconds.Num();
	common->Printf( "\n=== %s Benchmark %s ===\n", benchmark.openGL ? "OpenGL" : "Path Tracer", completed ? "Complete" : "Aborted (partial results)" );
	common->Printf( "  Demo: %s\n  Completed demo frames: %zu\n", benchmark.demo.c_str(), frames );
	if ( !benchmark.openGL ) common->Printf( "  Samples/pixel/frame: %d (batch %d)\n", benchmark.samples, benchmark.batch );
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
		if ( !benchmark.openGL ) common->Printf( "  GPU render total: %.3f s; mean %.3f ms/demo frame\n",
			benchmark.stats.gpuMilliseconds * 0.001, benchmark.stats.gpuMilliseconds / frames );
		common->Printf( "  Screen rendering total: %.3f s; mean %.3f ms/demo frame (includes readback/presentation)\n",
			benchmark.stats.renderMilliseconds * 0.001, benchmark.stats.renderMilliseconds / frames );
	}
	if ( benchmark.openGL ) common->Printf( "  OpenGL timings are CPU wall times including readback, not isolated GPU timings.\n"
		"  Frame times include image writes and playback pacing; screen times exclude image encoding/writing.\n" );
	else common->Printf( "  Wall/frame times include playback pacing and CPU work. GPU times include tracing, denoising and tone mapping,\n"
		"  but exclude scene uploads/BVH construction and readback. Wall/frame times include image encoding/writing;\n"
		"  GPU/screen rendering times exclude image encoding/writing.\n"
		"====================================\n" );
#ifdef HAVE_PATHTRACER
	r_ptDraw.SetBool( benchmark.savedDraw );
	r_ptMode.SetInteger( benchmark.savedMode );
	r_ptAccumulation.SetBool( benchmark.savedAccumulation );
	if ( !benchmark.openGL && RB_PT_GetBackend() ) RB_PT_GetBackend()->ResetAccumulation();
#endif
}

void R_BenchmarkNewFrame() {
	benchmark.ready = benchmark.rendered = false;
	benchmark.frameBatches = 0;
	benchmark.frameGpu = benchmark.frameRender = 0;
	benchmark.frameStart = Sys_MillisecondsPrecise();
	R_PT_ReportFrameBegin( benchmark.stats.frameMilliseconds.Num() + 1,
		benchmark.stats.frameMilliseconds.Num() / 30.0, benchmark.openGL ? "opengl" : "pathtrace" );
#ifdef HAVE_PATHTRACER
	if ( !benchmark.openGL && RB_PT_GetBackend() ) RB_PT_GetBackend()->ResetAccumulation();
#endif
}

bool R_BenchmarkAdvance() {
	if ( !PT_BenchmarkCanAdvance( benchmark.ready, Sys_MillisecondsPrecise() - benchmark.frameStart ) ) return false;
	if ( !R_PT_ReportFrameEnd( benchmark.lastImage, benchmark.width, benchmark.height,
		benchmark.lastImageMilliseconds, benchmark.frameRender ) ) {
		benchmark.ready = false;
		R_EndBenchmark( false );
		session->Stop();
		session->StartMenu();
		return false;
	}
	benchmark.stats.AddFrame( Sys_MillisecondsPrecise() - benchmark.frameStart, benchmark.frameGpu,
		benchmark.frameRender, benchmark.frameBatches );
	if ( benchmark.stats.frameMilliseconds.Num() % 30 == 0 ) {
		if ( benchmark.openGL ) common->Printf( "[glbench] %d demo frames completed\n", benchmark.stats.frameMilliseconds.Num() );
		else common->Printf( "[ptbench] %d demo frames completed at %d spp\n", benchmark.stats.frameMilliseconds.Num(), benchmark.samples );
	}
	benchmark.ready = false;
	return true;
}

#ifdef HAVE_PATHTRACER
void R_PT_BenchmarkRendered( int accumulatedFrames, float gpuMilliseconds, int width, int height ) {
	if ( !benchmark.active || benchmark.ready ) return;
	if ( accumulatedFrames != benchmark.frameBatches + 1 ) {
		common->Warning( "PT benchmark accumulation was reset or advanced unexpectedly; aborting" );
		R_EndBenchmark( false );
		return;
	}
	benchmark.width = width;
	benchmark.height = height;
	benchmark.frameGpu += gpuMilliseconds;
	benchmark.frameBatches++;
	benchmark.rendered = true;
}
#endif

// Called around the in-sequence screen update, after EndFrame has presented.
void R_BenchmarkScreenBegin() {
	R_PT_ReportTick();
	benchmark.screenStart = Sys_MillisecondsPrecise();
	benchmark.rendered = false;
}

static bool R_BenchmarkCheckGLErrors( const char *stage ) {
	bool ok = true;
	for ( int i = 0; i < 10; i++ ) {
		const GLenum error = qglGetError();
		if ( error == GL_NO_ERROR ) break;
		common->Warning( "GL benchmark frame %d: %s (OpenGL error 0x%x)",
			benchmark.stats.imagesWritten + 1, stage, error );
		ok = false;
	}
	return ok;
}

void R_BenchmarkCaptureGL() {
	if ( !R_GL_BenchmarkActive() || benchmark.ready ) return;
	benchmark.width = glConfig.vidWidth;
	benchmark.height = glConfig.vidHeight;
	R_IssueRenderCommands();
	// GL errors persist until queried; renderer errors must not masquerade as readback failures.
	R_BenchmarkCheckGLErrors( "renderer error before readback; checking capture separately" );
	GLint oldReadBuffer, oldPackAlignment;
	qglGetIntegerv( GL_READ_BUFFER, &oldReadBuffer );
	qglGetIntegerv( GL_PACK_ALIGNMENT, &oldPackAlignment );
	qglReadBuffer( GL_BACK );
	qglPixelStorei( GL_PACK_ALIGNMENT, 1 );
	const int rowBytes = benchmark.width * 3;
	idList<byte> pixels;
	pixels.SetNum( rowBytes * benchmark.height );
	qglReadPixels( 0, 0, benchmark.width, benchmark.height, GL_RGB, GL_UNSIGNED_BYTE, pixels.Ptr() );
	qglReadBuffer( oldReadBuffer );
	qglPixelStorei( GL_PACK_ALIGNMENT, oldPackAlignment );
	if ( !R_BenchmarkCheckGLErrors( "readback/state restoration failed; aborting" ) ) {
		R_EndBenchmark( false );
		return;
	}
	for ( int y = 0; y < benchmark.height / 2; y++ ) {
		byte *top = pixels.Ptr() + y * rowBytes, *bottom = pixels.Ptr() + ( benchmark.height - y - 1 ) * rowBytes;
		for ( int x = 0; x < rowBytes; x++ ) idSwap( top[x], bottom[x] );
	}
	benchmark.frameRender = Sys_MillisecondsPrecise() - benchmark.screenStart;
	const idStr image = va( "%s/frame_%06d.%s", benchmark.imageDirectory.c_str(),
		benchmark.stats.imagesWritten + 1, PT_BenchmarkImageExtension( benchmark.imageFormat ) );
	const double start = Sys_MillisecondsPrecise();
	R_PT_ReportPhase( "image_save" );
	if ( !R_WriteScreenshotImage( image, pixels.Ptr(), benchmark.width, benchmark.height, benchmark.imageFormat ) ) {
		fileSystem->RemoveFile( image );
		common->Warning( "GL benchmark could not save frame %d; aborting", benchmark.stats.imagesWritten + 1 );
		R_EndBenchmark( false );
		return;
	}
	benchmark.lastImage = image;
	benchmark.lastImageMilliseconds = Sys_MillisecondsPrecise() - start;
	benchmark.stats.AddImage( benchmark.lastImageMilliseconds );
	benchmark.ready = true;
}

void R_BenchmarkScreenComplete() {
	if ( benchmark.openGL ) return;
	if ( !benchmark.active || benchmark.ready ) return;
	if ( !benchmark.rendered ) {
		common->Warning( "PT benchmark did not render a main path-traced view; aborting" );
		R_EndBenchmark( false );
		return;
	}
	benchmark.frameRender += Sys_MillisecondsPrecise() - benchmark.screenStart;
	if ( benchmark.frameBatches == benchmark.batches ) {
		const idStr image = va( "%s/frame_%06d_%dspp.%s", benchmark.imageDirectory.c_str(),
			benchmark.stats.imagesWritten + 1, benchmark.samples, PT_BenchmarkImageExtension( benchmark.imageFormat ) );
		const double start = Sys_MillisecondsPrecise();
		R_PT_ReportPhase( "image_save" );
#ifdef HAVE_PATHTRACER
		if ( !RB_PT_SaveImage( image, benchmark.imageFormat ) ) {
			fileSystem->RemoveFile( image );
			common->Warning( "PT benchmark could not save frame %d; aborting", benchmark.stats.imagesWritten + 1 );
			R_EndBenchmark( false );
			return;
		}
#endif
		benchmark.lastImage = image;
		benchmark.lastImageMilliseconds = Sys_MillisecondsPrecise() - start;
		benchmark.stats.AddImage( benchmark.lastImageMilliseconds );
		benchmark.ready = true;
	}
}

static bool PT_BenchmarkImageDirectory( const idStr &demo, int samples, bool openGL, idStr &directory ) {
	idStr name;
	demo.ExtractFileName( name );
	name.StripFileExtension();
	const idStr parent = idStr( openGL ? "screenshots/glbench/" : "screenshots/ptbench/" ) + name;
	idFileList *runs = fileSystem->ListFiles( parent, "/" );
	for ( int run = 1; run <= 99999; run++ ) {
		const idStr candidate = openGL ? va( "run_%05d", run ) : va( "%dspp_run_%05d", samples, run );
		if ( runs->GetList().FindIndex( candidate ) < 0 ) {
			directory = parent + "/" + candidate;
			fileSystem->FreeFileList( runs );
			return true;
		}
	}
	fileSystem->FreeFileList( runs );
	common->Warning( "No free benchmark image directory under '%s'", parent.c_str() );
	return false;
}

static void R_StartBenchmark( const idCmdArgs &args, bool openGL ) {
	int samples = 0;
	if ( openGL ? args.Argc() != 2 : ( args.Argc() != 3 || !PT_ParseBenchmarkSamples( args.Argv( 2 ), samples ) ) ) {
		if ( openGL ) common->Warning( "Usage: r_glBenchmark <render_demo_name>" );
		else common->Warning( "Usage: r_ptBenchmark <render_demo_name> <spp 1..100000>" );
		return;
	}
	if ( R_BenchmarkActive() || R_GLSequenceActive()
#ifdef HAVE_PATHTRACER
		|| RB_PT_IsCapturing()
#endif
		) {
		common->Warning( "A renderer capture/benchmark is active; use r_glAbort or r_ptAbort first" );
		return;
	}
#ifdef HAVE_PATHTRACER
	if ( !openGL && !RB_PT_IsSupported() ) {
		common->Warning( "Path tracer benchmark requires a usable r_ptBackend" );
		return;
	}
#endif
	idStr demo = args.Argv( 1 );
	demo.BackSlashesToSlashes();
	if ( demo.IsEmpty() || demo.Find( ".." ) >= 0 || demo[0] == '/' || demo.Find( ":" ) >= 0 ) {
		common->Warning( "Benchmark requires a relative render demo name inside demos/" );
		return;
	}
	if ( demo.Icmpn( "demos/", 6 ) ) demo = idStr( "demos/" ) + demo;
	demo.DefaultFileExtension( ".demo" );
	idStr imageDirectory;
	if ( !PT_BenchmarkImageDirectory( demo, samples, openGL, imageDirectory ) ) return;
	if ( !session->StartBenchmarkDemo( demo ) ) {
		common->Warning( "Could not start render demo '%s'; benchmark not started", demo.c_str() );
		return;
	}
	if ( !R_PT_ReportBegin( args, openGL ? "glbenchmark" : "benchmark", demo, samples ) ) {
		session->Stop();
		session->StartMenu();
		return;
	}
	benchmark.stats = ptBenchmarkStatistics_t();
	benchmark.openGL = openGL;
	benchmark.demo = demo;
	benchmark.imageDirectory = imageDirectory;
	benchmark.imageFormat = cvarSystem->GetCVarInteger( "r_screenshotFormat" );
	benchmark.samples = samples;
	benchmark.batch = 1;
#ifdef HAVE_PATHTRACER
	if ( !openGL ) benchmark.batch = PT_BenchmarkBatchSize( samples, r_ptSamplesPerPixel.GetInteger() );
#endif
	benchmark.batches = openGL ? 1 : samples / benchmark.batch;
	benchmark.width = benchmark.height = 0;
#ifdef HAVE_PATHTRACER
	benchmark.savedDraw = r_ptDraw.GetBool();
	benchmark.savedMode = r_ptMode.GetInteger();
	benchmark.savedAccumulation = r_ptAccumulation.GetBool();
	r_ptDraw.SetBool( !openGL );
	if ( !openGL ) {
		r_ptMode.SetInteger( PT_MODE_PATHTRACE );
		r_ptAccumulation.SetBool( true );
	}
#endif
	benchmark.active = true;
	benchmark.start = Sys_MillisecondsPrecise();
	R_BenchmarkNewFrame();
	console->Close();
	if ( openGL ) {
		common->Printf( "\n[glbench] %s: every recorded frame, maximum 30 Hz playback, no skipping; r_glAbort cancels.\n"
			"[glbench] Output: %s/frame_NNNNNN.%s (viewport resolution, no HUD or overlays)\n",
			demo.c_str(), benchmark.imageDirectory.c_str(), PT_BenchmarkImageExtension( benchmark.imageFormat ) );
		return;
	}
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

void R_GL_Benchmark_f( const idCmdArgs &args ) {
	R_StartBenchmark( args, true );
}

#ifdef HAVE_PATHTRACER
void R_PT_Benchmark_f( const idCmdArgs &args ) {
	R_StartBenchmark( args, false );
}
#endif
