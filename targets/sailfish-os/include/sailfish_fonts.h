#pragma once
/* Sailfish OS system TrueType font provider (Harbour-safe).
 *
 * Supplies Sailfish OS system TTF fonts from /usr/share/fonts/ for generic CSS
 * font families (sans-serif, serif, monospace, system-ui) via
 * FontRegistry::setRuntimeFontProvider so apps without bundled @font-face
 * rules render crisp anti-aliased text instead of the 8x16 bitmap fallback.
 *
 * App-bundled @font-face families always take precedence: lookupFamily returns
 * -1 whenever generated::lookupFontFamily(family) already knows the name, and
 * fontBytes returns nullptr for any non-system family ID. */

#include "graphics/font.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gea::sailfish::fonts {

inline constexpr int kSystemFamilyBase = 10000;
inline constexpr int kSlotSans = 0;
inline constexpr int kSlotSerif = 1;
inline constexpr int kSlotMono = 2;
inline constexpr int kSlotCount = 3;

inline std::string normalizeFamilyName(const char *raw)
{
	if (!raw) return {};
	std::string out;
	while (*raw && std::isspace(static_cast<unsigned char>(*raw))) ++raw;
	std::size_t len = std::strlen(raw);
	while (len > 0 && std::isspace(static_cast<unsigned char>(raw[len - 1]))) --len;
	if (len >= 2 && ((raw[0] == '"' && raw[len - 1] == '"') || (raw[0] == '\'' && raw[len - 1] == '\''))) {
		++raw;
		len -= 2;
	}
	out.reserve(len);
	for (std::size_t i = 0; i < len; ++i) {
		out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw[i]))));
	}
	return out;
}

inline int classifySystemSlot(const char *family)
{
	const std::string name = normalizeFamilyName(family);
	if (name.empty()) return -1;
	if (name == "sans-serif" || name == "sans" || name == "system-ui" || name == "-apple-system" ||
	    name == "ui-sans-serif" || name == "sail sans pro" || name == "sailsanspro" ||
	    name == "liberation sans" || name == "dejavu sans" || name == "helvetica" ||
	    name == "arial" || name == "roboto" || name == "inter") {
		return kSlotSans;
	}
	if (name == "serif" || name == "ui-serif" || name == "liberation serif" || name == "dejavu serif" ||
	    name == "georgia" || name == "times" || name == "times new roman") {
		return kSlotSerif;
	}
	if (name == "monospace" || name == "mono" || name == "ui-monospace" || name == "liberation mono" ||
	    name == "dejavu sans mono" || name == "courier" || name == "courier new" ||
	    name == "menlo" || name == "consolas") {
		return kSlotMono;
	}
	return -1;
}

inline const char *slotCanonicalName(int slot)
{
	switch (slot) {
	case kSlotSans: return "sans-serif";
	case kSlotSerif: return "serif";
	case kSlotMono: return "monospace";
	default: return nullptr;
	}
}

