#pragma once
/* Touch finger slot tracking and primary/secondary routing for Sailfish OS.
 * Extracted into a header so native host tests can verify primary-finger
 * cache updates (Touchscreen::consumeLatestMove) and multi-touch isolation
 * without initializing SDL2 video. */

#include "touch.h"

#include <SDL2/SDL_touch.h>

namespace gea::sailfish::touch {

inline constexpr int kMaxFingers = 10;

struct FingerSlots {
	SDL_FingerID ids[kMaxFingers] = {};
	bool used[kMaxFingers] = {};

	int slot(SDL_FingerID id, bool allocate)
	{
		for (int i = 0; i < kMaxFingers; ++i)
			if (used[i] && ids[i] == id) return i;
		if (!allocate) return -1;
		for (int i = 0; i < kMaxFingers; ++i) {
			if (!used[i]) {
				used[i] = true;
				ids[i] = id;
				return i;
			}
		}
		return -1;
	}

	void release(int slotIndex)
	{
		if (slotIndex >= 0 && slotIndex < kMaxFingers) used[slotIndex] = false;
	}
};

inline int clampCanvasCoord(int value, int canvasSize)
{
	if (canvasSize <= 0 || value < 0) return 0;
	const int maxCoord = canvasSize - 1;
	return value > maxCoord ? maxCoord : value;
}

inline int normalizedToCanvasCoord(float normalized, int canvasSize)
{
	return clampCanvasCoord(static_cast<int>(normalized * static_cast<float>(canvasSize)), canvasSize);
}

// Route a touchscreen finger event:
// - pointerId == 0 (primary finger) goes through Touchscreen::injectEvent so
//   sailfish_sensors' latest-move cache is updated before TouchRuntime::dispatch
//   calls Touchscreen::consumeLatestMove() on Move events. Bypassing it leaves
//   the cache at (0,0) and collapses every touch drag/scroll.
// - pointerId >= 1 (secondary fingers) go straight to queueSecondary so they
//   never overwrite the primary finger's latest-move cache.
template <typename QueueSecondaryFn>
inline void routeFinger(gea::platform::touch::Phase phase, bool touching, float nx, float ny,
                        int canvasWidth, int canvasHeight, int pointerId,
                        QueueSecondaryFn &&queueSecondary)
{
	const int x = normalizedToCanvasCoord(nx, canvasWidth);
	const int y = normalizedToCanvasCoord(ny, canvasHeight);
	if (pointerId == 0) {
		gea::platform::touch::Touchscreen::injectEvent(phase, touching, x, y);
		return;
	}
	queueSecondary(phase, touching, x, y, pointerId);
}

}  // namespace gea::sailfish::touch
