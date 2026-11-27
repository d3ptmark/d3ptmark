#ifdef _WIN32
// the engine targets XP (_WIN32_WINNT 0x0501); this file only uses what it
// loads at runtime, so declare a modern target for the DXGI headers
#undef WINVER
#undef _WIN32_WINNT
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "renderer_pt/pt_gpu_telemetry.h"
#include "idlib/containers/List.h"
#include "idlib/containers/StrList.h"
#include "idlib/Lib.h"

const char *const ptGpuSensorNames[PT_GPU_SENSOR_COUNT] = {
	"temperature", "power", "core_clock", "memory_clock", "utilization",
	"memory", "fan", "clock_reasons", "power_pct", "fan_rpm",
	"cpu_temperature", "cpu_utilization", "cpu_power"
};

const char *const ptGpuSensorColumns[PT_GPU_SENSOR_COUNT] = {
	"gpu_temperature_c", "gpu_power_w", "gpu_sm_clock_mhz", "gpu_memory_clock_mhz", "gpu_utilization_pct",
	"gpu_memory_used_bytes", "gpu_fan_pct", "gpu_clock_limit_reasons", "gpu_power_pct", "gpu_fan_rpm",
	"cpu_temperature_c", "cpu_utilization_pct", "cpu_power_w"
};

static const unsigned int PT_PCI_VENDOR_NVIDIA	= 0x10de;
static const unsigned int PT_PCI_VENDOR_AMD		= 0x1002;
static const unsigned int PT_PCI_VENDOR_INTEL	= 0x8086;

void ptGpuTelemetrySample_t::Clear() {
	for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) {
		valid[i] = false;
		value[i] = 0.0;
	}
	errors.Clear();
}

void ptGpuTelemetrySample_t::Fail( ptGpuSensor_t sensor, const char *reason ) {
	valid[sensor] = false;
	if ( !errors.IsEmpty() ) errors += ";";
	errors += ptGpuSensorNames[sensor];
	errors += ":";
	errors += reason;
}

void idGpuTelemetry::NotOffered( ptGpuSensor_t sensor, const char *reason ) {
	if ( !unavailable.IsEmpty() ) unavailable += ";";
	unavailable += ptGpuSensorNames[sensor];
	unavailable += ":";
	unavailable += reason;
}

// monotonic seconds; Sys_MillisecondsPrecise is engine code, and the tests link idlib only
static double Seconds() {
#ifdef _WIN32
	LARGE_INTEGER frequency, counter;
	QueryPerformanceFrequency( &frequency );
	QueryPerformanceCounter( &counter );
	return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
	struct timespec now;
	clock_gettime( CLOCK_MONOTONIC, &now );
	return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
#endif
}

static idStr Lower( const idStr &text ) {
	idStr result = text;
	result.ToLower();
	return result;
}

static bool Contains( const idStr &text, const char *what ) {
	return text.Find( what ) >= 0;
}

static bool StartsWith( const idStr &text, const char *prefix ) {
	return idStr::Cmpn( text.c_str(), prefix, idStr::Length( prefix ) ) == 0;
}

static bool EndsWith( const idStr &text, const char *suffix ) {
	const int length = idStr::Length( suffix );
	return text.Length() >= length && idStr::Cmp( text.c_str() + text.Length() - length, suffix ) == 0;
}

// the characters from start to the end
static idStr From( const idStr &text, int start ) {
	return text.Mid( start, text.Length() - start );
}

static idStr Trim( const idStr &text ) {
	int begin = 0, end = text.Length();
	while ( begin < end && (unsigned char)text[begin] <= ' ' ) begin++;
	while ( end > begin && (unsigned char)text[end - 1] <= ' ' ) end--;
	return text.Mid( begin, end - begin );
}

static void SplitLines( const idStr &text, idStrList &lines ) {
	lines.Clear();
	int start = 0;
	while ( start < text.Length() ) {
		int end = text.Find( '\n', start );
		if ( end < 0 ) end = text.Length();
		lines.Append( text.Mid( start, end - start ) );
		start = end + 1;
	}
}

// case-insensitive equality or containment, so "Radeon RX 6800" matches "AMD Radeon RX 6800"
static bool SameDeviceName( const char *a, const char *b ) {
	const idStr x = Lower( Trim( a ) ), y = Lower( Trim( b ) );
	if ( x.IsEmpty() || y.IsEmpty() ) return false;
	return x == y || ( x.Length() >= 6 && Contains( y, x.c_str() ) ) || ( y.Length() >= 6 && Contains( x, y.c_str() ) );
}

static idStr Code( long long code ) {
	return idStr::Format( "%lld", code );
}

bool PT_GPU_ParsePciBusId( const char *text, unsigned int &domain, unsigned int &bus,
		unsigned int &device, unsigned int &function ) {
	unsigned int d = 0, b = 0, s = 0, f = 0;
	char tail = 0;
	if ( !text ) return false;
	if ( sscanf( text, "%x:%x:%x.%x%c", &d, &b, &s, &f, &tail ) == 4 ) {
	} else if ( sscanf( text, "%x:%x.%x%c", &b, &s, &f, &tail ) == 3 ) {
		d = 0;
	} else {
		return false;
	}
	if ( b > 0xff || s > 0x1f || f > 7 ) return false;
	domain = d; bus = b; device = s; function = f;
	return true;
}

unsigned int PT_GPU_VendorFromName( const char *name ) {
	const idStr text = " " + Lower( name ) + " ";
	if ( Contains( text, "nvidia" ) || Contains( text, "geforce" ) || Contains( text, "quadro" )
		|| Contains( text, "tesla" ) ) return PT_PCI_VENDOR_NVIDIA;
	if ( Contains( text, "intel" ) ) return PT_PCI_VENDOR_INTEL;
	if ( Contains( text, " amd" ) || Contains( text, "radeon" ) || Contains( text, " ati " )
		|| Contains( text, "advanced micro devices" ) ) return PT_PCI_VENDOR_AMD;
	return 0;
}

idStr PT_GPU_OfferedSensors( const idGpuTelemetry &telemetry ) {
	idStr result;
	for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) {
		if ( !telemetry.Offers( (ptGpuSensor_t)i ) ) continue;
		if ( !result.IsEmpty() ) result += ",";
		result += ptGpuSensorNames[i];
	}
	return result;
}

/*
===============================================================================

	Runtime library loading

===============================================================================
*/

#ifdef _WIN32
typedef HMODULE ptLibrary_t;
static ptLibrary_t PT_LoadLibrary( const char *name ) { return LoadLibraryA( name ); }
static void PT_FreeLibrary( ptLibrary_t library ) { if ( library ) FreeLibrary( library ); }
template<typename T> static bool PT_LoadSymbol( ptLibrary_t library, const char *name, T &function ) {
	function = reinterpret_cast<T>( reinterpret_cast<void *>( GetProcAddress( library, name ) ) );
	return function != NULL;
}
#else
typedef void *ptLibrary_t;
static ptLibrary_t PT_LoadLibrary( const char *name ) { return dlopen( name, RTLD_NOW | RTLD_LOCAL ); }
static void PT_FreeLibrary( ptLibrary_t library ) { if ( library ) dlclose( library ); }
template<typename T> static bool PT_LoadSymbol( ptLibrary_t library, const char *name, T &function ) {
	function = reinterpret_cast<T>( dlsym( library, name ) );
	return function != NULL;
}
#endif

/*
===============================================================================

	NVML provider. The handful of NVML declarations used here are stable ABI,
	so neither nvml.h nor the CUDA toolkit is needed to build it.

===============================================================================
*/

namespace ptnvml {
typedef int result_t;
typedef struct device_s *device_t;
struct utilization_t { unsigned int gpu, memory; };
struct memory_t { unsigned long long total, free, used; };
enum {
	SUCCESS = 0,
	ERROR_NOT_SUPPORTED = 3,
	TEMPERATURE_GPU = 0,
	CLOCK_SM = 1,
	CLOCK_MEM = 2
};
}

class idGpuTelemetryNVML : public idGpuTelemetry {
public:
	idGpuTelemetryNVML() : library( NULL ), device( NULL ), initialized( false ),
		init( NULL ), shutdown( NULL ), byPci( NULL ), count( NULL ), byIndex( NULL ), name( NULL ),
		temperature( NULL ), power( NULL ), clock( NULL ), utilization( NULL ), memory( NULL ),
		fan( NULL ), reasons( NULL ) {
		for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) offered[i] = false;
	}
	~idGpuTelemetryNVML() {
		if ( initialized ) shutdown();
		PT_FreeLibrary( library );
	}

	const char *Source() const { return "nvml"; }
	bool Offers( ptGpuSensor_t sensor ) const { return offered[sensor]; }

	bool Open( const ptGpuTelemetryTarget_t &target, idStr &failure ) {
		if ( target.vendorId && target.vendorId != PT_PCI_VENDOR_NVIDIA ) { failure = "not an NVIDIA device"; return false; }
#ifdef _WIN32
		library = PT_LoadLibrary( "nvml.dll" );
		if ( !library ) {
			// drivers before R460 install NVML only under NVSMI
			char programFiles[MAX_PATH];
			DWORD length = GetEnvironmentVariableA( "ProgramW6432", programFiles, sizeof( programFiles ) );
			if ( length == 0 || length >= sizeof( programFiles ) ) length = GetEnvironmentVariableA( "ProgramFiles", programFiles, sizeof( programFiles ) );
			if ( length > 0 && length < sizeof( programFiles ) ) {
				library = PT_LoadLibrary( ( idStr( programFiles ) + "\\NVIDIA Corporation\\NVSMI\\nvml.dll" ).c_str() );
			}
		}
#else
		library = PT_LoadLibrary( "libnvidia-ml.so.1" );
#endif
		if ( !library ) { failure = "NVML library not found"; return false; }
		if ( !PT_LoadSymbol( library, "nvmlInit_v2", init ) || !PT_LoadSymbol( library, "nvmlShutdown", shutdown ) ) {
			failure = "NVML library incomplete"; return false;
		}
		const ptnvml::result_t initResult = init();
		if ( initResult != ptnvml::SUCCESS ) { failure = "nvmlInit failed: " + Code( initResult ); return false; }
		initialized = true;

		PT_LoadSymbol( library, "nvmlDeviceGetHandleByPciBusId_v2", byPci );
		PT_LoadSymbol( library, "nvmlDeviceGetCount_v2", count );
		PT_LoadSymbol( library, "nvmlDeviceGetHandleByIndex_v2", byIndex );
		PT_LoadSymbol( library, "nvmlDeviceGetName", name );
		if ( !SelectDevice( target ) ) { failure = "no NVML device matches"; return false; }

		PT_LoadSymbol( library, "nvmlDeviceGetTemperature", temperature );
		PT_LoadSymbol( library, "nvmlDeviceGetPowerUsage", power );
		PT_LoadSymbol( library, "nvmlDeviceGetClockInfo", clock );
		PT_LoadSymbol( library, "nvmlDeviceGetUtilizationRates", utilization );
		PT_LoadSymbol( library, "nvmlDeviceGetMemoryInfo", memory );
		PT_LoadSymbol( library, "nvmlDeviceGetFanSpeed", fan );
		if ( !PT_LoadSymbol( library, "nvmlDeviceGetCurrentClocksEventReasons", reasons ) ) {
			PT_LoadSymbol( library, "nvmlDeviceGetCurrentClocksThrottleReasons", reasons );
		}

		// sensors the device reports as unsupported (e.g. laptop fans) are not offered
		ptGpuTelemetrySample_t probe;
		for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) offered[i] = true;
		offered[PT_GPU_POWER_PCT] = offered[PT_GPU_FAN_RPM] = false;
		for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) {
			if ( !offered[i] ) continue;
			offered[i] = Read( (ptGpuSensor_t)i, probe ) != ptnvml::ERROR_NOT_SUPPORTED;
			if ( !offered[i] ) NotOffered( (ptGpuSensor_t)i, "not supported by this device/driver" );
		}
		return true;
	}

	void Sample( ptGpuTelemetrySample_t &sample ) {
		for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) {
			if ( !offered[i] ) continue;
			const ptnvml::result_t result = Read( (ptGpuSensor_t)i, sample );
			if ( result != ptnvml::SUCCESS ) sample.Fail( (ptGpuSensor_t)i, Code( result ) );
		}
	}

