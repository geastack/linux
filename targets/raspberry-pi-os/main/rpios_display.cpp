/* targets/raspberry-pi-os/main/rpios_display.cpp
 * Linux desktop display backend — an SDL2 window under X11/Wayland.
 * Developed and tested on a Raspberry Pi 5 running the Raspberry Pi OS
 * desktop (labwc), but nothing here is Pi-specific.
 *
 * Architecture (same shape as targets/geaos/main/geaos_display.cpp, minus
 * the MTK OVL/fbdev machinery):
 *   - The framework's Canvas is bound to an in-process RGB565 buffer at the
 *     logical window resolution.
 *   - Display::flush() / flushRects() upload the dirty region of that buffer
 *     into an SDL_PIXELFORMAT_RGB565 streaming texture and present it. The
 *     renderer is created with PRESENTVSYNC, so a presenting frame blocks to
 *     vblank — the main loop reads rpios_display_consume_vsync_wait() to skip
 *     its own nanosleep on those frames (same double-pacing fix as geaos).
 *   - Display::pushClip / popClip forward to the Canvas clip stack — the
 *     framework relies on a working clip to constrain each dirty-region
 *     replay; stubbing them tanks fps (see the geaos header comment).
 *   - The window is an integer-scaled view of the logical canvas
 *     (GEA_RPIOS_SCALE, default 2): the canvas stays at the app's tuned
 *     logical size while the on-monitor window is comfortably large.
 *
 * Window size / scale / DPR come from the environment so apps tuned for a
 * particular panel can be reproduced exactly:
 *   GEA_RPIOS_WIDTH   logical canvas width   (default 410 — amoled-2.06)
 *   GEA_RPIOS_HEIGHT  logical canvas height  (default 502)
 *   GEA_RPIOS_SCALE   integer window scale   (default 2)
 *
 * The window is resizable: the main loop reacts to SDL_WINDOWEVENT_SIZE_CHANGED
 * by calling rpios_display_resize(), which reallocates the framebuffer +
 * texture at the new logical size (window size / scale) and re-points the
 * Canvas binding. F11 toggles borderless fullscreen (same resize path).
 */

#include "display.h"
#include "canvas.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int env_int(const char *name, int fallback, int lo, int hi)
{
	const char *v = std::getenv(name);
	if (!v || !*v) return fallback;
	const int n = std::atoi(v);
	if (n < lo || n > hi) return fallback;
	return n;
}

int g_canvas_width = env_int("GEA_RPIOS_WIDTH", gea::platform::display::kWidth, 64, 4096);
int g_canvas_height = env_int("GEA_RPIOS_HEIGHT", gea::platform::display::kHeight, 64, 4096);
int g_window_scale = env_int("GEA_RPIOS_SCALE", 2, 1, 8);

gea::framework::graphics::Canvas g_canvas;
uint16_t *g_framebuffer = nullptr;
size_t g_framebuffer_pixels = 0;

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
bool g_sdl_ok = false;
bool g_vsync_waited = false;

// Static-backdrop cache (see gea_backdrop_cache below). Globals so
// rpios_display_resize can grow it when the logical canvas grows.
uint16_t *g_backdrop_buffer = nullptr;
int g_backdrop_cap = 0;

std::vector<uint16_t> g_stream_rows;

int g_brightness = 100;
uint8_t g_alpha_global = 255;
int g_flush_rows = 0;
int g_flush_depth = 0;

void ensure_canvas()
{
	if (g_framebuffer) return;
	g_framebuffer_pixels = (size_t)g_canvas_width * (size_t)g_canvas_height;
	g_framebuffer = static_cast<uint16_t *>(std::calloc(g_framebuffer_pixels, sizeof(uint16_t)));
	g_canvas.bindPixels(g_framebuffer, g_canvas_width, g_canvas_height);
}

void ensure_window()
{
	if (g_sdl_ok) return;
	ensure_canvas();
	static bool attempted = false;
	if (attempted) return;
	attempted = true;

	// Fingers drive the touch pipeline directly (SDL_FINGER* in rpios_main);
	// without this hint SDL would synthesize mouse events from touches and the
	// same finger would inject twice.
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		std::fprintf(stderr, "[rpios display] SDL_Init failed: %s\n", SDL_GetError());
		return;
	}
