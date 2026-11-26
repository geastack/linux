/* targets/sailfish-os/main/sailfish_audio.cpp
 * Real SDL2 audio backend for gea::platform::audio on Sailfish OS.
 *
 * Uses SDL2's PulseAudio driver (compiled into Sailfish OS libSDL2-2.0.so.0)
 * and sets PULSE_PROP_media.role=x-maemo before opening the audio device so
 * Sailfish OS routes playback through the normal system volume policy instead
 * of ignoring hardware volume controls.
 *
 * Architecture mirrors apple_audio.mm:
 *   - Lock-free single-consumer command ring feeds a fixed voice table on the
 *     SDL audio callback thread (zero allocations on the render thread).
 *   - Supports Sine, Square, Sawtooth, and Triangle oscillators with
 *     sample-accurate start(when), stop(when), setValue(), setValueAtTime(),
 *     and smooth attack/release ramps to prevent clicks on short notes.
 *   - Also mixes one-shot PCM buffers (playPcm) and WAV files (playFile).
 *   - Falls back silently to a monotonic steady clock if audio is disabled
 *     (GEA_AUDIO_DISABLED=1) or no output device is available.
 */

#include "audio.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace gea::platform::audio {

namespace {

constexpr int kMaxOscillators = 512;
constexpr int kMaxVoices = 64;
constexpr int kMaxVoiceEvents = 8;
constexpr int kCommandCapacity = 2048;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kDefaultFrequency = 440.0;
constexpr double kMinFrequency = 8.0;
constexpr double kMaxFrequency = 20000.0;
constexpr double kAttackSeconds = 0.0025;
constexpr double kReleaseSeconds = 0.006;
constexpr double kVoiceGain = 0.25;
constexpr double kMaxVoiceSeconds = 30.0;
constexpr double kDefaultSampleRate = 48000.0;
constexpr Uint16 kDesiredBufferSamples = 512;

double steadySeconds()
{
	static const auto origin = std::chrono::steady_clock::now();
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
}

OscillatorType normalizeType(OscillatorType type)
{
	switch (type) {
	case OscillatorType::Sine:
	case OscillatorType::Square:
	case OscillatorType::Sawtooth:
	case OscillatorType::Triangle:
		return type;
	}
	return OscillatorType::Sine;
}

double normalizeFrequency(double frequency)
{
	if (!std::isfinite(frequency)) return kDefaultFrequency;
	if (frequency < kMinFrequency) return kMinFrequency;
	if (frequency > kMaxFrequency) return kMaxFrequency;
	return frequency;
}

double waveform(OscillatorType type, double phase)
{
	switch (type) {
	case OscillatorType::Square:
		return phase < 0.5 ? 1.0 : -1.0;
	case OscillatorType::Sawtooth:
		return 2.0 * phase - 1.0;
	case OscillatorType::Triangle:
		if (phase < 0.25) return 4.0 * phase;
		if (phase < 0.75) return 2.0 - 4.0 * phase;
		return 4.0 * phase - 4.0;
	case OscillatorType::Sine:
		break;
	}
	return std::sin(phase * kTwoPi);
}

bool envFlagSet(const char *name)
{
	const char *value = std::getenv(name);
	return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

enum class CommandKind : std::uint8_t {
	StartVoice,
	StopVoice,
	SetFrequency,
	SetType,
	ScheduleFrequency,
};

struct Command {
	CommandKind kind = CommandKind::StartVoice;
	OscillatorType type = OscillatorType::Sine;
	NativeAudioHandle voice = 0;
	double frequency = kDefaultFrequency;
	std::int64_t frame = 0;
};

class CommandQueue {
public:
	bool push(const Command &command)
	{
		std::lock_guard<std::mutex> guard(producers_);
		const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
		const std::uint32_t next = (tail + 1u) % kCommandCapacity;
		if (next == head_.load(std::memory_order_acquire)) return false;
		items_[tail] = command;
		tail_.store(next, std::memory_order_release);
		return true;
	}

	bool pop(Command &out)
	{
		const std::uint32_t head = head_.load(std::memory_order_relaxed);
		if (head == tail_.load(std::memory_order_acquire)) return false;
		out = items_[head];
		head_.store((head + 1u) % kCommandCapacity, std::memory_order_release);
		return true;
	}

private:
	Command items_[kCommandCapacity]{};
	std::atomic<std::uint32_t> head_{0};
	std::atomic<std::uint32_t> tail_{0};
	std::mutex producers_;
};

struct VoiceEvent {
	std::int64_t frame = 0;
	double phaseIncrement = 0.0;
};

struct Voice {
	bool active = false;
	NativeAudioHandle id = 0;
	OscillatorType type = OscillatorType::Sine;
	double phase = 0.0;
	double phaseIncrement = 0.0;
	std::int64_t startFrame = 0;
	std::int64_t stopFrame = 0;
	std::int64_t endFrame = 0;
	bool stopScheduled = false;
	VoiceEvent events[kMaxVoiceEvents]{};
	int eventHead = 0;
	int eventCount = 0;
};

struct ScheduledValue {
	double time = 0.0;
	double value = kDefaultFrequency;
};

struct OscillatorRecord {
	NativeAudioHandle handle = 0;
	OscillatorType type = OscillatorType::Sine;
	double frequency = kDefaultFrequency;
	bool connected = false;
	bool started = false;
	bool stopped = false;
	bool voiceLaunched = false;
	double startTime = 0.0;
	double stopTime = 0.0;
	ScheduledValue pending[kMaxVoiceEvents]{};
	int pendingCount = 0;
};

class Engine {
public:
	static Engine &instance()
	{
		// Deliberately leaked so the SDL audio thread never races a static
		// destructor during process shutdown.
		static Engine *engine = new Engine();
		return *engine;
	}

	double currentTime()
	{
		ensureSetup();
		return currentTimeLocked();
	}

	NativeAudioHandle createOscillator()
	{
		ensureSetup();
		std::lock_guard<std::mutex> guard(recordsMutex_);
		const int slot = nextSlot_;
		nextSlot_ = (nextSlot_ + 1) % kMaxOscillators;
		const NativeAudioHandle handle =
			(static_cast<NativeAudioHandle>(++generation_) << 10) | static_cast<NativeAudioHandle>(slot + 1);
		records_[slot] = OscillatorRecord{};
		records_[slot].handle = handle;
		return handle;
	}

	double frequency(NativeAudioHandle handle)
	{
		std::lock_guard<std::mutex> guard(recordsMutex_);
		const OscillatorRecord *record = find(handle);
		return record ? record->frequency : 0.0;
	}

	void setFrequency(NativeAudioHandle handle, double value)
	{
		Command command;
		bool live = false;
		{
			std::lock_guard<std::mutex> guard(recordsMutex_);
			OscillatorRecord *record = find(handle);
			if (record == nullptr || !std::isfinite(value)) return;
			record->frequency = value;
			live = record->voiceLaunched && !record->stopped;
			command = Command{CommandKind::SetFrequency, record->type, handle, normalizeFrequency(value), 0};
		}
		if (live) commands_.push(command);
	}

	OscillatorType type(NativeAudioHandle handle)
	{
		std::lock_guard<std::mutex> guard(recordsMutex_);
		const OscillatorRecord *record = find(handle);
		return record ? record->type : OscillatorType::Sine;
	}

	void setType(NativeAudioHandle handle, OscillatorType type)
	{
		Command command;
		bool live = false;
		{
			std::lock_guard<std::mutex> guard(recordsMutex_);
			OscillatorRecord *record = find(handle);
			if (record == nullptr) return;
			record->type = normalizeType(type);
			live = record->voiceLaunched && !record->stopped;
			command = Command{CommandKind::SetType, record->type, handle, record->frequency, 0};
		}
		if (live) commands_.push(command);
	}

	void scheduleFrequency(NativeAudioHandle handle, double value, double time)
	{
		ensureSetup();
		Command command;
		bool emit = false;
		{
			std::lock_guard<std::mutex> guard(recordsMutex_);
			OscillatorRecord *record = find(handle);
			if (record == nullptr || !std::isfinite(value)) return;
			if (record->voiceLaunched && !record->stopped) {
				emit = true;
				command = Command{CommandKind::ScheduleFrequency, record->type, handle,
				                  normalizeFrequency(value), frameFor(time)};
			} else if (record->pendingCount < kMaxVoiceEvents) {
				record->pending[record->pendingCount++] = ScheduledValue{time, value};
			}
		}
		if (emit) commands_.push(command);
	}

	void connect(NativeAudioHandle handle)
	{
		Command commands[kMaxVoiceEvents + 2];
		int count = 0;
		{
			std::lock_guard<std::mutex> guard(recordsMutex_);
			OscillatorRecord *record = find(handle);
			if (record == nullptr || record->connected) return;
			record->connected = true;
			if (record->started && !record->voiceLaunched) {
				record->voiceLaunched = true;
				count = launchCommands(*record, handle, commands);
			}
		}
		for (int i = 0; i < count; ++i) commands_.push(commands[i]);
	}

	void start(NativeAudioHandle handle, double when)
	{
		ensureSetup();
		Command commands[kMaxVoiceEvents + 2];
		int count = 0;
		{
			std::lock_guard<std::mutex> guard(recordsMutex_);
			OscillatorRecord *record = find(handle);
			if (record == nullptr || record->started) return;
			record->started = true;
			const double now = currentTimeLocked();
			double at = (std::isfinite(when) && when > 0.0) ? when : now;
			if (at < now) at = now;
			record->startTime = at;
			if (record->connected) {
				record->voiceLaunched = true;
				count = launchCommands(*record, handle, commands);
			}
		}
		for (int i = 0; i < count; ++i) commands_.push(commands[i]);
	}

	void stop(NativeAudioHandle handle, double when)
	{
		ensureSetup();
		Command command;
		bool emit = false;
		{
			std::lock_guard<std::mutex> guard(recordsMutex_);
			OscillatorRecord *record = find(handle);
			if (record == nullptr || !record->started || record->stopped) return;
			record->stopped = true;
			const double now = currentTimeLocked();
			double at = (std::isfinite(when) && when > 0.0) ? when : now;
			if (at < record->startTime) at = record->startTime;
			record->stopTime = at;
			emit = record->voiceLaunched;
			command = Command{CommandKind::StopVoice, record->type, handle, record->frequency, frameFor(at)};
		}
		if (emit) commands_.push(command);
	}

	int volume() const { return volume_.load(std::memory_order_relaxed); }

	void setVolume(int percent)
	{
		if (percent < 0) percent = 0;
		if (percent > 100) percent = 100;
		volume_.store(percent, std::memory_order_relaxed);
	}

	bool playPcm(const std::int16_t *samples, std::size_t sampleCount, int sampleRate, int channels)
	{
		ensureSetup();
		if (samples == nullptr || sampleCount == 0 || (device_ == 0 && !testMode_)) return false;
		if (channels < 1) channels = 1;
		if (channels > 2) channels = 2;
		if (sampleRate <= 0) sampleRate = 16000;
		const std::size_t inFrames = sampleCount / static_cast<std::size_t>(channels);
		if (inFrames == 0) return false;

		const double outRate = sampleRate_.load(std::memory_order_relaxed);
		const double ratio = static_cast<double>(sampleRate) / outRate;
		const std::size_t outFrames = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(static_cast<double>(inFrames) / ratio)));
		std::vector<float> stereo(outFrames * 2u);
		for (std::size_t i = 0; i < outFrames; ++i) {
			const double srcPos = static_cast<double>(i) * ratio;
			const std::size_t idx0 = std::min(inFrames - 1u, static_cast<std::size_t>(srcPos));
			const std::size_t idx1 = std::min(inFrames - 1u, idx0 + 1u);
			const float frac = static_cast<float>(srcPos - static_cast<double>(idx0));
			const float l0 = static_cast<float>(samples[idx0 * static_cast<std::size_t>(channels)]) / 32768.0f;
			const float l1 = static_cast<float>(samples[idx1 * static_cast<std::size_t>(channels)]) / 32768.0f;
			const float r0 = channels == 2
				? static_cast<float>(samples[idx0 * 2u + 1u]) / 32768.0f
				: l0;
			const float r1 = channels == 2
				? static_cast<float>(samples[idx1 * 2u + 1u]) / 32768.0f
				: l1;
			stereo[i * 2u] = l0 + (l1 - l0) * frac;
			stereo[i * 2u + 1u] = r0 + (r1 - r0) * frac;
		}

		if (!testMode_) SDL_LockAudioDevice(device_);
		pcmStereo_.swap(stereo);
		pcmCursor_ = 0;
		if (!testMode_) SDL_UnlockAudioDevice(device_);
		return true;
	}

