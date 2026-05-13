#include "AudioFilePlayer.h"

#include <cstdio>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// Minimal WAV loader for RIFF/WAVE PCM and IEEE float.
struct WavHeader {
	char riff[4];
	uint32_t chunkSize;
	char wave[4];
};

struct WavChunkHeader {
	char id[4];
	uint32_t size;
};

struct WavFormat {
	uint16_t audioFormat = 0; // 1 = PCM, 3 = IEEE float
	uint16_t numChannels;
	uint32_t sampleRate;
	uint32_t byteRate;
	uint16_t blockAlign;
	uint16_t bitsPerSample;
};

static bool readExact(FILE* f, void* dst, size_t size) {
	return std::fread(dst, 1, size, f) == size;
}

static int32_t read24Le(const unsigned char* p) {
	int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
	if (v & 0x00800000) v |= ~0x00ffffff;
	return v;
}

AudioFilePlayer::AudioFilePlayer()
	: sampleRate(0), channels(0), playhead(0), playing(false), loop(true) {}

bool AudioFilePlayer::loadWav(const std::string& path) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;

	WavHeader hdr{};
	if (std::fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { std::fclose(f); return false; }
	if (std::strncmp(hdr.riff, "RIFF", 4) != 0 || std::strncmp(hdr.wave, "WAVE", 4) != 0) { std::fclose(f); return false; }

	WavFormat fmt{};
	std::vector<unsigned char> buf;
	bool haveFmt = false;
	bool haveData = false;
	WavChunkHeader chunk{};
	while (readExact(f, &chunk, sizeof(chunk))) {
		long payloadStart = std::ftell(f);
		if (std::strncmp(chunk.id, "fmt ", 4) == 0) {
			if (chunk.size < 16) { std::fclose(f); return false; }
			if (!readExact(f, &fmt.audioFormat, sizeof(fmt.audioFormat)) ||
				!readExact(f, &fmt.numChannels, sizeof(fmt.numChannels)) ||
				!readExact(f, &fmt.sampleRate, sizeof(fmt.sampleRate)) ||
				!readExact(f, &fmt.byteRate, sizeof(fmt.byteRate)) ||
				!readExact(f, &fmt.blockAlign, sizeof(fmt.blockAlign)) ||
				!readExact(f, &fmt.bitsPerSample, sizeof(fmt.bitsPerSample))) {
				std::fclose(f);
				return false;
			}
			haveFmt = true;
		} else if (std::strncmp(chunk.id, "data", 4) == 0) {
			buf.resize(chunk.size);
			if (chunk.size > 0 && !readExact(f, buf.data(), chunk.size)) { std::fclose(f); return false; }
			haveData = true;
		}
		long next = payloadStart + (long)chunk.size + (long)(chunk.size & 1u);
		if (std::fseek(f, next, SEEK_SET) != 0) break;
		if (haveFmt && haveData) break;
	}
	std::fclose(f);

	if (!haveFmt || !haveData || fmt.numChannels == 0 || fmt.sampleRate == 0) return false;
	if (!((fmt.audioFormat == 1 && (fmt.bitsPerSample == 8 || fmt.bitsPerSample == 16 || fmt.bitsPerSample == 24 || fmt.bitsPerSample == 32)) ||
		  (fmt.audioFormat == 3 && fmt.bitsPerSample == 32))) {
		return false;
	}
	channels = fmt.numChannels;
	sampleRate = fmt.sampleRate;

	samples.clear();
	const size_t bytesPerSample = fmt.bitsPerSample / 8;
	if (bytesPerSample == 0) return false;
	samples.resize(buf.size() / bytesPerSample);
	if (fmt.audioFormat == 1 && fmt.bitsPerSample == 8) {
		for (size_t i = 0; i < samples.size(); ++i) samples[i] = std::clamp(((int)buf[i] - 128) / 128.0f, -1.0f, 1.0f);
	} else if (fmt.audioFormat == 1 && fmt.bitsPerSample == 16) {
		const int16_t* s = reinterpret_cast<const int16_t*>(buf.data());
		for (size_t i = 0; i < samples.size(); ++i) samples[i] = std::clamp(s[i] / 32768.0f, -1.0f, 1.0f);
	} else if (fmt.audioFormat == 1 && fmt.bitsPerSample == 24) {
		for (size_t i = 0; i < samples.size(); ++i) samples[i] = std::clamp(read24Le(buf.data() + i * 3) / 8388608.0f, -1.0f, 1.0f);
	} else if (fmt.audioFormat == 1 && fmt.bitsPerSample == 32) {
		const int32_t* s = reinterpret_cast<const int32_t*>(buf.data());
		for (size_t i = 0; i < samples.size(); ++i) samples[i] = std::clamp(s[i] / 2147483648.0f, -1.0f, 1.0f);
	} else if (fmt.audioFormat == 3 && fmt.bitsPerSample == 32) {
		const float* s = reinterpret_cast<const float*>(buf.data());
		for (size_t i = 0; i < samples.size(); ++i) samples[i] = std::clamp(s[i], -1.0f, 1.0f);
	}
	if (samples.empty()) return false;

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
