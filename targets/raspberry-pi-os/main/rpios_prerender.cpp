// SPDX-License-Identifier: Apache-2.0

#include "app.h"
#include "canvas.h"
#include "css/declarative.h"
#include "css/engine.h"
#include "display.h"
#include "host/timers.h"
#include "platform/prerender_fonts.h"
#include "platform/prerender_worker.h"
#if __has_include("platform/prerender_video.h")
#include "platform/prerender_video.h"
#define GEA_RPIOS_PRERENDER_VIDEO 1
#endif
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

bool rpios_prepare_prerender(int width, int height);
void rpios_set_prerender_time(int timestampMs);

extern "C" bool gea_linux_write_png(
	const std::uint8_t *pixels, std::uint32_t width, std::uint32_t height, std::size_t stride,
	bool fast, void *context, std::size_t (*writeBytes)(void *, const std::uint8_t *, std::size_t));

namespace {
using gea::platform::prerender::Request;

void prepareCanvas(int width, int height)
{
	if (!rpios_prepare_prerender(width, height)) {
		throw std::runtime_error("Could not allocate the Linux prerender canvas");
	}

	rpios_set_prerender_time(0);
	gea::host::resetAnimationFrameCallbacks();
	gea::host::resetScheduledTimers();
	gea::host::runAnimationFrameCallbacks(0);
	gea::platform::display::Display::clearNoFlush();
}

void startPageAnimations()
{
	if (gea::embedded::ui::Tree::instance().mountedRoot() < 0) {
		throw std::runtime_error("Could not mount the page");
	}

	gea::css::DeclarativeAnimations::scanAndStart(0);
}

void loadPage(int width, int height, double dpr)
{
	prepareCanvas(width, height);
	gea::framework::app::Application::init(width, height, dpr);
	startPageAnimations();
}

void paintFrame(int timeMs)
{
	rpios_set_prerender_time(timeMs);
	gea::embedded::ui::StyleSheet::instance().startCssAnimations(timeMs);
	gea::css::AnimationEngine::instance().tick(static_cast<std::uint32_t>(timeMs));
	gea::framework::app::Application::frame(timeMs);
}

void drawFrames(int fps, int timestampMs)
{
	for (std::int64_t frame = 0;; ++frame) {
		const auto tickMs = (frame * 1000 + fps / 2) / fps;
		const int timeMs = static_cast<int>(std::min<std::int64_t>(tickMs, timestampMs));
		paintFrame(timeMs);
		if (timeMs == timestampMs) return;
	}
}

bool fastPngFileCompression()
{
	const char *setting = std::getenv("GEA_PNG_COMPRESSION");
	if (!setting || std::string_view(setting) == "compact") return false;
	if (std::string_view(setting) == "fast") return true;
	throw std::runtime_error("GEA_PNG_COMPRESSION must be fast or compact");
}

void writePng(const char *path, int width, int height, bool fast, FILE *stream = nullptr,
	const char *mode = "wb")
{
	const auto *canvas = gea::platform::display::Display::canvas();
	const auto *pixels = canvas->pixels();
	int rowStride = canvas->strideBytes();
	const auto *data = reinterpret_cast<const std::uint8_t *>(pixels);
	std::vector<std::uint8_t> converted;

	if constexpr (GEA_EMBEDDED_PIXEL_FORMAT != GEA_PIXEL_RGBA8888 || std::endian::native != std::endian::little) {
		const auto stride = rowStride / sizeof(*pixels);
		rowStride = width * 4;
		converted.resize(static_cast<std::size_t>(rowStride) * height);
		auto *destination = converted.data();

		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				int red, green, blue, alpha;
				gea::framework::graphics::pixel::unpackNative8(pixels[y * stride + x], &red, &green, &blue, &alpha);
				*destination++ = static_cast<std::uint8_t>(red);
				*destination++ = static_cast<std::uint8_t>(green);
				*destination++ = static_cast<std::uint8_t>(blue);
				*destination++ = static_cast<std::uint8_t>(alpha);
			}
		}

		data = converted.data();
	}

	FILE *output = stream ? stream : std::fopen(path, mode);
	if (!output) throw std::runtime_error("Could not open PNG output");

	const bool written = gea_linux_write_png(
		data, width, height, rowStride, fast, output,
		[](void *context, const std::uint8_t *bytes, std::size_t size) {
			return std::fwrite(bytes, 1, size, static_cast<FILE *>(context));
		});
	const bool closed = stream || std::fclose(output) == 0;
	if (!written || !closed) throw std::runtime_error("Could not write PNG");
}

#if defined(GEA_RPIOS_PRERENDER_VIDEO)
class PngFrames {
public:
	PngFrames(std::string_view path, int width, int height, std::int64_t count)
		: path_(path), width_(width), height_(height),
		  digits_(std::max<std::size_t>(6, std::to_string(count - 1).size())),
		  fast_(fastPngFileCompression()) {}

	void write()
	{
		const auto number = std::to_string(index_++);
		const auto output = path_ + "/frame-" + std::string(digits_ - number.size(), '0') + number + ".png";
		writePng(output.c_str(), width_, height_, fast_, nullptr, "wbx");
	}

