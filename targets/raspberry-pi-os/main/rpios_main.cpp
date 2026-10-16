#include <cstdio>
#include "host/timers.h"
#include "host/worker.h"
#include "ui/tree_events.h"
/* targets/raspberry-pi-os/main/rpios_main.cpp
 * Linux desktop entry point. Runs the gea app loop with an SDL2 window as the
 * display (rpios_display.cpp) and SDL mouse/touch/keyboard events as input.
 *
 * Analogue of targets/geaos/main/geaos_main.cpp minus the watch UX (launcher
 * button overlay, settings drag, camera overlay, debug-input FIFO). The
 * runtime pipeline is:
 *   Application::init(w, h, dpr)
 *   per frame:
 *     pump SDL events:
 *       mouse (pointer 0) / fingers (multi-touch) → touch event queue
 *       keys/text → owned-window metadata queue, captured focus allocation → native dispatch/defaults
 *       wheel → Tree::scrollByKeyStep + input::queueRotaryDelta
 *       window resize/minimize/expose → display resize / pause / re-present
 *     dispatch queued events (touch → TouchRuntime; key/rotary drains)
 *     Application::frame(now_ms)
 *     Storage.flushPending()               // persist localStorage if dirty
 *     Tree::refresh(root, w, h)            // shared display-list path
 *       → Display::flushRects(...)         // SDL texture upload + present
 *
 * The key/rotary drains and the Storage load/flush calls mirror what
 * core/runtime.cpp does for targets that use the shared runtime loop — this
 * target excludes runtime.cpp (it runs its own loop), so it must replicate
 * that per-frame plumbing itself.
 */
#include "app.h"
#include "display.h"
#include "host/storage.h"
#include "host/timers.h"
#include "event.h"
#include "events.h"
#include "input.h"
#if __has_include("resident_apps.h")
#include "resident_apps.h"
#define GEA_RPIOS_HAS_RESIDENT_APPS 1
#else
#define GEA_RPIOS_HAS_RESIDENT_APPS 0
#endif
#include "services/frame_scheduler.h"
#include "css/declarative.h"
#include "css/engine.h"
#include "ui/document.h"
#include "ui/refresh_perf.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include "touch.h"

#include <SDL2/SDL.h>
#include "rpios_keyboard.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>
#include <signal.h>
#include <execinfo.h>

extern "C" int gea_embedded_now_ms(void);
extern "C" int rpios_canvas_width();
extern "C" int rpios_canvas_height();
extern "C" int rpios_display_consume_vsync_wait();
extern "C" void rpios_display_present();
extern "C" void rpios_display_toggle_fullscreen();
extern "C" int rpios_display_window_scale();
extern "C" Uint32 rpios_display_window_id();
extern "C" bool rpios_display_keyboard_focus();
extern "C" int rpios_display_resize(int new_width, int new_height);
extern "C" int gea_embedded_apps_launch(const char *app_id);
extern "C" void rpios_install_wifi_driver();

namespace gea::rpios {
void installAppLauncherPlatform(const char *currentAppId);
}

#ifndef GEA_RPIOS_APP_ID
#define GEA_RPIOS_APP_ID "app"
#endif

