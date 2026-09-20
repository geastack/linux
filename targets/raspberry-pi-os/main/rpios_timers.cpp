/* targets/raspberry-pi-os/main/rpios_timers.cpp
 * gea_embedded_now_ms() implementation + FrameScheduler glue.
 * Same POSIX CLOCK_MONOTONIC + mutex-guarded ring queue as
 * targets/geaos/main/geaos_timers.cpp. */

#include "services/frame_scheduler.h"

#include "event.h"

#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <time.h>

namespace {
int millisSinceBoot()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}
int g_frame_interval_ms = gea::framework::services::FrameScheduler::kDefaultFrameIntervalMs;

struct LinuxEventQueue {
	gea::framework::events::Event events[gea::framework::services::FrameScheduler::kEventQueueDepth]{};
	int head = 0;
	int tail = 0;
	int count = 0;
};

LinuxEventQueue g_event_queue;
bool g_event_queue_created = false;
pthread_mutex_t g_event_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

void pushEventLocked(const gea::framework::events::Event &event)
{
	if (g_event_queue.count == gea::framework::services::FrameScheduler::kEventQueueDepth) {
		// Scroll-pacing trace (GEA_SCROLL_TRACE=1): a full queue silently drops
		// the OLDEST event — if that's a touch Down/Up the gesture state
		// corrupts, which shows up as scroll jumps. Make the drop visible.
		static const bool scrollTrace = [] {
			const char *v = std::getenv("GEA_SCROLL_TRACE");
			return v && *v && *v != '0';
		}();
		if (scrollTrace) {
			const auto &dropped = g_event_queue.events[g_event_queue.head];
			std::fprintf(stderr, "[sc.qdrop] t=%d type=%d ph=%d\n", millisSinceBoot(),
			             static_cast<int>(dropped.type), static_cast<int>(dropped.touchPhase));
		}
		g_event_queue.head = (g_event_queue.head + 1) % gea::framework::services::FrameScheduler::kEventQueueDepth;
		g_event_queue.count--;
	}
	g_event_queue.events[g_event_queue.tail] = event;
	g_event_queue.tail = (g_event_queue.tail + 1) % gea::framework::services::FrameScheduler::kEventQueueDepth;
	g_event_queue.count++;
}
}  // namespace

extern "C" int gea_embedded_now_ms(void) { return millisSinceBoot(); }

namespace gea::framework::services {

EventQueue FrameScheduler::createEventQueue()
{
	pthread_mutex_lock(&g_event_queue_mutex);
	g_event_queue.head = 0;
	g_event_queue.tail = 0;
	g_event_queue.count = 0;
	g_event_queue_created = true;
	pthread_mutex_unlock(&g_event_queue_mutex);
	return EventQueue{&g_event_queue};
}

EventQueue FrameScheduler::eventQueue()
{
	pthread_mutex_lock(&g_event_queue_mutex);
	const bool created = g_event_queue_created;
	pthread_mutex_unlock(&g_event_queue_mutex);
	return created ? EventQueue{&g_event_queue} : EventQueue{};
}

bool FrameScheduler::sendEvent(const gea::framework::events::Event &event, int)
{
	pthread_mutex_lock(&g_event_queue_mutex);
	if (!g_event_queue_created) {
		pthread_mutex_unlock(&g_event_queue_mutex);
		return false;
	}
	pushEventLocked(event);
	pthread_mutex_unlock(&g_event_queue_mutex);
	return true;
}

bool FrameScheduler::receiveEvent(gea::framework::events::Event *event)
{
	if (!event) return false;
	pthread_mutex_lock(&g_event_queue_mutex);
	if (!g_event_queue_created || g_event_queue.count == 0) {
		pthread_mutex_unlock(&g_event_queue_mutex);
		return false;
	}
	*event = g_event_queue.events[g_event_queue.head];
	g_event_queue.head = (g_event_queue.head + 1) % kEventQueueDepth;
	g_event_queue.count--;
	pthread_mutex_unlock(&g_event_queue_mutex);
	return true;
}
void FrameScheduler::start(EventQueue) {}
void FrameScheduler::runFrame(const FrameCallbacks &callbacks)
{
	if (callbacks.frame) callbacks.frame(millisSinceBoot(), callbacks.context);
}
void FrameScheduler::setFrameIntervalMs(int intervalMs)
{
	if (intervalMs < kMinFrameIntervalMs) intervalMs = kMinFrameIntervalMs;
	if (intervalMs > kMaxFrameIntervalMs) intervalMs = kMaxFrameIntervalMs;
	g_frame_interval_ms = intervalMs;
}
int FrameScheduler::frameIntervalMs() { return g_frame_interval_ms; }
void FrameScheduler::setFrameRate(double fps)
{
	if (fps <= 0.0) return;
	setFrameIntervalMs(static_cast<int>(1000.0 / fps + 0.5));
}
double FrameScheduler::frameRate() { return 1000.0 / static_cast<double>(g_frame_interval_ms); }
int FrameScheduler::nowMs() { return millisSinceBoot(); }

}  // namespace gea::framework::services
