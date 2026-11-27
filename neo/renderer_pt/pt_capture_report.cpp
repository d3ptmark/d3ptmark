// STL headers must come before idlib: idlib/Str.h redefines snprintf/vsnprintf
// (use_idStr_snPrintf), which breaks the using-declarations in MSVC's <cstdio>.
#include <atomic>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <ctime>

#include "sys/platform.h"

#include "framework/Session.h"
#include "renderer/tr_local.h"
#include "renderer_pt/pt_capture_report.h"
#include "renderer_pt/pt_render.h"
#include "renderer_pt/pt_benchmark.h"
#include "sys/sys_sdl.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef HAVE_NVML_CAPTURE
#include <nvml.h>
#endif

static idCVar r_ptCaptureReport( "r_ptCaptureReport", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
	"write local settings, invocation and frame reports for PT captures and GL benchmarks" );
static idCVar r_ptCaptureTelemetry( "r_ptCaptureTelemetry", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
	"sample supported NVIDIA GPU sensors at approximately 1 Hz during renderer captures" );

static std::string Number( double value ) {
	std::ostringstream out;
	out.imbue( std::locale::classic() );
	out << std::fixed << std::setprecision( 6 ) << value;
	return out.str();
}

static bool Write( idFile *file, const std::string &text ) {
	return file && file->Write( text.data(), (int)text.size() ) == (int)text.size();
}

static int PhaseIndex( const char *phase ) {
	if ( !idStr::Cmp( phase, "opengl" ) ) return 1;
	if ( !idStr::Cmp( phase, "pathtrace" ) ) return 2;
	if ( !idStr::Cmp( phase, "image_save" ) ) return 3;
	if ( !idStr::Cmp( phase, "advance" ) ) return 4;
	return 0;
}

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
	idDict original, effective;
	ptBackendDeviceInfo_t device = {};
	bool haveDevice = false;
	int requestedSpp = 0, startFrame = 0, endFrame = 0, rows = 0, sourceFrame = 0;
	int captureBatch = 1;
	int width = 0, height = 0, spp = 0, dispatches = 0;
	double start = 0, frameStart = 0, sourceTime = 0;
	std::time_t startedUnix = 0;
	double gpu = 0, path = 0, denoise = 0, postprocess = 0, toneMap = 0;
	double scene = 0, backend = 0, readback = 0, totalGpu = 0, totalWall = 0;
	ptBackendStats_t lastStats = {};
	std::atomic<int> phase{0}, telemetryFrame{0};
	std::atomic<bool> stopTelemetry{false}, telemetryWriteFailed{false}, sensorError{false};
	SDL_Thread *thread = NULL;
#ifdef HAVE_NVML_CAPTURE
	decltype( SDL_LoadObject( "" ) ) library = NULL;
	nvmlDevice_t nvmlDevice = NULL;
	decltype( &nvmlInit_v2 ) init = NULL;
	decltype( &nvmlShutdown ) shutdown = NULL;
	decltype( &nvmlDeviceGetHandleByPciBusId_v2 ) deviceByPci = NULL;
	decltype( &nvmlDeviceGetTemperature ) temperature = NULL;
	decltype( &nvmlDeviceGetUtilizationRates ) utilization = NULL;
	decltype( &nvmlDeviceGetPowerUsage ) power = NULL;
	decltype( &nvmlDeviceGetClockInfo ) clock = NULL;
	decltype( &nvmlDeviceGetMemoryInfo ) memory = NULL;
	decltype( &nvmlDeviceGetFanSpeed ) fan = NULL;
	decltype( &nvmlDeviceGetCurrentClocksThrottleReasons ) throttle = NULL;
	bool nvmlInitialized = false;
#endif
} report;

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

