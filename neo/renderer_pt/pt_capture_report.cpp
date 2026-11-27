// STL headers must come before idlib: idlib/Str.h redefines snprintf/vsnprintf
// (use_idStr_snPrintf), which breaks the using-declarations in MSVC's <cstdio>.
#include <atomic>	// no idlib/engine equivalent for the telemetry thread flags
#include <stdio.h>
#include <time.h>

#include "sys/platform.h"

#include "framework/Session.h"
#include "renderer/tr_local.h"
#include "renderer_pt/pt_capture_report.h"
#include "renderer_pt/pt_render.h"
#include "renderer_pt/pt_benchmark.h"
#include "renderer_pt/pt_gpu_telemetry.h"
#include "sys/sys_sdl.h"

#ifdef _WIN32
#include <windows.h>
#endif

static idCVar r_ptCaptureReport( "r_ptCaptureReport", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
	"write local settings, invocation and frame reports for PT captures and GL benchmarks" );
static idCVar r_ptCaptureTelemetry( "r_ptCaptureTelemetry", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
	"sample GPU (NVML, Linux sysfs/fdinfo or Windows D3DKMT) and CPU sensors at approximately 1 Hz during renderer captures" );

/*
========================
Number

The engine never changes LC_NUMERIC, so printf formatting uses '.' decimals
========================
*/
static idStr Number( double value ) {
	return idStr::Format( "%.6f", value );
}

/*
========================
Integer
========================
*/
static idStr Integer( long long value ) {
	return idStr::Format( "%lld", value );
}

/*
========================
Write
========================
*/
static bool Write( idFile *file, const idStr &text ) {
	return file && file->Write( text.c_str(), text.Length() ) == text.Length();
}

/*
========================
PhaseIndex
========================
*/
static int PhaseIndex( const char *phase ) {
	if ( !idStr::Cmp( phase, "opengl" ) ) return 1;
	if ( !idStr::Cmp( phase, "pathtrace" ) ) return 2;
	if ( !idStr::Cmp( phase, "image_save" ) ) return 3;
	if ( !idStr::Cmp( phase, "advance" ) ) return 4;
	return 0;
}

/*
========================
PhaseName
========================
*/
static const char *PhaseName( int phase ) {
	static const char *names[] = { "setup", "opengl", "pathtrace", "image_save", "advance" };
	return names[phase];
}

static struct {
	bool active = false, failed = false, frameActive = false, sensorWarning = false;
	bool framePathtrace = false;
	bool openGL = false;
	idFile *frames = NULL, *settings = NULL, *telemetry = NULL;
	idStr directory, mode, source, command, sourceFingerprint, cpuModel, telemetryStatus;
	idStr telemetrySource, telemetrySensors, telemetryFailure, telemetryUnavailable;
	idDict original, effective;
	ptBackendDeviceInfo_t device = {};
	bool haveDevice = false;
	int requestedSpp = 0, startFrame = 0, endFrame = 0, rows = 0, sourceFrame = 0;
	int captureBatch = 1;
	int benchmarkPacing = -1;		// -1 not a benchmark, 0 capped at 30 Hz, 1 uncapped
	int benchmarkSaveImages = -1;	// -1 not a benchmark, 0 no, 1 yes
	int width = 0, height = 0, spp = 0, dispatches = 0;
	double start = 0, frameStart = 0, sourceTime = 0;
	time_t startedUnix = 0;
	double gpu = 0, path = 0, denoise = 0, postprocess = 0, toneMap = 0;
	double scene = 0, backend = 0, readback = 0, totalGpu = 0, totalWall = 0;
	ptBackendStats_t lastStats = {};
	std::atomic<int> phase{0}, telemetryFrame{0};
	std::atomic<bool> stopTelemetry{false}, telemetryWriteFailed{false}, sensorError{false};
	SDL_Thread *thread = NULL;
	idGpuTelemetry *gpuTelemetry = NULL, *cpuTelemetry = NULL;
} report;

