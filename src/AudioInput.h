#pragma once

#include <vector>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

#include "RtAudio.h"

#if defined(__LINUX_PULSE__)
struct pa_simple;
#endif

class AudioInput {
public:
	AudioInput();
	~AudioInput();

	bool startStream(unsigned int sampleRate, unsigned int channels);
	void stopStream();
	void readLatest(size_t maxSamples, std::vector<float>& out);
	bool readLatestIfNew(size_t maxSamples, std::vector<float>& out);
	unsigned int getActiveChannels() const { return numChannels; }
    unsigned int getActiveSampleRate() const { return activeSampleRate; }

	// Device management
	struct DeviceInfo {
		unsigned int id;
		std::string name;
		std::string sourceName;
		unsigned int inputChannels;
		unsigned int outputChannels;
		unsigned int duplexChannels;
		bool isDefault;
	};
	std::vector<DeviceInfo> listInputDevices();
	bool startStreamOnDevice(unsigned int deviceId, unsigned int sampleRate, unsigned int channels);
	bool isStreamRunning() const;
	const std::string& getLastError() const { return lastError; }
	unsigned int getCurrentDeviceId() const { return currentDeviceId; }
	double getCaptureLatencyMs() const;
	double getFreshBlockRateHz() const;

private:
	static int audioCallback(void* outputBuffer, void* inputBuffer, unsigned int nFrames,
			double streamTime, RtAudioStreamStatus status, void* userData);
	void pushSamples(const float* interleaved, unsigned int frames, unsigned int channels);
	void resetBufferState();
	bool openStreamOnDevice(unsigned int deviceId, unsigned int sampleRate, unsigned int channels, bool allowMonoFallback);
#if defined(__LINUX_PULSE__)
	std::vector<DeviceInfo> listPulseSources();
	bool startPulseSource(const DeviceInfo& device, unsigned int sampleRate);
	void stopPulseSource();
	static std::string trim(const std::string& s);
	static std::string prettifyPulseSourceName(const std::string& sourceName);
#endif

	RtAudio rtAudio;
	std::vector<float> ringBuffer;
	size_t ringWriteIndex;
	size_t totalSamplesWritten;
	size_t lastReadSampleCount;
	std::mutex mutex;
	unsigned int numChannels;
	unsigned int activeSampleRate;
	unsigned int currentDeviceId;
	std::atomic<double> lastCaptureLatencyMs;
	std::atomic<double> lastFreshBlockRateHz;
	std::atomic<unsigned long long> pulseBlocksCaptured;
	std::chrono::steady_clock::time_point lastPulseBlockTime;
	std::string lastError;
	std::vector<DeviceInfo> cachedDevices;
#if defined(__LINUX_PULSE__)
	pa_simple* pulseRecord;
	std::thread pulseThread;
	std::atomic<bool> pulseRunning;
#endif
};
