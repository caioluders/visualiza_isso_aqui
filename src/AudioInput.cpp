#include "AudioInput.h"

#include <cstdio>
#include <cstring>
#include <string>

AudioInput::AudioInput() : ringWriteIndex(0), numChannels(0), activeSampleRate(0) {
	// 1 second ring buffer at 48k * 2 channels default
	ringBuffer.resize(48000 * 2);
}

AudioInput::~AudioInput() {
	stopStream();
}

bool AudioInput::startStream(unsigned int sampleRate, unsigned int channels) {
	if (rtAudio.getDeviceCount() < 1) {
		std::fprintf(stderr, "No audio devices found!\n");
		return false;
	}
	RtAudio::StreamParameters iParams;
    iParams.deviceId = rtAudio.getDefaultInputDevice();
    // Clamp to device input channel capability
    RtAudio::DeviceInfo info;
    try { info = rtAudio.getDeviceInfo(iParams.deviceId); } catch (...) { info = {}; info.inputChannels = channels; }
    unsigned int usedChannels = std::max(1u, std::min(channels, info.inputChannels));
    iParams.nChannels = usedChannels;
	iParams.firstChannel = 0;

	RtAudio::StreamOptions options;
	options.flags = RTAUDIO_MINIMIZE_LATENCY;

    // Pick a supported sample rate if requested one is not supported
    unsigned int usedRate = sampleRate;
    if (!info.sampleRates.empty()) {
        bool supported = false;
        for (auto r : info.sampleRates) if (r == sampleRate) { supported = true; break; }
        if (!supported) {
            usedRate = info.preferredSampleRate ? info.preferredSampleRate : info.sampleRates.front();
            std::fprintf(stderr, "Requested %u Hz not supported. Using %u Hz on '%s'\n", sampleRate, usedRate, info.name.c_str());
        }
    }
    unsigned int bufferFrames = 512; // small latency
    numChannels = usedChannels;
    try {
        rtAudio.openStream(nullptr, &iParams, RTAUDIO_FLOAT32, usedRate, &bufferFrames, &AudioInput::audioCallback, this, &options);
        rtAudio.startStream();
        activeSampleRate = usedRate;
    } catch (...) {
        // Fallback to mono
        if (usedChannels > 1) {
            try {
                iParams.nChannels = 1;
                numChannels = 1;
                rtAudio.openStream(nullptr, &iParams, RTAUDIO_FLOAT32, usedRate, &bufferFrames, &AudioInput::audioCallback, this, &options);
                rtAudio.startStream();
                activeSampleRate = usedRate;
            } catch (...) {
                std::fprintf(stderr, "RtAudio error while opening/starting stream (fallback mono failed)\n");
                return false;
            }
        } else {
            std::fprintf(stderr, "RtAudio error while opening/starting stream\n");
            return false;
        }
    }
	return true;
}

void AudioInput::stopStream() {
	if (rtAudio.isStreamOpen()) {
		try {
			if (rtAudio.isStreamRunning()) rtAudio.stopStream();
			rtAudio.closeStream();
		} catch (...) {}
	}
}

std::vector<AudioInput::DeviceInfo> AudioInput::listInputDevices() {
	std::vector<DeviceInfo> result;
    std::vector<unsigned int> ids;
    try { ids = rtAudio.getDeviceIds(); } catch (...) { ids.clear(); }
    for (unsigned int id : ids) {
        RtAudio::DeviceInfo info;
        try { info = rtAudio.getDeviceInfo(id); } catch (...) { continue; }
        if (info.inputChannels > 0) {
            DeviceInfo di{};
            di.id = id;
            di.name = info.name;
            di.inputChannels = info.inputChannels;
            di.isDefault = info.isDefaultInput;
            result.push_back(di);
        }
    }
	return result;
}

bool AudioInput::startStreamOnDevice(unsigned int deviceIndex, unsigned int sampleRate, unsigned int channels) {
	stopStream();
	RtAudio::StreamParameters iParams;
	iParams.deviceId = deviceIndex;
    // Clamp based on device info
    RtAudio::DeviceInfo info;
    try { info = rtAudio.getDeviceInfo(deviceIndex); } catch (...) { info.inputChannels = channels; }
    unsigned int usedChannels = std::max(1u, std::min(channels, info.inputChannels));
    iParams.nChannels = usedChannels;
	iParams.firstChannel = 0;
	RtAudio::StreamOptions options;
	options.flags = RTAUDIO_MINIMIZE_LATENCY;
    // Pick a supported sample rate if requested one is not supported
    unsigned int usedRate = sampleRate;
    if (!info.sampleRates.empty()) {
        bool supported = false;
        for (auto r : info.sampleRates) if (r == sampleRate) { supported = true; break; }
        if (!supported) {
            usedRate = info.preferredSampleRate ? info.preferredSampleRate : info.sampleRates.front();
            std::fprintf(stderr, "Requested %u Hz not supported. Using %u Hz on '%s'\n", sampleRate, usedRate, info.name.c_str());
        }
    }
    unsigned int bufferFrames = 512;
    numChannels = usedChannels;
    try {
        rtAudio.openStream(nullptr, &iParams, RTAUDIO_FLOAT32, usedRate, &bufferFrames, &AudioInput::audioCallback, this, &options);
        rtAudio.startStream();
        activeSampleRate = usedRate;
        return true;
    } catch (...) {
        if (usedChannels > 1) {
            try {
                iParams.nChannels = 1;
                numChannels = 1;
                rtAudio.openStream(nullptr, &iParams, RTAUDIO_FLOAT32, usedRate, &bufferFrames, &AudioInput::audioCallback, this, &options);
                rtAudio.startStream();
                activeSampleRate = usedRate;
                return true;
            } catch (...) { return false; }
        }
        return false;
    }
}

int AudioInput::audioCallback(void* /*outputBuffer*/, void* inputBuffer, unsigned int nFrames,
		double /*streamTime*/, RtAudioStreamStatus status, void* userData) {
	AudioInput* self = static_cast<AudioInput*>(userData);
	if (status) {
		// over/underflow
	}
	if (inputBuffer) {
		self->pushSamples(reinterpret_cast<float*>(inputBuffer), nFrames, self->numChannels);
	}
	return 0;
}

void AudioInput::pushSamples(const float* interleaved, unsigned int frames, unsigned int channels) {
	std::lock_guard<std::mutex> lock(mutex);
	const size_t total = frames * channels;
	if (ringBuffer.size() < total) ringBuffer.resize(total);
	for (size_t i = 0; i < total; ++i) {
		ringBuffer[(ringWriteIndex + i) % ringBuffer.size()] = interleaved[i];
	}
	ringWriteIndex = (ringWriteIndex + total) % ringBuffer.size();
}

void AudioInput::readLatest(size_t maxSamples, std::vector<float>& out) {
    // Interpret maxSamples as number of frames requested; always return interleaved with current numChannels
    std::lock_guard<std::mutex> lock(mutex);
    if (numChannels == 0) { out.clear(); return; }
    size_t wantFloats = maxSamples * numChannels;
    size_t capacity = ringBuffer.size();
    size_t count = wantFloats > capacity ? capacity : wantFloats;
    size_t start = (ringWriteIndex + capacity - count) % capacity;
    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        out[i] = ringBuffer[(start + i) % capacity];
    }
}


