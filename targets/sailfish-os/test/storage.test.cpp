// Native unit tests for targets/sailfish-os/main/sailfish_storage.cpp.
// Covers GEA_SAILFISH_STORAGE_DIR creation, saveKv/loadKv round-trip, empty
// blob deletion, atomic writes (no leftover .tmp files), setString/getString
// framing & truncation safety, and resilience to corrupted settings.bin.

#include "services/storage_service.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using gea::framework::services::StorageService;

int main()
{
	const fs::path base = fs::temp_directory_path() / ("gea-sailfish-storage-test-" + std::to_string(getpid()));
	const fs::path nested = base / "org.geastack" / "probe_app";
	fs::remove_all(base);

	setenv("GEA_SAILFISH_STORAGE_DIR", nested.c_str(), 1);

	assert(StorageService::init());
	assert(fs::is_directory(nested));

	// 1. Initial loadKv returns empty cleanly.
	std::string blob = "stale";
	assert(StorageService::loadKv(blob));
	assert(blob.empty());

	// 2. Binary-safe saveKv / loadKv round-trip (includes embedded NUL bytes).
	const std::string binaryBlob("\x01\x00\x00\x00k\x02\x00\x00\x00v\x00", 12);
	StorageService::saveKv(binaryBlob);
	assert(fs::exists(nested / "localstorage.bin"));
	assert(!fs::exists(nested / "localstorage.bin.tmp"));

	std::string loaded;
	assert(StorageService::loadKv(loaded));
	assert(loaded == binaryBlob);

	// 3. Empty saveKv removes localstorage.bin.
	StorageService::saveKv("");
	assert(!fs::exists(nested / "localstorage.bin"));
	assert(StorageService::loadKv(loaded));
	assert(loaded.empty());

	// 4. setString / getString round-trip & small-buffer truncation.
	assert(StorageService::setString("theme", "dark"));
	assert(!fs::exists(nested / "settings.bin.tmp"));
	char buf[16]{};
	assert(StorageService::getString("theme", buf, sizeof(buf)));
	assert(std::string(buf) == "dark");
	assert(!StorageService::getString("missing", buf, sizeof(buf)));

	char tiny[3]{};
	assert(StorageService::getString("theme", tiny, sizeof(tiny)));
	assert(std::string(tiny) == "da");

	// 5. Truncated/corrupted settings.bin does not crash; writes recover cleanly.
	{
		std::ofstream corrupt(nested / "settings.bin", std::ios::binary | std::ios::trunc);
		const char junk[] = "\xff\xff\xff\x7fpartial";
		corrupt.write(junk, sizeof(junk) - 1);
	}
	assert(StorageService::setString("recovered", "yes"));
	assert(StorageService::getString("recovered", buf, sizeof(buf)));
	assert(std::string(buf) == "yes");

	fs::remove_all(base);
	std::puts("PASS sailfish_storage: nested init, binary KV round-trip, empty unlink, atomic tmp cleanup, settings truncation & corruption recovery");
	return 0;
}