static std::string DictJson( const idDict &dict ) {
	std::string result = "{";
	for ( int i = 0; i < dict.GetNumKeyVals(); i++ ) {
		const idKeyValue *kv = dict.GetKeyVal( i );
		if ( i ) result += ",";
		result += "\n    " + PT_ReportJsonString( kv->GetKey().c_str() ) + ":" + PT_ReportJsonString( kv->GetValue().c_str() );
	}
	return result + "\n  }";
}

static std::string StartupJson() {
	const idStrList &args = Com_GetStartupArguments();
	std::string result = "[";
	for ( int i = 0; i < args.Num(); i++ ) {
		if ( i ) result += ",";
		const bool redact = i > 0 && PT_ReportSensitiveName( args[i - 1].c_str() );
		result += PT_ReportJsonString( redact ? "[redacted]" : PT_ReportRedactArgument( args[i].c_str() ) );
	}
	return result + "]";
}

static bool Metadata( const char *status, const char *reason ) {
	std::string text = "{\n  \"schema_version\":1,\n  \"status\":" + PT_ReportJsonString( status )
		+ ",\n  \"reason\":" + PT_ReportJsonString( reason )
		+ ",\n  \"mode\":" + PT_ReportJsonString( report.mode.c_str() )
		+ ",\n  \"source\":" + PT_ReportJsonString( report.source.c_str() )
		+ ",\n  \"started_unix_s\":" + std::to_string( report.startedUnix )
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
#elif defined(D3_SDL3)
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
		+ ",\n  \"telemetry_interval_ms\":1000,\n  \"gpu_stage_timing\":" + ( report.openGL ? "false" : "true" )
		+ ",\n  \"requested_spp\":" + ( report.openGL ? "null" : Number( report.requestedSpp ) )
		+ ",\n  \"capture_batch_spp\":" + ( report.openGL ? "null" : std::to_string( report.captureBatch ) )
		+ ",\n  \"capture_target_dispatches\":" + ( report.openGL ? "null" : std::to_string( report.requestedSpp / report.captureBatch ) )
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
	const bool replaced = ok && std::rename( from, to ) == 0;
#endif
	if ( !replaced ) {
		common->Warning( "PT capture report metadata could not be written/replaced" );
		fileSystem->RemoveFile( temporary );
	}
	return replaced;
}

#ifdef HAVE_NVML_CAPTURE
static int SDLCALL TelemetryWorker( void * ) {
	while ( !report.stopTelemetry.load() ) {
		std::string errors;
		auto sensor = [&]( const char *name, nvmlReturn_t result, double value ) {
			if ( result == NVML_SUCCESS ) return Number( value );
			if ( !errors.empty() ) errors += ";";
			errors += std::string( name ) + ":" + Number( result );
			report.sensorError.store( true );
			return std::string();
		};
		const double elapsed = ( Sys_MillisecondsPrecise() - report.start ) * 0.001;
		std::string row = Number( elapsed ) + "," + Number( report.telemetryFrame.load() ) + ","
			+ PhaseName( report.phase.load() );
		unsigned int value = 0;
		nvmlUtilization_t use = {};
		nvmlMemory_t mem = {};
		unsigned long long reasons = 0;
		nvmlReturn_t rc = report.temperature ? report.temperature( report.nvmlDevice, NVML_TEMPERATURE_GPU, &value ) : NVML_ERROR_NOT_SUPPORTED;
		row += "," + sensor( "temperature", rc, value );
		rc = report.power ? report.power( report.nvmlDevice, &value ) : NVML_ERROR_NOT_SUPPORTED;
		row += "," + sensor( "power", rc, value * 0.001 );
		rc = report.clock ? report.clock( report.nvmlDevice, NVML_CLOCK_SM, &value ) : NVML_ERROR_NOT_SUPPORTED;
		row += "," + sensor( "sm_clock", rc, value );
		rc = report.clock ? report.clock( report.nvmlDevice, NVML_CLOCK_MEM, &value ) : NVML_ERROR_NOT_SUPPORTED;
		row += "," + sensor( "memory_clock", rc, value );
		rc = report.utilization ? report.utilization( report.nvmlDevice, &use ) : NVML_ERROR_NOT_SUPPORTED;
		row += "," + sensor( "utilization", rc, use.gpu );
		rc = report.memory ? report.memory( report.nvmlDevice, &mem ) : NVML_ERROR_NOT_SUPPORTED;
		row += "," + sensor( "memory", rc, (double)mem.used );
		rc = report.fan ? report.fan( report.nvmlDevice, &value ) : NVML_ERROR_NOT_SUPPORTED;
		row += "," + sensor( "fan", rc, value );
		rc = report.throttle ? report.throttle( report.nvmlDevice, &reasons ) : NVML_ERROR_NOT_SUPPORTED;
		row += "," + ( rc == NVML_SUCCESS ? std::to_string( reasons ) : sensor( "clock_reasons", rc, 0 ) );
		row += "," + PT_ReportCsvString( errors ) + "\n";
		if ( !Write( report.telemetry, row ) ) { report.telemetryWriteFailed.store( true ); break; }
		for ( int i = 0; i < 10 && !report.stopTelemetry.load(); i++ ) SDL_Delay( 100 );
	}
	return 0;
}
#endif

