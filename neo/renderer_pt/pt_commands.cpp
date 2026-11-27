#include "sys/platform.h"

#ifdef HAVE_PATHTRACER

#include "framework/Console.h"
#include "framework/Session.h"
#include "renderer/tr_local.h"
#include "renderer_pt/pt_render.h"
#include "renderer_pt/pt_benchmark.h"
#include "renderer_pt/pt_capture_report.h"
#include "framework/UsercmdGen.h"

/*
========================
Path Tracer Compare Mode
========================
*/

enum cuCmpState_t {
	CUCOMP_IDLE = 0,
	CUCOMP_SWITCH_TO_GL,
	CUCOMP_WAIT_GL_FRAME,
	CUCOMP_TAKE_GL_SCREENSHOT,
	CUCOMP_WAIT_CU_SAMPLES
};

static cuCmpState_t cuCmp_state = CUCOMP_IDLE;
static int cuCmp_targetSamples = 64;
static int cuCmp_glFramesWaited = 0;
static idStr cuCmp_glFilename;
static idStr cuCmp_cuFilename;
static int cuCmp_screenshotNumber = 0;
static int cuCmp_batch = 1;
static bool cuCmp_savedDraw = false, cuCmp_savedAccumulation = false;
static int cuCmp_savedMode = 0;

// extern console variables
extern idCVar r_ptDraw;
extern idCVar r_ptAccumulation;
extern idCVar r_ptMode;
extern idCVar r_ptSamplesPerPixel;

/*
========================
PT_CompareFinish
========================
*/
static void PT_CompareFinish( bool completed, const char *reason ) {
	R_PT_ReportEnd( completed, reason );
	r_ptDraw.SetBool( cuCmp_savedDraw );
	r_ptAccumulation.SetBool( cuCmp_savedAccumulation );
	r_ptMode.SetInteger( cuCmp_savedMode );
	if ( RB_PT_GetBackend() ) RB_PT_GetBackend()->ResetAccumulation();
	cuCmp_state = CUCOMP_IDLE;
}

/*
========================
R_PT_Compare_f
========================
*/
void R_PT_Compare_f( const idCmdArgs &args ) {
	if ( RB_PT_IsCapturing() ) {
		common->Warning( "A path tracer capture/benchmark is active; use r_ptAbort first" );
		return;
	}
	if ( !RB_PT_IsSupported() ) {
		common->Printf( "Path tracer is not available (no usable r_ptBackend)\n" );
		return;
	}

	// parse target sample count
	if ( args.Argc() > 2 || ( args.Argc() == 2 && !PT_ParseBenchmarkSamples( args.Argv( 1 ), cuCmp_targetSamples ) ) ) {
		common->Warning( "Usage: r_ptCompare [spp 1..100000]" );
		return;
	}
	if ( args.Argc() == 1 ) cuCmp_targetSamples = 64;
	cuCmp_batch = PT_BenchmarkBatchSize( cuCmp_targetSamples, r_ptSamplesPerPixel.GetInteger() );
	if ( !R_PT_ReportBegin( args, "compare", session->GetCurrentMapName(), cuCmp_targetSamples ) ) return;
	cuCmp_savedDraw = r_ptDraw.GetBool();
	cuCmp_savedAccumulation = r_ptAccumulation.GetBool();
	cuCmp_savedMode = r_ptMode.GetInteger();
	r_ptAccumulation.SetBool( true );
	r_ptMode.SetInteger( PT_MODE_PATHTRACE );

	// generate filenames
	cuCmp_screenshotNumber++;

	int format = cvarSystem->GetCVarInteger( "r_screenshotFormat" );
	const char *ext = "tga";
	switch ( format ) {
		case 1: ext = "bmp"; break;
		case 2: ext = "png"; break;
		case 3: ext = "jpg"; break;
		default: ext = "tga"; break;
	}

	const char *directory = R_PT_ReportDirectory() ? R_PT_ReportDirectory() : "screenshots";
	cuCmp_glFilename = va( "%s/compare_%04d_opengl.%s", directory, cuCmp_screenshotNumber, ext );
	cuCmp_cuFilename = va( "%s/compare_%04d_pt_%dspp.%s", directory, cuCmp_screenshotNumber, cuCmp_targetSamples, ext );

	common->Printf( "\n=== Path Tracer Compare ===" );
	common->Printf( "\n  Target samples: %d", cuCmp_targetSamples );
	common->Printf( "\n  OpenGL file:    %s", cuCmp_glFilename.c_str() );
	common->Printf( "\n  Path tracer:    %s", cuCmp_cuFilename.c_str() );
	common->Printf( "\n==============================\n" );

	// close the console so it doesn't appear in the screenshot
	console->Close();

	// start the state machine
	cuCmp_state = CUCOMP_SWITCH_TO_GL;
}