/*
========================
Settings
========================
*/
static idDict Settings() {
	idDict result;
	const idDict *all = cvarSystem->MoveCVarsToDict( -1 );
	for ( int i = 0; i < all->GetNumKeyVals(); i++ ) {
		const idKeyValue *kv = all->GetKeyVal( i );
		const idStr &name = kv->GetKey();
		if ( ( report.openGL && !name.Icmpn( "r_", 2 ) ) || !name.Icmpn( "r_pt", 4 ) || !name.Icmpn( "r_screenshot", 12 )
			|| !name.Icmp( "r_swapInterval" ) || !name.Icmp( "r_lightScale" )
			|| !name.Icmp( "r_useSMP" ) || !name.Icmp( "com_fixedTic" ) || !name.Icmp( "g_stopTime" ) ) {
			result.Set( name, PT_ReportSensitiveName( name.c_str() ) ? "[redacted]" : kv->GetValue().c_str() );
		}
	}
	return result;
}

/*
========================
DictJson
========================
*/
static idStr DictJson( const idDict &dict ) {
	idStr result = "{";
	for ( int i = 0; i < dict.GetNumKeyVals(); i++ ) {
		const idKeyValue *kv = dict.GetKeyVal( i );
		if ( i ) result += ",";
		result += "\n    " + PT_ReportJsonString( kv->GetKey().c_str() ) + ":" + PT_ReportJsonString( kv->GetValue().c_str() );
	}
	return result + "\n  }";
}

/*
========================
StartupJson
========================
*/
static idStr StartupJson() {
	const idStrList &args = Com_GetStartupArguments();
	idStr result = "[";
	for ( int i = 0; i < args.Num(); i++ ) {
		if ( i ) result += ",";
		const bool redact = i > 0 && PT_ReportSensitiveName( args[i - 1].c_str() );
		result += PT_ReportJsonString( redact ? idStr( "[redacted]" ) : PT_ReportRedactArgument( args[i] ) );
	}
	return result + "]";
}

