#ifndef __PT_GPU_TELEMETRY_H__
#define __PT_GPU_TELEMETRY_H__

/*
===============================================================================

	GPU and CPU sensor sampling for capture reports (telemetry.csv).

	GPU providers, tried in order:
	nvml	NVIDIA on Windows and Linux, runtime-loaded NVML (no nvml.h needed)
	sysfs	Linux amdgpu, i915 and xe through /sys (hwmon, DRM, PCI attributes),
	with utilization and memory from this process's DRM fdinfo where
	the driver has no device-wide attribute (i915, xe)
	d3dkmt	Windows, any WDDM 2.4+ GPU: D3DKMT adapter/node perf data plus the
	"GPU Engine" / "GPU Adapter Memory" performance counters (PDH)

	CPU provider (sampled alongside the GPU one):
	linux	/proc/stat, hwmon (k10temp, zenpower, coretemp) or thermal zones,
	powercap RAPL package energy (AMD and Intel)
	windows	GetSystemTimes, "Thermal Zone Information" and "Energy Meter"
	(RAPL package) performance counters

	The device is matched by PCI address when the backend provides one, then by
	name, then by being the only GPU of its PCI vendor. Sensors a provider does
	not offer stay blank without an error; sensors it offers but fails to read
	are blank and listed in the sample's errors.

	Uses idlib only (idStr, idList), no engine services, so the regression
	tests link it with idlib alone.

===============================================================================
*/

#include "sys/platform.h"
#include "idlib/Str.h"

// order matches the telemetry.csv sensor columns
enum ptGpuSensor_t {
	PT_GPU_TEMPERATURE_C,
	PT_GPU_POWER_W,
	PT_GPU_CORE_CLOCK_MHZ,			// NVIDIA: SM clock, others: graphics/shader clock
	PT_GPU_MEMORY_CLOCK_MHZ,
	PT_GPU_UTILIZATION_PCT,
	PT_GPU_MEMORY_USED_BYTES,
	PT_GPU_FAN_PCT,
	PT_GPU_CLOCK_LIMIT_REASONS,		// NVML clock event/throttle reason bitmask
	PT_GPU_POWER_PCT,				// Windows D3DKMT: percent of the board power limit
	PT_GPU_FAN_RPM,
	PT_CPU_TEMPERATURE_C,			// package/die temperature (Linux), ACPI thermal zone (Windows)
	PT_CPU_UTILIZATION_PCT,			// all logical CPUs
	PT_CPU_POWER_W,					// RAPL package
	PT_GPU_SENSOR_COUNT
};

extern const char *const ptGpuSensorNames[PT_GPU_SENSOR_COUNT];	// sensor_errors / metadata names
extern const char *const ptGpuSensorColumns[PT_GPU_SENSOR_COUNT];	// telemetry.csv column names

struct ptGpuTelemetryTarget_t {
	idStr			name;		// device name, e.g. CL_DEVICE_NAME or GL_RENDERER
	idStr			pciBusId;	// "domain:bus:device.function" (hex), empty if unknown
	unsigned int	vendorId;	// PCI vendor id, 0 if unknown

	ptGpuTelemetryTarget_t() : vendorId( 0 ) {}
};

struct ptGpuTelemetrySample_t {
	bool	valid[PT_GPU_SENSOR_COUNT];
	double	value[PT_GPU_SENSOR_COUNT];
	idStr	errors;	// "sensor:reason;sensor:reason"

	void			Clear();
	void			Set( ptGpuSensor_t sensor, double v ) { valid[sensor] = true; value[sensor] = v; }
	void			Fail( ptGpuSensor_t sensor, const char *reason );
};

class idGpuTelemetry {
public:
	virtual					~idGpuTelemetry() {}
	// sensors this provider could provide in principle but cannot here, with
	// the reason: "sensor:reason;sensor:reason" (e.g. a counter readable by root only)
	const idStr &			Unavailable() const { return unavailable; }
	virtual const char *	Source() const = 0;
	virtual bool			Offers( ptGpuSensor_t sensor ) const = 0;
	// called from the telemetry thread at about 1 Hz; rates (power from energy
	// counters, utilization from performance counters) are blank on the first call
	virtual void			Sample( ptGpuTelemetrySample_t &sample ) = 0;

protected:
	void					NotOffered( ptGpuSensor_t sensor, const char *reason );
	idStr	unavailable;
};

// tries every provider compiled for this platform; on failure returns NULL and
// describes why each provider declined in failure
idGpuTelemetry *			PT_GPU_OpenTelemetry( const ptGpuTelemetryTarget_t &target, idStr &failure );

// Linux sysfs provider rooted at sysRoot ("/sys"); DRM fdinfo is read from
// procSelf ("/proc/self"). Exposed for tests; NULL elsewhere
idGpuTelemetry *			PT_GPU_OpenSysfsTelemetry( const char *sysRoot, const ptGpuTelemetryTarget_t &target, idStr &failure,
								const char *procSelf = "/proc/self" );

// CPU temperature, utilization and power for this platform, NULL with the reason
// in failure when none can be read
idGpuTelemetry *			PT_CPU_OpenTelemetry( idStr &failure );

// Linux CPU provider rooted at sysRoot ("/sys") and procRoot ("/proc"), exposed for tests; NULL elsewhere
idGpuTelemetry *			PT_CPU_OpenLinuxTelemetry( const char *sysRoot, const char *procRoot, idStr &failure );

// "0000:03:00.0" or "00000000:03:00.0" -> fields; false if malformed
bool						PT_GPU_ParsePciBusId( const char *text, unsigned int &domain, unsigned int &bus,
								unsigned int &device, unsigned int &function );

// PCI vendor guessed from a device/vendor name (NVIDIA, AMD/Radeon, Intel), 0 if unknown
unsigned int				PT_GPU_VendorFromName( const char *name );

// comma separated names of the offered sensors
idStr						PT_GPU_OfferedSensors( const idGpuTelemetry &telemetry );

#endif /* !__PT_GPU_TELEMETRY_H__ */