/*
========================
RB_PT_CompareCheck
========================
*/
void RB_PT_CompareCheck( void ) {
	R_PT_ReportTick();
	switch ( cuCmp_state ) {
		// switch to OpenGL renderer
		case CUCOMP_SWITCH_TO_GL: {
			r_ptDraw.SetBool( false );
			R_PT_ReportFrameBegin( 0, 0, "opengl" );
			cuCmp_glFramesWaited = 0;
			cuCmp_state = CUCOMP_WAIT_GL_FRAME;
			common->Printf( "[cucomp] Switching to OpenGL renderer...\n" );
			break;
		}

		// wait 2 frames for the GL renderer to produce a stable image
		case CUCOMP_WAIT_GL_FRAME: {

			cuCmp_glFramesWaited++;
			if ( cuCmp_glFramesWaited >= 2 ) {
				cuCmp_state = CUCOMP_TAKE_GL_SCREENSHOT;
			}
			break;
		}

		// take the OpenGL screenshot
		case CUCOMP_TAKE_GL_SCREENSHOT: {
			const double saveStart = Sys_MillisecondsPrecise();
			R_PT_ReportPhase( "image_save" );
			tr.TakeScreenshot( glConfig.vidWidth, glConfig.vidHeight, cuCmp_glFilename.c_str(), 1, NULL );
			if ( !R_ScreenshotWriteSucceeded() || !R_PT_ReportFrameEnd( cuCmp_glFilename,
				glConfig.vidWidth, glConfig.vidHeight, Sys_MillisecondsPrecise() - saveStart ) ) {
				PT_CompareFinish( false, "opengl_capture_failed" );
				return;
			}
			common->Printf( "[cucomp] OpenGL screenshot saved: %s\n", cuCmp_glFilename.c_str() );
			// switch back to path tracer
			r_ptDraw.SetBool( true );
			if ( RB_PT_GetBackend() ) RB_PT_GetBackend()->ResetAccumulation();
			cuCmp_state = CUCOMP_WAIT_CU_SAMPLES;
			R_PT_ReportFrameBegin( 0, 0, "pathtrace" );
			common->Printf( "[cucomp] Switching to path tracer, accumulating %d samples...\n", cuCmp_targetSamples );
			break;
		}

		// check if the path tracer has accumulated enough samples
		case CUCOMP_WAIT_CU_SAMPLES: {
			if ( RB_PT_GetBackend() ) {
				int currentSamples = RB_PT_GetBackend()->GetAccumulatedFrames() * cuCmp_batch;
				if ( currentSamples > 0 && currentSamples % ( cuCmp_targetSamples / 10 + 1 ) == 0 ) {
					common->Printf( "[cucomp] Accumulating... %d / %d samples\n", currentSamples, cuCmp_targetSamples );
				}

				if ( currentSamples >= cuCmp_targetSamples ) {
					const double saveStart = Sys_MillisecondsPrecise();
					R_PT_ReportPhase( "image_save" );
					tr.TakeScreenshot( glConfig.vidWidth, glConfig.vidHeight, cuCmp_cuFilename.c_str(), 1, NULL );
					if ( !R_ScreenshotWriteSucceeded() || !R_PT_ReportFrameEnd( cuCmp_cuFilename,
						glConfig.vidWidth, glConfig.vidHeight, Sys_MillisecondsPrecise() - saveStart ) ) {
						PT_CompareFinish( false, "pt_capture_failed" );
						return;
					}
					common->Printf( "[cucomp] Path tracer screenshot saved: %s (%d samples)\n",
						cuCmp_cuFilename.c_str(), currentSamples );
					common->Printf( "[cucomp] Comparison complete!\n" );
					PT_CompareFinish( true, "" );
				}
			} else {
				common->Warning( "[cucomp] Path tracer not available, aborting comparison\n" );
				PT_CompareFinish( false, "backend_unavailable" );
			}
			break;
		}

		case CUCOMP_IDLE: {
			break;
		}

		default: {
			break;
		}
	}
}


