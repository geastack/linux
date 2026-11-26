// Native unit & concurrency tests for targets/sailfish-os/main/sailfish_audio.cpp.
// Exercises waveform synthesis, frequency clamping, attack/release envelopes,
// out-of-order setValueAtTime scheduling, volume bounds, voice stealing, handle
// generation wrap, playPcm resampling, GEA_AUDIO_DISABLED fallback, and CommandQueue
// multi-threaded stress without requiring physical audio output hardware.

#include "audio.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

// Include the implementation directly under test mode so internal Engine hooks
// (resetForTest / renderForTest) can drive deterministic PCM rendering.
#define GEA_SAILFISH_AUDIO_TEST 1
#include "../main/sailfish_audio.cpp"

using namespace gea::platform::audio;

namespace {

double rms(const std::vector<float> &interleaved, std::size_t startFrame, std::size_t frameCount)
{
	double sum = 0.0;
	for (std::size_t i = 0; i < frameCount; ++i) {
		const float l = interleaved[(startFrame + i) * 2u];
		const float r = interleaved[(startFrame + i) * 2u + 1u];
		assert(std::isfinite(l) && std::isfinite(r));
		assert(l >= -1.0f && l <= 1.0f);
		assert(r >= -1.0f && r <= 1.0f);
		sum += static_cast<double>(l) * l + static_cast<double>(r) * r;
	}
	return std::sqrt(sum / static_cast<double>(frameCount * 2u));
}

int zeroCrossings(const std::vector<float> &interleaved, std::size_t startFrame, std::size_t frameCount)
{
	int crossings = 0;
	float prev = interleaved[startFrame * 2u];
	for (std::size_t i = 1; i < frameCount; ++i) {
		const float cur = interleaved[(startFrame + i) * 2u];
		if ((prev <= 0.0f && cur > 0.0f) || (prev >= 0.0f && cur < 0.0f)) ++crossings;
		prev = cur;
	}
	return crossings;
}

}  // namespace

