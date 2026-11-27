// GPU telemetry: PCI/vendor parsing, the Linux sysfs provider (amdgpu, i915, xe,
// DRM fdinfo) and the Linux CPU provider against fake /sys and /proc trees.
// NVML, D3DKMT and the Windows CPU provider need real systems and are not covered.
// standard headers before idlib: idlib/Str.h redirects snprintf, which breaks MSVC's <cstdio>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "renderer_pt/pt_gpu_telemetry.h"

class idCommon;
idCommon *common = NULL;	// idlib links against the engine global

static int failures = 0;

#define CHECK( condition ) do { \
	if ( !( condition ) ) { std::fprintf( stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition ); failures++; } \
} while ( 0 )

/*
========================
Near
========================
*/
static bool Near( double a, double b ) { return std::fabs( a - b ) <= 1e-6 * std::fmax( 1.0, std::fabs( b ) ); }

static void TestParsing() {
	unsigned int domain = 99, bus = 99, device = 99, function = 99;
	CHECK( PT_GPU_ParsePciBusId( "00000000:03:00.0", domain, bus, device, function ) );
	CHECK( domain == 0 && bus == 3 && device == 0 && function == 0 );
	CHECK( PT_GPU_ParsePciBusId( "0001:c1:1f.7", domain, bus, device, function ) );
	CHECK( domain == 1 && bus == 0xc1 && device == 0x1f && function == 7 );
	CHECK( PT_GPU_ParsePciBusId( "02:00.1", domain, bus, device, function ) );
	CHECK( domain == 0 && bus == 2 && device == 0 && function == 1 );
	CHECK( !PT_GPU_ParsePciBusId( "", domain, bus, device, function ) );
	CHECK( !PT_GPU_ParsePciBusId( "0000:03:20.0", domain, bus, device, function ) );	// device > 0x1f
	CHECK( !PT_GPU_ParsePciBusId( "0000:03:00.0x", domain, bus, device, function ) );

	CHECK( PT_GPU_VendorFromName( "NVIDIA GeForce RTX 4090" ) == 0x10de );
	CHECK( PT_GPU_VendorFromName( "Quadro RTX 4000" ) == 0x10de );
	CHECK( PT_GPU_VendorFromName( "AMD Radeon RX 7900 XTX" ) == 0x1002 );
	CHECK( PT_GPU_VendorFromName( "Radeon RX 6800" ) == 0x1002 );
	CHECK( PT_GPU_VendorFromName( "Mesa Intel(R) Graphics (MTL)" ) == 0x8086 );
	CHECK( PT_GPU_VendorFromName( "Intel(R) Arc(TM) A770 Graphics" ) == 0x8086 );
	CHECK( PT_GPU_VendorFromName( "gfx1100" ) == 0 );
	CHECK( PT_GPU_VendorFromName( "llvmpipe" ) == 0 );
}

#ifndef _WIN32

static std::string root;

/*
========================
MakeDirs
========================
*/
static void MakeDirs( const std::string &path ) {
	for ( size_t slash = root.size() + 1; slash != std::string::npos; slash = path.find( '/', slash + 1 ) ) {
		mkdir( path.substr( 0, slash ).c_str(), 0755 );
	}
	mkdir( path.c_str(), 0755 );
}

/*
========================
WriteFile
========================
*/
static void WriteFile( const std::string &path, const std::string &text ) {
	MakeDirs( path.substr( 0, path.rfind( '/' ) ) );
	FILE *file = std::fopen( path.c_str(), "w" );
	if ( !file ) { std::fprintf( stderr, "cannot write %s\n", path.c_str() ); std::exit( 1 ); }
	std::fputs( text.c_str(), file );
	std::fclose( file );
}

/*
========================
RemoveEntry
========================
*/
static int RemoveEntry( const char *path, const struct stat *, int, struct FTW * ) {
	return std::remove( path );
}

