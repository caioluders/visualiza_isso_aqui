#include "AudioFilePlayer.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

// Minimal WAV loader (PCM 16-bit/stereo/mono)
struct WavHeader {
	char riff[4];
	uint32_t chunkSize;
	char wave[4];
};

struct WavFmt {
	char id[4]; // "fmt "
	uint32_t size; // 16 for PCM
	uint16_t audioFormat; // 1 = PCM
	uint16_t numChannels;
	uint32_t sampleRate;
	uint32_t byteRate;
	uint16_t blockAlign;
	uint16_t bitsPerSample;
};

struct WavDataTag {
	char id[4]; // "data"
	uint32_t size; // bytes
};

AudioFilePlayer::AudioFilePlayer()
	: sampleRate(0), channels(0), playhead(0), playing(false), loop(true) {}

bool AudioFilePlayer::loadWav(const std::string& path) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;

	WavHeader hdr{};
	if (std::fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { std::fclose(f); return false; }
	if (std::strncmp(hdr.riff, "RIFF", 4) != 0 || std::strncmp(hdr.wave, "WAVE", 4) != 0) { std::fclose(f); return false; }

	WavFmt fmt{};
	if (std::fread(&fmt, 1, sizeof(fmt), f) != sizeof(fmt)) { std::fclose(f); return false; }
	if (std::strncmp(fmt.id, "fmt ", 4) != 0) { std::fclose(f); return false; }
	if (fmt.audioFormat != 1 || (fmt.bitsPerSample != 16 && fmt.bitsPerSample != 32)) { std::fclose(f); return false; }
	channels = fmt.numChannels;
	sampleRate = fmt.sampleRate;

	// Skip any extra fmt bytes
	if (fmt.size > 16) std::fseek(f, fmt.size - 16, SEEK_CUR);

	// Find data chunk
	WavDataTag data{};
	while (true) {
		if (std::fread(&data, 1, sizeof(data), f) != sizeof(data)) { std::fclose(f); return false; }
		if (std::strncmp(data.id, "data", 4) == 0) break;
		std::fseek(f, data.size, SEEK_CUR);
	}

	std::vector<unsigned char> buf;
	buf.resize(data.size);
	if (data.size > 0) {
		if (std::fread(buf.data(), 1, data.size, f) != data.size) { std::fclose(f); return false; }
	}
	std::fclose(f);

	samples.clear();
	samples.resize(data.size / (fmt.bitsPerSample / 8));
	if (fmt.bitsPerSample == 16) {
		const int16_t* s = reinterpret_cast<const int16_t*>(buf.data());
		for (size_t i = 0; i < samples.size(); ++i) samples[i] = std::clamp(s[i] / 32768.0f, -1.0f, 1.0f);
	} else if (fmt.bitsPerSample == 32) {
		const float* s = reinterpret_cast<const float*>(buf.data());
		for (size_t i = 0; i < samples.size(); ++i) samples[i] = std::clamp(s[i], -1.0f, 1.0f);
	}

	filePath = path;
	playhead = 0;
	playing = true;
	return true;
}

void AudioFilePlayer::setLoop(bool enabled) { loop = enabled; }
void AudioFilePlayer::setPlaying(bool enabled) { playing = enabled; }
bool AudioFilePlayer::isPlaying() const { return playing; }
unsigned int AudioFilePlayer::getSampleRate() const { return sampleRate; }
unsigned int AudioFilePlayer::getChannels() const { return channels; }
const std::string& AudioFilePlayer::getPath() const { return filePath; }

size_t AudioFilePlayer::getNext(size_t framesRequested, std::vector<float>& outInterleaved) {
	if (!playing || samples.empty() || channels == 0) { outInterleaved.clear(); return 0; }
	const size_t floatsRequested = framesRequested * channels;
	outInterleaved.resize(floatsRequested);
	size_t provided = 0;
	while (provided < floatsRequested) {
		if (playhead >= samples.size()) {
			if (loop) playhead = 0; else { playing = false; break; }
		}
		size_t remaining = floatsRequested - provided;
		size_t available = samples.size() - playhead;
		size_t toCopy = std::min(remaining, available);
		std::memcpy(outInterleaved.data() + provided, samples.data() + playhead, toCopy * sizeof(float));
		provided += toCopy;
		playhead += toCopy;
	}
	return provided / channels; // frames provided
}