	bool playFile(const std::string &path)
	{
		ensureSetup();
		if (path.empty() || device_ == 0) return false;
		SDL_AudioSpec wavSpec{};
		Uint8 *wavBuf = nullptr;
		Uint32 wavLen = 0;
		if (SDL_LoadWAV(path.c_str(), &wavSpec, &wavBuf, &wavLen) == nullptr) return false;
		if (wavLen == 0) {
			SDL_FreeWAV(wavBuf);
			return false;
		}

		SDL_AudioCVT cvt{};
		const int outRate = static_cast<int>(std::llround(sampleRate_.load(std::memory_order_relaxed)));
		const int built = SDL_BuildAudioCVT(&cvt, wavSpec.format, wavSpec.channels, wavSpec.freq,
		                                    AUDIO_F32SYS, 2, outRate);
		if (built < 0) {
			SDL_FreeWAV(wavBuf);
			return false;
		}
		std::vector<float> stereo;
		if (built == 0) {
			const std::size_t floats = wavLen / sizeof(float);
			stereo.resize(floats);
			std::memcpy(stereo.data(), wavBuf, floats * sizeof(float));
		} else {
			std::vector<Uint8> work(static_cast<std::size_t>(wavLen) * static_cast<std::size_t>(cvt.len_mult));
			std::memcpy(work.data(), wavBuf, wavLen);
			cvt.buf = work.data();
			cvt.len = static_cast<int>(wavLen);
			if (SDL_ConvertAudio(&cvt) != 0) {
				SDL_FreeWAV(wavBuf);
				return false;
			}
			const std::size_t floats = static_cast<std::size_t>(cvt.len_cvt) / sizeof(float);
			stereo.resize(floats);
			std::memcpy(stereo.data(), work.data(), floats * sizeof(float));
		}
		SDL_FreeWAV(wavBuf);

		if (!testMode_) SDL_LockAudioDevice(device_);
		pcmStereo_.swap(stereo);
		pcmCursor_ = 0;
		if (!testMode_) SDL_UnlockAudioDevice(device_);
		return true;
	}