/*
========================
Path Tracer Sequence Renderer
========================
*/

enum cuSeqState_t {
	CUSEQ_IDLE = 0,
	CUSEQ_SETUP,
	CUSEQ_ACCUMULATE,
	CUSEQ_SCREENSHOT,
	CUSEQ_ADVANCE_UNPAUSE,
	CUSEQ_ADVANCE_WAIT,
	CUSEQ_DONE
};

static cuSeqState_t	cuSeq_state = CUSEQ_IDLE;
static int cuSeq_frameStart = 0;
static int cuSeq_frameEnd = 0;
static int cuSeq_currentFrame = 0;
static int cuSeq_targetSamples = 256;
static int cuSeq_startTime = 0;
static int cuSeq_saved_fixedTic = 0;
static bool cuSeq_saved_stopTime = false;
static bool cuSeq_saved_ptDraw = false;
static bool cuSeq_saved_accumulation = false;
static int cuSeq_saved_mode = 0, cuSeq_batch = 1;
static bool cuSeq_failed = false;

/*
========================
RB_PT_GetCaptureTargetFrames
========================
*/
int RB_PT_GetCaptureTargetFrames( void ) {
	if ( R_PT_BenchmarkActive() ) return R_PT_BenchmarkTargetFrames();
	if ( cuSeq_state == CUSEQ_ACCUMULATE || cuSeq_state == CUSEQ_SCREENSHOT ) return cuSeq_targetSamples / cuSeq_batch;
	return cuCmp_state == CUCOMP_WAIT_CU_SAMPLES ? cuCmp_targetSamples / cuCmp_batch : 0;
}

/*
========================
RB_PT_GetCaptureBatch
========================
*/
int RB_PT_GetCaptureBatch() {
	if ( R_PT_BenchmarkActive() ) return R_PT_BenchmarkBatch();
	if ( cuSeq_state != CUSEQ_IDLE ) return cuSeq_batch;
	return cuCmp_batch;
}

/*
========================
RB_PT_IsCapturing
========================
*/
bool RB_PT_IsCapturing( void ) {
	return cuSeq_state != CUSEQ_IDLE || cuCmp_state != CUCOMP_IDLE || R_BenchmarkActive() || R_GLSequenceActive();
}

