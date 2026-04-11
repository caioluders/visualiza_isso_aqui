#pragma once

#include <vector>
#include <mutex>
#include <string>
#include <cstddef>
#include <thread>
#include <atomic>

#include "RtAudio.h"

class AudioInput {
public:
	AudioInput();
	~AudioInput();

	bool startStream(unsigned int sampleRate, unsigned int channels);
	void stopStream();
	void readLatest(size_t maxSamples, std::vector<float>& out);
	unsigned int getActiveChannels() const { return numChannels; }
    unsigned int getActiveSampleRate() const { return activeSampleRate; }
	bool isStreamOpen() const;
	bool isStreamRunning() const;
	size_t getCapturedFrameCount() const;
	float getLastInputRms() const;
	std::string getActiveDeviceName() const;
	unsigned int getActiveDeviceId() const;
	bool startPulseSource(const std::string& sourceName, unsigned int sampleRate, unsigned int channels);

	// Device management
	struct DeviceInfo {
		unsigned int id;
		std::string name;
		unsigned int inputChannels;
		bool isDefault;
	};
	std::vector<DeviceInfo> listInputDevices();
	bool startStreamOnDevice(unsigned int deviceId, unsigned int sampleRate, unsigned int channels);

private:
	static int audioCallback(void* outputBuffer, void* inputBuffer, unsigned int nFrames,
			double streamTime, RtAudioStreamStatus status, void* userData);
	void pushSamples(const float* interleaved, unsigned int frames, unsigned int channels);
	bool openInputStream(unsigned int deviceId, unsigned int sampleRate, unsigned int channels);
	void resetCaptureStats();
	void stopPulseStream();

	RtAudio rtAudio;
#if defined(__LINUX_PULSE__)
	void* pulseHandle;
	std::thread pulseThread;
	std::atomic<bool> pulseRunning;
#endif
	std::vector<float> ringBuffer;
	size_t ringWriteIndex;
	size_t ringValidSamples;
	size_t totalFramesCaptured;
	float lastInputRms;
	mutable std::mutex mutex;
	unsigned int numChannels;
    unsigned int activeSampleRate;
	unsigned int activeDeviceId;
	std::string activeDeviceName;
};
