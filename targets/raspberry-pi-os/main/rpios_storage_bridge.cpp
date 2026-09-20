/* targets/raspberry-pi-os/main/rpios_storage_bridge.cpp
 * Persistence bridge for the geatsc runtime's `localStorage`.
 *
 * geatsc-compiled apps don't use gea::host::StorageFacade — the generated
 * runtime has its own in-RAM store, gea::runtime::host::Storage, shared
 * through gea::runtime::host::local_storage() (see the app's generated
 * host_globals.{h,cpp}). Nothing in the generated runtime persists it, so
 * without this bridge every localStorage write dies with the process.
 *
 * The bridge syncs that store with the target's StorageService file backend
 * (rpios_storage.cpp): load() before Application::init (app store init()
 * reads localStorage), flush() once per frame — serialize, compare against
 * the last persisted blob, write only on change. The blob framing matches
 * gea::host::StorageFacade (4-byte host-order length-prefixed key/value
 * chunks), so the two views stay interchangeable; a given app only ever
 * mutates one of them.
 *
 * This file #includes the app's generated headers, so unlike the other
 * rpios_*.cpp platform files it recompiles per app (the generated dir is
 * already on the include path).
 */

#include "runtime_pch.h"
#include "host_globals.h"

#include "services/storage_service.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace {

std::string g_last_blob;

std::string serializeEntries()
{
	auto &storage = gea::runtime::host::local_storage();
	std::string out;
	for (const auto &entry : storage.entries) {
		for (const std::string *part : {&entry.first, &entry.second}) {
			const std::uint32_t len = (std::uint32_t)part->size();
			char header[sizeof(len)];
			std::memcpy(header, &len, sizeof(len));
			out.append(header, sizeof(header));
			out.append(*part);
		}
	}
	return out;
}

}  // namespace

extern "C" void rpios_runtime_storage_load()
{
	std::string blob;
	gea::framework::services::StorageService::loadKv(blob);
	auto &storage = gea::runtime::host::local_storage();
	storage.entries.clear();
	std::size_t pos = 0;
	auto readChunk = [&](std::string &out) {
		if (pos + sizeof(std::uint32_t) > blob.size()) return false;
		std::uint32_t len = 0;
		std::memcpy(&len, blob.data() + pos, sizeof(len));
		pos += sizeof(len);
		if (pos + len > blob.size()) return false;
		out.assign(blob.data() + pos, len);
		pos += len;
		return true;
	};
	std::string k, v;
	while (readChunk(k) && readChunk(v)) storage.entries.push_back({k, v});
	storage.length = (double)storage.entries.size();
	g_last_blob = blob;
}

extern "C" void rpios_runtime_storage_flush()
{
	std::string blob = serializeEntries();
	if (blob == g_last_blob) return;
	gea::framework::services::StorageService::saveKv(blob);
	g_last_blob.swap(blob);
}