	void stopPlayback()
	{
		if (device_ == 0) {
			if (testMode_) {
				pcmStereo_.clear();
				pcmCursor_ = 0;
			}
			return;
		}
		SDL_LockAudioDevice(device_);
		pcmStereo_.clear();
		pcmCursor_ = 0;
		SDL_UnlockAudioDevice(device_);
	}

#ifdef GEA_SAILFISH_AUDIO_TEST
	void resetForTest(double sampleRate)
	{
		std::lock_guard<std::mutex> guard(setupMutex_);
		std::lock_guard<std::mutex> recGuard(recordsMutex_);
		testMode_ = true;
		setupDone_.store(true, std::memory_order_release);
		running_.store(true, std::memory_order_release);
		sampleRate_.store(sampleRate > 0.0 ? sampleRate : kDefaultSampleRate, std::memory_order_relaxed);
		const double rate = sampleRate_.load(std::memory_order_relaxed);
		attackFrames_ = std::max(1.0, kAttackSeconds * rate);
		releaseFrames_ = std::max(1.0, kReleaseSeconds * rate);
		frameOffset_.store(0, std::memory_order_relaxed);
		renderedFrames_.store(0, std::memory_order_relaxed);
		volume_.store(100, std::memory_order_relaxed);
		device_ = 0;
		for (auto &rec : records_) rec = OscillatorRecord{};
		nextSlot_ = 0;
		generation_ = 0;
		Command discard;
		while (commands_.pop(discard)) {}
		for (auto &v : voices_) v = Voice{};
		pcmStereo_.clear();
		pcmCursor_ = 0;
	}