namespace {

double devicePixelRatio()
{
	const char *v = std::getenv("GEA_RPIOS_DPR");
	if (v && *v) {
		const double dpr = std::atof(v);
		if (dpr >= 0.5 && dpr <= 8.0) return dpr;
	}
	return 2.0;
}

// Drive CSS animations each frame. Like geaos, this target runs its own loop
// and calls Application::frame() directly instead of going through the
// runtime's run_app_frame(), which is the only place that starts and ticks the
// CSS animation engine. Mirrors the geaos main loop / @geastack/core runtime.cpp.
void driveCssAnimations(std::uint32_t nowMs)
{
	const char *active = nullptr;
#if GEA_RPIOS_HAS_RESIDENT_APPS
	active = gea::framework::apps::ResidentApps::activeId();
#endif
	static char lastScanned[64] = {0};
	static bool singleAppScanned = false;
	const bool shouldScan = active ? std::strcmp(active, lastScanned) != 0 : !singleAppScanned;
	if (shouldScan) {
		if (active)
			std::strncpy(lastScanned, active, sizeof(lastScanned) - 1);
		else
			singleAppScanned = true;
		gea::css::DeclarativeAnimations::scanAndStart(nowMs);
		gea::embedded::ui::StyleSheet::instance().startCssAnimations(nowMs);
	}

	gea::css::AnimationEngine::instance().tick(nowMs);
}

volatile sig_atomic_t g_stop = 0;
bool g_paused = false;          // window minimized/hidden — skip frames
bool g_resize_pending = false;  // window size changed — apply before next frame
int g_resize_window_w = 0;
int g_resize_window_h = 0;

// Inject an SDL mouse position as a touch. SDL mouse-event coordinates are
// ALREADY in logical/canvas space: SDL_RenderSetLogicalSize installs a
// renderer event watch that rewrites SDL_MOUSEMOTION/BUTTON coordinates into
// the logical coordinate system. Do NOT call SDL_RenderWindowToLogical here —
// that scales a second time and lands every touch at half the true position
// (found via a uinput-verified drag that hit-tested at ~(126,185) when the
// cursor sat at logical (254,387)).
void injectMouse(gea::platform::touch::Phase phase, bool touching, int x, int y)
{
	const int maxX = rpios_canvas_width() - 1;
	const int maxY = rpios_canvas_height() - 1;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > maxX) x = maxX;
	if (y > maxY) y = maxY;
	gea::platform::touch::Touchscreen::injectEvent(phase, touching, x, y);
}

// ---- multi-touch (real touchscreens) --------------------------------------
// SDL fingers → TouchRuntime::queueTouchEvent with a stable pointer id.
// injectEvent can't be used here: it feeds only the primary pointer. Pointer 0
// is shared with the mouse — a touchscreen tap and a left-click behave
// identically (SDL's touch→mouse synthesis is disabled in rpios_display.cpp,
// so a finger never arrives twice).

constexpr int kMaxFingers = 10;
SDL_FingerID g_finger_ids[kMaxFingers];
bool g_finger_used[kMaxFingers] = {};

int fingerSlot(SDL_FingerID id, bool allocate)
{
	for (int i = 0; i < kMaxFingers; i++)
		if (g_finger_used[i] && g_finger_ids[i] == id) return i;
	if (!allocate) return -1;
	for (int i = 0; i < kMaxFingers; i++) {
		if (!g_finger_used[i]) {
			g_finger_used[i] = true;
			g_finger_ids[i] = id;
			return i;
		}
	}
	return -1;
}

void injectFinger(gea::platform::touch::Phase phase, bool touching, float nx, float ny, int pointerId)
{
	// tfinger coords are normalized; the renderer's logical-size event watch
	// keeps them proportional to the logical canvas.
	int x = (int)(nx * (float)rpios_canvas_width());
	int y = (int)(ny * (float)rpios_canvas_height());
	const int maxX = rpios_canvas_width() - 1;
	const int maxY = rpios_canvas_height() - 1;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > maxX) x = maxX;
	if (y > maxY) y = maxY;
	gea::framework::events::TouchRuntime::queueTouchEvent(
	    static_cast<gea::framework::events::TouchPhase>(phase), touching, x, y, pointerId);
}

void markNodeFullyDirty(gea::embedded::ui::Tree &tree, int nodeId)
{
	if (nodeId < 0 || nodeId >= tree.nodeCount()) return;
	tree.markNodeDisplayCommandsDirty(nodeId);
	tree.setDisplayListRebuildRequired(true);
	gea::embedded::ui::Node &node = tree.node(nodeId);
	node.render.dirty = 1;
	node.render.layout_dirty = 1;
	node.render.non_scroll_dirty = 1;
}

// ---- keyboard --------------------------------------------------------------