/*
========================
R_PT_Sequence_f
========================
*/
void R_PT_Sequence_f( const idCmdArgs &args ) {
	if ( R_PT_BenchmarkActive() ) {
		common->Warning( "A path tracer benchmark is active; use r_ptAbort first" );
		return;
	}
	if ( !RB_PT_IsSupported() ) {
		common->Printf( "Path tracer is not available (no usable r_ptBackend)\n" );
		return;
	}

	if ( cuSeq_state != CUSEQ_IDLE ) {
		common->Printf( "[cuseq] Sequence render already in progress. Use r_ptAbort to cancel.\n" );
		return;
	}

	if ( cuCmp_state != CUCOMP_IDLE ) {
		common->Printf( "[cuseq] A comparison (r_ptCompare) is in progress. Abort it first.\n" );
		return;
	}

	if ( args.Argc() < 3 || args.Argc() > 4 ) {
		common->Printf( "Usage: r_ptSequence <frame_start> <frame_end> [samples]\n" );
		common->Printf( "  frame_start  - First game frame to render (0+)\n" );
		common->Printf( "  frame_end    - Last game frame to render (inclusive)\n" );
		common->Printf( "  samples      - Path tracer samples per frame (default 256)\n" );
		common->Printf( "\nThe game will be paused and single-stepped forward between frames.\n" );
		common->Printf( "Output: screenshots/ptseq_NNNNN.<ext>\n" );
		return;
	}

	if ( !PT_ParseCaptureFrame( args.Argv( 1 ), cuSeq_frameStart )
		|| !PT_ParseCaptureFrame( args.Argv( 2 ), cuSeq_frameEnd ) ) {
		common->Warning( "PT sequence frame numbers must be nonnegative integers below INT_MAX" );
		return;
	}
	if ( cuSeq_frameEnd < cuSeq_frameStart ) {
		common->Printf( "[cuseq] Error: frame_end (%d) must be >= frame_start (%d)\n",
			cuSeq_frameEnd, cuSeq_frameStart );
		return;
	}

	if ( args.Argc() >= 4 ) {
		if ( !PT_ParseBenchmarkSamples( args.Argv( 3 ), cuSeq_targetSamples ) ) {
			common->Warning( "PT sequence spp must be an integer from 1 to 100000" );
			return;
		}
	} else {
		cuSeq_targetSamples = 256;
	}

	int totalFrames = cuSeq_frameEnd - cuSeq_frameStart + 1;
	cuSeq_batch = PT_BenchmarkBatchSize( cuSeq_targetSamples, r_ptSamplesPerPixel.GetInteger() );
	if ( !R_PT_ReportBegin( args, "sequence", session->GetCurrentMapName(), cuSeq_targetSamples,
		cuSeq_frameStart, cuSeq_frameEnd ) ) return;
	cuSeq_failed = false;

	common->Printf( "\n=== Path Tracer Sequence Render ===\n" );
	common->Printf( "  Frames:        %d - %d (%d total)\n", cuSeq_frameStart, cuSeq_frameEnd, totalFrames );
	common->Printf( "  Samples/frame: %d\n", cuSeq_targetSamples );
	if ( cuSeq_frameStart > 0 ) {
		common->Printf( "  Fast-forward:  %d frames via OpenGL\n", cuSeq_frameStart );
	}
	common->Printf( "  Output:        %s/ptseq_NNNNN.<ext>\n",
		R_PT_ReportDirectory() ? R_PT_ReportDirectory() : "screenshots" );
	common->Printf( "============================\n\n" );

	// close console so it doesn't appear in screenshots
	console->Close();

	cuSeq_currentFrame = 0;
	cuSeq_state = CUSEQ_SETUP;
}

