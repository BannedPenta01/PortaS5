#ifndef KYTY_COMMON_PERFORMANCE_PROFILE_H_
#define KYTY_COMMON_PERFORMANCE_PROFILE_H_

#include <cstdint>
#include <string>

namespace Config {

// User-selectable performance profile mode.
enum class PerformanceProfile { Auto, Handheld, Desktop, Off };

enum class RamTier {
	Unknown,
	Tier8GB,
	Tier16GB,
	Tier24GBPlus,
};

// Resolved, runtime-detected profile (Auto expands to Handheld or Desktop).
struct SystemProfile {
	PerformanceProfile mode = PerformanceProfile::Off;
	bool               amd_apu_detected = false;
	bool               handheld_detected = false;
	RamTier            ram_tier = RamTier::Unknown;
	unsigned           logical_threads = 1;
};

// Detect the host system once and cache the result.
const SystemProfile& GetSystemProfile();

// Resolve a user-configured mode against the detected system.
PerformanceProfile ResolveProfileMode(PerformanceProfile mode);

// Current process profile based on the loaded configuration.
PerformanceProfile CurrentProfile();

bool               IsHandheldProfile();
RamTier            GetRamTier();
unsigned           GetLogicalThreadCount();
uint64_t           GetTotalRamBytes();
uint64_t           GetPersistentCacheBudgetBytes();
unsigned           GetRecommendedParallelCompiles();

} // namespace Config

#endif /* KYTY_COMMON_PERFORMANCE_PROFILE_H_ */