/*
========================
Metadata
========================
*/
static bool Metadata( const char *status, const char *reason ) {
	idStr text = "{\n  \"schema_version\":1,\n  \"status\":" + PT_ReportJsonString( status )
		+ ",\n  \"reason\":" + PT_ReportJsonString( reason )
		+ ",\n  \"mode\":" + PT_ReportJsonString( report.mode.c_str() )
		+ ",\n  \"source\":" + PT_ReportJsonString( report.source.c_str() )
		+ ",\n  \"started_unix_s\":" + Integer( (long long)report.startedUnix )
		+ ",\n  \"source_fingerprint_fnv1a64\":" + PT_ReportJsonString( report.sourceFingerprint.c_str() )
		+ ",\n  \"startup_argv\":" + StartupJson()
		+ ",\n  \"capture_command\":" + PT_ReportJsonString( report.command.c_str() )
		+ ",\n  \"invocation_note\":\"Arguments exclude executable and shell quoting/redirections; known credential values are redacted. Local paths may remain. Review before sharing.\","
		+ "\n  \"engine_version\":" + PT_ReportJsonString( com_version.GetString() )
		+ ",\n  \"renderer_revision\":" + PT_ReportJsonString( PT_CAPTURE_REVISION )
		+ ",\n  \"build_time\":" + PT_ReportJsonString( __DATE__ " " __TIME__ )
		+ ",\n  \"rendering_model\":" + PT_ReportJsonString( report.openGL ? "OpenGL" :
#ifdef PT_PHYSICAL
			"PT_PHYSICAL"
#else
			"legacy"
#endif
		)
		+ ",\n  \"platform\":" + PT_ReportJsonString( D3_OSTYPE )
		+ ",\n  \"cpu_model\":" + ( report.cpuModel.IsEmpty() ? "null" : PT_ReportJsonString( report.cpuModel.c_str() ) )
		+ ",\n  \"system_ram_mb\":" + Number( Sys_GetSystemRam() )
		+ ",\n  \"cpu_logical_cores\":"
#ifdef D3_SDL1
		+ "null"
#elif defined( D3_SDL3 )
		+ Number( SDL_GetNumLogicalCPUCores() )
#else
		+ Number( SDL_GetCPUCount() )
#endif
		+ ",\n  \"gpu\":";
	if ( report.haveDevice && !idStr::Icmp( report.device.api, "opencl" ) ) {
		text += "{\"name\":" + PT_ReportJsonString( report.device.name )
			+ ",\"api\":\"opencl\""
			+ ",\"total_memory_bytes\":" + Number( (double)report.device.totalMemory )
			+ ",\"opencl_driver_version\":" + PT_ReportJsonString( report.device.driverString )
			+ ",\"opencl_device_version\":" + PT_ReportJsonString( report.device.apiVersion )
			+ ",\"compute_units\":" + Number( report.device.multiprocessors )
			+ ",\"opencl_c_major\":" + Number( report.device.computeMajor )
			+ ",\"opencl_c_minor\":" + Number( report.device.computeMinor ) + "}";
	} else if ( report.haveDevice ) {
		text += "{\"name\":" + PT_ReportJsonString( report.device.name )
			+ ",\"total_memory_bytes\":" + Number( (double)report.device.totalMemory )
			+ ",\"cuda_driver_version\":" + Number( report.device.driverVersion )
			+ ",\"cuda_runtime_version\":" + Number( report.device.runtimeVersion )
			+ ",\"multiprocessors\":" + Number( report.device.multiprocessors )
			+ ",\"compute_major\":" + Number( report.device.computeMajor )
			+ ",\"compute_minor\":" + Number( report.device.computeMinor ) + "}";
	} else if ( report.openGL ) text += "{\"name\":" + PT_ReportJsonString( glConfig.renderer_string ? glConfig.renderer_string : "" ) + "}";
	else text += "null";
	text += ",\n  \"telemetry_status\":" + PT_ReportJsonString( report.telemetryStatus.c_str() )
		+ ",\n  \"telemetry_source\":" + ( report.telemetrySource.IsEmpty() ? "null" : PT_ReportJsonString( report.telemetrySource.c_str() ) )
		+ ",\n  \"telemetry_sensors\":" + PT_ReportJsonString( report.telemetrySensors.c_str() )
		+ ",\n  \"telemetry_unavailable_reason\":" + ( report.telemetryFailure.IsEmpty() ? "null" : PT_ReportJsonString( report.telemetryFailure.c_str() ) )
		+ ",\n  \"telemetry_unavailable_sensors\":" + PT_ReportJsonString( report.telemetryUnavailable.c_str() )
		+ ",\n  \"telemetry_interval_ms\":1000,\n  \"gpu_stage_timing\":" + ( report.openGL ? "false" : "true" )
		+ ",\n  \"benchmark_pacing\":" + ( report.benchmarkPacing < 0 ? "null" : ( report.benchmarkPacing ? "\"uncapped\"" : "\"capped_30hz\"" ) )
		+ ",\n  \"benchmark_save_images\":" + ( report.benchmarkSaveImages < 0 ? "null" : ( report.benchmarkSaveImages ? "true" : "false" ) )
		+ ",\n  \"requested_spp\":" + ( report.openGL ? "null" : Number( report.requestedSpp ) )
		+ ",\n  \"capture_batch_spp\":" + ( report.openGL ? idStr( "null" ) : Integer( report.captureBatch ) )
		+ ",\n  \"capture_target_dispatches\":" + ( report.openGL ? idStr( "null" ) : Integer( report.requestedSpp / report.captureBatch ) )
		+ ",\n  \"requested_start_frame\":" + Number( report.startFrame )
		+ ",\n  \"requested_end_frame\":" + Number( report.endFrame )
		+ ",\n  \"completed_rows\":" + Number( report.rows )
		+ ",\n  \"gpu_total_ms\":" + ( report.openGL ? "null" : Number( report.totalGpu ) )
		+ ",\n  \"frame_wall_total_ms\":" + Number( report.totalWall )
		+ ",\n  \"elapsed_s\":" + Number( ( Sys_MillisecondsPrecise() - report.start ) * 0.001 )
		+ ",\n  \"settings_before\":" + DictJson( report.original )
		+ ",\n  \"settings_effective\":" + DictJson( report.effective )
		+ ",\n  \"timing_notes\":" + PT_ReportJsonString( report.openGL
			? "GL timings are CPU-observed wall durations, not isolated GPU timings. screen_render_ms includes rendering and pre-swap framebuffer readback; image_save_ms includes encoding/write. frame_wall_ms includes playback pacing and capture overhead. GPU/PT-specific metrics are unavailable. Settings changes are recorded in settings.csv."
			: "gpu_ms includes GPU tracing/denoising/postprocessing/tone mapping; backend_wall_ms includes scene build/upload, rendering, readback and PT blit. scene_ms and readback_ms are CPU-observed subsets. screen_render_ms is available for benchmark only. image_save_ms includes screenshot capture/encoding/write. Compute FPS excludes playback pacing; samples/s counts primary samples, not rays. Settings changes are recorded in settings.csv." ) + "\n}\n";
	const idStr temporary = report.directory + "/metadata.json.tmp";
	idFile *file = fileSystem->OpenFileWrite( temporary );
	if ( file ) file->ForceFlush();
	const bool ok = Write( file, text );
	if ( file ) fileSystem->CloseFile( file );
	const idStr from = fileSystem->RelativePathToOSPath( temporary, "fs_savepath" );
	const idStr to = fileSystem->RelativePathToOSPath( report.directory + "/metadata.json", "fs_savepath" );
#ifdef _WIN32
	const bool replaced = ok && MoveFileExA( from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH );
#else
	const bool replaced = ok && rename( from, to ) == 0;
#endif
	if ( !replaced ) {
		common->Warning( "PT capture report metadata could not be written/replaced" );
		fileSystem->RemoveFile( temporary );
	}
	return replaced;
}

