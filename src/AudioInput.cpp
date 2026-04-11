#include "AudioInput.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <cmath>
#include <vector>

#if defined(__LINUX_PULSE__)
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

AudioInput::AudioInput()
    :
#if defined(__LINUX_PULSE__)
      rtAudio(RtAudio::LINUX_PULSE),
      pulseHandle(nullptr),
      pulseThread(),
      pulseRunning(false),
#endif
      ringWriteIndex(0),
      ringValidSamples(0),
      totalFramesCaptured(0),
      lastInputRms(0.0f),
      numChannels(0),
      activeSampleRate(0),
      activeDeviceId((unsigned int)-1),
      activeDeviceName() {
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
	return openInputStream(rtAudio.getDefaultInputDevice(), sampleRate, channels);
}

bool AudioInput::openInputStream(unsigned int deviceId, unsigned int sampleRate, unsigned int channels) {
	stopStream();

	RtAudio::StreamParameters iParams;
    iParams.deviceId = deviceId;
    // Clamp to device input channel capability
    RtAudio::DeviceInfo info;
    try { info = rtAudio.getDeviceInfo(iParams.deviceId); } catch (...) { info = {}; info.inputChannels = channels; }
    if (info.inputChannels == 0) {
        std::fprintf(stderr, "RtAudio device %u has no input channels\n", iParams.deviceId);
        return false;
    }
    unsigned int usedChannels = std::max(1u, std::min(channels, info.inputChannels));
    iParams.nChannels = usedChannels;
	iParams.firstChannel = 0;

	RtAudio::StreamOptions options;
	options.flags = RTAUDIO_MINIMIZE_LATENCY;
	options.streamName = "visualiza_isso_aqui";

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
    activeDeviceId = iParams.deviceId;
    activeDeviceName = info.name.empty() ? std::string("Device ") + std::to_string(iParams.deviceId) : info.name;
    activeSampleRate = usedRate;

    auto tryOpen = [&](unsigned int streamChannels, const char* label) {
        if (rtAudio.isStreamOpen()) rtAudio.closeStream();
        iParams.nChannels = streamChannels;
        numChannels = streamChannels;
        resetCaptureStats();
        try {
            RtAudioErrorType openErr = rtAudio.openStream(nullptr, &iParams, RTAUDIO_FLOAT32, usedRate, &bufferFrames, &AudioInput::audioCallback, this, &options);
            if (openErr != RTAUDIO_NO_ERROR) {
                std::fprintf(stderr, "RtAudio %s openStream failed on '%s': %s\n", label, activeDeviceName.c_str(), rtAudio.getErrorText().c_str());
                return false;
            }
            RtAudioErrorType startErr = rtAudio.startStream();
            if (startErr != RTAUDIO_NO_ERROR) {
                std::fprintf(stderr, "RtAudio %s startStream failed on '%s': %s\n", label, activeDeviceName.c_str(), rtAudio.getErrorText().c_str());
                rtAudio.closeStream();
                return false;
            }
            return true;
        } catch (...) {
            std::fprintf(stderr, "RtAudio %s error while opening/starting stream on '%s'\n", label, activeDeviceName.c_str());
            return false;
        }
    };

    if (tryOpen(usedChannels, "input")) return true;
    if (usedChannels > 1 && tryOpen(1, "mono fallback")) return true;

    numChannels = 0;
    activeSampleRate = 0;
    return false;
}

void AudioInput::stopStream() {
	stopPulseStream();
	if (rtAudio.isStreamOpen()) {
		try {
			if (rtAudio.isStreamRunning()) rtAudio.stopStream();
			rtAudio.closeStream();
		} catch (...) {}
	}
	numChannels = 0;
	activeSampleRate = 0;
}

void AudioInput::stopPulseStream() {
#if defined(__LINUX_PULSE__)
	pulseRunning = false;
	if (pulseHandle) {
		pa_simple_free(static_cast<pa_simple*>(pulseHandle));
		pulseHandle = nullptr;
	}
	if (pulseThread.joinable()) pulseThread.join();
#endif
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
	return openInputStream(deviceIndex, sampleRate, channels);
}