	void finish() {}

private:
	std::string path_;
	int width_;
	int height_;
	std::size_t digits_;
	bool fast_;
	std::int64_t index_ = 0;
};
#endif

void capture(const Request &request, bool reuse)
{
	if (!reuse) {
		if (setenv("GEA_PRERENDER_INPUT", request.input.c_str(), 1) != 0) {
			throw std::runtime_error("Could not set prerender input");
		}

		loadPage(request.width, request.height, request.dpr);
		drawFrames(60, 0);
	}

	writePng(request.out.c_str(), request.width, request.height, fastPngFileCompression());
}

}

#if defined(GEA_RPIOS_PRERENDER_HTTP)
int gea_renderer_http_serve(int argc, char **argv);

namespace {
constexpr int httpWidth = 1920;
constexpr int httpHeight = 1080;
bool servingHttp = false;
}

bool httpPrerenderMode()
{
	return servingHttp;
}

void preparePage()
{
	prepareCanvas(httpWidth, httpHeight);
	gea::framework::app::Application::prepareMount(httpWidth, httpHeight, 1);
}

void abortPage()
{
	gea::framework::app::Application::finishMount(false);
}

void finishPage()
{
	gea::framework::app::Application::finishMount(true);
	startPageAnimations();
	drawFrames(60, 0);
}

std::vector<std::uint8_t> capturePng(bool fast)
{
	char *bytes = nullptr;
	std::size_t size = 0;
	FILE *stream = open_memstream(&bytes, &size);
	if (!stream) throw std::runtime_error("Could not open PNG memory stream");

	try {
		writePng(nullptr, httpWidth, httpHeight, fast, stream);
	} catch (...) {
		std::fclose(stream);
		std::free(bytes);
		throw;
	}

	const int closed = std::fclose(stream);
	std::unique_ptr<char, decltype(&std::free)> png(bytes, &std::free);
	if (closed != 0) throw std::runtime_error("Could not finalize PNG memory stream");
	return {bytes, bytes + size};
}
#endif

namespace {
template <typename Number>
Number parseNumber(std::string_view value)
{
	Number number{};
	auto result = std::from_chars(value.data(), value.data() + value.size(), number);
	if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
		throw std::runtime_error("Invalid prerender argument");
	}

	return number;
}
}

int rpios_prerender(int argc, char **argv)
{
	gea::platform::prerender::installFontProvider();
	if (argc == 2 && std::string_view(argv[1]) == "--gea-prerender-worker") {
		return gea::platform::prerender::runWorker(capture);
	}

	try {
		if (argc < 2) throw std::runtime_error("Expected a prerender command");
		if (std::string_view(argv[1]) == "--gea-prerender-http") {
#if defined(GEA_RPIOS_PRERENDER_HTTP)
			if (argc != 2) throw std::runtime_error("Unexpected HTTP prerender arguments");
			servingHttp = true;
			return gea_renderer_http_serve(argc, argv);
#else
			throw std::runtime_error("This Linux build has no HTTP prerender support");
#endif
		}

		const bool frames = std::string_view(argv[1]) == "--gea-prerender-frames";
		const bool video = frames || std::string_view(argv[1]) == "--gea-prerender-video";
		if (argc != (video ? 9 : 8)) {
			throw std::runtime_error(video
				? "Expected output, width, height, DPR, FPS, start time and end time"
				: "Expected output, width, height, DPR, FPS and timestamp");
		}
		const int width = parseNumber<int>(argv[3]);
		const int height = parseNumber<int>(argv[4]);
		const double dpr = parseNumber<double>(argv[5]);
		const int fps = parseNumber<int>(argv[6]);
		const int timestampMs = parseNumber<int>(argv[7]);
		if (!gea::platform::prerender::validViewport(width, height, dpr) || fps < 1 || fps > 1000 || timestampMs < 0) {
			throw std::runtime_error("Invalid prerender viewport or clock");
		}

		if (video) {
#if defined(GEA_RPIOS_PRERENDER_VIDEO)
			const int endMs = parseNumber<int>(argv[8]);
			if (endMs <= timestampMs) throw std::runtime_error("Invalid video time range");
			gea::platform::prerender::VideoClock clock(fps);
			if (frames) {
				const auto count = (static_cast<std::int64_t>(endMs - timestampMs) * fps + 999) / 1000;
				PngFrames stream(argv[2], width, height, count);
				loadPage(width, height, dpr);
				gea::platform::prerender::streamVideoFrames(stream, fps, timestampMs, endMs, paintFrame);
			} else {
				gea::platform::prerender::VideoFrames stream(width, height);
				loadPage(width, height, dpr);
				gea::platform::prerender::streamVideoFrames(stream, fps, timestampMs, endMs, paintFrame);
			}
			return 0;
#else
			throw std::runtime_error("Rebuild against a Core version with prerender video support");
#endif
		}

		loadPage(width, height, dpr);
		drawFrames(fps, timestampMs);
		writePng(argv[2], width, height, fastPngFileCompression());
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "[rpios prerender] %s\n", error.what());
		return 1;
	}
}