int main()
{
	// 1. Disabled-audio fallback must provide finite monotonic time without opening a device.
	{
		setenv("GEA_AUDIO_DISABLED", "1", 1);
		auto ctx = AudioSystem::sharedContext();
		const double t0 = ctx.currentTime();
		const double t1 = ctx.currentTime();
		assert(std::isfinite(t0) && std::isfinite(t1) && t1 >= t0);
		unsetenv("GEA_AUDIO_DISABLED");
	}

	auto &eng = Engine::instance();
	constexpr double kRate = 48000.0;

	// 2. All four waveforms render finite bounded stereo audio with smooth attack/release.
	for (OscillatorType wave : {OscillatorType::Sine, OscillatorType::Square, OscillatorType::Sawtooth, OscillatorType::Triangle}) {
		eng.resetForTest(kRate);
		auto ctx = AudioSystem::sharedContext();
		auto osc = ctx.createOscillator();
		osc.setType(wave);
		osc.frequency.setValue(440.0);
		osc.connect(ctx.destination());
		osc.start(0.0);
		osc.stop(0.04);  // 40 ms

		std::vector<float> buf(4800 * 2u, 0.0f);  // 100 ms
		eng.renderForTest(buf.data(), 4800);

		// First sample starts at 0.0 due to linear attack ramp (no initial click).
		assert(std::fabs(buf[0]) < 1e-6f);
		// Active window (10 ms .. 35 ms) has substantial energy.
		assert(rms(buf, 480, 1200) > 0.05);
		// Post-release tail (60 ms .. 100 ms) is completely silent.
		assert(rms(buf, 2880, 1920) < 1e-6);
	}

	// 3. Out-of-order setValueAtTime scheduling must execute chronologically.
	{
		eng.resetForTest(kRate);
		auto ctx = AudioSystem::sharedContext();
		auto osc = ctx.createOscillator();
		osc.setType(OscillatorType::Sine);
		osc.frequency.setValue(100.0);
		osc.connect(ctx.destination());
		osc.start(0.0);
		// Schedule later event FIRST, earlier event SECOND.
		osc.frequency.setValueAtTime(1200.0, 0.06);
		osc.frequency.setValueAtTime(300.0, 0.02);

		std::vector<float> buf(4800 * 2u, 0.0f);  // 100 ms
		eng.renderForTest(buf.data(), 4800);

		const int zcLow = zeroCrossings(buf, 240, 480);    // 5..15 ms (~100 Hz)
		const int zcMid = zeroCrossings(buf, 1200, 960);   // 25..45 ms (~300 Hz)
		const int zcHigh = zeroCrossings(buf, 3120, 960);  // 65..85 ms (~1200 Hz)
		assert(zcLow < zcMid && zcMid < zcHigh);
	}

	// 4. Volume clamping & frequency sanitization.
	{
		eng.resetForTest(kRate);
		AudioSystem::setVolume(-25);
		assert(AudioSystem::volume() == 0);
		AudioSystem::setVolume(175);
		assert(AudioSystem::volume() == 100);
		AudioSystem::setVolume(0);

		auto ctx = AudioSystem::sharedContext();
		auto osc = ctx.createOscillator();
		osc.frequency.setValue(std::numeric_limits<double>::quiet_NaN());
		assert(std::isfinite(osc.frequency.value()));
		osc.frequency.setValue(99999.0);
		osc.connect(ctx.destination());
		osc.start(0.0);

		std::vector<float> buf(512 * 2u, 1.0f);
		eng.renderForTest(buf.data(), 512);
		assert(rms(buf, 0, 512) == 0.0);
	}

	// 5. Voice stealing (> 64 simultaneous voices) & oscillator table wrap (> 512 handles).
	{
		eng.resetForTest(kRate);
		auto ctx = AudioSystem::sharedContext();
		std::vector<OscillatorNode> created;
		for (int i = 0; i < 600; ++i) {
			auto osc = ctx.createOscillator();
			osc.frequency.setValue(220.0 + (i % 20) * 10.0);
			osc.connect(ctx.destination());
			osc.start(0.0);
			created.push_back(osc);
		}
		// Stale handle from slot 0 (overwritten after 512 allocations) returns 0 frequency.
		assert(created[0].frequency.value() == 0.0);
		std::vector<float> buf(512 * 2u, 0.0f);
		eng.renderForTest(buf.data(), 512);
		assert(rms(buf, 64, 448) > 0.01);
	}

	// 6. playPcm mono resampling (16 kHz -> 48 kHz) and stopPlayback.
	{
		eng.resetForTest(kRate);
		std::vector<std::int16_t> mono(1600);
		for (std::size_t i = 0; i < mono.size(); ++i) {
			mono[i] = static_cast<std::int16_t>(std::sin(2.0 * 3.14159265 * 440.0 * i / 16000.0) * 16000.0);
		}
		assert(AudioSystem::playPcm(mono.data(), mono.size(), 16000, 1));
		std::vector<float> buf(4800 * 2u, 0.0f);
		eng.renderForTest(buf.data(), 4800);
		assert(rms(buf, 0, 4800) > 0.1);

		AudioSystem::stopPlayback();
		std::fill(buf.begin(), buf.end(), 1.0f);
		eng.renderForTest(buf.data(), 256);
		assert(rms(buf, 0, 256) == 0.0);
	}

	// 7. Concurrent producer threads vs audio render consumer under ASan/UBSan.
	{
		eng.resetForTest(kRate);
		std::vector<std::thread> threads;
		for (int t = 0; t < 4; ++t) {
			threads.emplace_back([t]() {
				auto ctx = AudioSystem::sharedContext();
				for (int i = 0; i < 60; ++i) {
					auto osc = ctx.createOscillator();
					osc.setType(static_cast<OscillatorType>((t + i) % 4));
					osc.frequency.setValue(300.0 + i * 5.0);
					osc.connect(ctx.destination());
					const double now = ctx.currentTime();
					osc.start(now);
					osc.frequency.setValueAtTime(500.0 + t * 25.0, now + 0.01);
					osc.stop(now + 0.02);
				}
			});
		}
		std::vector<float> buf(512 * 2u, 0.0f);
		for (int b = 0; b < 40; ++b) {
			eng.renderForTest(buf.data(), 512);
			assert(std::isfinite(buf[0]));
		}
		for (auto &th : threads) th.join();
	}

	std::puts("PASS sailfish_audio: waveforms, attack/release, chronological setValueAtTime, clamping, voice stealing, playPcm resampling, concurrent queue");
	return 0;
}
