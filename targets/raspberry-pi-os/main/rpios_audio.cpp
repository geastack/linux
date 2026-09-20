/* targets/raspberry-pi-os/main/rpios_audio.cpp
 * Audio is stubbed on the linux target for now. Apps just want sound to not
 * crash the runtime; an SDL_audio/ALSA oscillator backend is a follow-up.
 * Every method returns inert values and discards input.
 * (Same surface as macos_audio.cpp plus the playFile/playPcm/stopPlayback
 * trio geaos_audio.cpp defines — all are required to link host/audio.cpp.) */

#include "audio.h"

namespace gea::platform::audio {

namespace {
int g_volume = 80;
}

// AudioParam
AudioParam::AudioParam(NativeAudioHandle oscillator) : oscillator_(oscillator) {}
double AudioParam::value() const { return 0.0; }
void AudioParam::setValue(double) {}
void AudioParam::setValueAtTime(double, double) {}

// AudioNode
AudioNode::AudioNode(NativeAudioHandle native) : native_(native) {}
NativeAudioHandle AudioNode::nativeId() const { return native_; }

// AudioDestinationNode
AudioDestinationNode::AudioDestinationNode(NativeAudioHandle native) : AudioNode(native) {}

// OscillatorNode
OscillatorNode::OscillatorNode(NativeAudioHandle native) : AudioNode(native), frequency(native) {}
OscillatorType OscillatorNode::type() const { return OscillatorType::Sine; }
void OscillatorNode::setType(OscillatorType) {}
void OscillatorNode::connect(const AudioDestinationNode &) {}
void OscillatorNode::start(double) {}
void OscillatorNode::stop(double) {}

// AudioContext
double AudioContext::currentTime() const { return 0.0; }
AudioDestinationNode AudioContext::destination() const { return AudioDestinationNode(0); }
OscillatorNode AudioContext::createOscillator() const { return OscillatorNode(0); }

// AudioSystem
AudioContext AudioSystem::sharedContext() { return AudioContext{}; }
int AudioSystem::volume() { return g_volume; }
void AudioSystem::setVolume(int v)
{
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	g_volume = v;
}
bool AudioSystem::playFile(const std::string &) { return false; }
bool AudioSystem::playPcm(const std::int16_t *, std::size_t, int, int) { return false; }
void AudioSystem::stopPlayback() {}

}  // namespace gea::platform::audio