gea::rpios::keyboard::Producer g_keyboard;

// ---- per-frame input drains (mirrors core/runtime.cpp, which this target
// excludes because it runs its own loop) ------------------------------------

void dispatchRotaryInput()
{
	const int delta = gea::framework::input::consumeRotaryDelta();
	if (delta == 0) return;
	gea::embedded::ui::dispatchDocumentRotary(delta);
}

void dispatchKeyInput()
{
 g_keyboard.bind(rpios_display_window_id(),rpios_display_keyboard_focus());
 g_keyboard.drain(rpios_display_toggle_fullscreen);
 // Compatibility for non-SDL hardware producers. Physical SDL keys never enter
 // this integer queue, so they cannot be delivered twice or lose their metadata.
 gea::framework::input::HardwareKeyEvent key{};
 while(gea::framework::input::consumeKeyEvent(key)){
  auto owner=gea::embedded::ui::Document::instance().activeElement();
  if(!owner.valid())owner=gea::embedded::ui::NodeHandle(gea::embedded::ui::Tree::instance().mountedRoot());
  if(!owner.isConnected()||owner.disabled()||gea::embedded::ui::selector::disabled(gea::embedded::ui::Tree::instance(),owner.id()))continue;
  gea::framework::events::PointerEvent event{};
  event.type=key.pressed?gea::framework::events::PointerEventType::KeyDown:gea::framework::events::PointerEventType::KeyUp;
  event.keyCode=key.keyCode;event.targetId=owner.id();
  gea::embedded::ui::dispatchRootKeyEvent(event);
 }
}

void setPaused(bool paused)
{
	if (g_paused == paused) return;
	g_paused = paused;
	gea::framework::events::TouchRuntime::setDispatchEnabled(!paused);
	if (!paused) rpios_display_present();  // pixels may be stale after unminimize
}

void pumpSdlEvents()
{
	using gea::platform::touch::Phase;
	static bool mouseDown = false;
 g_keyboard.bind(rpios_display_window_id(),rpios_display_keyboard_focus());
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			g_stop = 1;
			break;
  case SDL_KEYDOWN:
  case SDL_KEYUP:
  case SDL_TEXTINPUT:
  case SDL_TEXTEDITING:
  case SDL_TEXTEDITING_EXT:
   g_keyboard.accept(event);
   if(event.type==SDL_TEXTEDITING_EXT)SDL_free(event.editExt.text);
   break;
		case SDL_MOUSEWHEEL: {
			static const bool inputTrace = [] {
				const char *v = std::getenv("GEA_INPUT_TRACE");
				return v && *v && *v != '0';
			}();
			int notches = event.wheel.y;
			if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) notches = -notches;
			if (notches != 0) {
				// Wheel-up (notches > 0) scrolls toward the top: negative dy.
				const bool scrolled = gea::embedded::ui::Tree::instance().scrollByKeyStep(-notches * 48);
				// Also surface it as rotary detents for apps that listen
				// (knob-first apps from the elecrow rotary board).
				gea::framework::input::queueRotaryDelta(-notches);
				if (inputTrace)
					std::fprintf(stderr, "[rpios.input] wheel notches=%d scrolled=%d\n", notches, scrolled ? 1 : 0);
			}
			break;
		}
		case SDL_MOUSEBUTTONDOWN:
			if (event.button.button == SDL_BUTTON_LEFT) {
				mouseDown = true;
				injectMouse(Phase::Down, true, event.button.x, event.button.y);
			}
			break;
		case SDL_MOUSEMOTION:
			if (mouseDown) {
				injectMouse(Phase::Move, true, event.motion.x, event.motion.y);
			}
			break;
		case SDL_MOUSEBUTTONUP:
			if (event.button.button == SDL_BUTTON_LEFT) {
				mouseDown = false;
				injectMouse(Phase::Up, false, event.button.x, event.button.y);
			}
			break;
		case SDL_FINGERDOWN: {
			const int slot = fingerSlot(event.tfinger.fingerId, true);
			if (slot >= 0) injectFinger(Phase::Down, true, event.tfinger.x, event.tfinger.y, slot);
			break;
		}
		case SDL_FINGERMOTION: {
			const int slot = fingerSlot(event.tfinger.fingerId, false);
			if (slot >= 0) injectFinger(Phase::Move, true, event.tfinger.x, event.tfinger.y, slot);
			break;
		}
		case SDL_FINGERUP: {
			const int slot = fingerSlot(event.tfinger.fingerId, false);
			if (slot >= 0) {
				injectFinger(Phase::Up, false, event.tfinger.x, event.tfinger.y, slot);
				g_finger_used[slot] = false;
			}
			break;
		}
		case SDL_WINDOWEVENT:
   g_keyboard.accept(event);
   if(event.window.windowID!=rpios_display_window_id())break;
			switch (event.window.event) {
			case SDL_WINDOWEVENT_CLOSE:
    g_stop=1;
    break;
   case SDL_WINDOWEVENT_SIZE_CHANGED:
				g_resize_pending = true;
				g_resize_window_w = event.window.data1;
				g_resize_window_h = event.window.data2;
				break;
			case SDL_WINDOWEVENT_FOCUS_LOST:
                gea::embedded::ui::dispatchWindowLifecycleEvent(gea::framework::events::PointerEventType::WindowBlur);
                break;
			case SDL_WINDOWEVENT_MINIMIZED:
			case SDL_WINDOWEVENT_HIDDEN:
				setPaused(true);
				break;
			case SDL_WINDOWEVENT_RESTORED:
			case SDL_WINDOWEVENT_SHOWN:
				setPaused(false);
				break;
			case SDL_WINDOWEVENT_EXPOSED:
				rpios_display_present();
				break;
			}
			break;
		}
	}
}