private:
	bool SelectDevice( const ptGpuTelemetryTarget_t &target ) {
		if ( !target.pciBusId.IsEmpty() && byPci && byPci( target.pciBusId.c_str(), &device ) == ptnvml::SUCCESS ) return true;
		unsigned int devices = 0;
		if ( !count || !byIndex || count( &devices ) != ptnvml::SUCCESS ) return false;
		int matches = 0;
		ptnvml::device_t named = NULL, only = NULL;
		for ( unsigned int i = 0; i < devices; i++ ) {
			ptnvml::device_t candidate = NULL;
			if ( byIndex( i, &candidate ) != ptnvml::SUCCESS ) continue;
			only = candidate;
			char text[96] = {};
			if ( name && name( candidate, text, sizeof( text ) ) == ptnvml::SUCCESS && SameDeviceName( text, target.name ) ) {
				named = candidate;
				matches++;
			}
		}
		if ( matches == 1 ) { device = named; return true; }
		// a single NVIDIA GPU is unambiguous when the target is known to be NVIDIA
		if ( matches == 0 && devices == 1 && only && target.vendorId == PT_PCI_VENDOR_NVIDIA ) { device = only; return true; }
		return false;
	}

	ptnvml::result_t Read( ptGpuSensor_t sensor, ptGpuTelemetrySample_t &sample ) {
		unsigned int value = 0;
		ptnvml::result_t result = ptnvml::ERROR_NOT_SUPPORTED;
		switch ( sensor ) {
		case PT_GPU_TEMPERATURE_C:
			if ( temperature ) result = temperature( device, ptnvml::TEMPERATURE_GPU, &value );
			if ( result == ptnvml::SUCCESS ) sample.Set( sensor, value );
			break;
		case PT_GPU_POWER_W:
			if ( power ) result = power( device, &value );
			if ( result == ptnvml::SUCCESS ) sample.Set( sensor, value * 0.001 );
			break;
		case PT_GPU_CORE_CLOCK_MHZ:
			if ( clock ) result = clock( device, ptnvml::CLOCK_SM, &value );
			if ( result == ptnvml::SUCCESS ) sample.Set( sensor, value );
			break;
		case PT_GPU_MEMORY_CLOCK_MHZ:
			if ( clock ) result = clock( device, ptnvml::CLOCK_MEM, &value );
			if ( result == ptnvml::SUCCESS ) sample.Set( sensor, value );
			break;
		case PT_GPU_UTILIZATION_PCT: {
			ptnvml::utilization_t use = {};
			if ( utilization ) result = utilization( device, &use );
			if ( result == ptnvml::SUCCESS ) sample.Set( sensor, use.gpu );
			break;
		}
		case PT_GPU_MEMORY_USED_BYTES: {
			ptnvml::memory_t mem = {};
			if ( memory ) result = memory( device, &mem );
			if ( result == ptnvml::SUCCESS ) sample.Set( sensor, (double)mem.used );
			break;
		}
		case PT_GPU_FAN_PCT:
			if ( fan ) result = fan( device, &value );
			if ( result == ptnvml::SUCCESS ) sample.Set( sensor, value );
			break;
		case PT_GPU_CLOCK_LIMIT_REASONS: {
			unsigned long long bits = 0;
			if ( reasons ) result = reasons( device, &bits );
			if ( result == ptnvml::SUCCESS ) sample.Set( sensor, (double)bits );
			break;
		}
		default:
			break;
		}
		return result;
	}

	ptLibrary_t			library;
	ptnvml::device_t	device;
	bool				initialized;
	bool				offered[PT_GPU_SENSOR_COUNT];

	ptnvml::result_t	( *init )();
	ptnvml::result_t	( *shutdown )();
	ptnvml::result_t	( *byPci )( const char *, ptnvml::device_t * );
	ptnvml::result_t	( *count )( unsigned int * );
	ptnvml::result_t	( *byIndex )( unsigned int, ptnvml::device_t * );
	ptnvml::result_t	( *name )( ptnvml::device_t, char *, unsigned int );
	ptnvml::result_t	( *temperature )( ptnvml::device_t, int, unsigned int * );
	ptnvml::result_t	( *power )( ptnvml::device_t, unsigned int * );
	ptnvml::result_t	( *clock )( ptnvml::device_t, int, unsigned int * );
	ptnvml::result_t	( *utilization )( ptnvml::device_t, ptnvml::utilization_t * );
	ptnvml::result_t	( *memory )( ptnvml::device_t, ptnvml::memory_t * );
	ptnvml::result_t	( *fan )( ptnvml::device_t, unsigned int * );
	ptnvml::result_t	( *reasons )( ptnvml::device_t, unsigned long long * );
};

/*
===============================================================================

	Linux sysfs provider: amdgpu, i915 and xe expose clocks, power/energy,
	temperatures and fans through hwmon plus driver specific attributes.

===============================================================================
*/

#ifndef _WIN32

// sysfs and procfs files report a size of 0 (or 4096), so read until EOF
static bool ReadText( const char *path, idStr &text ) {
	text.Clear();
	FILE *file = fopen( path, "r" );
	if ( !file ) return false;
	char buffer[4096];
	size_t count;
	while ( ( count = fread( buffer, 1, sizeof( buffer ), file ) ) > 0 ) text.Append( buffer, (int)count );
	const bool ok = !ferror( file );
	fclose( file );
	return ok;
}

static bool ReadNumber( const char *path, double &value ) {
	idStr text;
	if ( !ReadText( path, text ) ) return false;
	const char *begin = text.c_str();
	char *end = NULL;
	const double parsed = strtod( begin, &end );
	if ( end == begin ) return false;
	value = parsed;
	return true;
}

// sorted entries, without "." names
static void ListDirectory( const char *path, idStrList &entries ) {
	entries.Clear();
	DIR *directory = opendir( path );
	if ( !directory ) return;
	while ( struct dirent *entry = readdir( directory ) ) {
		if ( entry->d_name[0] != '.' ) entries.Append( entry->d_name );
	}
	closedir( directory );
	entries.Sort();
}

static idStr UeventValue( const idStr &uevent, const char *key ) {
	idStrList lines;
	SplitLines( uevent, lines );
	const idStr prefix = idStr( key ) + "=";
	for ( int i = 0; i < lines.Num(); i++ ) {
		if ( StartsWith( lines[i], prefix ) ) return Trim( From( lines[i], prefix.Length() ) );
	}
	return "";
}

static bool IsCardEntry( const idStr &name ) {
	if ( !StartsWith( name, "card" ) || name.Length() == 4 ) return false;
	for ( int i = 4; i < name.Length(); i++ ) if ( name[i] < '0' || name[i] > '9' ) return false;
	return true;
}

// MHz of the active level ("1: 1800Mhz *") in an amdgpu pp_dpm_* table
static bool ParseActiveDpm( const idStr &table, double &mhz ) {
	idStrList lines;
	SplitLines( table, lines );
	for ( int i = 0; i < lines.Num(); i++ ) {
		const idStr &line = lines[i];
		if ( line.Find( '*' ) < 0 ) continue;
		const int colon = line.Find( ':' );
		if ( colon < 0 ) continue;
		const char *begin = line.c_str() + colon + 1;
		char *end = NULL;
		const double value = strtod( begin, &end );
		if ( end == begin ) continue;
		mhz = value;
		return true;
	}
	return false;
}

static bool Exists( const idStr &path ) {
	return !path.IsEmpty() && access( path.c_str(), F_OK ) == 0;
}

// why a sensor file is unusable: missing, or present but not readable by this user
static idStr Unreadable( const idStr &path, const char *missing ) {
	return Exists( path ) ? path + " is readable by root only" : idStr( missing );
}

/*
	DRM fdinfo (Documentation/gpu/drm-usage-stats.rst): per client engine busy
	time ("drm-engine-render: 123 ns", i915 and amdgpu), or busy/total GPU
	cycles ("drm-cycles-rcs" / "drm-total-cycles-rcs", xe), and resident memory
	per region ("drm-resident-local0: 512 KiB"). Only this process's clients are
	read, which is what the benchmark puts on the GPU.
*/
struct ptDrmEngine_t {
	idStr		name;
	double		busyNs;			// summed over clients
	double		cycles;			// busy cycles, summed over clients
	double		totalCycles;	// GPU timestamp cycles
	bool		haveBusy, haveCycles, haveTotal;

	ptDrmEngine_t() : busyNs( 0.0 ), cycles( 0.0 ), totalCycles( 0.0 ), haveBusy( false ), haveCycles( false ), haveTotal( false ) {}
};

struct ptDrmUsage_t {
	idList<ptDrmEngine_t>	engines;
	double					localBytes, systemBytes;
	bool					haveLocal, haveMemory;
	int						clients;

