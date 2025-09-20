#pragma once

#include <vector>
#include <mutex>
#include <string>

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

	RtAudio rtAudio;
	std::vector<float> ringBuffer;
	size_t ringWriteIndex;
	std::mutex mutex;
	unsigned int numChannels;
    unsigned int activeSampleRate;
};