/*
========================
TelemetryHeader
========================
*/
static idStr TelemetryHeader() {
	idStr header = "elapsed_s,source_frame,phase";
	for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) { header += ","; header += ptGpuSensorColumns[i]; }
	return header + ",sensor_errors\n";
}

/*
========================
TelemetryWorker
========================
*/
static int SDLCALL TelemetryWorker( void * ) {
	ptGpuTelemetrySample_t sample;
	while ( !report.stopTelemetry.load() ) {
		sample.Clear();
		if ( report.gpuTelemetry ) report.gpuTelemetry->Sample( sample );
		if ( report.cpuTelemetry ) report.cpuTelemetry->Sample( sample );
		if ( !sample.errors.IsEmpty() ) report.sensorError.store( true );
		const double elapsed = ( Sys_MillisecondsPrecise() - report.start ) * 0.001;
		idStr row = Number( elapsed ) + "," + Number( report.telemetryFrame.load() ) + ","
			+ PhaseName( report.phase.load() );
		for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) {
			row += ",";
			if ( !sample.valid[i] ) continue;
			row += i == PT_GPU_CLOCK_LIMIT_REASONS ? idStr::Format( "%llu", (unsigned long long)sample.value[i] ) : Number( sample.value[i] );
		}
		row += "," + PT_ReportCsvString( sample.errors ) + "\n";
		if ( !Write( report.telemetry, row ) ) { report.telemetryWriteFailed.store( true ); break; }
		for ( int i = 0; i < 10 && !report.stopTelemetry.load(); i++ ) SDL_Delay( 100 );
	}
	return 0;
}

/*
========================
AppendItem
========================
*/
static void AppendItem( idStr &list, const char *separator, const idStr &item ) {
	if ( item.IsEmpty() ) return;
	if ( !list.IsEmpty() ) list += separator;
	list += item;
}

/*
========================
CloseTelemetry
========================
*/
static void CloseTelemetry() {
	delete report.gpuTelemetry;
	delete report.cpuTelemetry;
	report.gpuTelemetry = report.cpuTelemetry = NULL;
}