	ptDrmUsage_t() : localBytes( 0.0 ), systemBytes( 0.0 ), haveLocal( false ), haveMemory( false ), clients( 0 ) {}
	bool HaveEngines() const {
		for ( int i = 0; i < engines.Num(); i++ ) if ( engines[i].haveBusy || engines[i].haveCycles ) return true;
		return false;
	}
	double MemoryBytes() const { return haveLocal ? localBytes : systemBytes; }
	const ptDrmEngine_t *Find( const idStr &name ) const {
		for ( int i = 0; i < engines.Num(); i++ ) if ( engines[i].name == name ) return &engines[i];
		return NULL;
	}
	ptDrmEngine_t &Engine( const idStr &name ) {
		for ( int i = 0; i < engines.Num(); i++ ) if ( engines[i].name == name ) return engines[i];
		ptDrmEngine_t engine;
		engine.name = name;
		return engines[engines.Append( engine )];
	}
};

static double DrmBytes( const idStr &value ) {
	const char *begin = value.c_str();
	char *end = NULL;
	double bytes = strtod( begin, &end );
	if ( end == begin ) return -1.0;
	const idStr unit = Trim( end );
	if ( unit == "KiB" ) bytes *= 1024.0;
	else if ( unit == "MiB" ) bytes *= 1024.0 * 1024.0;
	else if ( unit == "GiB" ) bytes *= 1024.0 * 1024.0 * 1024.0;
	else if ( !unit.IsEmpty() ) return -1.0;
	return bytes;
}

static void ParseDrmFdinfo( const idStr &text, const idStr &pdev, idStrList &seenClients, ptDrmUsage_t &usage ) {
	idStrList lines, keys, values;
	SplitLines( text, lines );
	idStr driver, device, client;
	bool resident = false;
	for ( int i = 0; i < lines.Num(); i++ ) {
		const int colon = lines[i].Find( ':' );
		if ( colon < 0 || !StartsWith( lines[i], "drm-" ) ) continue;
		keys.Append( lines[i].Left( colon ) );
		values.Append( Trim( From( lines[i], colon + 1 ) ) );
		const idStr &key = keys[keys.Num() - 1];
		if ( key == "drm-driver" ) driver = values[values.Num() - 1];
		else if ( key == "drm-pdev" ) device = values[values.Num() - 1];
		else if ( key == "drm-client-id" ) client = values[values.Num() - 1];
		resident = resident || StartsWith( key, "drm-resident-" );
	}
	if ( driver.IsEmpty() || device.Icmp( pdev ) != 0 ) return;
	// several fds (dup, fork) can share one client; count it once
	if ( !client.IsEmpty() ) {
		if ( seenClients.FindIndex( client ) >= 0 ) return;
		seenClients.Append( client );
	}
	usage.clients++;
	for ( int i = 0; i < keys.Num(); i++ ) {
		const idStr &key = keys[i];
		const double value = strtod( values[i].c_str(), NULL );
		if ( StartsWith( key, "drm-engine-" ) && !StartsWith( key, "drm-engine-capacity-" ) ) {
			ptDrmEngine_t &engine = usage.Engine( From( key, 11 ) );
			engine.busyNs += value;
			engine.haveBusy = true;
		} else if ( StartsWith( key, "drm-total-cycles-" ) ) {
			ptDrmEngine_t &engine = usage.Engine( From( key, 17 ) );
			engine.totalCycles = Max( engine.totalCycles, value );
			engine.haveTotal = true;
		} else if ( StartsWith( key, "drm-cycles-" ) ) {
			ptDrmEngine_t &engine = usage.Engine( From( key, 11 ) );
			engine.cycles += value;
			engine.haveCycles = true;
		} else if ( StartsWith( key, "drm-resident-" ) || ( !resident && StartsWith( key, "drm-memory-" ) ) ) {
			// drm-memory-<region> is what amdgpu reported before drm-resident-*
			const idStr region = From( key, key.Find( '-', 4 ) + 1 );
			const double bytes = DrmBytes( values[i] );
			if ( bytes < 0.0 ) continue;
			usage.haveMemory = true;
			// i915 integrated GPUs list the firmware carve-out as "stolen-local0"; it is not device memory
			if ( ( Contains( region, "vram" ) || Contains( region, "local" ) ) && !Contains( region, "stolen" ) ) { usage.localBytes += bytes; usage.haveLocal = true; }
			else usage.systemBytes += bytes;	// only used when the device has no local memory
		}
	}
}

// busiest engine between two readings, in percent; false if nothing to compare
static bool DrmUtilization( const ptDrmUsage_t &before, const ptDrmUsage_t &after, double seconds, double &percent ) {
	bool any = false;
	percent = 0.0;
	for ( int i = 0; i < after.engines.Num(); i++ ) {
		const ptDrmEngine_t &now = after.engines[i];
		const ptDrmEngine_t *old = before.Find( now.name );
		if ( !old ) continue;
		if ( now.haveBusy && old->haveBusy && seconds > 0.0 && now.busyNs >= old->busyNs ) {
			percent = Max( percent, ( now.busyNs - old->busyNs ) * 1e-9 / seconds * 100.0 );
			any = true;
		}
		if ( now.haveCycles && old->haveCycles && now.haveTotal && old->haveTotal
			&& now.cycles >= old->cycles && now.totalCycles > old->totalCycles ) {
			percent = Max( percent, ( now.cycles - old->cycles ) / ( now.totalCycles - old->totalCycles ) * 100.0 );
			any = true;
		}
	}
	percent = Min( percent, 100.0 );
	return any;
}

class idGpuTelemetrySysfs : public idGpuTelemetry {
public:
	idGpuTelemetrySysfs() : drmUtilization( false ), drmMemory( false ), havePreviousDrm( false ), previousDrmTime( 0.0 ) {
		for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) offered[i] = false;
	}

	const char *Source() const { return source.c_str(); }
	bool Offers( ptGpuSensor_t sensor ) const { return offered[sensor]; }

	bool Open( const idStr &root, const ptGpuTelemetryTarget_t &target, idStr &failure, const idStr &procSelf ) {
		idStr device, uevent;
		unsigned int domain, bus, slot, function;
		if ( !target.pciBusId.IsEmpty() && PT_GPU_ParsePciBusId( target.pciBusId, domain, bus, slot, function ) ) {
			const idStr path = root + idStr::Format( "/bus/pci/devices/%04x:%02x:%02x.%x", domain, bus, slot, function );
			if ( ReadText( path + "/uevent", uevent ) ) device = path;
		}
		if ( device.IsEmpty() && !FindByVendor( root, target, device, failure ) ) return false;

		ReadText( device + "/uevent", uevent );
		const idStr driver = UeventValue( uevent, "DRIVER" );
		if ( driver != "amdgpu" && driver != "i915" && driver != "xe" ) {
			failure = "unsupported driver '" + driver + "'";
			return false;
		}
		source = driver + "_sysfs";
		fdinfoDirectory = procSelf + "/fdinfo";
		pdev = UeventValue( uevent, "PCI_SLOT_NAME" );
		if ( pdev.IsEmpty() ) pdev = From( device, device.Last( '/' ) + 1 );

		idStr card, hwmon;
		idStrList entries;
		ListDirectory( device + "/drm", entries );
		for ( int i = 0; i < entries.Num() && card.IsEmpty(); i++ ) if ( IsCardEntry( entries[i] ) ) card = device + "/drm/" + entries[i];
		ListDirectory( device + "/hwmon", entries );
		if ( entries.Num() ) hwmon = device + "/hwmon/" + entries[0];

		if ( driver == "amdgpu" ) {
			Add( PT_GPU_TEMPERATURE_C, VALUE, 0.001, Join( hwmon, "temp1_input" ) );
			Add( PT_GPU_POWER_W, VALUE, 1e-6, Join( hwmon, "power1_average" ), Join( hwmon, "power1_input" ) );
			if ( !Add( PT_GPU_CORE_CLOCK_MHZ, VALUE, 1e-6, Join( hwmon, "freq1_input" ) ) ) Add( PT_GPU_CORE_CLOCK_MHZ, DPM, 1.0, device + "/pp_dpm_sclk" );
			if ( !Add( PT_GPU_MEMORY_CLOCK_MHZ, VALUE, 1e-6, Join( hwmon, "freq2_input" ) ) ) Add( PT_GPU_MEMORY_CLOCK_MHZ, DPM, 1.0, device + "/pp_dpm_mclk" );
			Add( PT_GPU_UTILIZATION_PCT, VALUE, 1.0, device + "/gpu_busy_percent" );
			Add( PT_GPU_MEMORY_USED_BYTES, VALUE, 1.0, device + "/mem_info_vram_used" );
			AddPwm( Join( hwmon, "pwm1" ), Join( hwmon, "pwm1_max" ) );
			Add( PT_GPU_FAN_RPM, VALUE, 1.0, Join( hwmon, "fan1_input" ) );
		} else {
			if ( driver == "xe" ) {
				Add( PT_GPU_CORE_CLOCK_MHZ, VALUE, 1.0, device + "/tile0/gt0/freq0/act_freq", device + "/tile0/gt0/freq0/cur_freq" );
			} else {
				Add( PT_GPU_CORE_CLOCK_MHZ, VALUE, 1.0, Join( card, "gt_act_freq_mhz" ), Join( card, "gt/gt0/rps_act_freq_mhz" ), Join( card, "gt_cur_freq_mhz" ) );
			}
			// discrete GPUs expose an energy counter; integrated ones only through
			// the RAPL uncore domain, which is usually readable by root only
			if ( !Add( PT_GPU_POWER_W, ENERGY, 1e-6, Join( hwmon, "energy1_input" ), Join( hwmon, "energy2_input" ) ) ) {
				idStr rapl;
				const idStr uncore = root + "/class/powercap/intel-rapl:0:1";
				if ( ReadText( uncore + "/name", rapl ) && Trim( rapl ) == "uncore" ) {
					if ( !Add( PT_GPU_POWER_W, ENERGY, 1e-6, uncore + "/energy_uj" ) ) {
						NotOffered( PT_GPU_POWER_W, Unreadable( uncore + "/energy_uj", "RAPL uncore energy counter missing" ) );
					}
				} else {
					NotOffered( PT_GPU_POWER_W, "no hwmon energy counter or RAPL uncore domain" );
				}
			}
			entries.Clear();
			if ( !hwmon.IsEmpty() ) ListDirectory( hwmon, entries );
			for ( int i = 0; i < entries.Num() && !offered[PT_GPU_TEMPERATURE_C]; i++ ) {
				if ( StartsWith( entries[i], "temp" ) && entries[i].Length() > 6 && EndsWith( entries[i], "_input" ) ) {
					Add( PT_GPU_TEMPERATURE_C, VALUE, 0.001, Join( hwmon, entries[i] ) );
				}
			}
			Add( PT_GPU_FAN_RPM, VALUE, 1.0, Join( hwmon, "fan1_input" ) );
			if ( !offered[PT_GPU_TEMPERATURE_C] ) {
				NotOffered( PT_GPU_TEMPERATURE_C, hwmon.IsEmpty() ? "no GPU hwmon (integrated GPU, see cpu_temperature)" : "no hwmon temperature" );
			}
		}
		if ( !offered[PT_GPU_MEMORY_USED_BYTES] && driver == "amdgpu" ) NotOffered( PT_GPU_MEMORY_USED_BYTES, "mem_info_vram_used missing" );
		OpenDrmUsage();

		bool any = false;
		for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) any = any || offered[i];
		if ( !any ) { failure = "no readable " + driver + " sensors"; return false; }
		return true;
	}

	void Sample( ptGpuTelemetrySample_t &sample ) {
		const double now = Seconds();
		for ( int i = 0; i < sensors.Num(); i++ ) {
			sensor_t &s = sensors[i];
			double value = 0.0;
			switch ( s.kind ) {
			case VALUE:
				if ( ReadNumber( s.path, value ) ) sample.Set( s.id, value * s.scale );
				else sample.Fail( s.id, "unreadable" );
				break;
			case DPM: {
				idStr table;
				if ( ReadText( s.path, table ) && ParseActiveDpm( table, value ) ) sample.Set( s.id, value * s.scale );
				else sample.Fail( s.id, "unreadable" );
				break;
			}
			case PWM: {
				double maximum = 255.0;
				if ( !s.path2.IsEmpty() && ( !ReadNumber( s.path2, maximum ) || maximum <= 0.0 ) ) maximum = 255.0;
				if ( ReadNumber( s.path, value ) ) sample.Set( s.id, value * 100.0 / maximum );
				else sample.Fail( s.id, "unreadable" );
				break;
			}
			case ENERGY:
				if ( !ReadNumber( s.path, value ) ) { sample.Fail( s.id, "unreadable" ); s.havePrevious = false; break; }
				// counter wrap or first sample: no rate this time
				if ( s.havePrevious && value >= s.previousEnergy && now > s.previousTime ) {
					sample.Set( s.id, ( value - s.previousEnergy ) * s.scale / ( now - s.previousTime ) );
				}
				s.previousEnergy = value;
				s.previousTime = now;
				s.havePrevious = true;
				break;
			}
		}
		if ( drmUtilization || drmMemory ) SampleDrmUsage( sample, now );
	}