void dispatchPendingEvents()
{
	for (int i = 0; i < 64; ++i) {
		gea::framework::events::Event event{};
		if (!gea::framework::services::FrameScheduler::receiveEvent(&event)) return;
		switch (event.type) {
		case gea::framework::events::EventType::Touch:
			gea::framework::events::TouchRuntime::dispatchEvent(event);
			break;
		case gea::framework::events::EventType::Frame:
		case gea::framework::events::EventType::Timeout:
		case gea::framework::events::EventType::SettingsToggle:
			break;
		}
	}
}

void on_sigterm(int) { g_stop = 1; }

// Async-signal-safe-ish crash handler. Prints the faulting PC + a short
// backtrace before the process dies (same as geaos_main.cpp).
void on_fatal(int sig, siginfo_t *info, void *ctx)
{
	(void)ctx;
	dprintf(2, "\n[rpios] FATAL signal %d at addr %p\n", sig,
	        info ? info->si_addr : nullptr);
	void *frames[32];
	int n = backtrace(frames, 32);
	dprintf(2, "[rpios] backtrace (%d frames):\n", n);
	backtrace_symbols_fd(frames, n, 2);
	_exit(128 + sig);
}

}  // namespace

// Optional app-owned process cleanup, before the native runtime/static teardown.
// No callback on a skin/resident-app replacement; this is actual process exit.
extern "C" __attribute__((weak)) void gea_native_application_terminate() {}
namespace {
void geaRetireNativeProcess() noexcept {
 static bool retired = false; // Actual process exit on the application thread.
 if (retired) return;
 retired = true;
 try { g_keyboard.shutdown(); }
 catch (...) { std::fputs("Gea: SDL keyboard retirement failed\n", stderr); }
 try { gea_native_application_terminate(); }
 catch (...) { std::fputs("Gea: application termination callback failed; retiring SDK callbacks\n", stderr); }
 try { gea::host::retireScheduledCallbacks(); }
 catch (...) { std::fputs("Gea: scheduled callback retirement failed\n", stderr); }
 gea::embedded::ui::retireNativeEventRealm();
 try { gea::host::workers::retireMainContext(); }
 catch (...) { std::fputs("Gea: main Context retirement failed\n", stderr); }
}
struct GeaNativeTerminationGuard {
 ~GeaNativeTerminationGuard(){geaRetireNativeProcess();}
};
}