static void StartTelemetry() {
	report.telemetryStatus = "disabled";
	report.stopTelemetry.store( false );
	report.telemetryWriteFailed.store( false );
	report.sensorError.store( false );
	if ( !r_ptCaptureTelemetry.GetBool() ) return;
	report.telemetryStatus = "unavailable";
#ifdef HAVE_NVML_CAPTURE
#ifdef _WIN32
	report.library = SDL_LoadObject( "nvml.dll" );
#else
	report.library = SDL_LoadObject( "libnvidia-ml.so.1" );
#endif
	if ( report.library && report.haveDevice ) {
#define PT_NVML_LOAD(member, symbol) report.member = reinterpret_cast<decltype(report.member)>( SDL_LoadFunction( report.library, symbol ) )
		PT_NVML_LOAD( init, "nvmlInit_v2" );
		PT_NVML_LOAD( shutdown, "nvmlShutdown" );
		PT_NVML_LOAD( deviceByPci, "nvmlDeviceGetHandleByPciBusId_v2" );
		if ( report.init && report.shutdown && report.deviceByPci ) {
			report.nvmlInitialized = report.init() == NVML_SUCCESS;
			if ( report.nvmlInitialized && report.deviceByPci( report.device.pciBusId, &report.nvmlDevice ) == NVML_SUCCESS ) {
				PT_NVML_LOAD( temperature, "nvmlDeviceGetTemperature" );
				PT_NVML_LOAD( power, "nvmlDeviceGetPowerUsage" );
				PT_NVML_LOAD( clock, "nvmlDeviceGetClockInfo" );
				PT_NVML_LOAD( utilization, "nvmlDeviceGetUtilizationRates" );
				PT_NVML_LOAD( memory, "nvmlDeviceGetMemoryInfo" );
				PT_NVML_LOAD( fan, "nvmlDeviceGetFanSpeed" );
				PT_NVML_LOAD( throttle, "nvmlDeviceGetCurrentClocksThrottleReasons" );
#ifdef D3_SDL1
				report.thread = SDL_CreateThread( TelemetryWorker, NULL );
#else
				report.thread = SDL_CreateThread( TelemetryWorker, "pt-telemetry", NULL );
#endif
				if ( report.thread ) { report.telemetryStatus = "sampling"; return; }
				common->Warning( "PT telemetry thread could not start: %s", SDL_GetError() );
			}
		}
#undef PT_NVML_LOAD
	}
#endif
	common->Warning( "GPU telemetry unavailable for this capture; rendering and frame reporting continue" );
}

