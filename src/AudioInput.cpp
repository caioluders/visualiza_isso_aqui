#include "AudioInput.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#if defined(__LINUX_PULSE__)
#include <pulse/error.h>
#include <pulse/simple.h>
#include <pulse/def.h>
#endif

AudioInput::AudioInput()
#if defined(__LINUX_PULSE__)
	: rtAudio(RtAudio::LINUX_PULSE),
#else
	: rtAudio(),
#endif
	  ringWriteIndex(0), totalSamplesWritten(0), lastReadSampleCount(0), numChannels(0), activeSampleRate(0), currentDeviceId(0),
	  lastCaptureLatencyMs(0.0), lastFreshBlockRateHz(0.0), pulseBlocksCaptured(0), lastPulseBlockTime(std::chrono::steady_clock::now())
#if defined(__LINUX_PULSE__)
	  , pulseRecord(nullptr), pulseRunning(false)
#endif
{
	// 1 second ring buffer at 48k * 2 channels default
	ringBuffer.resize(48000 * 2);
}

AudioInput::~AudioInput() {
	stopStream();
}

void AudioInput::resetBufferState() {
	std::lock_guard<std::mutex> lock(mutex);
	ringWriteIndex = 0;
	totalSamplesWritten = 0;
	lastReadSampleCount = 0;
	lastCaptureLatencyMs = 0.0;
	lastFreshBlockRateHz = 0.0;
	pulseBlocksCaptured = 0;
	lastPulseBlockTime = std::chrono::steady_clock::now();
	std::fill(ringBuffer.begin(), ringBuffer.end(), 0.0f);
}

#if defined(__LINUX_PULSE__)
std::string AudioInput::trim(const std::string& s) {
	size_t start = 0;
	while (start < s.size() && std::isspace((unsigned char)s[start])) ++start;
	size_t end = s.size();
	while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
	return s.substr(start, end - start);
}

std::string AudioInput::prettifyPulseSourceName(const std::string& sourceName) {
	std::string label = sourceName;
	auto cut = label.find("alsa_");
	if (cut == 0) {
		size_t firstDot = label.find('.');
		if (firstDot != std::string::npos) label = label.substr(firstDot + 1);
	}
	for (char& c : label) {
		if (c == '.' || c == '_' || c == '-') c = ' ';
	}
	label = trim(label);
	if (sourceName.find(".monitor") != std::string::npos) label += " (playback)";
	else if (sourceName.find("mono-fallback") != std::string::npos || sourceName.find("mic") != std::string::npos) label += " (mic)";
	return label;
}

std::vector<AudioInput::DeviceInfo> AudioInput::listPulseSources() {
	std::vector<DeviceInfo> devices;
	FILE* fp = popen("pactl list short sources 2>/dev/null", "r");
	if (!fp) return devices;
	char line[2048];
	unsigned int nextId = 1;
	while (fgets(line, sizeof(line), fp)) {
		std::string row(line);
		std::vector<std::string> cols;
		std::stringstream ss(row);
		std::string part;
		while (std::getline(ss, part, '\t')) cols.push_back(trim(part));
		if (cols.size() < 4) continue;
		const std::string& sourceName = cols[1];
		const std::string& sampleSpec = cols[3];
		unsigned int channels = 1;
		size_t chPos = sampleSpec.find("ch");
		if (chPos != std::string::npos) {
			size_t numStart = sampleSpec.rfind(' ', chPos);
			if (numStart != std::string::npos) {
				channels = (unsigned int)std::max(1, std::atoi(sampleSpec.substr(numStart + 1, chPos - (numStart + 1)).c_str()));
			}
		}
		DeviceInfo d{};
		d.id = nextId++;
		d.name = prettifyPulseSourceName(sourceName);
		d.sourceName = sourceName;
		d.inputChannels = channels;
		d.outputChannels = sourceName.find(".monitor") != std::string::npos ? channels : 0;
		d.duplexChannels = d.outputChannels > 0 ? channels : 0;
		d.isDefault = false;
		devices.push_back(d);
	}
	pclose(fp);
	std::stable_sort(devices.begin(), devices.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
		const bool aMonitor = a.outputChannels > 0;
		const bool bMonitor = b.outputChannels > 0;
		if (aMonitor != bMonitor) return aMonitor > bMonitor;
		if (a.inputChannels != b.inputChannels) return a.inputChannels > b.inputChannels;
		return a.name < b.name;
	});
	for (unsigned int i = 0; i < devices.size(); ++i) devices[i].id = i + 1;
	return devices;
}

