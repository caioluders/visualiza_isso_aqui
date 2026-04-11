#include "FeatureExtractor.h"

#include <cmath>
#include <algorithm>
#include <cstring>

extern "C" {
#include <kiss_fftr.h>
}

FeatureExtractor::FeatureExtractor(unsigned int sr)
	: sampleRate(sr), fftSize(1024), fftr(nullptr), prevEnergy(0.0f) {
	window.resize(fftSize);
	for (int i = 0; i < fftSize; ++i) window[i] = 0.5f * (1.0f - std::cos(2.0f * 3.1415926535f * i / (fftSize - 1)));
	monoBuf.resize(fftSize);
	fftIn.resize(fftSize);
	mag.resize(fftSize / 2 + 1);
	fftr = kiss_fftr_alloc(fftSize, 0, nullptr, nullptr);
}

FeatureExtractor::~FeatureExtractor() {
	if (fftr) free(fftr);
}

FeatureExtractor::Features FeatureExtractor::compute(const std::vector<float>& interleavedStereo) {
	Features f{};
	if (interleavedStereo.empty()) return f;
	// mixdown
    size_t channels = interleavedStereo.empty() ? 2 : (interleavedStereo.size() % 2 == 0 ? 2 : 1);
    // If odd size, assume mono; else prefer stereo. This is a heuristic; upstream code ensures interleaved with active channels.
    const size_t frames = interleavedStereo.size() / channels;
	const size_t use = std::min<size_t>(frames, (size_t)fftSize);
    if (channels == 1) {
        for (size_t i = 0; i < use; ++i) monoBuf[i] = interleavedStereo[i];
    } else {
        for (size_t i = 0; i < use; ++i) {
            float l = interleavedStereo[i * 2 + 0];
            float r = interleavedStereo[i * 2 + 1];
            monoBuf[i] = (l + r) * 0.5f;
        }
    }
	for (size_t i = use; i < (size_t)fftSize; ++i) monoBuf[i] = 0.0f;

	// RMS
	float sumSq = 0.0f;
	for (size_t i = 0; i < use; ++i) sumSq += monoBuf[i] * monoBuf[i];
	f.rms = std::sqrt(sumSq / std::max<size_t>(1, use));

	// windowed FFT
	for (int i = 0; i < fftSize; ++i) fftIn[i] = monoBuf[i] * window[i];
	thread_local std::vector<kiss_fft_cpx> fftOut;
	fftOut.resize(fftSize / 2 + 1);
	kiss_fftr(fftr, fftIn.data(), fftOut.data());
	for (int i = 0; i <= fftSize / 2; ++i) {
		float re = fftOut[i].r;
		float im = fftOut[i].i;
		mag[i] = std::sqrt(re * re + im * im);
	}

	// band energies: 0-200, 200-2000, 2000-20000 Hz bins
	auto hzPerBin = (float)sampleRate / (float)fftSize;
	auto bin = [&](float hz) { return (int)std::clamp((int)std::round(hz / hzPerBin), 0, fftSize / 2); };
	int b0 = 0, b1 = bin(200.0f), b2 = bin(2000.0f), b3 = bin(20000.0f);
	float e0 = 0, e1 = 0, e2 = 0;
	for (int i = b0; i < b1; ++i) e0 += mag[i];
	for (int i = b1; i < b2; ++i) e1 += mag[i];
	for (int i = b2; i < b3; ++i) e2 += mag[i];
	// normalize by bin count
	f.bandLow = e0 / std::max(1, b1 - b0);
	f.bandMid = e1 / std::max(1, b2 - b1);
	f.bandHigh = e2 / std::max(1, b3 - b2);

	// simple onset: energy increase
	float energy = 0.0f;
	for (int i = 0; i <= fftSize / 2; ++i) energy += mag[i];
	float delta = std::max(0.0f, energy - prevEnergy);
	prevEnergy = energy * 0.95f + prevEnergy * 0.05f;
	f.onset = std::min(1.0f, delta / std::max(1.0f, prevEnergy) * 2.0f);

	return f;
}

