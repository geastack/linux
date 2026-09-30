/* targets/raspberry-pi-os/main/rpios_storage.cpp
 * File-backed StorageService — persistence for the app-facing `localStorage`
 * (gea::host::StorageFacade serializes the whole KV set to one opaque blob;
 * loadKv/saveKv store it verbatim) and for device-settings strings
 * (getString/setString, a small chunked KV file of our own).
 *
 * Layout: $GEA_RPIOS_STORAGE_DIR, else $XDG_DATA_HOME/gea/<app-id>, else
 * ~/.local/share/gea/<app-id>. Files: localstorage.bin, settings.bin.
 * Writes are atomic (tmp + rename) so a crash mid-write never corrupts the
 * previous state. saveKv is called from the frame task once per frame at most
 * (StorageFacade::flushPending only writes when dirty), so plain synchronous
 * IO is fine.
 *
 * The embedded analogue is targets/esp32/services/storage_service.cpp (NVS
 * blob "ls_kv"); no other desktop target implements persistence yet.
 */

#include "services/storage_service.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef GEA_RPIOS_APP_ID
#define GEA_RPIOS_APP_ID "app"
#endif

namespace {

std::string storageDir()
{
	static std::string dir = [] {
		const char *override_dir = std::getenv("GEA_RPIOS_STORAGE_DIR");
		if (override_dir && *override_dir) return std::string(override_dir);
		std::string base;
		const char *xdg = std::getenv("XDG_DATA_HOME");
		if (xdg && *xdg) {
			base = xdg;
		} else {
			const char *home = std::getenv("HOME");
			base = std::string(home && *home ? home : ".") + "/.local/share";
		}
		return base + "/gea/" + GEA_RPIOS_APP_ID;
	}();
	return dir;
}

// mkdir -p for the storage dir (each missing component, EEXIST is fine).
bool ensureDir(const std::string &path)
{
	std::string partial;
	partial.reserve(path.size());
	for (size_t i = 0; i <= path.size(); i++) {
		if (i < path.size() && path[i] != '/') {
			partial.push_back(path[i]);
			continue;
		}
		if (!partial.empty() && ::mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) return false;
		if (i < path.size()) partial.push_back('/');
	}
	return true;
}

bool readFile(const std::string &path, std::string &out)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (size < 0) { std::fclose(f); return false; }
	out.resize((size_t)size);
	const size_t got = size > 0 ? std::fread(&out[0], 1, (size_t)size, f) : 0;
	std::fclose(f);
	if (got != (size_t)size) { out.clear(); return false; }
	return true;
}

bool writeFileAtomic(const std::string &path, const std::string &data)
{
	if (!ensureDir(storageDir())) return false;
	const std::string tmp = path + ".tmp";
	FILE *f = std::fopen(tmp.c_str(), "wb");
	if (!f) return false;
	const bool wrote = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
	const bool flushed = std::fflush(f) == 0 && ::fsync(fileno(f)) == 0;
	std::fclose(f);
	if (!wrote || !flushed) { ::unlink(tmp.c_str()); return false; }
	if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
	return true;
}

std::string localStoragePath() { return storageDir() + "/localstorage.bin"; }
std::string settingsPath() { return storageDir() + "/settings.bin"; }

// Settings map, loaded lazily; persisted with the same 4-byte-length chunk
// framing StorageFacade uses for the localStorage blob.
std::unordered_map<std::string, std::string> &settings()
{
	static std::unordered_map<std::string, std::string> map = [] {
		std::unordered_map<std::string, std::string> loaded;
		std::string blob;
		if (readFile(settingsPath(), blob)) {
			size_t pos = 0;
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
			while (readChunk(k) && readChunk(v)) loaded[k] = v;
		}
		return loaded;
	}();
	return map;
}

bool persistSettings()
{
	std::string blob;
	for (const auto &kv : settings()) {
		for (const std::string *part : {&kv.first, &kv.second}) {
			const std::uint32_t len = (std::uint32_t)part->size();
			char header[sizeof(len)];
			std::memcpy(header, &len, sizeof(len));
			blob.append(header, sizeof(header));
			blob.append(*part);
		}
	}
	return writeFileAtomic(settingsPath(), blob);
}

}  // namespace

namespace gea::framework::services {

bool StorageService::init() { return ensureDir(storageDir()); }

bool StorageService::getString(const char *key, char *buf, unsigned capacity)
{
	if (!key || !buf || capacity == 0) return false;
	const auto &map = settings();
	const auto it = map.find(key);
	if (it == map.end()) return false;
	std::snprintf(buf, capacity, "%s", it->second.c_str());
	return true;
}

bool StorageService::setString(const char *key, const char *value)
{
	if (!key) return false;
	settings()[key] = value ? value : "";
	return persistSettings();
}

bool StorageService::loadKv(std::string &out)
{
	out.clear();
	readFile(localStoragePath(), out);
	return true;
}

void StorageService::saveKv(const std::string &blob)
{
	if (blob.empty()) {
		::unlink(localStoragePath().c_str());
		return;
	}
	writeFileAtomic(localStoragePath(), blob);
}

}  // namespace gea::framework::services