	void renderForTest(float *interleaved, std::size_t frameCount)
	{
		renderStereoFloat(interleaved, frameCount);
	}
#endif

private:
	Engine() = default;

	void ensureSetup()
	{
		if (setupDone_.load(std::memory_order_acquire)) return;
		std::lock_guard<std::mutex> guard(setupMutex_);
		if (setupDone_.load(std::memory_order_relaxed)) return;

		if (envFlagSet("GEA_AUDIO_DISABLED")) {
			if (envFlagSet("GEA_AUDIO_DEBUG")) {
				std::fprintf(stderr, "[sailfish audio] disabled via GEA_AUDIO_DISABLED\n");
			}
			setupDone_.store(true, std::memory_order_release);
			return;
		}

		// Sailfish OS PulseAudio policy ignores system volume controls for
		// streams without a recognized media.role. main() sets these before
		// spawning threads; only fill them in here if still unset.
		if (std::getenv("PULSE_PROP_media.role") == nullptr) {
			setenv("PULSE_PROP_media.role", "x-maemo", 0);
		}
#ifdef GEA_SAILFISH_APP_TITLE
		if (std::getenv("PULSE_PROP_application.name") == nullptr) {
			setenv("PULSE_PROP_application.name", GEA_SAILFISH_APP_TITLE, 0);
		}
#endif

		if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
			if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
				std::fprintf(stderr, "[sailfish audio] SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
				setupDone_.store(true, std::memory_order_release);
				return;
			}
		}