/*
========================
RB_PT_SequenceCheck
========================
*/
void RB_PT_SequenceCheck( void ) {
	R_PT_ReportTick();
	if ( cuSeq_state == CUSEQ_IDLE ) {
		return;
	}

	switch ( cuSeq_state ) {

		case CUSEQ_SETUP: {
			// save current cvar values
			idCVar *stopTimeCVar = cvarSystem->Find( "g_stoptime" );
			cuSeq_saved_stopTime = stopTimeCVar ? stopTimeCVar->GetBool() : false;
			cuSeq_saved_fixedTic = cvarSystem->GetCVarInteger( "com_fixedTic" );
			cuSeq_saved_ptDraw = r_ptDraw.GetBool();
			cuSeq_saved_accumulation = r_ptAccumulation.GetBool();
			cuSeq_saved_mode = r_ptMode.GetInteger();
			r_ptMode.SetInteger( PT_MODE_PATHTRACE );

			// set com_fixedTic 1 for deterministic single-tick stepping
			cvarSystem->SetCVarInteger( "com_fixedTic", 1 );

			// force accumulation on — without this, RenderView() resets accum_frame
			// to 0 every frame and the sequence never reaches its sample target
			r_ptAccumulation.SetBool( true );

			cuSeq_startTime = Sys_Milliseconds();

			if ( cuSeq_frameStart > 0 ) {
				// switch to GL for fast-forward
				r_ptDraw.SetBool( false );

				// unpause game to let it run forward
				if ( stopTimeCVar ) {
					stopTimeCVar->SetBool( false );
				}
				cuSeq_currentFrame = 0;
				cuSeq_state = CUSEQ_ADVANCE_WAIT;
				R_PT_ReportPhase( "advance" );
				common->Printf( "[cuseq] Fast-forwarding to frame %d using OpenGL renderer...\n", cuSeq_frameStart );
			} else {
				// start from frame 0 — enable path tracer and freeze immediately
				r_ptDraw.SetBool( true );
				if ( stopTimeCVar ) {
					stopTimeCVar->SetBool( true );
				}
				cuSeq_currentFrame = 0;
				// reset accumulation for first frame
				if ( RB_PT_GetBackend() ) {
					RB_PT_GetBackend()->ResetAccumulation();
				}
				cuSeq_state = CUSEQ_ACCUMULATE;
				R_PT_ReportFrameBegin( cuSeq_currentFrame, cuSeq_currentFrame * ( USERCMD_MSEC * 0.001 ), "pathtrace" );
				common->Printf( "[cuseq] Frame %d / %d: accumulating %d samples...\n",
					cuSeq_currentFrame, cuSeq_frameEnd, cuSeq_targetSamples );
			}
			break;
		}

		case CUSEQ_ACCUMULATE: {
			// accumulate path tracer samples on the frozen game frame
			if ( RB_PT_GetBackend() ) {
				int currentSamples = RB_PT_GetBackend()->GetAccumulatedFrames() * cuSeq_batch;

				// progress reporting every 25%
				int quarter = cuSeq_targetSamples / 4;
				if ( quarter > 0 && currentSamples > 0 && currentSamples % quarter == 0 &&
					currentSamples < cuSeq_targetSamples ) {
					common->Printf( "[cuseq] Frame %d: %d / %d samples (%d%%)\n",
						cuSeq_currentFrame, currentSamples, cuSeq_targetSamples,
						( currentSamples * 100 ) / cuSeq_targetSamples );
				}

				if ( currentSamples >= cuSeq_targetSamples ) {
					cuSeq_state = CUSEQ_SCREENSHOT;
				}
			} else {
				common->Warning( "[cuseq] Path tracer not available, aborting.\n" );
				cuSeq_failed = true;
				cuSeq_state = CUSEQ_DONE;
			}
			break;
		}

		case CUSEQ_SCREENSHOT: {
			// determine screenshot format
			int format = cvarSystem->GetCVarInteger( "r_screenshotFormat" );
			const char *ext = "tga";
			switch ( format ) {
				case 1: ext = "bmp"; break;
				case 2: ext = "png"; break;
				case 3: ext = "jpg"; break;
				default: ext = "tga"; break;
			}

			idStr filename = va( "%s/ptseq_%05d.%s",
				R_PT_ReportDirectory() ? R_PT_ReportDirectory() : "screenshots", cuSeq_currentFrame, ext );
			const double saveStart = Sys_MillisecondsPrecise();
			R_PT_ReportPhase( "image_save" );
			tr.TakeScreenshot( glConfig.vidWidth, glConfig.vidHeight, filename.c_str(), 1, NULL );
			if ( !R_ScreenshotWriteSucceeded() || !R_PT_ReportFrameEnd( filename,
				glConfig.vidWidth, glConfig.vidHeight, Sys_MillisecondsPrecise() - saveStart ) ) {
				cuSeq_failed = true;
				cuSeq_state = CUSEQ_DONE;
				break;
			}

			int totalFrames = cuSeq_frameEnd - cuSeq_frameStart + 1;
			int framesDone = cuSeq_currentFrame - cuSeq_frameStart + 1;
			float elapsedSec = (float)( Sys_Milliseconds() - cuSeq_startTime ) * 0.001f;

			common->Printf( "[cuseq] frame: %d | time: %.2fs | saved: %s (%d/%d, %d%%)\n",
				cuSeq_currentFrame, elapsedSec, filename.c_str(), framesDone, totalFrames,
				( framesDone * 100 ) / totalFrames );

			// check if this was the last frame
			if ( cuSeq_currentFrame >= cuSeq_frameEnd ) {
				cuSeq_state = CUSEQ_DONE;
			} else {
				cuSeq_state = CUSEQ_ADVANCE_UNPAUSE;
			}
			break;
		}

		case CUSEQ_ADVANCE_UNPAUSE: {
			R_PT_ReportPhase( "advance" );
			// unpause the game so the next RunGameTic executes
			idCVar *stopTimeCVar = cvarSystem->Find( "g_stoptime" );
			if ( stopTimeCVar ) {
				stopTimeCVar->SetBool( false );
			}
			cuSeq_state = CUSEQ_ADVANCE_WAIT;
			break;
		}

		case CUSEQ_ADVANCE_WAIT: {
			// Exactly one fixed game tick has elapsed since unpausing.
			cuSeq_currentFrame++;
			idCVar *stopTimeCVar = cvarSystem->Find( "g_stoptime" );
			if ( stopTimeCVar ) {
				stopTimeCVar->SetBool( true );
			}

			// if we haven't reached the start frame yet, keep advancing (in GL mode)
			if ( cuSeq_currentFrame < cuSeq_frameStart ) {
				if ( cuSeq_currentFrame % 100 == 0 ) {
					common->Printf( "[cuseq] Fast-forward (GL): frame %d / %d...\n",
						cuSeq_currentFrame, cuSeq_frameStart );
				}
				if ( stopTimeCVar ) {
					stopTimeCVar->SetBool( false );
				}
				cuSeq_state = CUSEQ_ADVANCE_WAIT;
			} else {
				// we've reached a renderable frame — switch to path tracer and accumulate
				if ( !r_ptDraw.GetBool() ) {
					r_ptDraw.SetBool( true );
					common->Printf( "[cuseq] Reached frame %d, switching to path tracer.\n",
						cuSeq_currentFrame );
				}
				// reset accumulation so samples start from 0 for this new game frame
				if ( RB_PT_GetBackend() ) {
					RB_PT_GetBackend()->ResetAccumulation();
				}
				cuSeq_state = CUSEQ_ACCUMULATE;
				R_PT_ReportFrameBegin( cuSeq_currentFrame, cuSeq_currentFrame * ( USERCMD_MSEC * 0.001 ), "pathtrace" );
				common->Printf( "[cuseq] Frame %d / %d: accumulating %d samples...\n",
					cuSeq_currentFrame, cuSeq_frameEnd, cuSeq_targetSamples );
			}
			break;
		}

		case CUSEQ_DONE: {
			R_PT_ReportEnd( !cuSeq_failed, cuSeq_failed ? "sequence_capture_failed" : "" );
			// restore saved cvar values
			idCVar *stopTimeCVar = cvarSystem->Find( "g_stoptime" );
			if ( stopTimeCVar ) {
				stopTimeCVar->SetBool( cuSeq_saved_stopTime );
			}

			// sanitize com_fixedTic: if the saved value is 1 (our sequence value,
			// possibly from a prior interrupted run), force it back to 0 (normal
			// variable timestep). com_fixedTic 1 = exactly 1 game tick per frame,
			// which causes slow-motion at any framerate other than exactly 60fps.
			int restoreFixedTic = cuSeq_saved_fixedTic;
			if ( restoreFixedTic == 1 ) {
				common->Printf( "[cuseq] Saved com_fixedTic was %d (sequence value), forcing to 0\n", restoreFixedTic );
				restoreFixedTic = 0;
			}
			cvarSystem->SetCVarInteger( "com_fixedTic", restoreFixedTic );
			r_ptDraw.SetBool( cuSeq_saved_ptDraw );
			r_ptAccumulation.SetBool( cuSeq_saved_accumulation );
			r_ptMode.SetInteger( cuSeq_saved_mode );

			common->Printf( "[cuseq] Restored: g_stopTime=%d, com_fixedTic=%d, r_ptDraw=%d\n",
				(int)cuSeq_saved_stopTime, restoreFixedTic, (int)cuSeq_saved_ptDraw );

			int totalFrames = cuSeq_frameEnd - cuSeq_frameStart + 1;
			float totalSec = (float)( Sys_Milliseconds() - cuSeq_startTime ) * 0.001f;
			int totalMin = (int)( totalSec / 60.0f );
			float remSec = totalSec - (float)( totalMin * 60 );
			common->Printf( "\n=== Path Tracer Sequence Render %s ===\n", cuSeq_failed ? "Aborted" : "Complete" );
			common->Printf( "  Rendered %d frames (%d - %d) at %d samples each.\n",
				totalFrames, cuSeq_frameStart, cuSeq_frameEnd, cuSeq_targetSamples );
			common->Printf( "  Total time: %dm %.1fs (%.1fs)\n", totalMin, remSec, totalSec );
			common->Printf( "  Avg time/frame: %.1fs\n", totalSec / totalFrames );
			common->Printf( "  Output: screenshots/ptseq_%05d - ptseq_%05d\n",
				cuSeq_frameStart, cuSeq_frameEnd );
			common->Printf( "=====================================\n\n" );

			cuSeq_state = CUSEQ_IDLE;
			break;
		}

		case CUSEQ_IDLE: {

		}

		default: {
			break;
		}
	}
}