/*
========================
TestAmdgpu
========================
*/
static void TestAmdgpu() {
	const std::string device = root + "/bus/pci/devices/0000:03:00.0";
	WriteFile( device + "/uevent", "DRIVER=amdgpu\nPCI_CLASS=30000\nPCI_ID=1002:73BF\nPCI_SLOT_NAME=0000:03:00.0\n" );
	WriteFile( device + "/gpu_busy_percent", "42\n" );
	WriteFile( device + "/mem_info_vram_used", "1073741824\n" );
	WriteFile( device + "/pp_dpm_mclk", "0: 96Mhz \n1: 456Mhz \n2: 1000Mhz *\n" );
	const std::string hwmon = device + "/hwmon/hwmon3";
	WriteFile( hwmon + "/temp1_input", "65000\n" );
	WriteFile( hwmon + "/power1_average", "150000000\n" );
	WriteFile( hwmon + "/freq1_input", "2100000000\n" );
	WriteFile( hwmon + "/pwm1", "128\n" );
	WriteFile( hwmon + "/pwm1_max", "255\n" );
	WriteFile( hwmon + "/fan1_input", "1500\n" );

	ptGpuTelemetryTarget_t target;
	target.name = "AMD Radeon RX 6800";
	target.pciBusId = "00000000:03:00.0";	// NVML/OpenCL form with an 8 digit domain
	target.vendorId = 0x1002;
	idStr failure;
	idGpuTelemetry *telemetry = PT_GPU_OpenSysfsTelemetry( root.c_str(), target, failure, ( root + "/noproc" ).c_str() );
	CHECK( telemetry != NULL );
	if ( !telemetry ) { std::fprintf( stderr, "amdgpu: %s\n", failure.c_str() ); return; }
	CHECK( std::string( telemetry->Source() ) == "amdgpu_sysfs" );
	CHECK( !telemetry->Offers( PT_GPU_CLOCK_LIMIT_REASONS ) && !telemetry->Offers( PT_GPU_POWER_PCT ) );

	ptGpuTelemetrySample_t sample;
	sample.Clear();
	telemetry->Sample( sample );
	CHECK( sample.errors.IsEmpty() );
	CHECK( sample.valid[PT_GPU_TEMPERATURE_C] && Near( sample.value[PT_GPU_TEMPERATURE_C], 65.0 ) );
	CHECK( sample.valid[PT_GPU_POWER_W] && Near( sample.value[PT_GPU_POWER_W], 150.0 ) );
	CHECK( sample.valid[PT_GPU_CORE_CLOCK_MHZ] && Near( sample.value[PT_GPU_CORE_CLOCK_MHZ], 2100.0 ) );
	CHECK( sample.valid[PT_GPU_MEMORY_CLOCK_MHZ] && Near( sample.value[PT_GPU_MEMORY_CLOCK_MHZ], 1000.0 ) );
	CHECK( sample.valid[PT_GPU_UTILIZATION_PCT] && Near( sample.value[PT_GPU_UTILIZATION_PCT], 42.0 ) );
	CHECK( sample.valid[PT_GPU_MEMORY_USED_BYTES] && Near( sample.value[PT_GPU_MEMORY_USED_BYTES], 1073741824.0 ) );
	CHECK( sample.valid[PT_GPU_FAN_PCT] && Near( sample.value[PT_GPU_FAN_PCT], 128.0 * 100.0 / 255.0 ) );
	CHECK( sample.valid[PT_GPU_FAN_RPM] && Near( sample.value[PT_GPU_FAN_RPM], 1500.0 ) );
	CHECK( !sample.valid[PT_GPU_CLOCK_LIMIT_REASONS] );

	// an offered sensor that stops being readable is reported, the rest still sample
	std::remove( ( device + "/gpu_busy_percent" ).c_str() );
	sample.Clear();
	telemetry->Sample( sample );
	CHECK( !sample.valid[PT_GPU_UTILIZATION_PCT] );
	CHECK( sample.errors == "utilization:unreadable" );
	CHECK( sample.valid[PT_GPU_TEMPERATURE_C] );
	CHECK( PT_GPU_OfferedSensors( *telemetry ) == "temperature,power,core_clock,memory_clock,utilization,memory,fan,fan_rpm" );
	delete telemetry;
}