		SDL_AudioSpec want{};
		SDL_AudioSpec have{};
		want.freq = static_cast<int>(kDefaultSampleRate);
		want.format = AUDIO_F32SYS;
		want.channels = 2;
		want.samples = kDesiredBufferSamples;
		want.callback = &Engine::audioCallback;
		want.userdata = this;

		const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(
			nullptr, 0, &want, &have,
			SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
		if (dev == 0) {
			std::fprintf(stderr, "[sailfish audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
			setupDone_.store(true, std::memory_order_release);
			return;
		}

		const double rate = have.freq > 0 ? static_cast<double>(have.freq) : kDefaultSampleRate;
		sampleRate_.store(rate, std::memory_order_relaxed);
		attackFrames_ = std::max(1.0, kAttackSeconds * rate);
		releaseFrames_ = std::max(1.0, kReleaseSeconds * rate);
		frameOffset_.store(static_cast<std::int64_t>(std::llround(steadySeconds() * rate)),
		                   std::memory_order_relaxed);
		renderedFrames_.store(0, std::memory_order_relaxed);
		device_ = dev;
		running_.store(true, std::memory_order_release);
		SDL_PauseAudioDevice(device_, 0);

		if (envFlagSet("GEA_AUDIO_DEBUG")) {
			const char *driver = SDL_GetCurrentAudioDriver();
			std::fprintf(stderr,
			             "[sailfish audio] started driver=%s freq=%d channels=%d samples=%u media.role=%s\n",
			             driver ? driver : "unknown", have.freq, static_cast<int>(have.channels),
			             static_cast<unsigned>(have.samples),
			             std::getenv("PULSE_PROP_media.role") ? std::getenv("PULSE_PROP_media.role") : "");
		}

		setupDone_.store(true, std::memory_order_release);
	}

	static void audioCallback(void *userdata, Uint8 *stream, int len)
	{
		if (userdata == nullptr || stream == nullptr || len <= 0) return;
		static_cast<Engine *>(userdata)->renderStereoFloat(
			reinterpret_cast<float *>(stream),
			static_cast<std::size_t>(len) / (sizeof(float) * 2u));
	}

	void renderStereoFloat(float *interleaved, std::size_t frameCount)
	{
		if (interleaved == nullptr || frameCount == 0) return;
		Command command;
		while (commands_.pop(command)) applyCommand(command);

		const std::int64_t base =
			frameOffset_.load(std::memory_order_relaxed) + renderedFrames_.load(std::memory_order_relaxed);
		const double vol = static_cast<double>(volume_.load(std::memory_order_relaxed)) / 100.0;
		const double oscGain = vol * kVoiceGain;
		const double attack = attackFrames_;
		const double release = releaseFrames_;

		const std::size_t pcmTotalFrames = pcmStereo_.size() / 2u;

		for (std::size_t i = 0; i < frameCount; ++i) {
			const std::int64_t frame = base + static_cast<std::int64_t>(i);
			double mixed = 0.0;

			for (Voice &voice : voices_) {
				if (!voice.active) continue;
				if (frame >= voice.endFrame) {
					voice.active = false;
					continue;
				}
				if (frame < voice.startFrame) continue;

				while (voice.eventCount > 0 && frame >= voice.events[voice.eventHead].frame) {
					voice.phaseIncrement = voice.events[voice.eventHead].phaseIncrement;
					voice.eventHead = (voice.eventHead + 1) % kMaxVoiceEvents;
					--voice.eventCount;
				}

				double level = 1.0;
				const double sinceStart = static_cast<double>(frame - voice.startFrame);
				if (sinceStart < attack) level = sinceStart / attack;
				if (voice.stopScheduled && frame >= voice.stopFrame) {
					const double sinceStop = static_cast<double>(frame - voice.stopFrame);
					const double decay = 1.0 - sinceStop / release;
					level *= decay > 0.0 ? decay : 0.0;
				}

				mixed += waveform(voice.type, voice.phase) * level;
				voice.phase += voice.phaseIncrement;
				if (voice.phase >= 1.0) voice.phase -= 1.0;
			}

			double left = mixed * oscGain;
			double right = left;

			if (pcmCursor_ < pcmTotalFrames) {
				left += static_cast<double>(pcmStereo_[pcmCursor_ * 2u]) * vol;
				right += static_cast<double>(pcmStereo_[pcmCursor_ * 2u + 1u]) * vol;
				++pcmCursor_;
			}

			left = std::clamp(left, -1.0, 1.0);
			right = std::clamp(right, -1.0, 1.0);
			interleaved[i * 2u] = static_cast<float>(left);
			interleaved[i * 2u + 1u] = static_cast<float>(right);
		}

		renderedFrames_.fetch_add(static_cast<std::int64_t>(frameCount), std::memory_order_relaxed);
	}

	OscillatorRecord *find(NativeAudioHandle handle)
	{
		if (handle == 0) return nullptr;
		const NativeAudioHandle slot = (handle & static_cast<NativeAudioHandle>(0x3ffu));
		if (slot == 0 || slot > static_cast<NativeAudioHandle>(kMaxOscillators)) return nullptr;
		OscillatorRecord &record = records_[slot - 1];
		return record.handle == handle ? &record : nullptr;
	}

	int launchCommands(const OscillatorRecord &record, NativeAudioHandle handle, Command *out) const
	{
		double initial = record.frequency;
		int count = 0;
		for (int i = 0; i < record.pendingCount; ++i) {
			if (record.pending[i].time <= record.startTime) initial = record.pending[i].value;
		}
		out[count++] = Command{CommandKind::StartVoice, record.type, handle, normalizeFrequency(initial),
		                       frameFor(record.startTime)};
		for (int i = 0; i < record.pendingCount; ++i) {
			const ScheduledValue &event = record.pending[i];
			if (event.time <= record.startTime) continue;
			out[count++] = Command{CommandKind::ScheduleFrequency, record.type, handle,
			                       normalizeFrequency(event.value), frameFor(event.time)};
		}
		if (record.stopped) {
			out[count++] = Command{CommandKind::StopVoice, record.type, handle,
			                       normalizeFrequency(record.frequency), frameFor(record.stopTime)};
		}
		return count;
	}

	double currentTimeLocked() const
	{
		if (!running_.load(std::memory_order_acquire)) return steadySeconds();
		const double rate = sampleRate_.load(std::memory_order_relaxed);
		const std::int64_t frames =
			frameOffset_.load(std::memory_order_relaxed) + renderedFrames_.load(std::memory_order_relaxed);
		return static_cast<double>(frames) / rate;
	}

	std::int64_t frameFor(double seconds) const
	{
		const double rate = sampleRate_.load(std::memory_order_relaxed);
		const double frame = seconds * rate;
		if (!std::isfinite(frame) || frame < 0.0) return 0;
		if (frame > 9.0e15) return static_cast<std::int64_t>(9.0e15);
		return static_cast<std::int64_t>(std::llround(frame));
	}

	void applyCommand(const Command &command)
	{
		switch (command.kind) {
		case CommandKind::StartVoice: {
			Voice *voice = allocateVoice();
			if (voice == nullptr) return;
			const double rate = sampleRate_.load(std::memory_order_relaxed);
			voice->active = true;
			voice->id = command.voice;
			voice->type = command.type;
			voice->phase = 0.0;
			voice->phaseIncrement = command.frequency / rate;
			voice->startFrame = command.frame;
			voice->stopFrame = 0;
			voice->stopScheduled = false;
			voice->endFrame = command.frame + static_cast<std::int64_t>(kMaxVoiceSeconds * rate);
			voice->eventHead = 0;
			voice->eventCount = 0;
			return;
		}
		case CommandKind::StopVoice: {
			Voice *voice = findVoice(command.voice);
			if (voice == nullptr) return;
			std::int64_t at = command.frame;
			if (at < voice->startFrame) at = voice->startFrame;
			voice->stopFrame = at;
			voice->stopScheduled = true;
			voice->endFrame = at + static_cast<std::int64_t>(releaseFrames_) + 1;
			return;
		}
		case CommandKind::SetFrequency: {
			Voice *voice = findVoice(command.voice);
			if (voice == nullptr) return;
			voice->phaseIncrement = command.frequency / sampleRate_.load(std::memory_order_relaxed);
			return;
		}
		case CommandKind::SetType: {
			Voice *voice = findVoice(command.voice);
			if (voice == nullptr) return;
			voice->type = command.type;
			return;
		}
		case CommandKind::ScheduleFrequency: {
			Voice *voice = findVoice(command.voice);
			if (voice == nullptr || voice->eventCount >= kMaxVoiceEvents) return;
			const double rate = sampleRate_.load(std::memory_order_relaxed);
			const VoiceEvent incoming{command.frame, command.frequency / rate};
			// Keep the small ring sorted by sample frame so out-of-order
			// setValueAtTime calls fire in chronological order.
			const int n = voice->eventCount;
			int insertPos = n;
			for (int i = 0; i < n; ++i) {
				const int idx = (voice->eventHead + i) % kMaxVoiceEvents;
				if (incoming.frame < voice->events[idx].frame) {
					insertPos = i;
					break;
				}
			}
			for (int i = n; i > insertPos; --i) {
				const int dst = (voice->eventHead + i) % kMaxVoiceEvents;
				const int src = (voice->eventHead + i - 1) % kMaxVoiceEvents;
				voice->events[dst] = voice->events[src];
			}
			voice->events[(voice->eventHead + insertPos) % kMaxVoiceEvents] = incoming;
			++voice->eventCount;
			return;
		}
		}
	}

	Voice *allocateVoice()
	{
		Voice *oldest = &voices_[0];
		for (Voice &voice : voices_) {
			if (!voice.active) return &voice;
			if (voice.startFrame < oldest->startFrame) oldest = &voice;
		}
		return oldest;
	}

	Voice *findVoice(NativeAudioHandle id)
	{
		for (Voice &voice : voices_) {
			if (voice.active && voice.id == id) return &voice;
		}
		return nullptr;
	}

	std::mutex setupMutex_;
	std::atomic<bool> setupDone_{false};
	std::atomic<bool> running_{false};
	std::atomic<double> sampleRate_{kDefaultSampleRate};
	std::atomic<std::int64_t> frameOffset_{0};
	std::atomic<std::int64_t> renderedFrames_{0};
	std::atomic<int> volume_{80};
	bool testMode_ = false;
	SDL_AudioDeviceID device_ = 0;
	double attackFrames_ = kAttackSeconds * kDefaultSampleRate;
	double releaseFrames_ = kReleaseSeconds * kDefaultSampleRate;

	std::mutex recordsMutex_;
	OscillatorRecord records_[kMaxOscillators]{};
	int nextSlot_ = 0;
	std::uint32_t generation_ = 0;

	CommandQueue commands_;
	Voice voices_[kMaxVoices]{};
	std::vector<float> pcmStereo_;
	std::size_t pcmCursor_ = 0;
};

}  // namespace