/*
========================
R_PT_Abort_f
========================
*/
void R_PT_Abort_f( const idCmdArgs &args ) {
	if ( R_PT_BenchmarkActive() ) {
		R_EndBenchmark( false );
		session->Stop();
		session->StartMenu();
		return;
	}

	if ( cuSeq_state == CUSEQ_IDLE && cuCmp_state == CUCOMP_IDLE ) {
		common->Printf( "[cuAbort] Nothing in progress to abort.\n" );
		return;
	}

	if ( cuSeq_state != CUSEQ_IDLE ) {
		R_PT_ReportEnd( false, "sequence_aborted" );
		common->Printf( "[cuseq] Aborting sequence render at frame %d (was in state %d)...\n",
			cuSeq_currentFrame, (int)cuSeq_state );

		if ( cuSeq_state == CUSEQ_SETUP ) {
			cuSeq_state = CUSEQ_IDLE;
			common->Printf( "[cuseq] Cancelled before capture setup; CVars unchanged.\n" );
			return;
		}

		// restore saved cvar values
		idCVar *stopTimeCVar = cvarSystem->Find( "g_stoptime" );
		if ( stopTimeCVar ) {
			stopTimeCVar->SetBool( cuSeq_saved_stopTime );
		}
		int restoreFixedTic = cuSeq_saved_fixedTic;
		if ( restoreFixedTic == 1 ) restoreFixedTic = 0;
		cvarSystem->SetCVarInteger( "com_fixedTic", restoreFixedTic );
		r_ptDraw.SetBool( cuSeq_saved_ptDraw );
		r_ptAccumulation.SetBool( cuSeq_saved_accumulation );
		r_ptMode.SetInteger( cuSeq_saved_mode );

		cuSeq_state = CUSEQ_IDLE;
		common->Printf( "[cuseq] Aborted. CVars restored.\n" );
	}

	if ( cuCmp_state != CUCOMP_IDLE ) {
		common->Printf( "[cucomp] Aborting comparison...\n" );
		PT_CompareFinish( false, "compare_aborted" );
		common->Printf( "[cucomp] Aborted.\n" );
	}
}

/*
========================
RB_PT_CancelCaptures
========================
*/
void RB_PT_CancelCaptures() {
	if ( R_PT_BenchmarkActive() ) R_EndBenchmark( false );
	else if ( RB_PT_IsCapturing() ) R_PT_Abort_f( idCmdArgs() );
}

#endif // HAVE_PATHTRACER