/*
========================
StartTelemetry
========================
*/
static void StartTelemetry() {
	report.telemetryStatus = "disabled";
	report.telemetrySource.Clear();
	report.telemetrySensors.Clear();
	report.telemetryFailure.Clear();
	report.telemetryUnavailable.Clear();
	report.stopTelemetry.store( false );
	report.telemetryWriteFailed.store( false );
	report.sensorError.store( false );
	if ( !r_ptCaptureTelemetry.GetBool() ) return;
	report.telemetryStatus = "unavailable";

	// match by the backend's PCI address; GL benchmarks only know the GL renderer/vendor
	ptGpuTelemetryTarget_t target;
	if ( report.haveDevice ) {
		target.name = report.device.name;
		target.pciBusId = report.device.pciBusId;
		target.vendorId = report.device.vendorId ? report.device.vendorId : PT_GPU_VendorFromName( report.device.name );
	} else if ( report.openGL ) {
		const char *renderer = glConfig.renderer_string ? glConfig.renderer_string : "";
		const char *vendor = glConfig.vendor_string ? glConfig.vendor_string : "";
		target.name = renderer;
		target.vendorId = PT_GPU_VendorFromName( idStr( vendor ) + " " + renderer );
	}
	idStr failure;
	if ( target.name.IsEmpty() && target.pciBusId.IsEmpty() ) {
		AppendItem( report.telemetryFailure, "; ", idStr( "gpu: device unknown" ) );
	} else {
		report.gpuTelemetry = PT_GPU_OpenTelemetry( target, failure );
		if ( !report.gpuTelemetry ) AppendItem( report.telemetryFailure, "; ", "gpu: " + failure );
	}
	report.cpuTelemetry = PT_CPU_OpenTelemetry( failure );
	if ( !report.cpuTelemetry ) AppendItem( report.telemetryFailure, "; ", "cpu: " + failure );

	idGpuTelemetry *const providers[2] = { report.gpuTelemetry, report.cpuTelemetry };
	for ( int i = 0; i < 2; i++ ) {
		if ( !providers[i] ) continue;
		AppendItem( report.telemetrySource, "+", providers[i]->Source() );
		AppendItem( report.telemetrySensors, ",", PT_GPU_OfferedSensors( *providers[i] ) );
		AppendItem( report.telemetryUnavailable, ";", providers[i]->Unavailable() );
	}
	if ( !report.telemetryUnavailable.IsEmpty() ) {
		common->Printf( "[ptreport] telemetry sensors not available: %s\n", report.telemetryUnavailable.c_str() );
	}

	if ( report.gpuTelemetry || report.cpuTelemetry ) {
#ifdef D3_SDL1
		report.thread = SDL_CreateThread( TelemetryWorker, NULL );
#else
		report.thread = SDL_CreateThread( TelemetryWorker, "pt-telemetry", NULL );
#endif
		if ( report.thread ) {
			report.telemetryStatus = "sampling";
			common->Printf( "[ptreport] telemetry: %s (%s)\n", report.telemetrySource.c_str(), report.telemetrySensors.c_str() );
			if ( !report.gpuTelemetry ) {
				common->Warning( "GPU telemetry unavailable for this capture (%s); CPU sensors are still sampled", report.telemetryFailure.c_str() );
			}
			return;
		}
		report.telemetryFailure = va( "telemetry thread could not start: %s", SDL_GetError() );
		CloseTelemetry();
		report.telemetrySource.Clear();
		report.telemetrySensors.Clear();
	}
	common->Warning( "telemetry unavailable for this capture (%s); rendering and frame reporting continue",
		report.telemetryFailure.c_str() );
}

/*
========================
StopTelemetry
========================
*/
static void StopTelemetry() {
	report.stopTelemetry.store( true );
	if ( report.thread ) { SDL_WaitThread( report.thread, NULL ); report.thread = NULL; }
	if ( report.telemetryWriteFailed.load() ) {
		report.telemetryStatus = "write_error";
		common->Warning( "PT telemetry write failed; partial telemetry retained" );
	} else if ( report.telemetryStatus == "sampling" ) {
		report.telemetryStatus = report.sensorError.load() ? "complete_partial_sensors" : "complete";
	}
	CloseTelemetry();
}

/*
========================
R_PT_ReportActive
========================
*/
bool R_PT_ReportActive() { return report.active; }

/*
========================
R_PT_ReportBenchmarkOptions
========================
*/
void R_PT_ReportBenchmarkOptions( bool uncapped, bool saveImages ) {
	report.benchmarkPacing = uncapped ? 1 : 0;
	report.benchmarkSaveImages = saveImages ? 1 : 0;
}
const char *R_PT_ReportDirectory() { return report.active ? report.directory.c_str() : NULL; }

void R_PT_ReportSettings() {
	if ( !report.active ) return;
	const idDict current = Settings();
	for ( int i = 0; i < current.GetNumKeyVals(); i++ ) {
		const idKeyValue *kv = current.GetKeyVal( i );
		const idKeyValue *old = report.effective.FindKey( kv->GetKey() );
		if ( old && old->GetValue() == kv->GetValue() ) continue;
		const idStr row = Number( ( Sys_MillisecondsPrecise() - report.start ) * 0.001 ) + ","
			+ PhaseName( report.phase.load() ) + "," + PT_ReportCsvString( kv->GetKey().c_str() )
			+ "," + PT_ReportCsvString( kv->GetValue().c_str() ) + "\n";
		if ( !Write( report.settings, row ) ) {
			report.failed = true;
			common->Warning( "PT capture settings report write failed" );
			break;
		}
	}
	report.effective = current;
}