namespace detail {

struct SystemFontSlot {
	bool attempted = false;
	std::vector<std::uint8_t> bytes;
};

struct SystemFontState {
	SystemFontSlot slots[kSlotCount];
	std::vector<std::string> customPaths[kSlotCount];
	bool installed = false;
};

inline SystemFontState &state()
{
	static SystemFontState s;
	return s;
}

inline bool readFileBytes(const char *path, std::vector<std::uint8_t> &out)
{
	if (!path || !*path) return false;
	std::FILE *fp = std::fopen(path, "rb");
	if (!fp) return false;
	if (std::fseek(fp, 0, SEEK_END) != 0) {
		std::fclose(fp);
		return false;
	}
	const long size = std::ftell(fp);
	if (size <= 0 || size > 16L * 1024L * 1024L) {
		std::fclose(fp);
		return false;
	}
	std::rewind(fp);
	std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
	const std::size_t got = std::fread(buf.data(), 1, buf.size(), fp);
	std::fclose(fp);
	if (got != buf.size()) return false;
	out = std::move(buf);
	return true;
}

inline const char *const *defaultCandidatesForSlot(int slot)
{
	static const char *const kSans[] = {
	    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
	    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
	    "/usr/share/fonts/sail-sans-pro/SailSansPro-Light.ttf",
	    nullptr,
	};
	static const char *const kSerif[] = {
	    "/usr/share/fonts/liberation/LiberationSerif-Regular.ttf",
	    "/usr/share/fonts/dejavu/DejaVuSerif.ttf",
	    nullptr,
	};
	static const char *const kMono[] = {
	    "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
	    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
	    nullptr,
	};
	switch (slot) {
	case kSlotSans: return kSans;
	case kSlotSerif: return kSerif;
	case kSlotMono: return kMono;
	default: return nullptr;
	}
}

inline bool ensureSlotLoaded(int slot)
{
	if (slot < 0 || slot >= kSlotCount) return false;
	auto &s = state();
	auto &entry = s.slots[slot];
	if (entry.attempted) return !entry.bytes.empty();
	entry.attempted = true;

	for (const auto &custom : s.customPaths[slot]) {
		if (readFileBytes(custom.c_str(), entry.bytes)) return true;
	}
	if (const char *const *candidates = defaultCandidatesForSlot(slot)) {
		for (int i = 0; candidates[i]; ++i) {
			if (readFileBytes(candidates[i], entry.bytes)) return true;
		}
	}
	return false;
}

inline int lookupFamily(const char *family)
{
	if (!family || !*family) return -1;
	// Never shadow an app-bundled @font-face family.
	if (gea::framework::graphics::generated::lookupFontFamily(family) >= 0) return -1;
	const int slot = classifySystemSlot(family);
	if (slot < 0) return -1;
	if (ensureSlotLoaded(slot)) return kSystemFamilyBase + slot;
	// Fall back to sans-serif if serif/monospace font is absent on device.
	if (slot != kSlotSans && ensureSlotLoaded(kSlotSans)) return kSystemFamilyBase + kSlotSans;
	return -1;
}

inline const char *familyName(int familyId)
{
	if (familyId < kSystemFamilyBase || familyId >= kSystemFamilyBase + kSlotCount) return nullptr;
	return slotCanonicalName(familyId - kSystemFamilyBase);
}

inline const std::uint8_t *fontBytes(int familyId, unsigned long *length)
{
	if (familyId < kSystemFamilyBase || familyId >= kSystemFamilyBase + kSlotCount) return nullptr;
	const int slot = familyId - kSystemFamilyBase;
	if (!ensureSlotLoaded(slot)) return nullptr;
	const auto &bytes = state().slots[slot].bytes;
	if (length) *length = static_cast<unsigned long>(bytes.size());
	return bytes.data();
}

inline const gea::framework::graphics::RuntimeFontProvider kProvider = {
    lookupFamily,
    familyName,
    fontBytes,
};

}  // namespace detail

// Optional override for unit tests or custom search paths.
inline void addSearchPath(int slot, const std::string &path)
{
	if (slot < 0 || slot >= kSlotCount || path.empty()) return;
	auto &s = detail::state();
	s.customPaths[slot].push_back(path);
	s.slots[slot].attempted = false;
	s.slots[slot].bytes.clear();
}

inline void install()
{
	auto &s = detail::state();
	if (s.installed) return;
	s.installed = true;
	gea::framework::graphics::FontRegistry::setRuntimeFontProvider(&detail::kProvider);
}

inline void shutdown()
{
	auto &s = detail::state();
	if (s.installed) {
		gea::framework::graphics::FontRegistry::setRuntimeFontProvider(nullptr);
		s.installed = false;
	}
	for (int i = 0; i < kSlotCount; ++i) {
		s.slots[i].attempted = false;
		s.slots[i].bytes.clear();
		s.customPaths[i].clear();
	}
}

}  // namespace gea::sailfish::fonts