bool AudioInput::startPulseSource(const DeviceInfo& device, unsigned int sampleRate) {
	stopPulseSource();
	resetBufferState();
	lastError.clear();

	const unsigned int pulseFramesPerRead = 256;
	pa_sample_spec spec{};
	spec.format = PA_SAMPLE_FLOAT32LE;
	spec.rate = sampleRate;
	spec.channels = (uint8_t)std::max(1u, device.inputChannels);
	pa_buffer_attr attr{};
	const uint32_t fragmentBytes = pulseFramesPerRead * spec.channels * sizeof(float);
	attr.fragsize = fragmentBytes;
	attr.maxlength = fragmentBytes * 4;
	attr.minreq = (uint32_t)-1;
	attr.prebuf = (uint32_t)-1;
	attr.tlength = (uint32_t)-1;
	int err = 0;
	pulseRecord = pa_simple_new(nullptr, "visualiza_isso_aqui", PA_STREAM_RECORD, device.sourceName.c_str(), "analysis", &spec, nullptr, &attr, &err);
	if (!pulseRecord) {
		lastError = pa_strerror(err);
		return false;
	}

	numChannels = device.inputChannels;
	activeSampleRate = sampleRate;
	currentDeviceId = device.id;
	pulseRunning = true;
	pulseThread = std::thread([this, channels = device.inputChannels, pulseFramesPerRead]() {
		std::vector<float> buffer(pulseFramesPerRead * channels, 0.0f);
		while (pulseRunning.load()) {
			int errLocal = 0;
			if (pa_simple_read(pulseRecord, buffer.data(), buffer.size() * sizeof(float), &errLocal) < 0) {
				lastError = pa_strerror(errLocal);
				pulseRunning = false;
				break;
			}
			int latencyErr = 0;
			const pa_usec_t latencyUsec = pa_simple_get_latency(pulseRecord, &latencyErr);
			const auto now = std::chrono::steady_clock::now();
			const double dtSec = std::chrono::duration<double>(now - lastPulseBlockTime).count();
			lastPulseBlockTime = now;
			if (latencyErr == 0) {
				lastCaptureLatencyMs = double(latencyUsec) / 1000.0;
			}
			if (dtSec > 1e-6) {
				lastFreshBlockRateHz = 1.0 / dtSec;
			}
			++pulseBlocksCaptured;
			pushSamples(buffer.data(), pulseFramesPerRead, channels);
		}
	});
	return true;
}

void AudioInput::stopPulseSource() {
	pulseRunning = false;
	if (pulseThread.joinable()) pulseThread.join();
	if (pulseRecord) {
		pa_simple_free(pulseRecord);
		pulseRecord = nullptr;
	}
}
#endif

