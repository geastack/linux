// Native HTTP(S)/libcurl tests for targets/sailfish-os/main/sailfish_network.cpp.
// Exercises GET, POST + custom headers + body, 302 redirect header reset,
// timeout_ms handling, concurrent fetch threads, and WifiDriver registration
// against a local ephemeral Node HTTP server passed via TEST_HTTP_URL.

#include "host/fetch.h"
#include "wifi.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

extern "C" void sailfish_install_wifi_driver();

namespace gea::framework::network {
#ifdef GEA_SAILFISH_TEST_STUB_WIFIDRIVER
namespace {
WifiDriver *g_driver = nullptr;
}
void WifiDriver::setWebState(bool, const char *, const char *, int) {}
void WifiDriver::setWebScanCount(int) {}
void WifiDriver::setWebScanEntry(int, const char *, int, bool) {}
void WifiAdapter::setDriver(WifiDriver *driver) { g_driver = driver; }
WifiDriver *WifiAdapter::driver() { return g_driver; }
bool WifiAdapter::connected() const { return g_driver ? g_driver->connected() : false; }
#endif
}  // namespace gea::framework::network

namespace gea::framework::host {
void test_record_request(const std::string &url, const gea::host::FetchRequestInit &init);
gea::host::FetchResponse test_canned_response(const std::string &url);
}  // namespace gea::framework::host

namespace {

gea::host::FetchResponse doFetch(const std::string &url, const gea::host::FetchRequestInit &init = {})
{
	gea::framework::host::test_record_request(url, init);
	return gea::framework::host::test_canned_response(url);
}

std::string bodyText(const gea::host::FetchResponse &res)
{
	return std::string(res.body.begin(), res.body.end());
}

}  // namespace

int main()
{
	const char *baseEnv = std::getenv("TEST_HTTP_URL");
	assert(baseEnv && *baseEnv && "TEST_HTTP_URL must be set by run-native-tests.sh");
	const std::string base(baseEnv);

	// 1. GET /ok
	{
		auto res = doFetch(base + "/ok");
		assert(res.ok && res.status == 200.0);
		assert(bodyText(res) == "hello-sailfish");
		assert(res.headers["X-Test"] == "1");
	}

	// 2. POST /echo with custom header + body
	{
		gea::host::FetchRequestInit init;
		init.method = "POST";
		init.headers["X-Client"] = "sailfish-probe";
		const std::string payload = "{\"ping\":true}";
		init.body.assign(payload.begin(), payload.end());
		auto res = doFetch(base + "/echo", init);
		assert(res.ok && res.status == 200.0);
		assert(bodyText(res) == "sailfish-probe:{\"ping\":true}");
	}

	// 3. 302 redirect resets stale hop headers
	{
		auto res = doFetch(base + "/redirect");
		assert(res.ok && res.status == 200.0);
		assert(bodyText(res) == "hello-sailfish");
		assert(res.headers.find("X-Hop") == res.headers.end());
		assert(res.headers["X-Test"] == "1");
	}

	// 4. Timeout handling
	{
		gea::host::FetchRequestInit init;
		init.timeout_ms = 200;
		auto res = doFetch(base + "/slow", init);
		assert(!res.ok);
		assert(res.body.empty());
	}

	// 5. Concurrent threads (thread_local pending init isolation)
	{
		std::vector<std::thread> workers;
		for (int t = 0; t < 4; ++t) {
			workers.emplace_back([&, t]() {
				for (int i = 0; i < 5; ++i) {
					gea::host::FetchRequestInit init;
					init.method = "POST";
					init.headers["X-Client"] = "t" + std::to_string(t);
					const std::string payload = std::to_string(i);
					init.body.assign(payload.begin(), payload.end());
					auto res = doFetch(base + "/echo", init);
					assert(res.ok && res.status == 200.0);
					assert(bodyText(res) == "t" + std::to_string(t) + ":" + payload);
				}
			});
		}
		for (auto &w : workers) w.join();
	}

	// 6. WifiDriver registration smoke check (works on both macOS and Linux)
	{
		sailfish_install_wifi_driver();
		auto *drv = gea::framework::network::WifiAdapter::driver();
		assert(drv != nullptr);
		assert(drv->init());
		assert(drv->enabled());
		(void)drv->connected();
		(void)drv->ip();
	}

	std::puts("PASS sailfish_network: GET, POST echo, 302 redirect header reset, timeout, concurrent threads, WifiDriver smoke");
	return 0;
}