static void StopTelemetry() {
	report.stopTelemetry.store( true );
	if ( report.thread ) { SDL_WaitThread( report.thread, NULL ); report.thread = NULL; }
	if ( report.telemetryWriteFailed.load() ) {
		report.telemetryStatus = "write_error";
		common->Warning( "PT GPU telemetry write failed; partial telemetry retained" );
	} else if ( report.telemetryStatus == "sampling" ) {
		report.telemetryStatus = report.sensorError.load() ? "complete_partial_sensors" : "complete";
	}
#ifdef HAVE_NVML_CAPTURE
	if ( report.nvmlInitialized ) report.shutdown();
	report.nvmlInitialized = false;
	if ( report.library ) SDL_UnloadObject( report.library );
	report.library = NULL;
	report.nvmlDevice = NULL;
#endif
}

bool R_PT_ReportActive() { return report.active; }
const char *R_PT_ReportDirectory() { return report.active ? report.directory.c_str() : NULL; }

void R_PT_ReportSettings() {
	if ( !report.active ) return;
	const idDict current = Settings();
	for ( int i = 0; i < current.GetNumKeyVals(); i++ ) {
		const idKeyValue *kv = current.GetKeyVal( i );
		const idKeyValue *old = report.effective.FindKey( kv->GetKey() );
		if ( old && old->GetValue() == kv->GetValue() ) continue;
		const std::string row = Number( ( Sys_MillisecondsPrecise() - report.start ) * 0.001 ) + ","
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
	std::string command;
	for ( int i = 0; i < args.Argc(); ++i ) {
		if ( i ) command += " ";
		const bool redact = i > 0 && PT_ReportSensitiveName( args.Argv( i - 1 ) );
		command += PT_ReportJsonString( redact ? "[redacted]" : PT_ReportRedactArgument( args.Argv( i ) ) );
	}
	report.command = command.c_str();
	report.requestedSpp = requestedSpp; report.startFrame = startFrame; report.endFrame = endFrame;
	report.captureBatch = report.openGL ? 1 : PT_BenchmarkBatchSize( requestedSpp, cvarSystem->GetCVarInteger( "r_ptSamplesPerPixel" ) );
	report.rows = 0; report.totalGpu = report.totalWall = 0; report.start = Sys_MillisecondsPrecise();
	report.startedUnix = std::time( NULL );
	report.phase.store( 0 ); report.telemetryFrame.store( startFrame );
	report.original = Settings(); report.effective.Clear();
	report.sourceFingerprint.Clear(); report.cpuModel.Clear();
#ifdef __linux__
	std::ifstream cpu( "/proc/cpuinfo" );
	std::string line;
	while ( std::getline( cpu, line ) ) {
		if ( line.compare( 0, 10, "model name" ) == 0 && line.find( ':' ) != std::string::npos ) {
			report.cpuModel = line.substr( line.find( ':' ) + 1 ).c_str(); report.cpuModel.StripLeading( ' ' ); break;
		}
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
			else { std::ostringstream hex; hex << std::hex << hash; report.sourceFingerprint = hex.str().c_str(); }
		} else common->Warning( "PT capture demo fingerprint unavailable" );
	}
	report.frames = fileSystem->OpenFileWrite( report.directory + "/frames.csv" );
	report.settings = fileSystem->OpenFileWrite( report.directory + "/settings.csv" );
	report.telemetry = fileSystem->OpenFileWrite( report.directory + "/telemetry.csv" );
	for ( idFile *file : { report.frames, report.settings, report.telemetry } ) {
		if ( file ) file->ForceFlush();
	}
	report.active = true;
	const bool headers = Write( report.frames, "frame,source_frame,source_time_s,phase,spp,dispatches,trace_width,trace_height,image_width,image_height,gpu_ms,tracing_ms,denoising_ms,postprocess_ms,tonemap_ms,scene_ms,backend_wall_ms,readback_ms,screen_render_ms,image_save_ms,frame_wall_ms,compute_fps,end_to_end_fps,primary_samples_per_s,triangles,lights,backend_memory_bytes,image_file\n" )
		&& Write( report.settings, "elapsed_s,phase,key,value\n" )
		&& Write( report.telemetry, "elapsed_s,source_frame,phase,gpu_temperature_c,gpu_power_w,gpu_sm_clock_mhz,gpu_memory_clock_mhz,gpu_utilization_pct,gpu_memory_used_bytes,gpu_fan_pct,gpu_clock_limit_reasons,sensor_errors\n" );
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

void R_PT_ReportPhase( const char *phase ) {
	if ( report.active ) report.phase.store( PhaseIndex( phase ) );
}

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
	const std::string gpu = pt ? Number( report.gpu ) : "";
	const double computeFps = report.gpu > 0 ? 1000.0 / report.gpu : 0;
	std::string row = std::to_string( report.rows + 1 ) + "," + std::to_string( report.sourceFrame ) + "," + Number( report.sourceTime )
		+ "," + ( pt ? "pathtrace" : "opengl" ) + "," + std::to_string( report.spp ) + "," + std::to_string( report.dispatches )
		+ "," + std::to_string( report.width ) + "," + std::to_string( report.height ) + "," + std::to_string( imageWidth ) + "," + std::to_string( imageHeight )
		+ "," + gpu + "," + ( pt ? Number( report.path ) : "" ) + "," + ( pt ? Number( report.denoise ) : "" )
		+ "," + ( pt ? Number( report.postprocess ) : "" ) + "," + ( pt ? Number( report.toneMap ) : "" )
		+ "," + ( pt ? Number( report.scene ) : "" ) + "," + ( pt ? Number( report.backend ) : "" )
		+ "," + ( pt ? Number( report.readback ) : "" ) + "," + ( screenMilliseconds >= 0 ? Number( screenMilliseconds ) : "" )
		+ "," + Number( imageMilliseconds ) + "," + Number( wall ) + "," + ( pt ? Number( computeFps ) : "" )
		+ "," + Number( wall > 0 ? 1000.0 / wall : 0 )
		+ "," + ( pt ? Number( (double)report.width * report.height * report.spp * computeFps ) : "" )
		+ "," + ( pt ? std::to_string( report.lastStats.numTriangles ) : "" )
		+ "," + ( pt ? std::to_string( report.lastStats.numLights ) : "" )
		+ "," + ( pt ? std::to_string( report.lastStats.deviceBytes ) : "" ) + "," + PT_ReportCsvString( image ) + "\n";
	if ( !Write( report.frames, row ) ) {
		report.failed = true; common->Warning( "PT capture frame report write failed" ); return false;
	}
	report.rows++; report.totalGpu += report.gpu; report.totalWall += wall;
	report.frameActive = false;
	return true;
}

void R_PT_ReportTick() {
	if ( !report.active ) return;
	if ( !report.sensorWarning && report.sensorError.load() ) {
		report.sensorWarning = true;
		common->Warning( "Some GPU telemetry sensors are unavailable; see telemetry.csv sensor_errors" );
	}
}

void R_PT_ReportEnd( bool completed, const char *reason ) {
	if ( !report.active ) return;
	if ( report.settings && !report.failed ) R_PT_ReportSettings();
	StopTelemetry();
	if ( !Metadata( report.failed ? "report_error" : ( completed ? "completed" : "aborted" ), reason ) ) report.failed = true;
	for ( idFile *file : { report.frames, report.settings, report.telemetry } ) {
		if ( file ) fileSystem->CloseFile( file );
	}
	report.frames = report.settings = report.telemetry = NULL;
	report.original.Clear();
	report.effective.Clear();
	common->Printf( "[ptreport] %s: %d rows in %s/\n", report.failed ? "write error" : ( completed ? "complete" : "partial" ), report.rows, report.directory.c_str() );
	report.active = report.frameActive = false;
}
