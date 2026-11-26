// Native unit tests for Sailfish OS touchscreen routing (sailfish_touch.h + sailfish_sensors.cpp).
// Regression test for native touchscreen scrolling: primary finger (pointer 0)
// Move events must update Touchscreen's latest-move cache so TouchRuntime's
// consumeLatestMove() receives real drag coordinates instead of stale (0,0).

#include "sailfish_touch.h"

#include <cassert>
#include <cstdio>
#include <vector>

using gea::platform::touch::Phase;
using gea::platform::touch::Touchscreen;
using gea::sailfish::touch::FingerSlots;
using gea::sailfish::touch::kMaxFingers;
using gea::sailfish::touch::normalizedToCanvasCoord;
using gea::sailfish::touch::routeFinger;

namespace {

struct RecordedEvent {
	Phase phase;
	bool touching;
	int x;
	int y;
	int pointerId;
};

std::vector<RecordedEvent> g_primaryEvents;
std::vector<RecordedEvent> g_secondaryEvents;

void onPrimaryTouch(Phase phase, bool touching, int x, int y)
{
	g_primaryEvents.push_back({phase, touching, x, y, 0});
}

void onSecondaryTouch(Phase phase, bool touching, int x, int y, int pointerId)
{
	g_secondaryEvents.push_back({phase, touching, x, y, pointerId});
}

}  // namespace

int main()
{
	assert(Touchscreen::init());
	Touchscreen::setObserver(onPrimaryTouch);

	constexpr int kW = 1080;
	constexpr int kH = 2400;

	// 1. Primary finger drag updates Touchscreen::consumeLatestMove on every move
	//    (this is what TouchRuntime::dispatchEvent reads for pointerId == 0 Move).
	FingerSlots slots;
	const int p0 = slots.slot(101, true);
	assert(p0 == 0);

	routeFinger(Phase::Down, true, 0.5f, 0.75f, kW, kH, p0, onSecondaryTouch);
	assert(g_primaryEvents.size() == 1);
	assert(g_primaryEvents[0].phase == Phase::Down);
	assert(g_primaryEvents[0].x == 540);
	assert(g_primaryEvents[0].y == 1800);

	// First drag move upward (scrolling content down).
	routeFinger(Phase::Move, true, 0.5f, 0.70f, kW, kH, p0, onSecondaryTouch);
	int cx = -1, cy = -1;
	Touchscreen::consumeLatestMove(&cx, &cy);
	assert(cx == 540);
	assert(cy == 1680);

	// Second drag move further upward.
	routeFinger(Phase::Move, true, 0.5f, 0.60f, kW, kH, p0, onSecondaryTouch);
	Touchscreen::consumeLatestMove(&cx, &cy);
	assert(cx == 540);
	assert(cy == 1440);

	// Coalesced moves: multiple moves before consumeLatestMove return the freshest coords.
	routeFinger(Phase::Move, true, 0.5f, 0.55f, kW, kH, p0, onSecondaryTouch);
	routeFinger(Phase::Move, true, 0.5f, 0.50f, kW, kH, p0, onSecondaryTouch);
	Touchscreen::consumeLatestMove(&cx, &cy);
	assert(cx == 540);
	assert(cy == 1200);

	// 2. Secondary finger (pointerId >= 1) routes to queueSecondary and MUST NOT
	//    overwrite the primary finger's latest-move cache.
	const int p1 = slots.slot(202, true);
	assert(p1 == 1);
	const std::size_t primaryBeforeSecondary = g_primaryEvents.size();
	routeFinger(Phase::Down, true, 0.1f, 0.1f, kW, kH, p1, onSecondaryTouch);
	routeFinger(Phase::Move, true, 0.2f, 0.2f, kW, kH, p1, onSecondaryTouch);
	routeFinger(Phase::Up, false, 0.2f, 0.2f, kW, kH, p1, onSecondaryTouch);
	slots.release(p1);

	assert(g_primaryEvents.size() == primaryBeforeSecondary);
	assert(g_secondaryEvents.size() == 3);
	assert(g_secondaryEvents[1].pointerId == 1);
	assert(g_secondaryEvents[1].x == 216);
	assert(g_secondaryEvents[1].y == 480);

	// Primary cache still holds primary finger's latest coordinates.
	Touchscreen::consumeLatestMove(&cx, &cy);
	assert(cx == 540);
	assert(cy == 1200);

	// Primary finger up clears touching state and releases slot 0.
	routeFinger(Phase::Up, false, 0.5f, 0.50f, kW, kH, p0, onSecondaryTouch);
	slots.release(p0);
	assert(Touchscreen::read(&cx, &cy) == 0);
	assert(slots.slot(101, false) == -1);

	// 3. Coordinate clamping handles out-of-range normalized values safely.
	assert(normalizedToCanvasCoord(-0.5f, kW) == 0);
	assert(normalizedToCanvasCoord(0.0f, kW) == 0);
	assert(normalizedToCanvasCoord(1.0f, kW) == kW - 1);
	assert(normalizedToCanvasCoord(1.5f, kW) == kW - 1);
	assert(normalizedToCanvasCoord(0.5f, 0) == 0);

	// 4. FingerSlots enforces kMaxFingers capacity and reuses freed slots.
	FingerSlots cap;
	for (int i = 0; i < kMaxFingers; ++i) {
		assert(cap.slot(1000 + i, true) == i);
	}
	assert(cap.slot(9999, true) == -1);
	cap.release(3);
	assert(cap.slot(9999, true) == 3);

	std::puts("PASS sailfish_touch: primary cache drag/coalesce, secondary isolation, clamping, finger slots");
	return 0;
}