bool AudioInput::startPulseSource(const std::string& sourceName, unsigned int sampleRate, unsigned int channels) {
#if defined(__LINUX_PULSE__)
	stopStream();
	channels = std::max(1u, std::min(channels, 2u));
	if (sampleRate == 0) sampleRate = 48000;

	pa_sample_spec spec{};
	spec.format = PA_SAMPLE_FLOAT32LE;
	spec.rate = sampleRate;
	spec.channels = static_cast<uint8_t>(channels);

	pa_buffer_attr attr{};
	attr.maxlength = static_cast<uint32_t>(-1);
	attr.tlength = static_cast<uint32_t>(-1);
	attr.prebuf = static_cast<uint32_t>(-1);
	attr.minreq = static_cast<uint32_t>(-1);
	attr.fragsize = 512 * channels * sizeof(float);

	int error = 0;
	pa_simple* handle = pa_simple_new(
		nullptr,
		"visualiza_isso_aqui",
		PA_STREAM_RECORD,
		sourceName.empty() ? nullptr : sourceName.c_str(),
		"Audio analysis",
		&spec,
		nullptr,
		&attr,
		&error);
	if (!handle) {
		std::fprintf(stderr, "Pulse source open failed for '%s': %s\n", sourceName.c_str(), pa_strerror(error));
		return false;
	}

	pulseHandle = handle;
	pulseRunning = true;
	numChannels = channels;
	activeSampleRate = sampleRate;
	activeDeviceId = (unsigned int)-1;
	activeDeviceName = sourceName.empty() ? "Pulse default source" : sourceName;
	resetCaptureStats();

	pulseThread = std::thread([this, handle, channels] {
		const unsigned int framesPerRead = 512;
		std::vector<float> buffer(framesPerRead * channels, 0.0f);
		while (pulseRunning.load()) {
			int readError = 0;
			if (pa_simple_read(handle, buffer.data(), buffer.size() * sizeof(float), &readError) < 0) {
				if (pulseRunning.load()) {
					std::fprintf(stderr, "Pulse source read failed: %s\n", pa_strerror(readError));
				}
				break;
			}
			pushSamples(buffer.data(), framesPerRead, channels);
		}
	});
	return true;
#else
	(void)sourceName;
	(void)sampleRate;
	(void)channels;
	return false;
#endif
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
	float sumSq = 0.0f;
	for (size_t i = 0; i < total; ++i) {
		float v = interleaved[i];
		ringBuffer[(ringWriteIndex + i) % ringBuffer.size()] = v;
		sumSq += v * v;
	}
	ringWriteIndex = (ringWriteIndex + total) % ringBuffer.size();
	ringValidSamples = std::min(ringBuffer.size(), ringValidSamples + total);
	totalFramesCaptured += frames;
	lastInputRms = total > 0 ? std::sqrt(sumSq / float(total)) : 0.0f;
}

void AudioInput::readLatest(size_t maxSamples, std::vector<float>& out) {
    // Interpret maxSamples as number of frames requested; always return interleaved with current numChannels
    std::lock_guard<std::mutex> lock(mutex);
    if (numChannels == 0) { out.clear(); return; }
    size_t wantFloats = maxSamples * numChannels;
    size_t capacity = ringBuffer.size();
    size_t count = std::min({wantFloats, capacity, ringValidSamples});
    if (count == 0) { out.clear(); return; }
    size_t start = (ringWriteIndex + capacity - count) % capacity;
    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        out[i] = ringBuffer[(start + i) % capacity];
    }
}

bool AudioInput::isStreamOpen() const {
#if defined(__LINUX_PULSE__)
    if (pulseRunning.load()) return true;
#endif
    try { return rtAudio.isStreamOpen(); } catch (...) { return false; }
}

bool AudioInput::isStreamRunning() const {
#if defined(__LINUX_PULSE__)
    if (pulseRunning.load()) return true;
#endif
    try { return rtAudio.isStreamRunning(); } catch (...) { return false; }
}

size_t AudioInput::getCapturedFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return totalFramesCaptured;
}

float AudioInput::getLastInputRms() const {
    std::lock_guard<std::mutex> lock(mutex);
    return lastInputRms;
}

std::string AudioInput::getActiveDeviceName() const {
    std::lock_guard<std::mutex> lock(mutex);
    return activeDeviceName;
}

unsigned int AudioInput::getActiveDeviceId() const {
    std::lock_guard<std::mutex> lock(mutex);
    return activeDeviceId;
}

void AudioInput::resetCaptureStats() {
    std::lock_guard<std::mutex> lock(mutex);
    ringWriteIndex = 0;
    ringValidSamples = 0;
    totalFramesCaptured = 0;
    lastInputRms = 0.0f;
    ringBuffer.assign(std::max(1u, activeSampleRate ? activeSampleRate : 48000u) * std::max(1u, numChannels), 0.0f);
}
