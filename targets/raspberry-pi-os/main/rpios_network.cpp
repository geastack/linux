/* targets/raspberry-pi-os/main/rpios_network.cpp
 * Real HTTP(S) for the framework's fetch facade, via libcurl.
 *
 * core's host/host/fetch.cpp has three arms: browser (emscripten), ESP-IDF,
 * and a desktop fallback that calls two WEAK hooks —
 *   test_record_request(url, init)   then   test_canned_response(url)
 * — and returns whatever the latter produces (empty by default). The Android
 * target established the pattern of strong-overriding that seam to inject
 * real responses; this file does the same with libcurl (already a Raspberry
 * Pi OS system package).
 *
 * test_canned_response only receives the URL, so test_record_request stashes
 * the request init in a thread_local for it to pick up. Both hooks are called
 * back-to-back on the same thread by FetchHost::fetch — the caller's thread
 * for a synchronous app fetch, or a detached worker for fetchAsync — so the
 * thread_local is safe under concurrent async fetches, and blocking curl
 * transfers only stall the thread that asked for them.
 */

#include "host/fetch.h"
#include "wifi.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

namespace {

thread_local gea::host::FetchRequestInit t_pending_init;
thread_local bool t_has_pending_init = false;

size_t writeBody(char *data, size_t size, size_t nmemb, void *user)
{
	auto *body = static_cast<std::vector<std::uint8_t> *>(user);
	body->insert(body->end(), reinterpret_cast<std::uint8_t *>(data),
	             reinterpret_cast<std::uint8_t *>(data) + size * nmemb);
	return size * nmemb;
}

struct HeaderState {
	gea::host::FetchHeaderMap *headers;
	std::string *statusText;
};

size_t writeHeader(char *data, size_t size, size_t nmemb, void *user)
{
	auto *state = static_cast<HeaderState *>(user);
	std::string line(data, size * nmemb);
	while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
	if (line.rfind("HTTP/", 0) == 0) {
		// Status line — headers of any previous hop (redirect) are stale.
		state->headers->clear();
		const size_t code = line.find(' ');
		if (code != std::string::npos) {
			const size_t text = line.find(' ', code + 1);
			if (text != std::string::npos) *state->statusText = line.substr(text + 1);
		}
		return size * nmemb;
	}
	const size_t colon = line.find(':');
	if (colon != std::string::npos) {
		std::string key = line.substr(0, colon);
		size_t start = colon + 1;
		while (start < line.size() && line[start] == ' ') start++;
		(*state->headers)[key] = line.substr(start);
	}
	return size * nmemb;
}

gea::host::FetchResponse curlFetch(const std::string &url, const gea::host::FetchRequestInit &init)
{
	static std::once_flag globalInit;
	std::call_once(globalInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

	gea::host::FetchResponse response;
	CURL *curl = curl_easy_init();
	if (!curl) return response;

	HeaderState headerState{&response.headers, &response.status_text};
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &writeHeader);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headerState);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // required off the main thread
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // gzip/deflate, auto-decoded
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)(init.timeout_ms > 0 ? init.timeout_ms : 30000));
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "gea-rpios/1.0");

	if (init.method == "POST") {
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
	} else if (!init.method.empty() && init.method != "GET") {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, init.method.c_str());
	}
	if (!init.body.empty() || init.method == "POST" || init.method == "PUT" || init.method == "PATCH") {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, init.body.data());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)init.body.size());
	}

	curl_slist *requestHeaders = nullptr;
	for (const auto &kv : init.headers) {
		const std::string line = kv.first + ": " + kv.second;
		requestHeaders = curl_slist_append(requestHeaders, line.c_str());
	}
	if (requestHeaders) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, requestHeaders);

	const CURLcode rc = curl_easy_perform(curl);
	if (rc == CURLE_OK) {
		long code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		response.status = (double)code;
		response.ok = code >= 200 && code < 300;
	} else {
		std::fprintf(stderr, "[rpios net] fetch %s failed: %s\n", url.c_str(), curl_easy_strerror(rc));
		response.body.clear();
	}

	if (requestHeaders) curl_slist_free_all(requestHeaders);
	curl_easy_cleanup(curl);
	return response;
}

}  // namespace

namespace gea::framework::host {

// Strong overrides of the weak desktop-fallback hooks in host/host/fetch.cpp.
void test_record_request(const std::string &, const gea::host::FetchRequestInit &init)
{
	t_pending_init = init;
	t_has_pending_init = true;
}

gea::host::FetchResponse test_canned_response(const std::string &url)
{
	gea::host::FetchRequestInit init;
	if (t_has_pending_init) {
		init = std::move(t_pending_init);
		t_pending_init = {};
		t_has_pending_init = false;
	}
	return curlFetch(url, init);
}

}  // namespace gea::framework::host

namespace {

// WiFi *status* driver. Several apps gate remote fetches on
// `wifi().connected()` (e.g. maps skips tile downloads without it), so the
// desktop must report its real link state even though fetch itself just uses
// the host TCP stack. "Connected" = the kernel has a default route — true for
// wired Ethernet too, which is the honest answer to "can I reach the
// network". Scanning/configuration are desktop no-ops (NetworkManager owns
// the link).
class LinuxWifiDriver final : public gea::framework::network::WifiDriver {
public:
	bool init() override { return true; }
	bool enabled() const override { return true; }
	void setEnabled(bool) override {}
	bool connected() const override { return !defaultRouteInterface().empty(); }
	int rssi() override { return connected() ? -50 : 0; }
	std::string ssid() override { return defaultRouteInterface(); }
	std::string ip() const override
	{
		const std::string iface = defaultRouteInterface();
		if (iface.empty()) return "";
		std::string result;
		struct ifaddrs *addrs = nullptr;
		if (getifaddrs(&addrs) != 0) return "";
		for (struct ifaddrs *a = addrs; a; a = a->ifa_next) {
			if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
			if (iface != a->ifa_name) continue;
			char buf[INET_ADDRSTRLEN] = {0};
			const auto *sin = reinterpret_cast<const struct sockaddr_in *>(a->ifa_addr);
			if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) result = buf;
			break;
		}
		freeifaddrs(addrs);
		return result;
	}
	std::string mac() override
	{
		const std::string iface = defaultRouteInterface();
		if (iface.empty()) return "";
		std::ifstream f("/sys/class/net/" + iface + "/address");
		std::string mac;
		std::getline(f, mac);
		return mac;
	}
	void configure(const std::string &, const std::string &) override {}
	void scan() override {}
	bool scanning() const override { return false; }
	int scanCount() const override { return 0; }
	gea::framework::network::WifiNetwork networkAt(int) const override { return {}; }
	std::vector<gea::framework::network::WifiNetwork> scanResults() const override { return {}; }

private:
	// Interface of the kernel default route (/proc/net/route destination 0).
	static std::string defaultRouteInterface()
	{
		std::ifstream route("/proc/net/route");
		std::string line;
		std::getline(route, line);  // header
		while (std::getline(route, line)) {
			std::istringstream fields(line);
			std::string iface, dest;
			if (!(fields >> iface >> dest)) continue;
			if (dest == "00000000") return iface;
		}
		return "";
	}
};

}  // namespace

extern "C" void rpios_install_wifi_driver()
{
	static LinuxWifiDriver driver;
	gea::framework::network::WifiAdapter::setDriver(&driver);
}