private:
	// device-wide attributes are preferred; this process's DRM clients fill the
	// gaps (i915 and xe have no utilization or memory attribute)
	void OpenDrmUsage() {
		if ( offered[PT_GPU_UTILIZATION_PCT] && offered[PT_GPU_MEMORY_USED_BYTES] ) return;
		ptDrmUsage_t usage;
		ReadDrmUsage( usage );
		const idStr none = usage.clients ? idStr( "the driver's fdinfo has no " ) : "no DRM client of this process on " + pdev + " has ";
		if ( !offered[PT_GPU_UTILIZATION_PCT] ) {
			drmUtilization = offered[PT_GPU_UTILIZATION_PCT] = usage.HaveEngines();
			if ( !drmUtilization ) NotOffered( PT_GPU_UTILIZATION_PCT, none + "engine usage" );
		}
		if ( !offered[PT_GPU_MEMORY_USED_BYTES] ) {
			drmMemory = offered[PT_GPU_MEMORY_USED_BYTES] = usage.haveMemory;
			if ( !drmMemory ) NotOffered( PT_GPU_MEMORY_USED_BYTES, none + "memory usage" );
		}
		if ( drmUtilization || drmMemory ) source += "+fdinfo";
		previousDrm = usage;
		previousDrmTime = Seconds();
		havePreviousDrm = true;
	}

	void ReadDrmUsage( ptDrmUsage_t &usage ) const {
		idStrList clients, entries;
		ListDirectory( fdinfoDirectory, entries );
		for ( int i = 0; i < entries.Num(); i++ ) {
			idStr text;
			if ( ReadText( fdinfoDirectory + "/" + entries[i], text ) && Contains( text, "drm-driver:" ) ) {
				ParseDrmFdinfo( text, pdev, clients, usage );
			}
		}
	}

	void SampleDrmUsage( ptGpuTelemetrySample_t &sample, double now ) {
		ptDrmUsage_t usage;
		ReadDrmUsage( usage );
		if ( drmUtilization ) {
			double percent = 0.0;
			if ( !usage.clients ) sample.Fail( PT_GPU_UTILIZATION_PCT, "no_drm_client" );
			else if ( havePreviousDrm && DrmUtilization( previousDrm, usage, now - previousDrmTime, percent ) ) sample.Set( PT_GPU_UTILIZATION_PCT, percent );
			// a client that appeared since the last reading has no rate yet
		}
		if ( drmMemory ) {
			if ( usage.haveMemory ) sample.Set( PT_GPU_MEMORY_USED_BYTES, usage.MemoryBytes() );
			else sample.Fail( PT_GPU_MEMORY_USED_BYTES, "no_drm_client" );
		}
		previousDrm = usage;
		previousDrmTime = now;
		havePreviousDrm = true;
	}

	enum kind_t { VALUE, DPM, PWM, ENERGY };
	struct sensor_t {
		ptGpuSensor_t	id;
		kind_t			kind;
		double			scale;
		idStr			path, path2;
		bool			havePrevious;
		double			previousEnergy, previousTime;
	};

	// empty when the directory is missing, so the candidate is skipped
	static idStr Join( const idStr &directory, const char *file ) {
		return directory.IsEmpty() ? idStr() : directory + "/" + file;
	}

	static bool Readable( kind_t kind, const idStr &path ) {
		if ( path.IsEmpty() ) return false;
		if ( kind == DPM ) {
			idStr table;
			double mhz;
			return ReadText( path, table ) && ParseActiveDpm( table, mhz );
		}
		double value;
		return ReadNumber( path, value );
	}

	// adds the first readable candidate path
	bool Add( ptGpuSensor_t id, kind_t kind, double scale, const idStr &a,
			const idStr &b = idStr(), const idStr &c = idStr() ) {
		if ( offered[id] ) return true;
		const idStr *candidates[3] = { &a, &b, &c };
		for ( int i = 0; i < 3; i++ ) {
			if ( !Readable( kind, *candidates[i] ) ) continue;
			sensor_t s;
			s.id = id; s.kind = kind; s.scale = scale; s.path = *candidates[i];
			s.havePrevious = false; s.previousEnergy = 0.0; s.previousTime = 0.0;
			sensors.Append( s );
			offered[id] = true;
			return true;
		}
		return false;
	}

	void AddPwm( const idStr &pwm, const idStr &maximum ) {
		if ( !Readable( VALUE, pwm ) ) return;
		sensor_t s;
		s.id = PT_GPU_FAN_PCT; s.kind = PWM; s.scale = 1.0; s.path = pwm;
		s.path2 = Readable( VALUE, maximum ) ? maximum : idStr();
		s.havePrevious = false; s.previousEnergy = 0.0; s.previousTime = 0.0;
		sensors.Append( s );
		offered[PT_GPU_FAN_PCT] = true;
	}

	// no PCI address from the backend: accept the only supported GPU of the target's vendor
	static bool FindByVendor( const idStr &root, const ptGpuTelemetryTarget_t &target,
			idStr &device, idStr &failure ) {
		idStrList found, slots, cards;
		ListDirectory( root + "/class/drm", cards );
		for ( int i = 0; i < cards.Num(); i++ ) {
			if ( !IsCardEntry( cards[i] ) ) continue;
			const idStr path = root + "/class/drm/" + cards[i] + "/device";
			idStr uevent;
			if ( !ReadText( path + "/uevent", uevent ) ) continue;
			const idStr driver = UeventValue( uevent, "DRIVER" );
			if ( driver != "amdgpu" && driver != "i915" && driver != "xe" ) continue;
			unsigned int vendor = 0, product = 0;
			sscanf( UeventValue( uevent, "PCI_ID" ).c_str(), "%x:%x", &vendor, &product );
			if ( target.vendorId && vendor != target.vendorId ) continue;
			const idStr slot = UeventValue( uevent, "PCI_SLOT_NAME" );
			if ( slots.FindIndex( slot ) >= 0 ) continue;
			slots.Append( slot );
			found.Append( path );
		}
		if ( found.Num() == 1 ) { device = found[0]; return true; }
		failure = found.Num() == 0 ? "no amdgpu/i915/xe GPU found" : "several GPUs match and no PCI address is known";
		return false;
	}

	idStr						source;
	idList<sensor_t>			sensors;
	bool						offered[PT_GPU_SENSOR_COUNT];

	idStr						fdinfoDirectory, pdev;
	bool						drmUtilization, drmMemory, havePreviousDrm;
	ptDrmUsage_t				previousDrm;
	double						previousDrmTime;
};

#endif // !_WIN32

idGpuTelemetry *PT_GPU_OpenSysfsTelemetry( const char *sysRoot, const ptGpuTelemetryTarget_t &target, idStr &failure,
		const char *procSelf ) {
#ifdef _WIN32
	(void)sysRoot; (void)target; (void)procSelf;
	failure = "sysfs is Linux only";
	return NULL;
#else
	idGpuTelemetrySysfs *telemetry = new idGpuTelemetrySysfs();
	if ( telemetry->Open( sysRoot, target, failure, procSelf ) ) return telemetry;
	delete telemetry;
	return NULL;
#endif
}

/*
===============================================================================

	Linux CPU provider: utilization from /proc/stat, temperature from the CPU
	hwmon driver (or a CPU thermal zone), package power from the powercap RAPL
	energy counters (intel_rapl also serves AMD Zen). Since the PLATYPUS fixes
	energy_uj is readable by root only on most distributions.

===============================================================================
*/

#ifndef _WIN32