// AudioParam
AudioParam::AudioParam(NativeAudioHandle oscillator) : oscillator_(oscillator) {}
double AudioParam::value() const { return Engine::instance().frequency(oscillator_); }
void AudioParam::setValue(double value) { Engine::instance().setFrequency(oscillator_, value); }
void AudioParam::setValueAtTime(double value, double start_time)
{
	Engine::instance().scheduleFrequency(oscillator_, value, start_time);
}

// AudioNode
AudioNode::AudioNode(NativeAudioHandle native) : native_(native) {}
NativeAudioHandle AudioNode::nativeId() const { return native_; }

// AudioDestinationNode
AudioDestinationNode::AudioDestinationNode(NativeAudioHandle native) : AudioNode(native) {}

// OscillatorNode
OscillatorNode::OscillatorNode(NativeAudioHandle native) : AudioNode(native), frequency(native) {}
OscillatorType OscillatorNode::type() const { return Engine::instance().type(nativeId()); }
void OscillatorNode::setType(OscillatorType type) { Engine::instance().setType(nativeId(), type); }
void OscillatorNode::connect(const AudioDestinationNode &) { Engine::instance().connect(nativeId()); }
void OscillatorNode::start(double when) { Engine::instance().start(nativeId(), when); }
void OscillatorNode::stop(double when) { Engine::instance().stop(nativeId(), when); }

