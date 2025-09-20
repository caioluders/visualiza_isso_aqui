#include "AudioAnalysis.h"

/*
Implementation overview
  See AudioAnalysis.h for a detailed description of each feature.
  The code below is written verbosely, favoring clarity and explicit steps.
*/

#include <cmath>
#include <algorithm>
#include <numeric>
#include <cstring>

extern "C" {
#include <kiss_fftr.h>
}

namespace {
static inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
}

AudioAnalysis::AudioAnalysis(unsigned int sr, const Config& c)
	: sampleRate(sr), cfg(c), hzPerBin(float(sr) / float(c.fftSize)), fftr(nullptr),
	  lowBandEnergyAvg(0.0f), lastBeatTimeSec(-1e9f), beatEnvelope(0.0f), timeSec(0.0f) {
	// Allocate windows and buffers
	window.resize(cfg.fftSize);
	for (int i = 0; i < cfg.fftSize; ++i) window[i] = 0.5f * (1.0f - std::cos(2.0f * 3.1415926535f * i / (cfg.fftSize - 1)));
	mono.assign(cfg.fftSize, 0.0f);
	fftIn.assign(cfg.fftSize, 0.0f);
	mag.assign(cfg.fftSize / 2 + 1, 0.0f);
	prevMag.assign(cfg.fftSize / 2 + 1, 0.0f);

	fftr = kiss_fftr_alloc(cfg.fftSize, 0, nullptr, nullptr);

	initializeBands();
	bandEnergyHistoryAvg.assign(cfg.numBands, 1e-6f);
	bandOnsetHistory.assign(cfg.numBands * std::max(1, cfg.onsetHistory), 0.0f);
	bandPrevEnergy.assign(cfg.numBands, 0.0f);
	bandOnsetMean.assign(cfg.numBands, 0.0f);
	bandOnsetVar.assign(cfg.numBands, 0.0f);
}

AudioAnalysis::AudioAnalysis(unsigned int sr)
	: AudioAnalysis(sr, Config()) {}

AudioAnalysis::~AudioAnalysis() {
	if (fftr) free(fftr);
}

void AudioAnalysis::initializeBands() {
	// Log-spaced bands between lowBandHz and highBandHz (inclusive of edges)
	bandEdgesHz.resize(cfg.numBands + 1);
	float logLow = std::log10(std::max(1.0f, cfg.lowBandHz));
	float logHigh = std::log10(std::max(cfg.lowBandHz + 1.0f, cfg.highBandHz));
	for (int i = 0; i <= cfg.numBands; ++i) {
		float t = float(i) / float(cfg.numBands);
		bandEdgesHz[i] = std::pow(10.0f, logLow + t * (logHigh - logLow));
	}
	bandEdgeBins.resize(cfg.numBands + 1);
	for (int i = 0; i <= cfg.numBands; ++i) {
		// Map edges to bins; enforce monotonic increase to avoid empty bands
		int bin = int(std::round(bandEdgesHz[i] / hzPerBin));
		bin = std::max(0, std::min(bin, cfg.fftSize / 2));
		if (i > 0) {
			bin = std::max(bin, bandEdgeBins[i-1] + 1); // ensure at least one bin per band
			bin = std::min(bin, cfg.fftSize / 2);       // clamp to nyquist
		}
		bandEdgeBins[i] = bin;
	}
	// Ensure last edge is exactly nyquist bin
	bandEdgeBins.back() = std::max(bandEdgeBins.back(), cfg.fftSize / 2);
}

void AudioAnalysis::computeFftAndMag(const std::vector<float>& interleavedStereo) {
	// Mixdown to mono
	const size_t channels = 2;
	const size_t frames = interleavedStereo.size() / channels;
	const size_t use = std::min<size_t>(frames, (size_t)cfg.fftSize);
	for (size_t i = 0; i < use; ++i) {
		float l = interleavedStereo[i * 2 + 0];
		float r = interleavedStereo[i * 2 + 1];
		mono[i] = 0.5f * (l + r);
	}
	for (size_t i = use; i < (size_t)cfg.fftSize; ++i) mono[i] = 0.0f;

	// Window and FFT
	for (int i = 0; i < cfg.fftSize; ++i) fftIn[i] = mono[i] * window[i];
	kiss_fft_cpx* out = (kiss_fft_cpx*)malloc(sizeof(kiss_fft_cpx) * (cfg.fftSize / 2 + 1));
	kiss_fftr(fftr, fftIn.data(), out);
	for (int i = 0; i <= cfg.fftSize / 2; ++i) {
		float re = out[i].r;
		float im = out[i].i;
		mag[i] = std::sqrt(re * re + im * im);
	}
	free(out);
}