class idCpuTelemetryLinux : public idGpuTelemetry {
public:
	idCpuTelemetryLinux() : havePreviousStat( false ), previousBusy( 0.0 ), previousTotal( 0.0 ),
		havePreviousEnergy( false ), previousEnergyTime( 0.0 ) {
		for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) offered[i] = false;
	}

	const char *Source() const { return "linux_cpu"; }
	bool Offers( ptGpuSensor_t sensor ) const { return offered[sensor]; }

	bool Open( const idStr &sysRoot, const idStr &procRoot, idStr &failure ) {
		statPath = procRoot + "/stat";
		double busy, total;
		offered[PT_CPU_UTILIZATION_PCT] = ReadStat( busy, total );
		if ( !offered[PT_CPU_UTILIZATION_PCT] ) NotOffered( PT_CPU_UTILIZATION_PCT, statPath + " unreadable" );

		offered[PT_CPU_TEMPERATURE_C] = FindTemperature( sysRoot );
		if ( !offered[PT_CPU_TEMPERATURE_C] ) NotOffered( PT_CPU_TEMPERATURE_C, "no k10temp/zenpower/coretemp hwmon or CPU thermal zone" );

		FindPackageEnergy( sysRoot );
		offered[PT_CPU_POWER_W] = energyPaths.Num() > 0;

		if ( !offered[PT_CPU_UTILIZATION_PCT] && !offered[PT_CPU_TEMPERATURE_C] && !offered[PT_CPU_POWER_W] ) {
			failure = "no readable CPU sensors (" + unavailable + ")";
			return false;
		}
		return true;
	}

	void Sample( ptGpuTelemetrySample_t &sample ) {
		if ( offered[PT_CPU_UTILIZATION_PCT] ) {
			double busy, total;
			if ( !ReadStat( busy, total ) ) {
				sample.Fail( PT_CPU_UTILIZATION_PCT, "unreadable" );
				havePreviousStat = false;
			} else {
				if ( havePreviousStat && total > previousTotal && busy >= previousBusy ) {
					sample.Set( PT_CPU_UTILIZATION_PCT, Min( 100.0, ( busy - previousBusy ) * 100.0 / ( total - previousTotal ) ) );
				}
				previousBusy = busy;
				previousTotal = total;
				havePreviousStat = true;
			}
		}
		if ( offered[PT_CPU_TEMPERATURE_C] ) {
			double value;
			if ( ReadNumber( temperaturePath, value ) ) sample.Set( PT_CPU_TEMPERATURE_C, value * 0.001 );
			else sample.Fail( PT_CPU_TEMPERATURE_C, "unreadable" );
		}
		if ( offered[PT_CPU_POWER_W] ) SamplePower( sample );
	}

private:
	// jiffies: busy excludes idle and iowait; guest time is already in user
	bool ReadStat( double &busy, double &total ) const {
		idStr text;
		if ( !ReadText( statPath, text ) || !StartsWith( text, "cpu " ) ) return false;
		const char *p = text.c_str() + 4;
		double field[8] = {};
		int count = 0;
		while ( count < 8 ) {
			char *end = NULL;
			const double value = strtod( p, &end );
			if ( end == p || *p == '\n' ) break;
			field[count++] = value;
			p = end;
			if ( *p == '\n' ) break;
		}
		if ( count < 4 ) return false;
		total = 0.0;
		for ( int i = 0; i < count; i++ ) total += field[i];
		busy = total - field[3] - ( count > 4 ? field[4] : 0.0 );
		return true;
	}

	bool FindTemperature( const idStr &sysRoot ) {
		// preferred driver first; Tdie is Tctl without AMD's fan-curve offset
		static const char *const drivers[] = { "zenpower", "k10temp", "coretemp", "cpu_thermal" };
		static const char *const labels[] = { "Tdie", "Tctl", "Package id 0" };
		const idStr base = sysRoot + "/class/hwmon";
		idStrList monitors, entries;
		ListDirectory( base, monitors );
		for ( size_t d = 0; d < sizeof( drivers ) / sizeof( drivers[0] ); d++ ) {
			for ( int m = 0; m < monitors.Num(); m++ ) {
				const idStr hwmon = base + "/" + monitors[m];
				idStr name;
				if ( !ReadText( hwmon + "/name", name ) || Trim( name ) != drivers[d] ) continue;
				ListDirectory( hwmon, entries );
				for ( size_t l = 0; l < sizeof( labels ) / sizeof( labels[0] ); l++ ) {
					for ( int e = 0; e < entries.Num(); e++ ) {
						const idStr &entry = entries[e];
						idStr label;
						if ( !StartsWith( entry, "temp" ) || entry.Length() < 7 || !EndsWith( entry, "_label" ) ) continue;
						if ( !ReadText( hwmon + "/" + entry, label ) || Trim( label ) != labels[l] ) continue;
						if ( UseTemperature( hwmon + "/" + entry.Left( entry.Length() - 6 ) + "_input" ) ) return true;
					}
				}
				if ( UseTemperature( hwmon + "/temp1_input" ) ) return true;
			}
		}
		static const char *const zones[] = { "x86_pkg_temp", "cpu-thermal", "cpu_thermal" };
		const idStr thermal = sysRoot + "/class/thermal";
		ListDirectory( thermal, entries );
		for ( size_t z = 0; z < sizeof( zones ) / sizeof( zones[0] ); z++ ) {
			for ( int e = 0; e < entries.Num(); e++ ) {
				idStr type;
				if ( !StartsWith( entries[e], "thermal_zone" ) || !ReadText( thermal + "/" + entries[e] + "/type", type ) ) continue;
				if ( Trim( type ) == zones[z] && UseTemperature( thermal + "/" + entries[e] + "/temp" ) ) return true;
			}
		}
		return false;
	}

	bool UseTemperature( const idStr &path ) {
		double value;
		if ( !ReadNumber( path, value ) ) return false;
		temperaturePath = path;
		return true;
	}

	// every package domain (intel-rapl:N, not the intel-rapl:N:M subdomains), summed for multi-socket
	void FindPackageEnergy( const idStr &sysRoot ) {
		const idStr base = sysRoot + "/class/powercap";
		idStrList entries;
		ListDirectory( base, entries );
		idStr why = "no RAPL package domain in " + base;
		for ( int i = 0; i < entries.Num(); i++ ) {
			const idStr &entry = entries[i];
			// intel-rapl-mmio:N duplicates the package domain for power limits only
			int colons = 0;
			for ( int c = 0; c < entry.Length(); c++ ) colons += entry[c] == ':';
			if ( !StartsWith( entry, "intel-rapl:" ) || colons != 1 ) continue;
			const idStr domain = base + "/" + entry;
			idStr name;
			if ( !ReadText( domain + "/name", name ) || !StartsWith( Trim( name ), "package" ) ) continue;
			double value, range = 0.0;
			if ( !ReadNumber( domain + "/energy_uj", value ) ) {
				why = Unreadable( domain + "/energy_uj", "RAPL energy counter missing" );
				energyPaths.Clear();
				energyRanges.Clear();
				break;
			}
			ReadNumber( domain + "/max_energy_range_uj", range );
			energyPaths.Append( domain + "/energy_uj" );
			energyRanges.Append( range );
		}
		energyPrevious.AssureSize( energyPaths.Num(), 0.0 );
		if ( energyPaths.Num() == 0 ) NotOffered( PT_CPU_POWER_W, why );
	}

	void SamplePower( ptGpuTelemetrySample_t &sample ) {
		const double now = Seconds();
		idList<double> values;
		values.AssureSize( energyPaths.Num(), 0.0 );
		for ( int i = 0; i < energyPaths.Num(); i++ ) {
			if ( !ReadNumber( energyPaths[i], values[i] ) ) {
				sample.Fail( PT_CPU_POWER_W, "unreadable" );
				havePreviousEnergy = false;
				return;
			}
		}
		if ( havePreviousEnergy && now > previousEnergyTime ) {
			double joules = 0.0;
			bool ok = true;
			for ( int i = 0; i < values.Num(); i++ ) {
				double delta = values[i] - energyPrevious[i];
				if ( delta < 0.0 ) {
					if ( energyRanges[i] <= 0.0 ) { ok = false; break; }	// wrapped, range unknown: skip this rate
					delta += energyRanges[i];
				}
				joules += delta * 1e-6;
			}
			if ( ok ) sample.Set( PT_CPU_POWER_W, joules / ( now - previousEnergyTime ) );
		}
		energyPrevious = values;
		previousEnergyTime = now;
		havePreviousEnergy = true;
	}

	bool					offered[PT_GPU_SENSOR_COUNT];
	idStr					statPath, temperaturePath;
	bool					havePreviousStat;
	double					previousBusy, previousTotal;
	idStrList				energyPaths;
	idList<double>			energyRanges, energyPrevious;
	bool					havePreviousEnergy;
	double					previousEnergyTime;
};

#endif // !_WIN32

idGpuTelemetry *PT_CPU_OpenLinuxTelemetry( const char *sysRoot, const char *procRoot, idStr &failure ) {
#ifdef _WIN32
	(void)sysRoot; (void)procRoot;
	failure = "Linux only";
	return NULL;
#else
	idCpuTelemetryLinux *telemetry = new idCpuTelemetryLinux();
	if ( telemetry->Open( sysRoot, procRoot, failure ) ) return telemetry;
	delete telemetry;
	return NULL;
#endif
}

/*
===============================================================================

	Windows D3DKMT + PDH provider, vendor neutral (what Task Manager shows).
	D3DKMT adapter/node perf data (WDDM 2.4+, Windows 10 1803) provides
	temperature, power as a share of the power limit, fan speed and clocks;
	the "GPU Engine" and "GPU Adapter Memory" counters provide utilization and
	memory use. gdi32.dll, pdh.dll and dxgi.dll are loaded at runtime; the
	structures below mirror d3dkmthk.h and pdh.h.

===============================================================================
*/

#ifdef _WIN32