/*
========================
R_PT_ReportBegin
========================
*/
bool R_PT_ReportBegin( const idCmdArgs &args, const char *mode, const char *source, int requestedSpp, int startFrame, int endFrame ) {
	if ( report.active ) {
		common->Warning( "A PT capture report is already active" );
		return false;
	}
	if ( !r_ptCaptureReport.GetBool() ) return true;
	report.openGL = idStr::Icmp( mode, "glbenchmark" ) == 0;
	const idStr parent = report.openGL ? "screenshots/glreports/benchmark" : idStr( "screenshots/ptreports/" ) + mode;
	idFileList *dirs = fileSystem->ListFiles( parent, "/" );
	bool found = false;
	for ( int i = 1; i <= 99999; i++ ) {
		const idStr name = va( "run_%05d", i );
		if ( dirs->GetList().FindIndex( name ) < 0 ) { report.directory = parent + "/" + name; found = true; break; }
	}
	fileSystem->FreeFileList( dirs );
	if ( !found ) { common->Warning( "No free PT capture report directory" ); return false; }
	report.failed = report.frameActive = report.sensorWarning = false;
	report.telemetryStatus = "not_started";
	report.telemetryWriteFailed.store( false );
	report.sensorError.store( false );
	report.mode = report.openGL ? "benchmark" : mode; report.source = source;
	idStr command;
	for ( int i = 0; i < args.Argc(); ++i ) {
		if ( i ) command += " ";
		const bool redact = i > 0 && PT_ReportSensitiveName( args.Argv( i - 1 ) );
		command += PT_ReportJsonString( redact ? idStr( "[redacted]" ) : PT_ReportRedactArgument( args.Argv( i ) ) );
	}
	report.command = command;
	report.requestedSpp = requestedSpp; report.startFrame = startFrame; report.endFrame = endFrame;
	report.benchmarkPacing = report.benchmarkSaveImages = -1;
	report.captureBatch = report.openGL ? 1 : PT_BenchmarkBatchSize( requestedSpp, cvarSystem->GetCVarInteger( "r_ptSamplesPerPixel" ) );
	report.rows = 0; report.totalGpu = report.totalWall = 0; report.start = Sys_MillisecondsPrecise();
	report.startedUnix = time( NULL );
	report.phase.store( 0 ); report.telemetryFrame.store( startFrame );
	report.original = Settings(); report.effective.Clear();
	report.sourceFingerprint.Clear(); report.cpuModel.Clear();
#ifdef __linux__
	// /proc files report a zero size, so idFile cannot read them
	if ( FILE *cpu = fopen( "/proc/cpuinfo", "r" ) ) {
		char text[512];
		while ( fgets( text, sizeof( text ), cpu ) ) {
			const char *colon = strchr( text, ':' );
			if ( idStr::Cmpn( text, "model name", 10 ) != 0 || !colon ) continue;
			report.cpuModel = colon + 1;
			report.cpuModel.StripLeading( ' ' );
			report.cpuModel.StripTrailingWhitespace();
			break;
		}
		fclose( cpu );
	}
#endif
	report.haveDevice = false;
#ifdef HAVE_PATHTRACER
	if ( !report.openGL ) report.haveDevice = R_PT_GetDeviceInfo( report.device );
#endif
	if ( !report.haveDevice && !report.openGL ) common->Warning( "PT capture GPU information unavailable" );
	if ( !report.mode.Icmp( "benchmark" ) || report.openGL ) {
		idFile *demo = fileSystem->OpenFileRead( source );
		if ( demo ) {
			uint64_t hash = 14695981039346656037ULL;
			byte bytes[65536];
			int count;
			while ( ( count = demo->Read( bytes, sizeof( bytes ) ) ) > 0 ) {
				for ( int i = 0; i < count; i++ ) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
			}
			fileSystem->CloseFile( demo );
			if ( count < 0 ) common->Warning( "PT capture demo fingerprint read failed" );
			else report.sourceFingerprint = idStr::Format( "%llx", (unsigned long long)hash );
		} else common->Warning( "PT capture demo fingerprint unavailable" );
	}
	report.frames = fileSystem->OpenFileWrite( report.directory + "/frames.csv" );
	report.settings = fileSystem->OpenFileWrite( report.directory + "/settings.csv" );
	report.telemetry = fileSystem->OpenFileWrite( report.directory + "/telemetry.csv" );
	idFile *const files[3] = { report.frames, report.settings, report.telemetry };
	for ( int i = 0; i < 3; i++ ) {
		if ( files[i] ) files[i]->ForceFlush();
	}
	report.active = true;
	const bool headers = Write( report.frames, "frame,source_frame,source_time_s,phase,spp,dispatches,trace_width,trace_height,image_width,image_height,gpu_ms,tracing_ms,denoising_ms,postprocess_ms,tonemap_ms,scene_ms,backend_wall_ms,readback_ms,screen_render_ms,image_save_ms,frame_wall_ms,compute_fps,end_to_end_fps,primary_samples_per_s,triangles,lights,backend_memory_bytes,image_file\n" )
		&& Write( report.settings, "elapsed_s,phase,key,value\n" )
		&& Write( report.telemetry, TelemetryHeader() );
	if ( !headers ) {
		common->Warning( "PT capture report files could not be opened/written" );
		report.failed = true; R_PT_ReportEnd( false, "report_open_failed" ); return false;
	}
	common->Printf( "[ptreport] %s/ (local files only)\n", report.directory.c_str() );
	for ( int i = 0; i < report.original.GetNumKeyVals(); i++ ) {
		const idKeyValue *kv = report.original.GetKeyVal( i );
		common->Printf( "[ptreport] before: %s = %s\n", kv->GetKey().c_str(), kv->GetValue().c_str() );
	}
	StartTelemetry();
	R_PT_ReportSettings();
	if ( !Metadata( "running", "" ) || report.failed ) {
		report.failed = true; R_PT_ReportEnd( false, "report_metadata_failed" ); return false;
	}
	return true;
}