void AudioAnalysis::computeBroadband(AnalysisFrame& out) {
	// RMS over current mono buffer
	float sumSq = 0.0f;
	for (int i = 0; i < cfg.fftSize; ++i) sumSq += mono[i] * mono[i];
	out.rms = std::sqrt(sumSq / float(cfg.fftSize));

	// Spectral centroid
	float num = 0.0f, den = 0.0f;
	for (int i = 0; i <= cfg.fftSize / 2; ++i) {
		float f = i * hzPerBin;
		num += f * mag[i];
		den += mag[i];
	}
	out.spectralCentroidHz = (den > 1e-6f) ? (num / den) : 0.0f;

	// Spectral flux (positive changes only)
	float flux = 0.0f;
	for (int i = 0; i <= cfg.fftSize / 2; ++i) {
		float d = mag[i] - prevMag[i];
		if (d > 0.0f) flux += d;
		prevMag[i] = mag[i];
	}
	out.spectralFlux = flux;
}

void AudioAnalysis::computeBands(AnalysisFrame& out) {
	out.bandEnergy.assign(cfg.numBands, 0.0f);
	for (int b = 0; b < cfg.numBands; ++b) {
		int a = bandEdgeBins[b];
		int z = bandEdgeBins[b+1];
		if (z <= a) z = a + 1; // safety: guarantee at least one bin
		float e = 0.0f;
		for (int i = a; i < z; ++i) e += mag[i];
		out.bandEnergy[b] = e / float(std::max(1, z - a));
	}

	// Short-term normalization (AGC) per band using EMA
	out.bandEnergyNorm = out.bandEnergy;
	for (int b = 0; b < cfg.numBands; ++b) {
		float ema = bandEnergyHistoryAvg[b] = 0.98f * bandEnergyHistoryAvg[b] + 0.02f * (out.bandEnergy[b] + 1e-6f);
		out.bandEnergyNorm[b] = out.bandEnergy[b] / ema;
		out.bandEnergyNorm[b] = clamp01(out.bandEnergyNorm[b]);
	}
}

void AudioAnalysis::computeOnsets(AnalysisFrame& out, float dtSec) {
	out.bandOnset.assign(cfg.numBands, 0.0f);
	const int H = std::max(1, cfg.onsetHistory);
	// Use energy increase per band as onset function
	for (int b = 0; b < cfg.numBands; ++b) {
		float e = out.bandEnergy[b];
		float prev = bandPrevEnergy[b];
		float inc = std::max(0.0f, e - prev);
		bandPrevEnergy[b] = e;

		// Update per-band EMA mean/variance of inc
		float mean = bandOnsetMean[b] = 0.97f * bandOnsetMean[b] + 0.03f * inc;
		float diff = inc - mean;
		float var = bandOnsetVar[b] = 0.97f * bandOnsetVar[b] + 0.03f * (diff * diff);
		float stdv = std::sqrt(std::max(0.0f, var));
		float threshold = cfg.onsetThresholdBase * (mean + cfg.onsetVarianceWeight * stdv + 1e-6f);
		float strength = 0.0f;
		if (inc > threshold) strength = clamp01((inc - threshold) / (threshold + 1e-6f));
		out.bandOnset[b] = strength;
	}
}

static float cubicEaseOut(float t) {
	float u = 1.0f - t; return 1.0f - u*u*u;
}