#ifndef GEA_RPIOS_APP_TITLE
#define GEA_RPIOS_APP_TITLE "gea"
#endif
	g_window = SDL_CreateWindow(GEA_RPIOS_APP_TITLE,
	                            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                            g_canvas_width * g_window_scale,
	                            g_canvas_height * g_window_scale,
	                            SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
	if (!g_window) {
		std::fprintf(stderr, "[rpios display] SDL_CreateWindow failed: %s\n", SDL_GetError());
		return;
	}
	g_renderer = SDL_CreateRenderer(g_window, -1,
	                                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!g_renderer) {
		// Software renderer fallback (no vsync pacing; the main loop's
		// nanosleep then caps the rate instead).
		g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
	}
	if (!g_renderer) {
		std::fprintf(stderr, "[rpios display] SDL_CreateRenderer failed: %s\n", SDL_GetError());
		return;
	}
	// Integer-scale the logical canvas to the window; nearest keeps pixels crisp.
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	SDL_RenderSetLogicalSize(g_renderer, g_canvas_width, g_canvas_height);
	g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB565,
	                              SDL_TEXTUREACCESS_STREAMING,
	                              g_canvas_width, g_canvas_height);
	if (!g_texture) {
		std::fprintf(stderr, "[rpios display] SDL_CreateTexture failed: %s\n", SDL_GetError());
		return;
	}
	SDL_RendererInfo info{};
	SDL_GetRendererInfo(g_renderer, &info);
	g_sdl_ok = true;
	std::fprintf(stderr, "[rpios display] SDL window %dx%d (scale %d, renderer %s%s)\n",
	             g_canvas_width, g_canvas_height, g_window_scale, info.name,
	             (info.flags & SDL_RENDERER_PRESENTVSYNC) ? ", vsync" : "");
}

void upload_rect(int x0, int y0, int x1, int y1)
{
	if (!g_sdl_ok || !g_framebuffer) return;
	x0 = std::max(0, x0);
	y0 = std::max(0, y0);
	x1 = std::min(g_canvas_width - 1, x1);
	y1 = std::min(g_canvas_height - 1, y1);
	if (x0 > x1 || y0 > y1) return;
	SDL_Rect rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
	const uint16_t *src = g_framebuffer + (size_t)y0 * g_canvas_width + x0;
	SDL_UpdateTexture(g_texture, &rect, src, g_canvas_width * (int)sizeof(uint16_t));
}

// The texture holds the complete current frame (updated per dirty rect), so a
// present is always a full-texture copy into the backbuffer plus a flip.
void present_texture()
{
	if (!g_sdl_ok) return;
	SDL_RenderClear(g_renderer);
	SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
	SDL_RenderPresent(g_renderer);
	g_vsync_waited = true;
}

// Copy a block of RGB565 rows into the software canvas (used by streamRect so
// g_framebuffer stays the complete current frame for the next full upload).
void copy_rgb565_rows_to_canvas(const uint16_t *pixels, int x, int y, int width, int rows)
{
	if (!pixels || !g_framebuffer || width <= 0 || rows <= 0) return;
	for (int row = 0; row < rows; row++) {
		const int dstY = y + row;
		if (dstY < 0 || dstY >= g_canvas_height) continue;
		int dstX = x;
		int srcOffset = 0;
		int run = width;
		if (dstX < 0) { srcOffset = -dstX; run -= srcOffset; dstX = 0; }
		if (dstX + run > g_canvas_width) run = g_canvas_width - dstX;
		if (run <= 0) continue;
		std::memcpy(g_framebuffer + (size_t)dstY * g_canvas_width + dstX,
		            pixels + (size_t)row * width + srcOffset,
		            (size_t)run * sizeof(uint16_t));
	}
}

}  // namespace

extern "C" int rpios_canvas_width()  { return g_canvas_width; }
extern "C" int rpios_canvas_height() { return g_canvas_height; }

