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

#include <png.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

bool rpios_prepare_prerender(int width, int height);
void rpios_set_prerender_time(int timestampMs);

namespace {
using gea::platform::prerender::Request;

void loadPage(int width, int height, double dpr)
{
	if (!rpios_prepare_prerender(width, height)) {
		throw std::runtime_error("Could not allocate the Linux prerender canvas");
	}

	rpios_set_prerender_time(0);
	gea::host::resetAnimationFrameCallbacks();
	gea::host::resetScheduledTimers();
	gea::host::runAnimationFrameCallbacks(0);
	gea::platform::display::Display::clearNoFlush();
	gea::framework::app::Application::init(width, height, dpr);
	if (gea::embedded::ui::Tree::instance().mountedRoot() < 0) {
		throw std::runtime_error("Could not mount the page");
	}

	gea::css::DeclarativeAnimations::scanAndStart(0);
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

void writePng(const char *path, int width, int height, FILE *stream = nullptr)
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

	png_image image{};
	image.version = PNG_IMAGE_VERSION;
	image.width = width;
	image.height = height;
	image.format = PNG_FORMAT_RGBA;
	const bool written = stream
		? png_image_write_to_stdio(&image, stream, 0, data, rowStride, nullptr) != 0
		: png_image_write_to_file(&image, path, 0, data, rowStride, nullptr) != 0;
	const std::string error(image.message);
	png_image_free(&image);
	if (!written) throw std::runtime_error("Could not write PNG: " + error);
}

void capture(const Request &request, bool reuse)
{
	if (!reuse) {
		if (setenv("GEA_PRERENDER_INPUT", request.input.c_str(), 1) != 0) {
			throw std::runtime_error("Could not set prerender input");
		}

		loadPage(request.width, request.height, request.dpr);
		drawFrames(60, 0);
	}

	writePng(request.out.c_str(), request.width, request.height);
}

}

#if defined(GEA_RPIOS_PRERENDER_HTTP)
int gea_renderer_http_serve(int argc, char **argv);

std::size_t rpios_capture_http_png(const char *input, bool reuse, const char **bytes)
{
	static constexpr int width = 1920;
	static constexpr int height = 1080;
	static std::string mountedInput;
	static std::unique_ptr<char, decltype(&std::free)> png(nullptr, &std::free);
	static std::size_t pngSize = 0;

	*bytes = nullptr;
	try {
		if (!reuse || mountedInput != input) {
			png.reset();
			pngSize = 0;
			mountedInput.clear();
			if (setenv("GEA_PRERENDER_INPUT", input, 1) != 0) {
				throw std::runtime_error("Could not set prerender input");
			}

			loadPage(width, height, 1);
			drawFrames(60, 0);
			mountedInput = input;
		}

		// A reused HTTP capture never advances the clock or redraws the canvas.
		if (png) {
			*bytes = png.get();
			return pngSize;
		}

		char *buffer = nullptr;
		std::size_t size = 0;
		FILE *stream = open_memstream(&buffer, &size);
		if (!stream) throw std::runtime_error("Could not open PNG memory stream");

		try {
			writePng(nullptr, width, height, stream);
		} catch (...) {
			std::fclose(stream);
			std::free(buffer);
			throw;
		}

		const int closed = std::fclose(stream);
		if (closed != 0) {
			std::free(buffer);
			throw std::runtime_error("Could not finalize PNG memory stream");
		}

		png.reset(buffer);
		pngSize = size;
		*bytes = png.get();
		return pngSize;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "[rpios prerender] %s\n", error.what());
		return 0;
	}
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
			return gea_renderer_http_serve(argc, argv);
#else
			throw std::runtime_error("This Linux build has no HTTP prerender support");
#endif
		}

		const bool video = std::string_view(argv[1]) == "--gea-prerender-video";
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
			gea::platform::prerender::VideoFrames stream(width, height);
			gea::platform::prerender::VideoClock clock(fps);
			loadPage(width, height, dpr);
			gea::platform::prerender::streamVideoFrames(stream, fps, timestampMs, endMs, paintFrame);
			return 0;
#else
			throw std::runtime_error("Rebuild against a Core version with prerender video support");
#endif
		}

		loadPage(width, height, dpr);
		drawFrames(fps, timestampMs);
		writePng(argv[2], width, height);
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "[rpios prerender] %s\n", error.what());
		return 1;
	}
}
