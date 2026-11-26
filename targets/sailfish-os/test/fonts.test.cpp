// Native unit tests for targets/sailfish-os/include/sailfish_fonts.h.
// Verifies family name normalization/classification, system TTF loading via
// FontRegistry::setRuntimeFontProvider, app-bundled @font-face precedence, and
// graceful fallback when no system font exists.

#include "sailfish_fonts.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

namespace gea::framework::graphics {

namespace {
const RuntimeFontProvider *g_provider = nullptr;
bool g_appHasSans = false;
}  // namespace

void FontRegistry::setRuntimeFontProvider(const RuntimeFontProvider *provider)
{
	g_provider = provider;
}

namespace generated {
int lookupFontFamily(const char *family)
{
	if (g_appHasSans && family && std::strcmp(family, "sans-serif") == 0) return 0;
	if (family && std::strcmp(family, "AppCustomFont") == 0) return 1;
	return -1;
}
}  // namespace generated

}  // namespace gea::framework::graphics

int main()
{
	using namespace gea::sailfish::fonts;

	// 1. Family normalization & classification
	assert(normalizeFamilyName("  'Sans-Serif' ") == "sans-serif");
	assert(normalizeFamilyName("\"Liberation Sans\"") == "liberation sans");
	assert(classifySystemSlot("sans-serif") == kSlotSans);
	assert(classifySystemSlot("system-ui") == kSlotSans);
	assert(classifySystemSlot("Sail Sans Pro") == kSlotSans);
	assert(classifySystemSlot("serif") == kSlotSerif);
	assert(classifySystemSlot("monospace") == kSlotMono);
	assert(classifySystemSlot("UnknownCustomFont") == -1);
	assert(classifySystemSlot("") == -1);
	assert(classifySystemSlot(nullptr) == -1);

	// 2. Load a synthetic TTF file via addSearchPath and verify provider hooks
	const std::string tmpPath = "/tmp/gea-sailfish-fonts-test-" + std::to_string(getpid()) + ".ttf";
	{
		std::ofstream out(tmpPath, std::ios::binary);
		out.write("FAKE_TTF_BYTES_1234", 19);
	}

	addSearchPath(kSlotSans, tmpPath);
	install();
	assert(gea::framework::graphics::g_provider != nullptr);

	const auto *provider = gea::framework::graphics::g_provider;
	const int sansId = provider->lookupFamily("sans-serif");
	assert(sansId == kSystemFamilyBase + kSlotSans);
	assert(std::strcmp(provider->familyName(sansId), "sans-serif") == 0);

	unsigned long len = 0;
	const std::uint8_t *bytes = provider->fontBytes(sansId, &len);
	assert(bytes != nullptr);
	assert(len == 19);
	assert(std::memcmp(bytes, "FAKE_TTF_BYTES_1234", 19) == 0);

	// Unknown / app-bundled family IDs must NOT be served by the system provider.
	assert(provider->fontBytes(0, &len) == nullptr);
	assert(provider->fontBytes(1, &len) == nullptr);
	assert(provider->familyName(0) == nullptr);
	assert(provider->lookupFamily("AppCustomFont") == -1);

	// If an app bundles its own "sans-serif", the app-bundled font wins.
	gea::framework::graphics::g_appHasSans = true;
	assert(provider->lookupFamily("sans-serif") == -1);
	gea::framework::graphics::g_appHasSans = false;

	// Clean shutdown unregisters provider and releases buffers.
	shutdown();
	assert(gea::framework::graphics::g_provider == nullptr);
	std::remove(tmpPath.c_str());

	std::puts("PASS sailfish_fonts: classification, TTF loading, app-font precedence, clean shutdown");
	return 0;
}