/*
========================
TestI915ByVendor
========================
*/
static void TestI915ByVendor() {
	// no PCI address: the only GPU of the target vendor under /sys/class/drm is used
	const std::string intel = root + "/class/drm/card0/device";
	WriteFile( intel + "/uevent", "DRIVER=i915\nPCI_ID=8086:7D55\nPCI_SLOT_NAME=0000:00:02.0\n" );
	WriteFile( intel + "/drm/card0/gt_act_freq_mhz", "1250\n" );
	WriteFile( intel + "/hwmon/hwmon5/energy1_input", "1000000\n" );
	const std::string amd = root + "/class/drm/card1/device";
	WriteFile( amd + "/uevent", "DRIVER=amdgpu\nPCI_ID=1002:73BF\nPCI_SLOT_NAME=0000:03:00.0\n" );
	WriteFile( amd + "/gpu_busy_percent", "7\n" );
	WriteFile( root + "/class/drm/card0-eDP-1/status", "connected\n" );	// connectors are not cards

	ptGpuTelemetryTarget_t target;
	target.name = "Intel(R) Graphics [0x7d55]";
	target.vendorId = 0x8086;
	idStr failure;
	idGpuTelemetry *telemetry = PT_GPU_OpenSysfsTelemetry( root.c_str(), target, failure, ( root + "/noproc" ).c_str() );
	CHECK( telemetry != NULL );
	if ( !telemetry ) { std::fprintf( stderr, "i915: %s\n", failure.c_str() ); return; }
	CHECK( std::string( telemetry->Source() ) == "i915_sysfs" );
	CHECK( telemetry->Offers( PT_GPU_POWER_W ) && telemetry->Offers( PT_GPU_CORE_CLOCK_MHZ ) );
	CHECK( !telemetry->Offers( PT_GPU_UTILIZATION_PCT ) && !telemetry->Offers( PT_GPU_TEMPERATURE_C ) );
	CHECK( !telemetry->Offers( PT_CPU_TEMPERATURE_C ) );
	// blank sensors are explained
	CHECK( telemetry->Unavailable().Find( "temperature:" ) >= 0 );
	CHECK( telemetry->Unavailable().Find( "utilization:no DRM client" ) >= 0 );

	// power comes from the energy counter, so the first sample has no rate and no error
	ptGpuTelemetrySample_t sample;
	sample.Clear();
	telemetry->Sample( sample );
	CHECK( sample.errors.IsEmpty() );
	CHECK( !sample.valid[PT_GPU_POWER_W] );
	CHECK( sample.valid[PT_GPU_CORE_CLOCK_MHZ] && Near( sample.value[PT_GPU_CORE_CLOCK_MHZ], 1250.0 ) );
	usleep( 100000 );
	WriteFile( intel + "/hwmon/hwmon5/energy1_input", "2000000\n" );	// +1 J
	sample.Clear();
	telemetry->Sample( sample );
	CHECK( sample.errors.IsEmpty() );
	CHECK( sample.valid[PT_GPU_POWER_W] && sample.value[PT_GPU_POWER_W] > 1.0 && sample.value[PT_GPU_POWER_W] < 10.5 );
	// counter wrap: no rate, no error
	WriteFile( intel + "/hwmon/hwmon5/energy1_input", "5\n" );
	sample.Clear();
	telemetry->Sample( sample );
	CHECK( !sample.valid[PT_GPU_POWER_W] && sample.errors.IsEmpty() );
	delete telemetry;

	// without a vendor the two GPUs are ambiguous
	target.vendorId = 0;
	telemetry = PT_GPU_OpenSysfsTelemetry( root.c_str(), target, failure, ( root + "/noproc" ).c_str() );
	CHECK( telemetry == NULL );
	CHECK( failure.Find( "several GPUs" ) >= 0 );
	delete telemetry;
}

/*
========================
TestUnsupportedDriver
========================
*/
static void TestUnsupportedDriver() {
	WriteFile( root + "/bus/pci/devices/0000:01:00.0/uevent", "DRIVER=nvidia\nPCI_ID=10DE:1EB1\n" );
	ptGpuTelemetryTarget_t target;
	target.pciBusId = "0000:01:00.0";
	target.vendorId = 0x10de;
	idStr failure;
	idGpuTelemetry *telemetry = PT_GPU_OpenSysfsTelemetry( root.c_str(), target, failure, ( root + "/noproc" ).c_str() );
	CHECK( telemetry == NULL );
	CHECK( failure.Find( "nvidia" ) >= 0 );
	delete telemetry;
}

