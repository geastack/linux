/* targets/raspberry-pi-os/main/rpios_sensors.cpp
 * Accelerometer + memory-diagnostics stubs, plus the platform Touchscreen.
 *
 * Unlike geaos (which spawns an evdev reader thread), the linux target has no
 * hardware touch reader: the SDL event pump in rpios_main.cpp translates mouse
 * events into Touchscreen::injectEvent calls on the main thread. This file
 * only keeps the observer + latest-point cache the shared touch_runtime
 * expects (see targets/geaos/main/geaos_sensors.cpp for the contract). */

#include "imu.h"
#include "memory.h"
#include "touch.h"

#include <cstdint>
#include <pthread.h>

namespace gea::platform::sensors {

void Accelerometer::init() {}
void Accelerometer::close() {}
void Accelerometer::calibrateBias() {}
int Accelerometer::tiltX() { return 0; }
int Accelerometer::tiltY() { return 0; }
double Accelerometer::accelerationX() { return 0.0; }
double Accelerometer::accelerationY() { return 0.0; }
double Accelerometer::accelerationZ() { return 9.80665; }
double Accelerometer::gyroscopeX() { return 0.0; }
double Accelerometer::gyroscopeY() { return 0.0; }
double Accelerometer::gyroscopeZ() { return 0.0; }
void Accelerometer::setWebTilt(int, int) {}

}  // namespace gea::platform::sensors

// Memory diagnostics — zeros; the embedded-only metrics have no desktop analog.
namespace gea::platform::memory {

std::uint32_t Memory::internalFree() { return 0; }
std::uint32_t Memory::internalLargestFreeBlock() { return 0; }
std::uint32_t Memory::internalMinimumFree() { return 0; }
std::uint32_t Memory::psramFree() { return 0; }
std::uint32_t Memory::currentTaskStackHighWaterMark() { return 0; }
std::uint32_t Memory::geaMainStackBytes() { return 0; }
std::uint32_t Memory::geaInitStackBytes() { return 0; }
std::uint32_t Memory::appFrameStackWords() { return 0; }
std::uint32_t Memory::appFrameStackBytes() { return 0; }
std::uint32_t Memory::displayFlushConfiguredRows() { return 0; }
std::uint32_t Memory::displayFlushConfiguredDepth() { return 0; }
std::uint32_t Memory::displayFlushBufferMaxBytes() { return 0; }
std::uint32_t Memory::displayFlushRows() { return 0; }
std::uint32_t Memory::displayFlushDepth() { return 0; }
std::uint32_t Memory::displayFlushBufferBytes() { return 0; }

}  // namespace gea::platform::memory

namespace gea::platform::touch {

namespace {

struct TouchCache {
	bool touching = false;
	int x = 0;
	int y = 0;
};

Touchscreen::Observer g_observer = nullptr;
TouchCache g_cache;
pthread_mutex_t g_touch_mutex = PTHREAD_MUTEX_INITIALIZER;

void emitTouch(Phase phase, bool touching, int x, int y)
{
	pthread_mutex_lock(&g_touch_mutex);
	g_cache.touching = touching;
	g_cache.x = x;
	g_cache.y = y;
	Touchscreen::Observer observer = g_observer;
	pthread_mutex_unlock(&g_touch_mutex);
	if (observer) observer(phase, touching, x, y);
}

}  // namespace

void Touchscreen::setObserver(Observer observer)
{
	g_observer = observer;
}

void Touchscreen::injectEvent(Phase phase, bool touching, int x, int y)
{
	// Mirror a hardware reader: cache the coords (consumeLatestMove reads the
	// cache) and notify the observer. Without the cache update, dispatch()'s
	// consumeLatestMove() would overwrite injected move coords with a stale
	// (0,0), collapsing every injected drag to a single bogus jump.
	emitTouch(phase, touching, x, y);
}

bool Touchscreen::init()
{
	// No reader thread: SDL mouse events are injected from the main loop.
	return true;
}

int Touchscreen::read(int *x, int *y)
{
	return readCached(x, y);
}

int Touchscreen::readCached(int *x, int *y)
{
	pthread_mutex_lock(&g_touch_mutex);
	const TouchCache cache = g_cache;
	pthread_mutex_unlock(&g_touch_mutex);
	if (x) *x = cache.x;
	if (y) *y = cache.y;
	return cache.touching ? 1 : 0;
}

void Touchscreen::consumeLatestMove(int *x, int *y)
{
	(void)readCached(x, y);
}

}  // namespace gea::platform::touch