typedef UINT ptKmtHandle_t;
struct ptKmtOpenAdapterFromLuid_t { LUID AdapterLuid; ptKmtHandle_t hAdapter; };
struct ptKmtCloseAdapter_t { ptKmtHandle_t hAdapter; };
struct ptKmtQueryAdapterInfo_t { ptKmtHandle_t hAdapter; int Type; void *pPrivateDriverData; UINT PrivateDriverDataSize; };
struct ptKmtAdapterAddress_t { UINT BusNumber; UINT DeviceNumber; UINT FunctionNumber; };
struct ptKmtAdapterPerfData_t {
	UINT32 PhysicalAdapterIndex;
	ULONGLONG MemoryFrequency, MaxMemoryFrequency, MaxMemoryFrequencyOC, MemoryBandwidth, PCIEBandwidth;
	ULONG FanRPM;
	ULONG Power;			// tenths of a percent of the power limit
	ULONG Temperature;		// tenths of a degree Celsius
	UCHAR PowerStateOverride;
};
struct ptKmtAdapterPerfDataCaps_t {
	UINT32 PhysicalAdapterIndex;
	ULONGLONG MaxMemoryBandwidth, MaxPCIEBandwidth;
	ULONG MaxFanRPM, TemperatureMax, TemperatureWarning;
};
struct ptKmtNodePerfData_t {
	UINT32 NodeOrdinal, PhysicalAdapterIndex;
	ULONGLONG Frequency, MaxFrequency, MaxFrequencyOC;
	ULONG Voltage, VoltageMax, VoltageMaxOC;
	ULONGLONG MaxTransitionLatency;
};
enum {
	PT_KMTQAITYPE_ADAPTERADDRESS		= 6,
	PT_KMTQAITYPE_NODEPERFDATA			= 61,
	PT_KMTQAITYPE_ADAPTERPERFDATA		= 62,
	PT_KMTQAITYPE_ADAPTERPERFDATA_CAPS	= 63
};

typedef LONG ptPdhStatus_t;
typedef HANDLE ptPdhHandle_t;
struct ptPdhValue_t {
	DWORD CStatus;
	union { LONG longValue; double doubleValue; LONGLONG largeValue; LPCSTR ansiValue; LPCWSTR wideValue; };
};
struct ptPdhItem_t { LPWSTR szName; ptPdhValue_t FmtValue; };
static const DWORD			PT_PDH_FMT_DOUBLE			= 0x00000200;
static const DWORD			PT_PDH_FMT_NOCAP100			= 0x00008000;
static const ptPdhStatus_t	PT_PDH_MORE_DATA			= (ptPdhStatus_t)0x800007D2L;
static const DWORD			PT_PDH_CSTATUS_VALID_DATA	= 0;
static const DWORD			PT_PDH_CSTATUS_NEW_DATA		= 1;

static idStr Narrow( const wchar_t *text ) {
	const int bytes = WideCharToMultiByte( CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL );
	if ( bytes <= 1 ) return "";
	idList<char> buffer;
	buffer.SetNum( bytes );
	WideCharToMultiByte( CP_UTF8, 0, text, -1, buffer.Ptr(), bytes, NULL, NULL );
	buffer[bytes - 1] = '\0';
	return buffer.Ptr();
}

// a per-engine accumulator; a handful of engines, so a linear search is fine
struct ptNamedValue_t {
	idStr	name;
	double	value;
};

static void AddNamedValue( idList<ptNamedValue_t> &list, const idStr &name, double value ) {
	for ( int i = 0; i < list.Num(); i++ ) {
		if ( list[i].name == name ) { list[i].value += value; return; }
	}
	ptNamedValue_t entry;
	entry.name = name;
	entry.value = value;
	list.Append( entry );
}

class idGpuTelemetryD3DKMT : public idGpuTelemetry {
public:
	idGpuTelemetryD3DKMT() : gdi( NULL ), pdh( NULL ), adapter( 0 ), haveAdapter( false ), integrated( false ),
		query( NULL ), engines( NULL ), dedicated( NULL ), shared( NULL ),
		openAdapter( NULL ), closeAdapter( NULL ), queryInfo( NULL ),
		pdhOpen( NULL ), pdhAdd( NULL ), pdhCollect( NULL ), pdhArray( NULL ), pdhClose( NULL ) {
		for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) offered[i] = false;
		memset( &luid, 0, sizeof( luid ) );
	}
	~idGpuTelemetryD3DKMT() {
		if ( query && pdhClose ) pdhClose( query );
		if ( haveAdapter && closeAdapter ) {
			ptKmtCloseAdapter_t close = { adapter };
			closeAdapter( &close );
		}
		PT_FreeLibrary( pdh );
		PT_FreeLibrary( gdi );
	}

	const char *Source() const { return "d3dkmt"; }
	bool Offers( ptGpuSensor_t sensor ) const { return offered[sensor]; }

	bool Open( const ptGpuTelemetryTarget_t &target, idStr &failure ) {
		gdi = PT_LoadLibrary( "gdi32.dll" );
		if ( !gdi || !PT_LoadSymbol( gdi, "D3DKMTOpenAdapterFromLuid", openAdapter )
			|| !PT_LoadSymbol( gdi, "D3DKMTCloseAdapter", closeAdapter )
			|| !PT_LoadSymbol( gdi, "D3DKMTQueryAdapterInfo", queryInfo ) ) {
			failure = "D3DKMT entry points not found"; return false;
		}
		if ( !SelectAdapter( target, failure ) ) return false;

		ptKmtAdapterPerfData_t perf;
		if ( QueryPerf( perf ) ) {
			offered[PT_GPU_TEMPERATURE_C] = perf.Temperature > 0;
			offered[PT_GPU_POWER_PCT] = perf.Power > 0;
			offered[PT_GPU_MEMORY_CLOCK_MHZ] = perf.MemoryFrequency > 0;
			ptKmtAdapterPerfDataCaps_t caps;
			memset( &caps, 0, sizeof( caps ) );
			// zero-RPM fans read 0 at idle, so trust the capability over the first reading
			offered[PT_GPU_FAN_RPM] = Query( PT_KMTQAITYPE_ADAPTERPERFDATA_CAPS, &caps, sizeof( caps ) ) ? caps.MaxFanRPM > 0 : perf.FanRPM > 0;
		}
		ptKmtNodePerfData_t node;
		offered[PT_GPU_CORE_CLOCK_MHZ] = QueryNode( node ) && node.Frequency > 0;
		OpenCounters();
		NotOffered( PT_GPU_POWER_W, "D3DKMT reports power only as a share of the board limit (gpu_power_pct)" );
		if ( !offered[PT_GPU_TEMPERATURE_C] ) NotOffered( PT_GPU_TEMPERATURE_C, "the driver reports no temperature through D3DKMT" );
		if ( !offered[PT_GPU_UTILIZATION_PCT] ) NotOffered( PT_GPU_UTILIZATION_PCT, "\\GPU Engine counters unavailable" );
		if ( !offered[PT_GPU_MEMORY_USED_BYTES] ) NotOffered( PT_GPU_MEMORY_USED_BYTES, "\\GPU Adapter Memory counters unavailable" );

		bool any = false;
		for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) any = any || offered[i];
		if ( !any ) { failure = "adapter reports no sensors (needs WDDM 2.4+)"; return false; }
		return true;
	}

	void Sample( ptGpuTelemetrySample_t &sample ) {
		if ( offered[PT_GPU_TEMPERATURE_C] || offered[PT_GPU_POWER_PCT] || offered[PT_GPU_MEMORY_CLOCK_MHZ] || offered[PT_GPU_FAN_RPM] ) {
			ptKmtAdapterPerfData_t perf;
			const bool ok = QueryPerf( perf );
			if ( offered[PT_GPU_TEMPERATURE_C] ) { if ( ok && perf.Temperature > 0 ) sample.Set( PT_GPU_TEMPERATURE_C, perf.Temperature * 0.1 ); else sample.Fail( PT_GPU_TEMPERATURE_C, "unavailable" ); }
			if ( offered[PT_GPU_POWER_PCT] ) { if ( ok ) sample.Set( PT_GPU_POWER_PCT, perf.Power * 0.1 ); else sample.Fail( PT_GPU_POWER_PCT, "unavailable" ); }
			if ( offered[PT_GPU_MEMORY_CLOCK_MHZ] ) { if ( ok && perf.MemoryFrequency > 0 ) sample.Set( PT_GPU_MEMORY_CLOCK_MHZ, perf.MemoryFrequency * 1e-6 ); else sample.Fail( PT_GPU_MEMORY_CLOCK_MHZ, "unavailable" ); }
			if ( offered[PT_GPU_FAN_RPM] ) { if ( ok ) sample.Set( PT_GPU_FAN_RPM, perf.FanRPM ); else sample.Fail( PT_GPU_FAN_RPM, "unavailable" ); }
		}
		if ( offered[PT_GPU_CORE_CLOCK_MHZ] ) {
			ptKmtNodePerfData_t node;
			if ( QueryNode( node ) && node.Frequency > 0 ) sample.Set( PT_GPU_CORE_CLOCK_MHZ, node.Frequency * 1e-6 );
			else sample.Fail( PT_GPU_CORE_CLOCK_MHZ, "unavailable" );
		}
		if ( !query ) return;
		const ptPdhStatus_t collected = pdhCollect( query );
		if ( collected != 0 ) {
			if ( offered[PT_GPU_UTILIZATION_PCT] ) sample.Fail( PT_GPU_UTILIZATION_PCT, Code( collected ) );
			if ( offered[PT_GPU_MEMORY_USED_BYTES] ) sample.Fail( PT_GPU_MEMORY_USED_BYTES, Code( collected ) );
			return;
		}
		if ( offered[PT_GPU_UTILIZATION_PCT] ) {
			// busiest engine, summed over processes (Task Manager's "GPU" column)
			idList<ptNamedValue_t> perEngine;
			idList<byte> buffer;
			DWORD items = 0;
			const ptPdhItem_t *values = Values( engines, buffer, items );
			for ( DWORD i = 0; values && i < items; i++ ) {
				if ( !values[i].szName || ( values[i].FmtValue.CStatus != PT_PDH_CSTATUS_VALID_DATA && values[i].FmtValue.CStatus != PT_PDH_CSTATUS_NEW_DATA ) ) continue;
				const idStr instance = Lower( Narrow( values[i].szName ) );
				if ( !Contains( instance, luidTag.c_str() ) ) continue;
				const int engine = instance.Find( "_eng_" );
				if ( engine < 0 ) continue;
				const int type = instance.Find( "_engtype", true, engine );
				AddNamedValue( perEngine, type < 0 ? From( instance, engine ) : instance.Mid( engine, type - engine ), values[i].FmtValue.doubleValue );
			}
			// rate counter: the first collection after opening has no data yet
			if ( values ) {
				double busiest = 0.0;
				for ( int e = 0; e < perEngine.Num(); e++ ) busiest = Max( busiest, perEngine[e].value );
				sample.Set( PT_GPU_UTILIZATION_PCT, Min( busiest, 100.0 ) );
			}
		}
		if ( offered[PT_GPU_MEMORY_USED_BYTES] ) {
			double used = 0.0;
			bool ok = SumCounter( dedicated, used );
			if ( ok && integrated && shared ) ok = SumCounter( shared, used );
			if ( ok ) sample.Set( PT_GPU_MEMORY_USED_BYTES, used );
			else sample.Fail( PT_GPU_MEMORY_USED_BYTES, "unavailable" );
		}
	}