/*
========================
TestI915Fdinfo
========================
*/
static void TestI915Fdinfo() {
	const std::string device = root + "/bus/pci/devices/0000:00:02.0";
	WriteFile( device + "/uevent", "DRIVER=i915\nPCI_ID=8086:A7A0\nPCI_SLOT_NAME=0000:00:02.0\n" );
	WriteFile( device + "/drm/card1/gt_act_freq_mhz", "1300\n" );
	const std::string fdinfo = root + "/proc/self/fdinfo";
	const std::string client =
		"pos:\t0\nflags:\t02100002\ndrm-driver:\ti915\ndrm-client-id:\t7\ndrm-pdev:\t0000:00:02.0\n"
		"drm-engine-render:\t%llu ns\ndrm-engine-compute:\t%llu ns\ndrm-engine-capacity-video:\t2\n"
		"drm-total-system0:\t300 MiB\ndrm-resident-system0:\t%llu KiB\ndrm-resident-stolen-local0:\t0\n";
	char text[512];
	idStr::snPrintf( text, sizeof( text ), client.c_str(), 1000000000ULL, 5000000000ULL, 1024ULL );
	WriteFile( fdinfo + "/3", "pos:\t0\nflags:\t02\n" );		// not a DRM fd
	WriteFile( fdinfo + "/5", text );
	WriteFile( fdinfo + "/6", text );		// same client through a dup'ed fd: counted once
	WriteFile( fdinfo + "/9", "drm-driver:\ti915\ndrm-client-id:\t8\ndrm-pdev:\t0000:03:00.0\ndrm-engine-render:\t9 ns\n" );	// other GPU

	ptGpuTelemetryTarget_t target;
	target.pciBusId = "0000:00:02.0";
	target.vendorId = 0x8086;
	idStr failure;
	idGpuTelemetry *telemetry = PT_GPU_OpenSysfsTelemetry( root.c_str(), target, failure, ( root + "/proc/self" ).c_str() );
	CHECK( telemetry != NULL );
	if ( !telemetry ) { std::fprintf( stderr, "i915 fdinfo: %s\n", failure.c_str() ); return; }
	CHECK( std::string( telemetry->Source() ) == "i915_sysfs+fdinfo" );
	CHECK( telemetry->Offers( PT_GPU_UTILIZATION_PCT ) && telemetry->Offers( PT_GPU_MEMORY_USED_BYTES ) );

	usleep( 200000 );
	// compute engine +0.1 s busy in ~0.2 s wall: ~50% (sleep may run long, so a range)
	idStr::snPrintf( text, sizeof( text ), client.c_str(), 1000000000ULL, 5100000000ULL, 2048ULL );
	WriteFile( fdinfo + "/5", text );
	WriteFile( fdinfo + "/6", text );
	ptGpuTelemetrySample_t sample;
	sample.Clear();
	telemetry->Sample( sample );
	CHECK( sample.errors.IsEmpty() );
	CHECK( sample.valid[PT_GPU_UTILIZATION_PCT] && sample.value[PT_GPU_UTILIZATION_PCT] > 10.0 && sample.value[PT_GPU_UTILIZATION_PCT] <= 50.5 );
	CHECK( sample.valid[PT_GPU_MEMORY_USED_BYTES] && Near( sample.value[PT_GPU_MEMORY_USED_BYTES], 2048.0 * 1024.0 ) );

	// the client went away: reported, not zero-filled
	std::remove( ( fdinfo + "/5" ).c_str() );
	std::remove( ( fdinfo + "/6" ).c_str() );
	sample.Clear();
	telemetry->Sample( sample );
	CHECK( !sample.valid[PT_GPU_UTILIZATION_PCT] && !sample.valid[PT_GPU_MEMORY_USED_BYTES] );
	CHECK( sample.errors == "utilization:no_drm_client;memory:no_drm_client" );
	delete telemetry;
}