// Returns 1 (and clears the flag) if a present blocked on vsync since the last
// call — the main loop uses this to avoid double-pacing (its own nanosleep on
// top of SDL's vsynced present). Same contract as geaos_display_consume_vsync_wait.
extern "C" int rpios_display_consume_vsync_wait()
{
	if (g_vsync_waited) { g_vsync_waited = false; return 1; }
	return 0;
}

// Full-screen scratch for the renderer's static-backdrop cache (see
// DisplayList::maybeBakeStaticBackdrop and the geaos_display.cpp comment: the
// weak nullptr default makes dirty-region replay erase static content painted
// before an animated subtree). Desktop RAM is plentiful; one RGB565 canvas.
// Grown by rpios_display_resize when the logical canvas outgrows it.
extern "C" std::uint16_t *gea_backdrop_cache(int *cap_px)
{
	if (!g_backdrop_buffer && g_canvas_width > 0 && g_canvas_height > 0) {
		g_backdrop_cap = g_canvas_width * g_canvas_height;
		g_backdrop_buffer = static_cast<std::uint16_t *>(std::malloc((size_t)g_backdrop_cap * sizeof(std::uint16_t)));
		if (!g_backdrop_buffer) g_backdrop_cap = 0;
	}
	if (cap_px) *cap_px = g_backdrop_buffer ? g_backdrop_cap : 0;
	return g_backdrop_buffer;
}

// Re-present the current texture (window exposed/restored after being
// obscured; the framework doesn't know pixels were lost).
extern "C" void rpios_display_present()
{
	if (g_sdl_ok) present_texture();
}

extern "C" void rpios_display_toggle_fullscreen()
{
	if (!g_window) return;
	const bool full = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
	SDL_SetWindowFullscreen(g_window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
	// The resulting SDL_WINDOWEVENT_SIZE_CHANGED drives the resize path.
}

extern "C" int rpios_display_window_scale() { return g_window_scale; }

// Rebuild the framebuffer + streaming texture at a new logical size (window
// resized or fullscreen toggled). The canvas rebinds to the new buffer; the
// caller (rpios_main) then updates the framework viewport metrics and marks
// the tree fully dirty so the next frame repaints everything.
extern "C" int rpios_display_resize(int new_width, int new_height)
{
	if (new_width < 64) new_width = 64;
	if (new_height < 64) new_height = 64;
	if (new_width > 4096) new_width = 4096;
	if (new_height > 4096) new_height = 4096;
	if (new_width == g_canvas_width && new_height == g_canvas_height) return 0;

	const size_t pixels = (size_t)new_width * (size_t)new_height;
	uint16_t *fb = static_cast<uint16_t *>(std::calloc(pixels, sizeof(uint16_t)));
	if (!fb) return 0;

	if (g_sdl_ok) {
		SDL_Texture *texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB565,
		                                         SDL_TEXTUREACCESS_STREAMING,
		                                         new_width, new_height);
		if (!texture) {
			std::fprintf(stderr, "[rpios display] resize texture failed: %s\n", SDL_GetError());
			std::free(fb);
			return 0;
		}
		SDL_DestroyTexture(g_texture);
		g_texture = texture;
	}

	std::free(g_framebuffer);
	g_framebuffer = fb;
	g_framebuffer_pixels = pixels;
	g_canvas_width = new_width;
	g_canvas_height = new_height;
	g_canvas.bindPixels(g_framebuffer, g_canvas_width, g_canvas_height);
	if (g_sdl_ok) SDL_RenderSetLogicalSize(g_renderer, g_canvas_width, g_canvas_height);

	const int cap = new_width * new_height;
	if (g_backdrop_buffer && cap > g_backdrop_cap) {
		uint16_t *grown = static_cast<uint16_t *>(std::realloc(g_backdrop_buffer, (size_t)cap * sizeof(uint16_t)));
		if (grown) {
			g_backdrop_buffer = grown;
			g_backdrop_cap = cap;
		}
		// realloc failure keeps the old (smaller) cache; the renderer checks
		// the cap and skips baking when it doesn't fit.
	}
	return 1;
}