private:
	bool Query( int type, void *data, UINT size ) {
		ptKmtQueryAdapterInfo_t info = { adapter, type, data, size };
		return queryInfo( &info ) >= 0;
	}
	bool QueryPerf( ptKmtAdapterPerfData_t &perf ) {
		memset( &perf, 0, sizeof( perf ) );
		return Query( PT_KMTQAITYPE_ADAPTERPERFDATA, &perf, sizeof( perf ) );
	}
	// node 0 is the 3D engine on the drivers this was checked against
	bool QueryNode( ptKmtNodePerfData_t &node ) {
		memset( &node, 0, sizeof( node ) );
		return Query( PT_KMTQAITYPE_NODEPERFDATA, &node, sizeof( node ) );
	}

	struct candidate_t {
		LUID			luid;
		unsigned int	vendor;
		idStr			name;
		SIZE_T			dedicatedMemory;
		bool			haveAddress;
		ptKmtAdapterAddress_t address;
	};

	bool SelectAdapter( const ptGpuTelemetryTarget_t &target, idStr &failure ) {
		ptLibrary_t dxgi = PT_LoadLibrary( "dxgi.dll" );
		HRESULT ( WINAPI *createFactory )( REFIID, void ** ) = NULL;
		IDXGIFactory1 *factory = NULL;
		if ( !dxgi || !PT_LoadSymbol( dxgi, "CreateDXGIFactory1", createFactory )
			|| FAILED( createFactory( __uuidof( IDXGIFactory1 ), (void **)&factory ) ) || !factory ) {
			PT_FreeLibrary( dxgi );
			failure = "DXGI unavailable";
			return false;
		}
		idList<candidate_t> candidates;
		IDXGIAdapter1 *dxgiAdapter = NULL;
		for ( UINT i = 0; factory->EnumAdapters1( i, &dxgiAdapter ) != DXGI_ERROR_NOT_FOUND; i++ ) {
			DXGI_ADAPTER_DESC1 desc;
			if ( SUCCEEDED( dxgiAdapter->GetDesc1( &desc ) ) && !( desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) ) {
				candidate_t c;
				c.luid = desc.AdapterLuid;
				c.vendor = desc.VendorId;
				c.name = Narrow( desc.Description );
				c.dedicatedMemory = desc.DedicatedVideoMemory;
				c.haveAddress = false;
				memset( &c.address, 0, sizeof( c.address ) );
				ptKmtOpenAdapterFromLuid_t open;
				open.AdapterLuid = c.luid;
				open.hAdapter = 0;
				if ( openAdapter( &open ) >= 0 ) {
					ptKmtQueryAdapterInfo_t info = { open.hAdapter, PT_KMTQAITYPE_ADAPTERADDRESS, &c.address, sizeof( c.address ) };
					c.haveAddress = queryInfo( &info ) >= 0;
					ptKmtCloseAdapter_t close = { open.hAdapter };
					closeAdapter( &close );
				}
				candidates.Append( c );
			}
			dxgiAdapter->Release();
		}
		factory->Release();
		PT_FreeLibrary( dxgi );

		int chosen = -1;
		unsigned int domain, bus, slot, function;
		if ( !target.pciBusId.IsEmpty() && PT_GPU_ParsePciBusId( target.pciBusId, domain, bus, slot, function ) ) {
			for ( int i = 0; i < candidates.Num() && chosen < 0; i++ ) {
				const ptKmtAdapterAddress_t &a = candidates[i].address;
				if ( candidates[i].haveAddress && a.BusNumber == bus && a.DeviceNumber == slot && a.FunctionNumber == function ) chosen = (int)i;
			}
		}
		for ( int pass = 0; pass < 2 && chosen < 0; pass++ ) {
			// pass 0: by name, pass 1: the only adapter of the target vendor (or the only one at all)
			int matches = 0, match = -1;
			for ( int i = 0; i < candidates.Num(); i++ ) {
				const bool hit = pass == 0 ? SameDeviceName( candidates[i].name, target.name )
					: ( !target.vendorId || candidates[i].vendor == target.vendorId );
				if ( hit ) { matches++; match = (int)i; }
			}
			if ( matches == 1 ) chosen = match;
		}
		if ( chosen < 0 ) {
			failure = candidates.Num() == 0 ? "no hardware adapter found" : "no unique adapter matches";
			return false;
		}

		luid = candidates[chosen].luid;
		integrated = candidates[chosen].dedicatedMemory < ( (SIZE_T)512 << 20 );
		char tag[64];
		idStr::snPrintf( tag, sizeof( tag ), "luid_0x%08lx_0x%08lx", (unsigned long)luid.HighPart, (unsigned long)luid.LowPart );
		luidTag = tag;
		ptKmtOpenAdapterFromLuid_t open;
		open.AdapterLuid = luid;
		open.hAdapter = 0;
		if ( openAdapter( &open ) < 0 ) { failure = "D3DKMTOpenAdapterFromLuid failed"; return false; }
		adapter = open.hAdapter;
		haveAdapter = true;
		return true;
	}

	void OpenCounters() {
		pdh = PT_LoadLibrary( "pdh.dll" );
		if ( !pdh || !PT_LoadSymbol( pdh, "PdhOpenQueryW", pdhOpen ) || !PT_LoadSymbol( pdh, "PdhAddEnglishCounterW", pdhAdd )
			|| !PT_LoadSymbol( pdh, "PdhCollectQueryData", pdhCollect ) || !PT_LoadSymbol( pdh, "PdhGetFormattedCounterArrayW", pdhArray )
			|| !PT_LoadSymbol( pdh, "PdhCloseQuery", pdhClose ) ) return;
		if ( pdhOpen( NULL, 0, &query ) != 0 ) { query = NULL; return; }
		offered[PT_GPU_UTILIZATION_PCT] = pdhAdd( query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engines ) == 0;
		offered[PT_GPU_MEMORY_USED_BYTES] = pdhAdd( query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &dedicated ) == 0;
		if ( pdhAdd( query, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &shared ) != 0 ) shared = NULL;
		if ( !offered[PT_GPU_UTILIZATION_PCT] && !offered[PT_GPU_MEMORY_USED_BYTES] ) {
			pdhClose( query );
			query = NULL;
			return;
		}
		pdhCollect( query );	// utilization is a rate and needs a first collection
	}

	const ptPdhItem_t *Values( ptPdhHandle_t counter, idList<byte> &buffer, DWORD &items ) {
		DWORD bytes = 0;
		items = 0;
		ptPdhStatus_t status = pdhArray( counter, PT_PDH_FMT_DOUBLE | PT_PDH_FMT_NOCAP100, &bytes, &items, NULL );
		if ( status != PT_PDH_MORE_DATA || bytes == 0 ) return NULL;
		buffer.AssureSize( (int)bytes, 0 );
		status = pdhArray( counter, PT_PDH_FMT_DOUBLE | PT_PDH_FMT_NOCAP100, &bytes, &items, (ptPdhItem_t *)buffer.Ptr() );
		return status == 0 ? (const ptPdhItem_t *)buffer.Ptr() : NULL;
	}

	bool SumCounter( ptPdhHandle_t counter, double &sum ) {
		idList<byte> buffer;
		DWORD items = 0;
		const ptPdhItem_t *values = Values( counter, buffer, items );
		if ( !values ) return false;
		bool found = false;
		for ( DWORD i = 0; i < items; i++ ) {
			if ( !values[i].szName || ( values[i].FmtValue.CStatus != PT_PDH_CSTATUS_VALID_DATA && values[i].FmtValue.CStatus != PT_PDH_CSTATUS_NEW_DATA ) ) continue;
			if ( !Contains( Lower( Narrow( values[i].szName ) ), luidTag.c_str() ) ) continue;
			sum += values[i].FmtValue.doubleValue;
			found = true;
		}
		return found;
	}

	ptLibrary_t		gdi, pdh;
	ptKmtHandle_t	adapter;
	bool			haveAdapter, integrated;
	LUID			luid;
	idStr			luidTag;
	bool			offered[PT_GPU_SENSOR_COUNT];
	ptPdhHandle_t	query, engines, dedicated, shared;

	LONG			( APIENTRY *openAdapter )( ptKmtOpenAdapterFromLuid_t * );
	LONG			( APIENTRY *closeAdapter )( ptKmtCloseAdapter_t * );
	LONG			( APIENTRY *queryInfo )( ptKmtQueryAdapterInfo_t * );
	ptPdhStatus_t	( WINAPI *pdhOpen )( LPCWSTR, DWORD_PTR, ptPdhHandle_t * );
	ptPdhStatus_t	( WINAPI *pdhAdd )( ptPdhHandle_t, LPCWSTR, DWORD_PTR, ptPdhHandle_t * );
	ptPdhStatus_t	( WINAPI *pdhCollect )( ptPdhHandle_t );
	ptPdhStatus_t	( WINAPI *pdhArray )( ptPdhHandle_t, DWORD, LPDWORD, LPDWORD, ptPdhItem_t * );
	ptPdhStatus_t	( WINAPI *pdhClose )( ptPdhHandle_t );
};

/*
===============================================================================

	Windows CPU provider. Utilization from GetSystemTimes; temperature from the
	"Thermal Zone Information" counters (ACPI thermal zones: firmware defined,
	often close to the package temperature, on some boards a static value);
	package power from the "Energy Meter" RAPL_Package*_PKG counters (Windows 11,
	or Windows 10 with an EMI driver), which count picowatt-hours. Neither needs
	administrator rights.

===============================================================================
*/

static const DWORD PT_PDH_FMT_LARGE = 0x00000400;

class idCpuTelemetryWindows : public idGpuTelemetry {
public:
	idCpuTelemetryWindows() : pdh( NULL ), query( NULL ), temperature( NULL ), energy( NULL ), tenthsKelvin( false ),
		havePreviousTimes( false ), previousIdle( 0 ), previousTotal( 0 ),
		havePreviousEnergy( false ), previousEnergy( 0.0 ), previousEnergyTime( 0.0 ),
		pdhOpen( NULL ), pdhAdd( NULL ), pdhCollect( NULL ), pdhArray( NULL ), pdhClose( NULL ) {
		for ( int i = 0; i < PT_GPU_SENSOR_COUNT; i++ ) offered[i] = false;
	}
	~idCpuTelemetryWindows() {
		if ( query && pdhClose ) pdhClose( query );
		PT_FreeLibrary( pdh );
	}

