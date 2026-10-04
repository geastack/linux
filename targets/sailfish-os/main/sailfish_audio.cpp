/* targets/sailfish-os/main/sailfish_audio.cpp
 * Oscillator tones go to the Sailfish audio device through SDL2.
 * An app schedules a beep by creating an oscillator,
 * setting its frequency, connecting it, then start(now)/stop(now + seconds).
 * stop() turns that interval into samples. SDL's callback mixes overlapping
 * notes. playFile stays unsupported; this target has no WAV file reader.
 * The app's Sailjail profile still needs the Audio permission or the device
 * open fails and the failure is logged once. */

#include "audio.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace gea::platform::audio {

namespace {

constexpr int kSampleRate = 44100;
constexpr int kMaxOscillators = 8;
constexpr int kMaxTones = 8;
constexpr int kMaxToneMs = 4000;
constexpr int kFadeSamples = kSampleRate / 125;
constexpr double kPi = 3.14159265358979323846;

struct Oscillator {
	bool used = false;
	OscillatorType type = OscillatorType::Sine;
	double frequencyHz = 440.0;
	bool connected = false;
	bool started = false;
	double startTime = 0.0;
};

struct Tone {
	bool active = false;
	OscillatorType type = OscillatorType::Sine;
	double frequencyHz = 440.0;
	double phase = 0.0;
	int delaySamples = 0;
	int remaining = 0;
	int total = 0;
};

Oscillator g_oscillators[kMaxOscillators]{};
Tone g_tones[kMaxTones]{};
int g_nextOscillator = 0;
std::atomic<int> g_volume{80};
SDL_AudioDeviceID g_device = 0;
bool g_openFailed = false;

double nowSeconds()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1000000000.0;
}

int oscillatorSlot(NativeAudioHandle handle)
{
	if (handle == 0 || handle > static_cast<NativeAudioHandle>(kMaxOscillators)) return -1;
	return static_cast<int>(handle) - 1;
}

float waveform(OscillatorType type, double phase)
{
	switch (type) {
	case OscillatorType::Square:
		return phase < 0.5 ? 1.f : -1.f;
	case OscillatorType::Sawtooth:
		return static_cast<float>(phase * 2.0 - 1.0);
	case OscillatorType::Triangle:
		return static_cast<float>(phase < 0.5 ? phase * 4.0 - 1.0 : 3.0 - phase * 4.0);
	case OscillatorType::Sine:
	default:
		return static_cast<float>(std::sin(phase * 2.0 * kPi));
	}
}

void audioCallback(void *, Uint8 *stream, int length)
{
	auto *out = reinterpret_cast<std::int16_t *>(stream);
	const int frames = length / static_cast<int>(sizeof(std::int16_t));
	const float gain = (std::clamp(g_volume.load(), 0, 100) / 100.f) * 7000.f;
	for (int frame = 0; frame < frames; ++frame) {
		float mix = 0.f;
		for (Tone &tone : g_tones) {
			if (!tone.active) continue;
			if (tone.delaySamples > 0) {
				--tone.delaySamples;
				continue;
			}
			if (tone.remaining <= 0) {
				tone.active = false;
				continue;
			}
			const int played = tone.total - tone.remaining;
			float envelope = 1.f;
			if (played < kFadeSamples) envelope = played / static_cast<float>(kFadeSamples);
			if (tone.remaining < kFadeSamples) {
				envelope = std::min(envelope, tone.remaining / static_cast<float>(kFadeSamples));
			}
			mix += waveform(tone.type, tone.phase) * envelope;
			tone.phase += tone.frequencyHz / kSampleRate;
			if (tone.phase >= 1.0) tone.phase -= std::floor(tone.phase);
			if (--tone.remaining <= 0) tone.active = false;
		}
		const float sample = std::clamp(mix * gain, -32768.f, 32767.f);
		out[frame] = static_cast<std::int16_t>(sample);
	}
}

bool ensureDevice()
{
	if (g_device != 0) return true;
	if (g_openFailed) return false;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		std::fprintf(stderr, "[sailfish audio] SDL audio init failed: %s\n", SDL_GetError());
		g_openFailed = true;
		return false;
	}
	SDL_AudioSpec requested{};
	requested.freq = kSampleRate;
	requested.format = AUDIO_S16SYS;
	requested.channels = 1;
	requested.samples = 512;
	requested.callback = audioCallback;
	SDL_AudioSpec obtained{};
	g_device = SDL_OpenAudioDevice(nullptr, 0, &requested, &obtained, 0);
	if (g_device == 0) {
		std::fprintf(stderr, "[sailfish audio] device open failed: %s\n", SDL_GetError());
		g_openFailed = true;
		return false;
	}
	SDL_PauseAudioDevice(g_device, 0);
	std::fprintf(stderr, "[sailfish audio] open %d Hz\n", obtained.freq);
	return true;
}