void AudioAnalysis::computeBeat(AnalysisFrame& out, float dtSec) {
	// Use lowest few bands as kick proxy
	int lowEnd = std::max(1, cfg.numBands / 6);
	float lowSum = 0.0f;
	for (int b = 0; b < lowEnd; ++b) lowSum += out.bandEnergyNorm[b];
	float lowAvg = lowSum / float(lowEnd);

	// Maintain history for adaptive threshold
	lowBandEnergyHistory.push_back(lowAvg);
	if ((int)lowBandEnergyHistory.size() > cfg.onsetHistory) lowBandEnergyHistory.pop_back();
	float mean = 0.0f, var = 0.0f;
	for (float v : lowBandEnergyHistory) mean += v;
	if (!lowBandEnergyHistory.empty()) mean /= float(lowBandEnergyHistory.size());
	for (float v : lowBandEnergyHistory) { float d = v - mean; var += d * d; }
	if (!lowBandEnergyHistory.empty()) var /= float(lowBandEnergyHistory.size());
	float stdv = std::sqrt(std::max(0.0f, var));
	float threshold = cfg.onsetThresholdBase * (mean + cfg.onsetVarianceWeight * stdv + 1e-6f);

	// Refractory handling
	bool canTrigger = (timeSec - lastBeatTimeSec) >= cfg.beatMinIntervalSec;
	bool trigger = canTrigger && (lowAvg > threshold);
	out.beatTriggered = trigger;
	if (trigger) {
		float ibi = (lastBeatTimeSec > 0.0f) ? (timeSec - lastBeatTimeSec) : 0.0f;
		lastBeatTimeSec = timeSec;
		beatEnvelope = 1.0f; // start envelope
		if (ibi > 0.0f) {
			ibiHistorySec.push_back(ibi);
			if (ibiHistorySec.size() > 16) ibiHistorySec.pop_front();
		}
	}

	// Envelope decay
	if (beatEnvelope > 0.0f) {
		float step = dtSec / std::max(1e-3f, cfg.beatEnvelopeHoldSec);
		beatEnvelope = std::max(0.0f, beatEnvelope - step);
	}
	out.beatEnvelope = cubicEaseOut(1.0f - beatEnvelope);

	// BPM estimate: median IBI over recent beats
	if (ibiHistorySec.size() >= 5) {
		std::vector<float> scratch(ibiHistorySec.begin(), ibiHistorySec.end());
		std::sort(scratch.begin(), scratch.end());
		float medianIbi = scratch[scratch.size()/2];
		out.bpm = (medianIbi > 1e-3f) ? 60.0f / medianIbi : 0.0f;
	} else {
		out.bpm = 0.0f;
	}
}

void AudioAnalysis::computeHPSS(AnalysisFrame& out) {
	// Approximate median filters for percussive (time) and harmonic (freq)
	magHistory.push_front(mag);
	if ((int)magHistory.size() > cfg.hpssTimeMedian) magHistory.pop_back();

	std::vector<float> timeMed(mag.size(), 0.0f);
	std::vector<float> freqMed(mag.size(), 0.0f);

	// Time median per bin
	{
		const int Wt = std::min<int>((int)magHistory.size(), cfg.hpssTimeMedian);
		std::vector<float> scratch; scratch.reserve(Wt);
		for (size_t i = 0; i < mag.size(); ++i) {
			scratch.clear();
			for (int t = 0; t < Wt; ++t) scratch.push_back(magHistory[t][i]);
			std::nth_element(scratch.begin(), scratch.begin() + scratch.size()/2, scratch.end());
			timeMed[i] = scratch[scratch.size()/2];
		}
	}
	// Frequency median in a neighborhood per frame
	{
		const int Wf = std::max(1, cfg.hpssFreqMedian);
		std::vector<float> scratch; scratch.reserve(Wf*2+1);
		for (size_t i = 0; i < mag.size(); ++i) {
			int a = std::max<int>(0, (int)i - Wf);
			int z = std::min<int>((int)mag.size()-1, (int)i + Wf);
			scratch.clear();
			for (int k = a; k <= z; ++k) scratch.push_back(mag[k]);
			std::nth_element(scratch.begin(), scratch.begin() + scratch.size()/2, scratch.end());
			freqMed[i] = scratch[scratch.size()/2];
		}
	}

	float perE = 0.0f, harE = 0.0f;
	for (size_t i = 0; i < mag.size(); ++i) {
		float p = std::max(0.0f, mag[i] - freqMed[i]);
		float h = std::max(0.0f, mag[i] - timeMed[i]);
		perE += p;
		harE += h;
	}
	out.percussiveEnergy = perE;
	out.harmonicEnergy = harE;
	out.percussiveRatio = (perE + harE) > 1e-6f ? perE / (perE + harE) : 0.0f;
}

AudioAnalysis::AnalysisFrame AudioAnalysis::processInterleavedStereo(const std::vector<float>& interleavedStereo) {
	AnalysisFrame out;
	if (interleavedStereo.empty()) return out;

	float dtSec = float(cfg.fftSize) / float(sampleRate);
	timeSec += dtSec;

	computeFftAndMag(interleavedStereo);
	computeBroadband(out);
	computeBands(out);
	computeOnsets(out, dtSec);
	computeBeat(out, dtSec);
	computeHPSS(out);
	return out;
}