// AudioContext
double AudioContext::currentTime() const { return Engine::instance().currentTime(); }
AudioDestinationNode AudioContext::destination() const { return AudioDestinationNode(1); }
OscillatorNode AudioContext::createOscillator() const
{
	return OscillatorNode(static_cast<NativeAudioHandle>(Engine::instance().createOscillator()));
}

// AudioSystem
AudioContext AudioSystem::sharedContext() { return AudioContext{}; }
int AudioSystem::volume() { return Engine::instance().volume(); }
void AudioSystem::setVolume(int v) { Engine::instance().setVolume(v); }
bool AudioSystem::playFile(const std::string &path) { return Engine::instance().playFile(path); }
bool AudioSystem::playPcm(const std::int16_t *samples, std::size_t sample_count, int sample_rate, int channels)
{
	return Engine::instance().playPcm(samples, sample_count, sample_rate, channels);
}
void AudioSystem::stopPlayback() { Engine::instance().stopPlayback(); }
void AudioSystem::flushPlayback() { Engine::instance().stopPlayback(); }

}  // namespace gea::platform::audio

#ifdef GEA_SAILFISH_AUDIO_TEST
namespace gea::sailfish::audio_test {

void resetForTest(double sampleRate)
{
	auto &eng = gea::platform::audio::Engine::instance();
	eng.resetForTest(sampleRate);
}

void renderForTest(float *interleaved, std::size_t frameCount)
{
	gea::platform::audio::Engine::instance().renderForTest(interleaved, frameCount);
}

}  // namespace gea::sailfish::audio_test
#endif