bool AudioInput::openStreamOnDevice(unsigned int deviceId, unsigned int sampleRate, unsigned int channels, bool allowMonoFallback) {
	RtAudio::StreamParameters iParams;
	iParams.deviceId = deviceId;
	iParams.firstChannel = 0;

	RtAudio::DeviceInfo info;
	try {
		info = rtAudio.getDeviceInfo(deviceId);
	} catch (...) {
		lastError = rtAudio.getErrorText();
		if (lastError.empty()) lastError = "Failed to query audio device information";
		return false;
	}

	unsigned int usedChannels = std::max(1u, std::min(channels, info.inputChannels));
	iParams.nChannels = usedChannels;

	unsigned int usedRate = sampleRate;
	if (!info.sampleRates.empty()) {
		bool supported = false;
		for (auto r : info.sampleRates) {
			if (r == sampleRate) {
				supported = true;
				break;
			}
		}
		if (!supported) {
			usedRate = info.preferredSampleRate ? info.preferredSampleRate : info.sampleRates.front();
		}
	}

	RtAudio::StreamOptions options;
	options.flags = RTAUDIO_MINIMIZE_LATENCY;
	unsigned int bufferFrames = 512;
	numChannels = usedChannels;

	try {
		rtAudio.openStream(nullptr, &iParams, RTAUDIO_FLOAT32, usedRate, &bufferFrames, &AudioInput::audioCallback, this, &options);
		rtAudio.startStream();
		activeSampleRate = usedRate;
		currentDeviceId = deviceId;
		lastError.clear();
		return true;
	} catch (...) {
		lastError = rtAudio.getErrorText();
		if (lastError.empty()) lastError = "RtAudio failed to open/start the selected device";
	}

	if (allowMonoFallback && usedChannels > 1) {
		try {
			iParams.nChannels = 1;
			numChannels = 1;
			rtAudio.openStream(nullptr, &iParams, RTAUDIO_FLOAT32, usedRate, &bufferFrames, &AudioInput::audioCallback, this, &options);
			rtAudio.startStream();
			activeSampleRate = usedRate;
			currentDeviceId = deviceId;
			lastError.clear();
			return true;
		} catch (...) {
			lastError = rtAudio.getErrorText();
			if (lastError.empty()) lastError = "RtAudio mono fallback failed";
		}
	}

	return false;
}

bool AudioInput::startStream(unsigned int sampleRate, unsigned int channels) {
#if defined(__LINUX_PULSE__)
	stopPulseSource();
	resetBufferState();
	cachedDevices = listPulseSources();
	if (cachedDevices.empty()) {
		lastError = "No PulseAudio/PipeWire sources found";
		return false;
	}
	for (const auto& d : cachedDevices) {
		if (d.outputChannels > 0 && startPulseSource(d, sampleRate)) return true;
	}
	for (const auto& d : cachedDevices) {
		if (startPulseSource(d, sampleRate)) return true;
	}
	return false;
#else
	stopStream();
	resetBufferState();
	lastError.clear();

	if (rtAudio.getDeviceCount() < 1) {
		lastError = "No audio devices found";
		std::fprintf(stderr, "%s\n", lastError.c_str());
		return false;
	}

	std::vector<unsigned int> ids;
	try {
		ids = rtAudio.getDeviceIds();
	} catch (...) {
		ids.clear();
	}
	if (ids.empty()) {
		lastError = "No audio input devices available";
		return false;
	}

	std::stable_sort(ids.begin(), ids.end(), [&](unsigned int a, unsigned int b) {
		auto rank = [&](unsigned int id) {
			int score = 100;
			try {
				RtAudio::DeviceInfo info = rtAudio.getDeviceInfo(id);
				std::string name = info.name;
				std::string lower = name;
				std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
				if (info.isDefaultInput) score -= 40;
				if (lower.find("pipewire") != std::string::npos) score -= 25;
				if (lower.find("pulse") != std::string::npos) score -= 20;
				if (lower.find("default") != std::string::npos) score -= 15;
				if (lower.rfind("hw:", 0) == 0) score += 20;
			} catch (...) {}
			return score;
		};
		return rank(a) < rank(b);
	});

	std::string failureLog;
	for (unsigned int id : ids) {
		RtAudio::DeviceInfo info;
		try {
			info = rtAudio.getDeviceInfo(id);
		} catch (...) {
			continue;
		}
		if (info.inputChannels == 0) continue;
		if (openStreamOnDevice(id, sampleRate, channels, true)) {
			return true;
		}
		if (!failureLog.empty()) failureLog += " | ";
		failureLog += info.name + ": " + lastError;
		stopStream();
		resetBufferState();
	}

	lastError = failureLog.empty() ? "Failed to open any audio input device" : failureLog;
	std::fprintf(stderr, "%s\n", lastError.c_str());
	return false;
#endif
}

void AudioInput::stopStream() {
#if defined(__LINUX_PULSE__)
	stopPulseSource();
#endif
	if (rtAudio.isStreamOpen()) {
		try {
			if (rtAudio.isStreamRunning()) rtAudio.stopStream();
			rtAudio.closeStream();
		} catch (...) {}
	}
	std::lock_guard<std::mutex> lock(mutex);
	ringWriteIndex = 0;
	totalSamplesWritten = 0;
	lastReadSampleCount = 0;
	std::fill(ringBuffer.begin(), ringBuffer.end(), 0.0f);
	numChannels = 0;
	activeSampleRate = 0;
	currentDeviceId = 0;
}