void submitTone(OscillatorType type, double frequencyHz, int durationMs, int delayMs)
{
	if (frequencyHz <= 0.0 || durationMs <= 0) return;
	if (durationMs > kMaxToneMs) durationMs = kMaxToneMs;
	if (delayMs < 0) delayMs = 0;
	if (!ensureDevice()) return;

	SDL_LockAudioDevice(g_device);
	Tone *slot = nullptr;
	for (Tone &tone : g_tones) {
		if (!tone.active) {
			slot = &tone;
			break;
		}
	}
	if (!slot) slot = &g_tones[0];
	slot->active = true;
	slot->type = type;
	slot->frequencyHz = frequencyHz;
	slot->phase = 0.0;
	slot->delaySamples = (delayMs * kSampleRate) / 1000;
	slot->total = (durationMs * kSampleRate) / 1000;
	slot->remaining = slot->total;
	SDL_UnlockAudioDevice(g_device);
}

}  // namespace

AudioParam::AudioParam(NativeAudioHandle oscillator) : oscillator_(oscillator) {}

double AudioParam::value() const
{
	const int slot = oscillatorSlot(oscillator_);
	if (slot < 0) return 0.0;
	return g_oscillators[slot].frequencyHz;
}

void AudioParam::setValue(double value)
{
	const int slot = oscillatorSlot(oscillator_);
	if (slot < 0) return;
	g_oscillators[slot].frequencyHz = value;
}

void AudioParam::setValueAtTime(double value, double)
{
	setValue(value);
}

AudioNode::AudioNode(NativeAudioHandle native) : native_(native) {}
NativeAudioHandle AudioNode::nativeId() const { return native_; }

AudioDestinationNode::AudioDestinationNode(NativeAudioHandle native) : AudioNode(native) {}

OscillatorNode::OscillatorNode(NativeAudioHandle native) : AudioNode(native), frequency(native) {}

OscillatorType OscillatorNode::type() const
{
	const int slot = oscillatorSlot(nativeId());
	if (slot < 0) return OscillatorType::Sine;
	return g_oscillators[slot].type;
}

void OscillatorNode::setType(OscillatorType type)
{
	const int slot = oscillatorSlot(nativeId());
	if (slot < 0) return;
	g_oscillators[slot].type = type;
}

void OscillatorNode::connect(const AudioDestinationNode &)
{
	const int slot = oscillatorSlot(nativeId());
	if (slot < 0) return;
	g_oscillators[slot].connected = true;
}

void OscillatorNode::start(double when)
{
	const int slot = oscillatorSlot(nativeId());
	if (slot < 0) return;
	if (when <= 0.0) when = nowSeconds();
	g_oscillators[slot].startTime = when;
	g_oscillators[slot].started = true;
}

void OscillatorNode::stop(double when)
{
	const int slot = oscillatorSlot(nativeId());
	if (slot < 0) return;
	Oscillator &oscillator = g_oscillators[slot];
	if (!oscillator.connected) return;

	const double now = nowSeconds();
	const double startTime = oscillator.started ? oscillator.startTime : now;
	if (when <= 0.0) when = now;
	if (when < startTime) when = startTime;
	const int durationMs = static_cast<int>((when - startTime) * 1000.0 + 0.5);
	const int delayMs = startTime > now ? static_cast<int>((startTime - now) * 1000.0 + 0.5) : 0;
	submitTone(oscillator.type, oscillator.frequencyHz, durationMs, delayMs);
	oscillator.started = false;
}

double AudioContext::currentTime() const { return nowSeconds(); }

AudioDestinationNode AudioContext::destination() const { return AudioDestinationNode(1); }

OscillatorNode AudioContext::createOscillator() const
{
	const int slot = g_nextOscillator;
	g_nextOscillator = (g_nextOscillator + 1) % kMaxOscillators;
	g_oscillators[slot] = Oscillator{};
	g_oscillators[slot].used = true;
	return OscillatorNode(static_cast<NativeAudioHandle>(slot + 1));
}

AudioContext AudioSystem::sharedContext() { return AudioContext{}; }

int AudioSystem::volume() { return g_volume.load(); }

void AudioSystem::setVolume(int volume)
{
	if (volume < 0) volume = 0;
	if (volume > 100) volume = 100;
	g_volume.store(volume);
}

bool AudioSystem::playFile(const std::string &) { return false; }

bool AudioSystem::playPcm(const std::int16_t *, std::size_t, int, int) { return false; }

void AudioSystem::stopPlayback()
{
	if (g_device == 0) return;
	SDL_LockAudioDevice(g_device);
	for (Tone &tone : g_tones) tone = Tone{};
	SDL_UnlockAudioDevice(g_device);
}

// No PCM stream output is queued by this backend (only the ESP32 runtime
// streams PCM), so a flush has nothing to discard.
void AudioSystem::flushPlayback() {}

}  // namespace gea::platform::audio
