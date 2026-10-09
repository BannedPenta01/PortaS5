#include "common/performanceProfile.h"

#include <common/emulatorConfig.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace Config {

namespace {

bool Contains(const std::string& haystack, std::string_view needle) {
	return haystack.find(needle) != std::string::npos;
}

std::string ToLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

std::string ReadFirstLine(const std::string& path) {
	std::ifstream in(path);
	std::string   line;
	std::getline(in, line);
	return line;
}

std::string ReadProcessCpuFlag(const std::string& tag) {
	std::ifstream in("/proc/cpuinfo");
	std::string   line;
	while (std::getline(in, line)) {
		if (line.find(tag) != std::string::npos) {
			const auto colon = line.find(':');
			if (colon != std::string::npos) {
				return line.substr(colon + 1);
			}
		}
	}
	return {};
}

uint64_t ReadTotalRamBytes() {
	std::ifstream in("/proc/meminfo");
	std::string   line;
	while (std::getline(in, line)) {
		if (line.rfind("MemTotal:", 0) == 0) {
			uint64_t kb = 0;
			const auto first_digit = line.find_first_of("0123456789", 9);
			std::from_chars_result se = std::from_chars(
			    line.data() + first_digit, line.data() + line.size(), kb);
			if (se.ec != std::errc {} || kb == 0) {
				return 0;
			}
			return kb * 1024;
		}
	}
	return 0;
}

bool DetectAmdApu(const std::string& cpu_model_lower) {
	if (!Contains(cpu_model_lower, "amd")) {
		return false;
	}
	// Steam Deck / Ally / embedded handheld APUs.
	static const char* const kApuTokens[] = {
	    "custom apu 0405", // Steam Deck
	    "custom apu 1035", // newer AMD handheld APUs
	    "z1 extreme",
	    "z1",
	    "7040", // Ryzen 7 7840U / 7640U / Phoenix class
	    "8040", // Ryzen 7 8840U / Hawk Point
	    "9000", // Ryzen AI 9 HX 370 (Strix)
	    "ai 9 hx",
	    "ai 9 2",
	    "6800u",
	    "6825u",
	    "6850u", // Rembrandt
	    "7730u",
	    "7530u",
	    "7535u",
	    "7640u",
	    "7740u",
	    "5825u",
	    "5800u", // Barcelo
	    "5500u", // Lucienne
	    "7840hs",
	    "7940hs",
	    "8640u",
	    "8740u",
	    "8840u",
	    "8845hs",
	    "8945hs",
	    "8840hs",
	    "krackan",
	    "strix",
	    "mendocino",
	    "cezanne",
	    "rembrandt",
	};
	for (const char* token : kApuTokens) {
		if (Contains(cpu_model_lower, token)) {
			return true;
		}
	}
	return false;
}

bool DetectHandheldDevice() {
	static const char* const kHandheldTokens[] = {
	    "ally",         "ROG Ally",      "Jupiter",        "Galileo",
	    "Steam Deck",   "Legion Go",     "Ayaneo",         "OneXPlayer",
	    "AOKZOE",       "GPD Win",       "Win 4",          "Loki Zero",
	    "ROG Ally X",   "Z1 Extreme",    "Z1E",            "Handheld",
	};
	const std::string product = ToLower(ReadFirstLine("/sys/class/dmi/id/product_name"));
	const std::string vendor  = ToLower(ReadFirstLine("/sys/class/dmi/id/sys_vendor"));
	const std::string board   = ToLower(ReadFirstLine("/sys/class/dmi/id/board_name"));
	for (const char* token : kHandheldTokens) {
		const std::string t = ToLower(token);
		if (Contains(product, t) || Contains(board, t) || Contains(vendor, t)) {
			return true;
		}
	}
	// Batocera/EmuDeck/SteamOS profiles often expose handheld hints via DMI
	const std::string family = ToLower(ReadFirstLine("/sys/class/dmi/id/product_family"));
	if (Contains(family, "deck") || Contains(family, "ally") || Contains(family, "handheld")) {
		return true;
	}
	return false;
}

RamTier ReadRamTier() {
	const uint64_t ram = ReadTotalRamBytes();
	if (ram == 0) {
		return RamTier::Unknown;
	}
	constexpr uint64_t kGB = 1024ull * 1024ull * 1024ull;
	if (ram < 10 * kGB) {
		return RamTier::Tier8GB;
	}
	if (ram < 20 * kGB) {
		return RamTier::Tier16GB;
	}
	return RamTier::Tier24GBPlus;
}

} // namespace

const SystemProfile& GetSystemProfile() {
	static SystemProfile profile = []() {
		SystemProfile p;
		p.logical_threads = std::thread::hardware_concurrency();
		if (p.logical_threads == 0) {
			p.logical_threads = 1;
		}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const std::string cpu_model = ToLower(ReadProcessCpuFlag("Model Name"));
#else
		const std::string cpu_model = ToLower(ReadProcessCpuFlag("model name"));
#endif
		p.amd_apu_detected  = DetectAmdApu(cpu_model);
		p.handheld_detected = DetectHandheldDevice();
		p.ram_tier          = ReadRamTier();
		return p;
	}();
	return profile;
}

PerformanceProfile ResolveProfileMode(PerformanceProfile mode) {
	if (mode != PerformanceProfile::Auto) {
		return mode;
	}
	const SystemProfile& p = GetSystemProfile();
	if (p.amd_apu_detected || p.handheld_detected) {
		return PerformanceProfile::Handheld;
	}
	return PerformanceProfile::Desktop;
}

PerformanceProfile CurrentProfile() {
	return ResolveProfileMode(GetPerformanceProfile());
}

bool IsHandheldProfile() {
	return CurrentProfile() == PerformanceProfile::Handheld;
}

RamTier GetRamTier() {
	return GetSystemProfile().ram_tier;
}

unsigned GetLogicalThreadCount() {
	return GetSystemProfile().logical_threads;
}

uint64_t GetTotalRamBytes() {
	return ReadTotalRamBytes();
}

uint64_t GetPersistentCacheBudgetBytes() {
	switch (GetRamTier()) {
	case RamTier::Tier8GB:
		return 256ull * 1024 * 1024;
	case RamTier::Tier16GB:
		return 512ull * 1024 * 1024;
	case RamTier::Tier24GBPlus:
	case RamTier::Unknown:
	default:
		return 1024ull * 1024 * 1024;
	}
}

unsigned GetRecommendedParallelCompiles() {
	const SystemProfile& p = GetSystemProfile();
	unsigned cores = p.logical_threads;
	if (cores <= 2) {
		return 1;
	}
	if (IsHandheldProfile()) {
		return std::max(1u, std::min(cores / 2, 4u));
	}
	return std::max(1u, cores / 2);
}

} // namespace Config