std::vector<AudioInput::DeviceInfo> AudioInput::listInputDevices() {
#if defined(__LINUX_PULSE__)
	cachedDevices = listPulseSources();
	return cachedDevices;
#else
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
            di.outputChannels = info.outputChannels;
            di.duplexChannels = info.duplexChannels;
            di.isDefault = info.isDefaultInput;
            result.push_back(di);
        }
    }
	std::stable_sort(result.begin(), result.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
		const bool aMonitorLike = a.inputChannels > 0 && a.outputChannels > 0;
		const bool bMonitorLike = b.inputChannels > 0 && b.outputChannels > 0;
		if (aMonitorLike != bMonitorLike) return aMonitorLike > bMonitorLike;
		if (a.isDefault != b.isDefault) return a.isDefault > b.isDefault;
		return a.name < b.name;
	});
	return result;
#endif
}

bool AudioInput::startStreamOnDevice(unsigned int deviceIndex, unsigned int sampleRate, unsigned int channels) {
#if defined(__LINUX_PULSE__)
	(void)channels;
	if (cachedDevices.empty()) cachedDevices = listPulseSources();
	auto it = std::find_if(cachedDevices.begin(), cachedDevices.end(), [&](const DeviceInfo& d) { return d.id == deviceIndex; });
	if (it == cachedDevices.end()) {
		lastError = "Selected PulseAudio/PipeWire source no longer exists";
		return false;
	}
	return startPulseSource(*it, sampleRate);
#else
	const unsigned int previousDeviceId = currentDeviceId;
	const unsigned int previousSampleRate = activeSampleRate;
	const unsigned int previousChannels = numChannels;
	stopStream();
	resetBufferState();
	lastError.clear();

	if (openStreamOnDevice(deviceIndex, sampleRate, channels, true)) {
		return true;
	}

	const std::string requestedError = lastError;
	if (previousDeviceId != 0 && previousSampleRate > 0 && previousChannels > 0 && previousDeviceId != deviceIndex) {
		stopStream();
		resetBufferState();
		if (openStreamOnDevice(previousDeviceId, previousSampleRate, previousChannels, true)) {
			lastError = "Failed to open requested device. Restored previous input: " + requestedError;
			return false;
		}
	}

	lastError = requestedError;
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
	for (size_t i = 0; i < total; ++i) {
		ringBuffer[(ringWriteIndex + i) % ringBuffer.size()] = interleaved[i];
	}
	ringWriteIndex = (ringWriteIndex + total) % ringBuffer.size();
	totalSamplesWritten += total;
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

bool AudioInput::readLatestIfNew(size_t maxSamples, std::vector<float>& out) {
	std::lock_guard<std::mutex> lock(mutex);
	if (numChannels == 0) {
		out.clear();
		return false;
	}

	const size_t wantFloats = maxSamples * numChannels;
	const size_t capacity = ringBuffer.size();
	if (wantFloats == 0 || capacity == 0) {
		out.clear();
		return false;
	}

	const size_t availableSinceLastRead = totalSamplesWritten - lastReadSampleCount;
	if (availableSinceLastRead < wantFloats) {
		out.clear();
		return false;
	}

	const size_t count = wantFloats > capacity ? capacity : wantFloats;
	const size_t start = (ringWriteIndex + capacity - count) % capacity;
	out.resize(count);
	for (size_t i = 0; i < count; ++i) {
		out[i] = ringBuffer[(start + i) % capacity];
	}
	lastReadSampleCount = totalSamplesWritten;
	return true;
}

bool AudioInput::isStreamRunning() const {
#if defined(__LINUX_PULSE__)
	return pulseRunning.load();
#else
	try {
		return rtAudio.isStreamRunning();
	} catch (...) {
		return false;
	}
#endif
}

double AudioInput::getCaptureLatencyMs() const {
	return lastCaptureLatencyMs.load();
}

double AudioInput::getFreshBlockRateHz() const {
	return lastFreshBlockRateHz.load();
}