/*
========================
TestXeFdinfoCycles
========================
*/
static void TestXeFdinfoCycles() {
	const std::string device = root + "/bus/pci/devices/0000:04:00.0";
	WriteFile( device + "/uevent", "DRIVER=xe\nPCI_ID=8086:E20B\nPCI_SLOT_NAME=0000:04:00.0\n" );
	WriteFile( device + "/tile0/gt0/freq0/act_freq", "2400\n" );
	const std::string fdinfo = root + "/proc/xe/fdinfo";
	const char *format = "drm-driver:\txe\ndrm-client-id:\t3\ndrm-pdev:\t0000:04:00.0\n"
		"drm-cycles-rcs:\t%llu\ndrm-total-cycles-rcs:\t%llu\ndrm-cycles-ccs:\t%llu\ndrm-total-cycles-ccs:\t%llu\n"
		"drm-resident-gtt:\t100 MiB\ndrm-resident-vram0:\t3 GiB\n";
	char text[512];
	idStr::snPrintf( text, sizeof( text ), format, 100ULL, 1000ULL, 0ULL, 1000ULL );
	WriteFile( fdinfo + "/4", text );

	ptGpuTelemetryTarget_t target;
	target.pciBusId = "0000:04:00.0";
	idStr failure;
	idGpuTelemetry *telemetry = PT_GPU_OpenSysfsTelemetry( root.c_str(), target, failure, ( root + "/proc/xe" ).c_str() );
	CHECK( telemetry != NULL );
	if ( !telemetry ) { std::fprintf( stderr, "xe fdinfo: %s\n", failure.c_str() ); return; }
	// ccs busy 750 of 1000 cycles, rcs 100 of 1000: busiest engine wins
	idStr::snPrintf( text, sizeof( text ), format, 200ULL, 2000ULL, 750ULL, 2000ULL );
	WriteFile( fdinfo + "/4", text );
	ptGpuTelemetrySample_t sample;
	sample.Clear();
	telemetry->Sample( sample );
	CHECK( sample.errors.IsEmpty() );
	CHECK( sample.valid[PT_GPU_UTILIZATION_PCT] && Near( sample.value[PT_GPU_UTILIZATION_PCT], 75.0 ) );
	// local memory is reported when the device has it
	CHECK( sample.valid[PT_GPU_MEMORY_USED_BYTES] && Near( sample.value[PT_GPU_MEMORY_USED_BYTES], 3.0 * 1024 * 1024 * 1024 ) );
	delete telemetry;
}