	const char *Source() const { return "windows_cpu"; }
	bool Offers( ptGpuSensor_t sensor ) const { return offered[sensor]; }

	bool Open( idStr &failure ) {
		unsigned long long idle, total;
		offered[PT_CPU_UTILIZATION_PCT] = ReadTimes( idle, total );
		if ( !offered[PT_CPU_UTILIZATION_PCT] ) NotOffered( PT_CPU_UTILIZATION_PCT, "GetSystemTimes failed" );

		pdh = PT_LoadLibrary( "pdh.dll" );
		if ( pdh && PT_LoadSymbol( pdh, "PdhOpenQueryW", pdhOpen ) && PT_LoadSymbol( pdh, "PdhAddEnglishCounterW", pdhAdd )
			&& PT_LoadSymbol( pdh, "PdhCollectQueryData", pdhCollect ) && PT_LoadSymbol( pdh, "PdhGetFormattedCounterArrayW", pdhArray )
			&& PT_LoadSymbol( pdh, "PdhCloseQuery", pdhClose ) && pdhOpen( NULL, 0, &query ) == 0 ) {
			tenthsKelvin = pdhAdd( query, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0, &temperature ) == 0;
			if ( !tenthsKelvin && pdhAdd( query, L"\\Thermal Zone Information(*)\\Temperature", 0, &temperature ) != 0 ) temperature = NULL;
			if ( pdhAdd( query, L"\\Energy Meter(*)\\Energy", 0, &energy ) != 0 ) energy = NULL;
			if ( pdhCollect( query ) == 0 ) {
				double value;
				offered[PT_CPU_TEMPERATURE_C] = temperature && ReadTemperature( value );
				offered[PT_CPU_POWER_W] = energy && ReadEnergy( value );
				if ( offered[PT_CPU_POWER_W] ) { previousEnergy = value; previousEnergyTime = Seconds(); havePreviousEnergy = true; }
			}
		} else {
			query = NULL;
		}
		if ( !offered[PT_CPU_TEMPERATURE_C] ) NotOffered( PT_CPU_TEMPERATURE_C, "no ACPI thermal zone counter" );
		if ( !offered[PT_CPU_POWER_W] ) NotOffered( PT_CPU_POWER_W, "no Energy Meter RAPL package counter (Windows 11 or an EMI driver needed)" );
		if ( query && !offered[PT_CPU_TEMPERATURE_C] && !offered[PT_CPU_POWER_W] ) { pdhClose( query ); query = NULL; }

		if ( !offered[PT_CPU_UTILIZATION_PCT] && !offered[PT_CPU_TEMPERATURE_C] && !offered[PT_CPU_POWER_W] ) {
			failure = "no readable CPU sensors (" + unavailable + ")";
			return false;
		}
		return true;
	}

	void Sample( ptGpuTelemetrySample_t &sample ) {
		if ( offered[PT_CPU_UTILIZATION_PCT] ) {
			unsigned long long idle, total;
			if ( !ReadTimes( idle, total ) ) {
				sample.Fail( PT_CPU_UTILIZATION_PCT, "GetSystemTimes" );
				havePreviousTimes = false;
			} else {
				if ( havePreviousTimes && total > previousTotal && idle >= previousIdle ) {
					const double busy = (double)( ( total - previousTotal ) - ( idle - previousIdle ) );
					sample.Set( PT_CPU_UTILIZATION_PCT, Max( 0.0, Min( 100.0, busy * 100.0 / (double)( total - previousTotal ) ) ) );
				}
				previousIdle = idle;
				previousTotal = total;
				havePreviousTimes = true;
			}
		}
		if ( !query ) return;
		const ptPdhStatus_t collected = pdhCollect( query );
		if ( collected != 0 ) {
			if ( offered[PT_CPU_TEMPERATURE_C] ) sample.Fail( PT_CPU_TEMPERATURE_C, Code( collected ) );
			if ( offered[PT_CPU_POWER_W] ) sample.Fail( PT_CPU_POWER_W, Code( collected ) );
			return;
		}
		double value;
		if ( offered[PT_CPU_TEMPERATURE_C] ) {
			if ( ReadTemperature( value ) ) sample.Set( PT_CPU_TEMPERATURE_C, value );
			else sample.Fail( PT_CPU_TEMPERATURE_C, "unavailable" );
		}
		if ( offered[PT_CPU_POWER_W] ) {
			const double now = Seconds();
			if ( !ReadEnergy( value ) ) { sample.Fail( PT_CPU_POWER_W, "unavailable" ); havePreviousEnergy = false; return; }
			if ( havePreviousEnergy && value >= previousEnergy && now > previousEnergyTime ) {
				sample.Set( PT_CPU_POWER_W, ( value - previousEnergy ) * 3.6e-9 / ( now - previousEnergyTime ) );	// pWh -> J
			}
			previousEnergy = value;
			previousEnergyTime = now;
			havePreviousEnergy = true;
		}
	}

private:
	static unsigned long long Ticks( const FILETIME &time ) {
		return ( (unsigned long long)time.dwHighDateTime << 32 ) | time.dwLowDateTime;
	}

	// kernel time includes idle time
	static bool ReadTimes( unsigned long long &idle, unsigned long long &total ) {
		FILETIME idleTime, kernelTime, userTime;
		if ( !GetSystemTimes( &idleTime, &kernelTime, &userTime ) ) return false;
		idle = Ticks( idleTime );
		total = Ticks( kernelTime ) + Ticks( userTime );
		return true;
	}

	const ptPdhItem_t *Values( ptPdhHandle_t counter, DWORD format, idList<byte> &buffer, DWORD &items ) {
		DWORD bytes = 0;
		items = 0;
		ptPdhStatus_t status = pdhArray( counter, format, &bytes, &items, NULL );
		if ( status != PT_PDH_MORE_DATA || bytes == 0 ) return NULL;
		buffer.AssureSize( (int)bytes, 0 );
		status = pdhArray( counter, format, &bytes, &items, (ptPdhItem_t *)buffer.Ptr() );
		return status == 0 ? (const ptPdhItem_t *)buffer.Ptr() : NULL;
	}

	static bool ValidItem( const ptPdhItem_t &item ) {
		return item.szName && ( item.FmtValue.CStatus == PT_PDH_CSTATUS_VALID_DATA || item.FmtValue.CStatus == PT_PDH_CSTATUS_NEW_DATA );
	}

	// hottest zone, Celsius
	bool ReadTemperature( double &celsius ) {
		idList<byte> buffer;
		DWORD items = 0;
		const ptPdhItem_t *values = Values( temperature, PT_PDH_FMT_DOUBLE | PT_PDH_FMT_NOCAP100, buffer, items );
		bool found = false;
		for ( DWORD i = 0; values && i < items; i++ ) {
			if ( !ValidItem( values[i] ) ) continue;
			const double kelvin = values[i].FmtValue.doubleValue * ( tenthsKelvin ? 0.1 : 1.0 );
			if ( kelvin <= 200.0 ) continue;	// unpopulated zones read 0
			celsius = found ? Max( celsius, kelvin - 273.15 ) : kelvin - 273.15;
			found = true;
		}
		return found;
	}

	// cumulative picowatt-hours of the package domains (not _Total, which adds the subdomains)
	bool ReadEnergy( double &picowattHours ) {
		idList<byte> buffer;
		DWORD items = 0;
		const ptPdhItem_t *values = Values( energy, PT_PDH_FMT_LARGE, buffer, items );
		bool found = false;
		picowattHours = 0.0;
		for ( DWORD i = 0; values && i < items; i++ ) {
			if ( !ValidItem( values[i] ) ) continue;
			const idStr instance = Lower( Narrow( values[i].szName ) );
			if ( !StartsWith( instance, "rapl_package" ) || !EndsWith( instance, "_pkg" ) ) continue;
			picowattHours += (double)values[i].FmtValue.largeValue;
			found = true;
		}
		return found;
	}

	ptLibrary_t			pdh;
	ptPdhHandle_t		query, temperature, energy;
	bool				tenthsKelvin;
	bool				offered[PT_GPU_SENSOR_COUNT];
	bool				havePreviousTimes;
	unsigned long long	previousIdle, previousTotal;
	bool				havePreviousEnergy;
	double				previousEnergy, previousEnergyTime;

	ptPdhStatus_t	( WINAPI *pdhOpen )( LPCWSTR, DWORD_PTR, ptPdhHandle_t * );
	ptPdhStatus_t	( WINAPI *pdhAdd )( ptPdhHandle_t, LPCWSTR, DWORD_PTR, ptPdhHandle_t * );
	ptPdhStatus_t	( WINAPI *pdhCollect )( ptPdhHandle_t );
	ptPdhStatus_t	( WINAPI *pdhArray )( ptPdhHandle_t, DWORD, LPDWORD, LPDWORD, ptPdhItem_t * );
	ptPdhStatus_t	( WINAPI *pdhClose )( ptPdhHandle_t );
};

#endif // _WIN32

/*
===============================================================================

	PT_GPU_OpenTelemetry

===============================================================================
*/

idGpuTelemetry *PT_GPU_OpenTelemetry( const ptGpuTelemetryTarget_t &target, idStr &failure ) {
	idStr why;
	failure.Clear();

	idGpuTelemetryNVML *nvml = new idGpuTelemetryNVML();
	if ( nvml->Open( target, why ) ) return nvml;
	delete nvml;
	failure = "nvml: " + why;

#ifdef _WIN32
	idGpuTelemetryD3DKMT *kmt = new idGpuTelemetryD3DKMT();
	why.Clear();
	if ( kmt->Open( target, why ) ) return kmt;
	delete kmt;
	failure += "; d3dkmt: " + why;
#else
	why.Clear();
	idGpuTelemetry *sysfs = PT_GPU_OpenSysfsTelemetry( "/sys", target, why );
	if ( sysfs ) return sysfs;
	failure += "; sysfs: " + why;
#endif
	return NULL;
}

idGpuTelemetry *PT_CPU_OpenTelemetry( idStr &failure ) {
	failure.Clear();
#ifdef _WIN32
	idCpuTelemetryWindows *telemetry = new idCpuTelemetryWindows();
	if ( telemetry->Open( failure ) ) return telemetry;
	delete telemetry;
	return NULL;
#else
	return PT_CPU_OpenLinuxTelemetry( "/sys", "/proc", failure );
#endif
}