/*
========================
R_PT_ReportPhase
========================
*/
void R_PT_ReportPhase( const char *phase ) {
	if ( report.active ) report.phase.store( PhaseIndex( phase ) );
}

/*
========================
R_PT_ReportFrameBegin
========================
*/
void R_PT_ReportFrameBegin( int sourceFrame, double sourceTimeSeconds, const char *phase ) {
	if ( !report.active ) return;
	if ( report.frameActive ) {
		report.failed = true;
		common->Warning( "PT capture report frame started before the preceding frame was completed" );
		return;
	}
	report.frameActive = true;
	report.framePathtrace = PhaseIndex( phase ) == 2;
	report.sourceFrame = sourceFrame; report.sourceTime = sourceTimeSeconds;
	report.telemetryFrame.store( sourceFrame ); R_PT_ReportPhase( phase );
	report.frameStart = Sys_MillisecondsPrecise();
	report.gpu = report.path = report.denoise = report.postprocess = report.toneMap = 0;
	report.scene = report.backend = report.readback = 0;
	report.width = report.height = report.spp = report.dispatches = 0;
	report.lastStats = {};
	R_PT_ReportSettings();
	if ( report.rows == 0 && !Metadata( "running", "" ) ) report.failed = true;
}

/*
========================
R_PT_ReportRendered
========================
*/
void R_PT_ReportRendered( const ptRenderParams_t &params, const ptBackendStats_t &stats,
	double sceneMilliseconds, double backendMilliseconds, double readbackMilliseconds ) {
	if ( !report.active || !report.frameActive ) return;
	R_PT_ReportSettings();
	report.width = params.width; report.height = params.height;
	report.spp += params.samplesPerPixel; report.dispatches++;
	report.gpu += stats.traceMs; report.path += stats.pathMs; report.denoise += stats.denoiseMs;
	report.postprocess += stats.postprocessMs; report.toneMap += stats.toneMapMs;
	report.scene += sceneMilliseconds; report.backend += backendMilliseconds; report.readback += readbackMilliseconds;
	report.lastStats = stats;
}