/*
========================
TestLinuxCpu
========================
*/
static void TestLinuxCpu() {
	const std::string sys = root + "/cpusys", proc = root + "/cpuproc";
	WriteFile( proc + "/stat", "cpu  100 0 100 700 100 0 0 0 0 0\ncpu0 50 0 50 350 50 0 0 0 0 0\nintr 1\n" );
	// a GPU hwmon first in the list must not be mistaken for the CPU
	WriteFile( sys + "/class/hwmon/hwmon0/name", "amdgpu\n" );
	WriteFile( sys + "/class/hwmon/hwmon0/temp1_input", "70000\n" );
	WriteFile( sys + "/class/hwmon/hwmon1/name", "k10temp\n" );
	WriteFile( sys + "/class/hwmon/hwmon1/temp1_input", "61000\n" );
	WriteFile( sys + "/class/hwmon/hwmon1/temp1_label", "Tctl\n" );
	WriteFile( sys + "/class/hwmon/hwmon1/temp3_input", "55500\n" );
	WriteFile( sys + "/class/hwmon/hwmon1/temp3_label", "Tccd1\n" );
	const std::string rapl = sys + "/class/powercap/intel-rapl:0";
	WriteFile( rapl + "/name", "package-0\n" );
	WriteFile( rapl + "/energy_uj", "262143000000\n" );
	WriteFile( rapl + "/max_energy_range_uj", "262143328850\n" );
	WriteFile( sys + "/class/powercap/intel-rapl-mmio:0/name", "package-0\n" );	// limits only, no energy
	WriteFile( sys + "/class/powercap/intel-rapl:0:0/name", "core\n" );		// subdomain, not summed
	WriteFile( sys + "/class/powercap/intel-rapl:0:0/energy_uj", "5\n" );

	idStr failure;
	idGpuTelemetry *telemetry = PT_CPU_OpenLinuxTelemetry( sys.c_str(), proc.c_str(), failure );
	CHECK( telemetry != NULL );
	if ( !telemetry ) { std::fprintf( stderr, "linux cpu: %s\n", failure.c_str() ); return; }
	CHECK( std::string( telemetry->Source() ) == "linux_cpu" );
	CHECK( PT_GPU_OfferedSensors( *telemetry ) == "cpu_temperature,cpu_utilization,cpu_power" );
	CHECK( telemetry->Unavailable().IsEmpty() );

	// rates need a previous reading: only the temperature in the first sample
	ptGpuTelemetrySample_t sample;
	sample.Clear();
	telemetry->Sample( sample );
	CHECK( sample.errors.IsEmpty() );
	CHECK( sample.valid[PT_CPU_TEMPERATURE_C] && Near( sample.value[PT_CPU_TEMPERATURE_C], 61.0 ) );
	CHECK( !sample.valid[PT_CPU_UTILIZATION_PCT] && !sample.valid[PT_CPU_POWER_W] );

	usleep( 100000 );
	// +300 busy (user+system+irq+softirq) +100 idle +0 iowait -> 75%
	WriteFile( proc + "/stat", "cpu  250 0 200 800 100 25 25 0 0 0\n" );
	// counter wrapped: 328850 uJ to the range end plus 1671150 -> 2 J
	WriteFile( rapl + "/energy_uj", "1671150\n" );
	sample.Clear();
	telemetry->Sample( sample );
	CHECK( sample.errors.IsEmpty() );
	CHECK( sample.valid[PT_CPU_UTILIZATION_PCT] && Near( sample.value[PT_CPU_UTILIZATION_PCT], 75.0 ) );
	CHECK( sample.valid[PT_CPU_POWER_W] && sample.value[PT_CPU_POWER_W] > 2.0 && sample.value[PT_CPU_POWER_W] < 20.5 );
	CHECK( !sample.valid[PT_GPU_TEMPERATURE_C] );
	delete telemetry;

	// coretemp package sensor, RAPL present but root-only, no /proc/stat
	const std::string intel = root + "/cpusys2";
	WriteFile( intel + "/class/hwmon/hwmon2/name", "coretemp\n" );
	WriteFile( intel + "/class/hwmon/hwmon2/temp2_input", "48000\n" );
	WriteFile( intel + "/class/hwmon/hwmon2/temp2_label", "Core 0\n" );
	WriteFile( intel + "/class/hwmon/hwmon2/temp1_input", "52000\n" );
	WriteFile( intel + "/class/hwmon/hwmon2/temp1_label", "Package id 0\n" );
	WriteFile( intel + "/class/powercap/intel-rapl:0/name", "package-0\n" );
	WriteFile( intel + "/class/powercap/intel-rapl:0/energy_uj", "1\n" );
	const bool root_user = geteuid() == 0;
	chmod( ( intel + "/class/powercap/intel-rapl:0/energy_uj" ).c_str(), 0 );
	telemetry = PT_CPU_OpenLinuxTelemetry( intel.c_str(), ( root + "/missing" ).c_str(), failure );
	CHECK( telemetry != NULL );
	if ( !telemetry ) return;
	CHECK( telemetry->Offers( PT_CPU_TEMPERATURE_C ) && !telemetry->Offers( PT_CPU_UTILIZATION_PCT ) );
	if ( !root_user ) {
		CHECK( !telemetry->Offers( PT_CPU_POWER_W ) );
		CHECK( telemetry->Unavailable().Find( "cpu_power:" ) >= 0 );
		CHECK( telemetry->Unavailable().Find( "readable by root only" ) >= 0 );
	}
	sample.Clear();
	telemetry->Sample( sample );
	CHECK( sample.valid[PT_CPU_TEMPERATURE_C] && Near( sample.value[PT_CPU_TEMPERATURE_C], 52.0 ) );
	delete telemetry;
	chmod( ( intel + "/class/powercap/intel-rapl:0/energy_uj" ).c_str(), 0644 );

	// nothing readable at all
	telemetry = PT_CPU_OpenLinuxTelemetry( ( root + "/nothing" ).c_str(), ( root + "/nothing" ).c_str(), failure );
	CHECK( telemetry == NULL );
	CHECK( failure.Find( "no readable CPU sensors" ) >= 0 );
	delete telemetry;
}

#endif // !_WIN32

/*
========================
main
========================
*/
int main() {
	TestParsing();
#ifndef _WIN32
	char directory[] = "/tmp/pt_gpu_telemetry_XXXXXX";
	if ( !mkdtemp( directory ) ) { std::perror( "mkdtemp" ); return 1; }
	root = directory;
	TestAmdgpu();
	TestI915ByVendor();
	TestUnsupportedDriver();
	TestI915Fdinfo();
	TestXeFdinfoCycles();
	TestLinuxCpu();
	nftw( directory, RemoveEntry, 16, FTW_DEPTH | FTW_PHYS );
#endif
	if ( failures ) {
		std::fprintf( stderr, "%d check(s) failed\n", failures );
		return 1;
	}
	std::printf( "pt_gpu_telemetry_test passed\n" );
	return 0;
}
