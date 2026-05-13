#pragma once

#include <vector>

struct kiss_fftr_state; // forward decl from kissfft

class FeatureExtractor {
public:
	struct Features {
		float rms = 0.0f;
		float bandLow = 0.0f;
		float bandMid = 0.0f;
		float bandHigh = 0.0f;
		float onset = 0.0f;
	};

	explicit FeatureExtractor(unsigned int sampleRate);
	~FeatureExtractor();
	FeatureExtractor(const FeatureExtractor&) = delete;
	FeatureExtractor& operator=(const FeatureExtractor&) = delete;
	FeatureExtractor(FeatureExtractor&& other) noexcept;
	FeatureExtractor& operator=(FeatureExtractor&& other) noexcept;

	Features compute(const std::vector<float>& interleaved, unsigned int channels = 2);

private:
	unsigned int sampleRate;
	int fftSize;
	kiss_fftr_state* fftr;
	std::vector<float> window;
	std::vector<float> monoBuf;
	std::vector<float> fftIn;
	std::vector<float> mag;
	float prevEnergy;
};