int main(int argc, char **argv)
{
	GeaNativeTerminationGuard terminationGuard;
	(void)argc; (void)argv;

	setvbuf(stdout, nullptr, _IOLBF, 0);
	setvbuf(stderr, nullptr, _IOLBF, 0);

	signal(SIGTERM, on_sigterm);
	signal(SIGINT,  on_sigterm);
	{
		struct sigaction sa{};
		sa.sa_sigaction = on_fatal;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &sa, nullptr);
		sigaction(SIGBUS,  &sa, nullptr);
		sigaction(SIGABRT, &sa, nullptr);
	}

	if (!gea::platform::display::Display::init()) {
		std::fprintf(stderr, "[rpios] display init failed (no SDL window)\n");
		return 1;
	}
	gea::platform::display::Display::start();

	int w = rpios_canvas_width();
	int h = rpios_canvas_height();
	const double dpr = devicePixelRatio();

	std::fprintf(stderr, "[rpios] init %dx%d dpr=%.1f app=%s\n", w, h, dpr, GEA_RPIOS_APP_ID);

	// Record the app identity: AppManager platform (Apps.currentInstalledAppId)
	// and the C-side current-id used by generated launcher code.
	gea_embedded_apps_launch(GEA_RPIOS_APP_ID);
	gea::rpios::installAppLauncherPlatform(GEA_RPIOS_APP_ID);
	// Report the desktop's real link state — apps gate remote fetches on it.
	rpios_install_wifi_driver();

	// The current compiler binds localStorage to the shared StorageFacade.
	// Restore it before app top-level/mount; flush only its dirty state per frame.
	gea::framework::services::StorageService::init();
	gea::host::Storage.load();

	gea::framework::app::Application::init(w, h, dpr);
	std::fprintf(stderr, "[rpios] init returned\n");

	auto eventQueue = gea::framework::services::FrameScheduler::createEventQueue();
	if (!eventQueue || !gea::framework::events::TouchRuntime::start()) {
		std::fprintf(stderr, "[rpios] touch input unavailable\n");
	}

	// Physical-keyboard text entry for focused <input> nodes (SDL_TEXTINPUT).
	SDL_StartTextInput();

	auto &tree = gea::embedded::ui::Tree::instance();

	// Per-second frame-rate logger, off by default; GEA_LOOP_PERF=1 enables it
	// (same diagnostic as geaos_main.cpp's [geaos.loop] line).
	static const bool loopPerfLog = [] {
		const char *v = std::getenv("GEA_LOOP_PERF");
		return v && *v && *v != '0';
	}();
	int64_t fps_window_start_ns = 0;
	int fps_window_frames = 0;
	int64_t fps_window_app_frame_us = 0;
	int64_t fps_window_tree_refresh_us = 0;

	while (!g_stop) {
		struct timespec t0;
		clock_gettime(CLOCK_MONOTONIC, &t0);

		pumpSdlEvents();

		// Minimized/hidden: keep pumping events (so quit works) but skip
		// frames entirely; input dispatch is gated off in setPaused.
		if (g_paused) {
			struct timespec idle{0, 100000000L};  // 100 ms
			nanosleep(&idle, nullptr);
			continue;
		}

		// Apply a window resize before the frame: rebuild the framebuffer +
		// texture at the new logical size, re-derive the framework viewport,
		// and mark every node dirty so the next refresh repaints the world.
		if (g_resize_pending) {
			g_resize_pending = false;
			const int scale = rpios_display_window_scale();
			const int newW = g_resize_window_w / (scale > 0 ? scale : 1);
			const int newH = g_resize_window_h / (scale > 0 ? scale : 1);
			if (rpios_display_resize(newW, newH)) {
				w = rpios_canvas_width();
				h = rpios_canvas_height();
				gea::embedded::ui::setViewportMetrics(w, h, dpr);
				gea::embedded::ui::Document::setPreferredMountSize(w, h);
                gea::embedded::ui::dispatchWindowLifecycleEvent(gea::framework::events::PointerEventType::WindowResize);
				const int count = tree.nodeCount();
				for (int id = 0; id < count; ++id) markNodeFullyDirty(tree, id);
				std::fprintf(stderr, "[rpios] resized to %dx%d (window %dx%d)\n",
				             w, h, g_resize_window_w, g_resize_window_h);
			}
		}

		dispatchPendingEvents();

		struct timespec t_af0;
		clock_gettime(CLOCK_MONOTONIC, &t_af0);
		const int nowMs = gea_embedded_now_ms();
		dispatchRotaryInput();
		dispatchKeyInput();
		driveCssAnimations(static_cast<std::uint32_t>(nowMs));
		gea::framework::app::Application::frame(nowMs);
		// Persist localStorage mutations from this frame (no-op unless changed;
		// mirrors runtime.cpp's per-frame Storage.flushPending()).
		gea::host::Storage.flushPending();
		struct timespec t_af1;
		clock_gettime(CLOCK_MONOTONIC, &t_af1);

		int root = tree.mountedRoot();
		if (root >= 0) {
			gea::embedded::ui::NodeHandle(root).style().width(w);
			gea::embedded::ui::NodeHandle(root).style().height(h);
			tree.refresh(root, w, h);
		}

		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		long elapsedNs = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);

		fps_window_frames++;
		fps_window_app_frame_us += ((t_af1.tv_sec - t_af0.tv_sec) * 1000000L + (t_af1.tv_nsec - t_af0.tv_nsec) / 1000L);
		fps_window_tree_refresh_us += ((t1.tv_sec - t_af1.tv_sec) * 1000000L + (t1.tv_nsec - t_af1.tv_nsec) / 1000L);
		int64_t now_ns = (int64_t)t1.tv_sec * 1000000000LL + (int64_t)t1.tv_nsec;
		if (fps_window_start_ns == 0) fps_window_start_ns = now_ns;
		if (now_ns - fps_window_start_ns >= 1000000000L) {
			if (loopPerfLog) {
				const double window_s = (now_ns - fps_window_start_ns) / 1e9;
				const double fps_real = fps_window_frames / window_s;
				const double appMs = (fps_window_app_frame_us / 1000.0) / fps_window_frames;
				const double treeMs = (fps_window_tree_refresh_us / 1000.0) / fps_window_frames;
				const auto rafs = gea::host::animationFramePerfStatsRead();
				gea::host::animationFramePerfStatsReset();
				std::fprintf(stderr, "[rpios.loop] fps=%.1f frames=%d window=%.2fs | app=%.2fms tree.refresh=%.2fms | raf req=%lld ran=%lld drop=%lld\n",
				             fps_real, fps_window_frames, window_s, appMs, treeMs,
				             (long long)rafs.requestCount, (long long)rafs.callbackCount,
				             (long long)rafs.droppedCount);
			}
			fps_window_start_ns = now_ns;
			fps_window_frames = 0;
			fps_window_app_frame_us = 0;
			fps_window_tree_refresh_us = 0;
		}

		// If the display presented this frame, SDL's vsynced present already
		// paced it — an extra nanosleep would beat against the vsync period
		// (same double-pacing trap as geaos). Only sleep on idle frames.
		if (!rpios_display_consume_vsync_wait()) {
			const long sleepNs = (long)gea::framework::services::FrameScheduler::frameIntervalMs() * 1000000L;
			long remainNs = sleepNs - elapsedNs;
			if (remainNs > 0) {
				struct timespec ts { remainNs / 1000000000L, remainNs % 1000000000L };
				nanosleep(&ts, nullptr);
			}
		}
	}

	std::fprintf(stderr, "[rpios] shutting down\n");
	return 0;
}