namespace gea::platform::display {

bool Display::init() { ensure_window(); return g_sdl_ok; }
bool Display::start() { return true; }

gea::framework::graphics::Canvas *Display::canvas()
{
	ensure_canvas();
	return &g_canvas;
}

// The retained renderer only rebinds the canvas on the ESP32 fused-replay
// flush; here it just re-asserts the full-frame staging binding.
void Display::rebindCanvasToFramebuffer()
{
	ensure_canvas();
	g_canvas.bindPixels(g_framebuffer, g_canvas_width, g_canvas_height);
}

void Display::clear()
{
	ensure_canvas();
	std::memset(g_framebuffer, 0, g_framebuffer_pixels * sizeof(uint16_t));
	flush();
}

void Display::clearNoFlush()
{
	ensure_canvas();
	std::memset(g_framebuffer, 0, g_framebuffer_pixels * sizeof(uint16_t));
}

void Display::print(const char *) {}

void Display::flush()
{
	ensure_window();
	upload_rect(0, 0, g_canvas_width - 1, g_canvas_height - 1);
	present_texture();
	g_canvas.resetDirty();
}

void Display::flushRects(const DisplayFlushRect *rects, int count, bool)
{
	ensure_window();
	if (!rects || count <= 0) return;
	for (int i = 0; i < count; i++) {
		upload_rect(rects[i].x0, rects[i].y0, rects[i].x1, rects[i].y1);
	}
	present_texture();
	g_canvas.resetDirty();
}

bool Display::streamRect(int x, int y, int w, int h, DisplayStreamRasterFn raster, void *user)
{
	if (!raster || w <= 0 || h <= 0) return false;
	ensure_window();
	if (!g_sdl_ok) return false;

	int x0 = std::max(0, x);
	int y0 = std::max(0, y);
	int x1 = std::min(g_canvas_width - 1, x + w - 1);
	int y1 = std::min(g_canvas_height - 1, y + h - 1);
	if (x0 > x1 || y0 > y1) return true;

	const int width = x1 - x0 + 1;
	const int maxRows = std::max(1, g_flush_rows > 0 ? g_flush_rows : 80);
	g_stream_rows.resize((size_t)width * (size_t)maxRows);
	for (int row = y0; row <= y1; row += maxRows) {
		int rows = maxRows;
		if (row + rows > y1 + 1) rows = y1 - row + 1;
		raster(g_stream_rows.data(), width, rows, x0, row, user);
		copy_rgb565_rows_to_canvas(g_stream_rows.data(), x0, row, width, rows);
	}
	upload_rect(x0, y0, x1, y1);
	present_texture();
	g_canvas.resetDirty();
	return true;
}

bool Display::present(const DisplayPresentCommand *, int)
{
	// No batched hardware present here. Returning false routes endBatch()
	// through replayPresentBatchToCanvas so the canvas pixel buffer receives
	// each frame (see the macos_display.mm comment: returning true silently
	// drops per-frame draws and freezes canvas animations on frame 1).
	return false;
}

void Display::setFlushConfig(int chunk_rows, int queue_depth) { g_flush_rows = chunk_rows; g_flush_depth = queue_depth; }
int Display::flushChunkRows() { return g_flush_rows; }
int Display::flushQueueDepth() { return g_flush_depth; }
int Display::flushBufferBytes() { return 0; }

// Clip forwarding to the Canvas — without this the framework's per-dirty-rect
// clip narrowing is lost and Canvas::fillRect iterates the full canvas for
// every dirty region (20× cost; see geaos_display.cpp).
void Display::pushClip(int x, int y, int w, int h) { ensure_canvas(); g_canvas.pushClip(x, y, w, h); }
void Display::popClip()                            { ensure_canvas(); g_canvas.popClip(); }
void Display::resetClip()                          { ensure_canvas(); g_canvas.resetClip(); }
void Display::setAlpha(uint8_t a) { g_alpha_global = a; }
uint8_t Display::alpha() { return g_alpha_global; }
int Display::brightness() { return g_brightness; }
void Display::setBrightness(int b)
{
	if (b < 0) b = 0;
	if (b > 100) b = 100;
	g_brightness = b;
}

// SDL's PRESENTVSYNC already paces presents; the explicit TE-style frame wait
// used by some panels is not needed here.
void Display::setVSync(bool) {}
void Display::invalidate() {}
bool Display::vsyncEnabled() { return false; }
void Display::vsyncWaitForFrame() {}

void Display::clip(int *x0, int *y0, int *x1, int *y1)
{
	if (x0) *x0 = 0;
	if (y0) *y0 = 0;
	if (x1) *x1 = g_canvas_width - 1;
	if (y1) *y1 = g_canvas_height - 1;
}

void Display::fillRect(int x, int y, int w, int h, uint16_t color) { ensure_canvas(); g_canvas.fillRect(x, y, w, h, color); }
void Display::scrollRect(int x, int y, int w, int h, int dx, int dy) { ensure_canvas(); g_canvas.scrollRect(x, y, w, h, dx, dy); }
void Display::resetScrollRegion() {}
void Display::strokeRect(int x, int y, int w, int h, uint16_t color) { ensure_canvas(); g_canvas.strokeRect(x, y, w, h, color); }
void Display::fillCircle(int x, int y, int r, uint16_t color) { ensure_canvas(); g_canvas.fillCircle(x, y, r, color); }
void Display::strokeCircle(int x, int y, int r, uint16_t color) { ensure_canvas(); g_canvas.strokeCircle(x, y, r, color); }
void Display::drawLine(int x0, int y0, int x1, int y1, uint16_t color) { ensure_canvas(); g_canvas.drawLine(x0, y0, x1, y1, color); }
void Display::drawArc(int cx, int cy, int r, int s, int e, uint16_t color) { ensure_canvas(); g_canvas.drawArc(cx, cy, r, s, e, color); }
void Display::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) { ensure_canvas(); g_canvas.fillTriangle(x0, y0, x1, y1, x2, y2, color); }
void Display::drawText(const char *text, int x, int y, uint16_t color, float scale)
{
	ensure_canvas();
	g_canvas.drawText(text, x, y, color, scale);
}
void Display::drawTextFont(const char *text, int x, int y, uint16_t color, int font_id)
{
	ensure_canvas();
	g_canvas.drawTextFont(text, x, y, color, font_id);
}
void Display::drawTextFontFamily(const char *text, int x, int y, uint16_t color, int family_id, int size_px)
{
	ensure_canvas();
	g_canvas.drawTextFontFamily(text, x, y, color, family_id, size_px);
}
void Display::setPixel(int x, int y, uint16_t color) { ensure_canvas(); g_canvas.fillRect(x, y, 1, 1, color); }
void Display::fillRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, uint16_t color) { ensure_canvas(); g_canvas.fillRoundedRect(x, y, w, h, tl, tr, br, bl, color); }
void Display::fillRoundedRectBoxesRgb565(const int16_t *xs, const int16_t *ys, int count, int w, int h,
                                         int tl, int tr, int br, int bl, const uint16_t *colors)
{
	ensure_canvas();
	g_canvas.fillRoundedRectBoxesRgb565(xs, ys, count, w, h, tl, tr, br, bl, colors);
}
void Display::strokeRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, int lw, uint16_t color)
{
	ensure_canvas();
	g_canvas.strokeRoundedRect(x, y, w, h, tl, tr, br, bl, lw, color);
}
void Display::blitImage(const gea::framework::graphics::pixel::native_t *src, const uint8_t *alpha, int src_w, int src_h, int dx, int dy)
{
	ensure_canvas();
	g_canvas.drawImage(src, alpha, src_w, src_h, dx, dy);
}
void Display::blitImageScaled(const gea::framework::graphics::pixel::native_t *src, const uint8_t *alpha, int src_w, int src_h,
                              int dx, int dy, int dst_w, int dst_h)
{
	ensure_canvas();
	g_canvas.drawImage(src, alpha, src_w, src_h, dx, dy, dst_w, dst_h);
}
void Display::setWorldOverlay(const uint16_t *, int, int, int, int) {}
void Display::setWorldScroll(int) {}

}  // namespace gea::platform::display