/*
========================
R_PT_ReportFrameEnd
========================
*/
bool R_PT_ReportFrameEnd( const char *image, int imageWidth, int imageHeight, double imageMilliseconds, double screenMilliseconds ) {
	if ( !report.active ) return true;
	if ( !report.frameActive ) {
		report.failed = true;
		common->Warning( "PT capture report frame ended without a matching frame start" );
		return false;
	}
	if ( report.failed ) return false;
	if ( report.framePathtrace ? ( report.dispatches == 0 || report.spp != report.requestedSpp ) : report.dispatches != 0 ) {
		report.failed = true;
		common->Warning( "PT capture report expected %d spp, received %d in %d dispatches",
			report.requestedSpp, report.spp, report.dispatches );
		return false;
	}
	const double wall = Sys_MillisecondsPrecise() - report.frameStart;
	const bool pt = report.dispatches > 0;
	if ( !pt ) { report.width = imageWidth; report.height = imageHeight; }
	const idStr gpu = pt ? Number( report.gpu ) : idStr();
	const double computeFps = report.gpu > 0 ? 1000.0 / report.gpu : 0;
	idStr row = Integer( report.rows + 1 ) + "," + Integer( report.sourceFrame ) + "," + Number( report.sourceTime )
		+ "," + ( pt ? "pathtrace" : "opengl" ) + "," + Integer( report.spp ) + "," + Integer( report.dispatches )
		+ "," + Integer( report.width ) + "," + Integer( report.height ) + "," + Integer( imageWidth ) + "," + Integer( imageHeight )
		+ "," + gpu + "," + ( pt ? Number( report.path ) : "" ) + "," + ( pt ? Number( report.denoise ) : "" )
		+ "," + ( pt ? Number( report.postprocess ) : "" ) + "," + ( pt ? Number( report.toneMap ) : "" )
		+ "," + ( pt ? Number( report.scene ) : "" ) + "," + ( pt ? Number( report.backend ) : "" )
		+ "," + ( pt ? Number( report.readback ) : "" ) + "," + ( screenMilliseconds >= 0 ? Number( screenMilliseconds ) : "" )
		+ "," + Number( imageMilliseconds ) + "," + Number( wall ) + "," + ( pt ? Number( computeFps ) : "" )
		+ "," + Number( wall > 0 ? 1000.0 / wall : 0 )
		+ "," + ( pt ? Number( (double)report.width * report.height * report.spp * computeFps ) : "" )
		+ "," + ( pt ? Integer( report.lastStats.numTriangles ) : idStr() )
		+ "," + ( pt ? Integer( report.lastStats.numLights ) : idStr() )
		+ "," + ( pt ? idStr::Format( "%llu", (unsigned long long)report.lastStats.deviceBytes ) : idStr() ) + "," + PT_ReportCsvString( image ) + "\n";
	if ( !Write( report.frames, row ) ) {
		report.failed = true; common->Warning( "PT capture frame report write failed" ); return false;
	}
	report.rows++; report.totalGpu += report.gpu; report.totalWall += wall;
	report.frameActive = false;
	return true;
}

/*
========================
R_PT_ReportTick
========================
*/
void R_PT_ReportTick() {
	if ( !report.active ) return;
	if ( !report.sensorWarning && report.sensorError.load() ) {
		report.sensorWarning = true;
		common->Warning( "Some GPU telemetry sensors are unavailable; see telemetry.csv sensor_errors" );
	}
}

/*
========================
R_PT_ReportEnd
========================
*/
void R_PT_ReportEnd( bool completed, const char *reason ) {
	if ( !report.active ) return;
	if ( report.settings && !report.failed ) R_PT_ReportSettings();
	StopTelemetry();
	if ( !Metadata( report.failed ? "report_error" : ( completed ? "completed" : "aborted" ), reason ) ) report.failed = true;
	idFile *const files[3] = { report.frames, report.settings, report.telemetry };
	for ( int i = 0; i < 3; i++ ) {
		if ( files[i] ) fileSystem->CloseFile( files[i] );
	}
	report.frames = report.settings = report.telemetry = NULL;
	report.original.Clear();
	report.effective.Clear();
	common->Printf( "[ptreport] %s: %d rows in %s/\n", report.failed ? "write error" : ( completed ? "complete" : "partial" ), report.rows, report.directory.c_str() );
	report.active = report.frameActive = false;
}
