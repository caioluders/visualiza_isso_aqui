#pragma once

#include <string>
#include <vector>

class AudioFilePlayer {
public:
	AudioFilePlayer();

	bool loadWav(const std::string& path);
	void setLoop(bool enabled);
	void setPlaying(bool enabled);
	bool isPlaying() const;
	unsigned int getSampleRate() const;
	unsigned int getChannels() const;
	const std::string& getPath() const;

	// Fetch next interleaved samples (frames * channels floats). Returns number of frames actually provided.
	size_t getNext(size_t framesRequested, std::vector<float>& outInterleaved);

private:
	std::string filePath;
	std::vector<float> samples; // interleaved floats in [-1,1]
	unsigned int sampleRate;
	unsigned int channels;
	size_t playhead; // index in floats
	bool playing;
	bool loop;
};


