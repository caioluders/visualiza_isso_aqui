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
#include <cstdlib>

extern "C" {
#include <kiss_fftr.h>
}

namespace {
static inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
static inline float safeLerp(float a, float b, float t) { return a + (b - a) * clamp01(t); }
}

AudioAnalysis::AudioAnalysis(unsigned int sr, const Config& c)
	: sampleRate(0), cfg(), hzPerBin(0.0f), fftr(nullptr),
	  lowBandEnergyAvg(0.0f), lastBeatTimeSec(-1e9f), beatEnvelope(0.0f), timeSec(0.0f),
	  structureLow(0.0f), structureMid(0.0f), structureHigh(0.0f),
	  prevStructureLow(0.0f), prevStructureMid(0.0f), prevStructureHigh(0.0f),
	  structureEnergyAvg(0.0f), energyDeltaAvg(0.0f), buildUpLevel(0.0f),
	  dropPulse(0.0f), layerChangePulse(0.0f), isolatedHitPulse(0.0f), breakLevel(0.0f) {
	reset(sr, c);
}

void AudioAnalysis::reset(unsigned int sr, const Config& c) {
	if (fftr) {
		free(fftr);
		fftr = nullptr;
	}

	sampleRate = sr;
	cfg = c;
	cfg.fftSize = std::max(64, cfg.fftSize);
	cfg.numBands = std::max(3, cfg.numBands);
	cfg.lowBandHz = std::max(1.0f, cfg.lowBandHz);
	cfg.highBandHz = std::max(cfg.lowBandHz + 1.0f, cfg.highBandHz);
	cfg.onsetHistory = std::max(1, cfg.onsetHistory);
	cfg.hpssTimeMedian = std::max(1, cfg.hpssTimeMedian);
	cfg.hpssFreqMedian = std::max(1, cfg.hpssFreqMedian);
	hzPerBin = float(sampleRate) / float(cfg.fftSize);

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
	bandPrevEnergy.assign(cfg.numBands, 0.0f);
	bandOnsetMean.assign(cfg.numBands, 0.0f);
	bandOnsetVar.assign(cfg.numBands, 0.0f);

	magHistory.clear();
	lowBandEnergyAvg = 0.0f;
	lowBandEnergyHistory.clear();
	lastBeatTimeSec = -1e9f;
	ibiHistorySec.clear();
	beatEnvelope = 0.0f;
	timeSec = 0.0f;
	structureLow = structureMid = structureHigh = 0.0f;
	prevStructureLow = prevStructureMid = prevStructureHigh = 0.0f;
	structureEnergyAvg = 0.0f;
	energyDeltaAvg = 0.0f;
	buildUpLevel = 0.0f;
	dropPulse = 0.0f;
	layerChangePulse = 0.0f;
	isolatedHitPulse = 0.0f;
	breakLevel = 0.0f;
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

void AudioAnalysis::computeFftAndMag(const std::vector<float>& interleaved, unsigned int channels) {
	// Mixdown to mono
	channels = std::max(1u, channels);
	const size_t frames = interleaved.size() / channels;
	const size_t use = std::min<size_t>(frames, (size_t)cfg.fftSize);
	for (size_t i = 0; i < use; ++i) {
		float sum = 0.0f;
		for (unsigned int c = 0; c < channels; ++c) {
			sum += interleaved[i * channels + c];
		}
		mono[i] = sum / float(channels);
	}
	for (size_t i = use; i < (size_t)cfg.fftSize; ++i) mono[i] = 0.0f;

	// Window and FFT
	for (int i = 0; i < cfg.fftSize; ++i) fftIn[i] = mono[i] * window[i];
	thread_local std::vector<kiss_fft_cpx> fftOut;
	fftOut.resize(cfg.fftSize / 2 + 1);
	kiss_fftr(fftr, fftIn.data(), fftOut.data());
	for (int i = 0; i <= cfg.fftSize / 2; ++i) {
		float re = fftOut[i].r;
		float im = fftOut[i].i;
		mag[i] = std::sqrt(re * re + im * im);
	}
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

void AudioAnalysis::computeOnsets(AnalysisFrame& out) {
	out.bandOnset.assign(cfg.numBands, 0.0f);
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
	if ((int)lowBandEnergyHistory.size() > cfg.onsetHistory) lowBandEnergyHistory.pop_front();
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
	out.beatEnvelope = cubicEaseOut(beatEnvelope);

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

	thread_local std::vector<float> timeMed;
	thread_local std::vector<float> freqMed;
	timeMed.assign(mag.size(), 0.0f);
	freqMed.assign(mag.size(), 0.0f);

	// Time median per bin
	{
		const int Wt = std::min<int>((int)magHistory.size(), cfg.hpssTimeMedian);
		thread_local std::vector<float> scratch;
		scratch.reserve(Wt);
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
		thread_local std::vector<float> scratch;
		scratch.reserve(Wf*2+1);
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

void AudioAnalysis::computeStructure(AnalysisFrame& out, float dtSec) {
	float rawLow = 0.0f, rawMid = 0.0f, rawHigh = 0.0f;
	int lowCount = 0, midCount = 0, highCount = 0;
	for (int b = 0; b < cfg.numBands && b < (int)out.bandEnergyNorm.size(); ++b) {
		float centerHz = 0.5f * (bandEdgesHz[b] + bandEdgesHz[b + 1]);
		float v = out.bandEnergyNorm[b];
		if (centerHz < 250.0f) {
			rawLow += v;
			++lowCount;
		} else if (centerHz < 4000.0f) {
			rawMid += v;
			++midCount;
		} else {
			rawHigh += v;
			++highCount;
		}
	}
	rawLow = lowCount > 0 ? rawLow / float(lowCount) : 0.0f;
	rawMid = midCount > 0 ? rawMid / float(midCount) : 0.0f;
	rawHigh = highCount > 0 ? rawHigh / float(highCount) : 0.0f;

	float alpha = 1.0f - std::exp(-dtSec / std::max(1e-3f, cfg.structureSmoothingSec));
	structureLow = safeLerp(structureLow, rawLow, alpha);
	structureMid = safeLerp(structureMid, rawMid, alpha);
	structureHigh = safeLerp(structureHigh, rawHigh, alpha);

	float dLow = structureLow - prevStructureLow;
	float dMid = structureMid - prevStructureMid;
	float dHigh = structureHigh - prevStructureHigh;
	float rawDelta = std::sqrt((dLow * dLow + dMid * dMid + dHigh * dHigh) / 3.0f);
	energyDeltaAvg = safeLerp(energyDeltaAvg, rawDelta, 0.04f);
	float normalizedDelta = rawDelta / std::max(0.03f, energyDeltaAvg * 4.0f);

	float energyNow = (structureLow + structureMid + structureHigh) / 3.0f;
	float previousEnergy = (prevStructureLow + prevStructureMid + prevStructureHigh) / 3.0f;
	structureEnergyAvg = safeLerp(structureEnergyAvg, energyNow, 0.01f);

	float onsetMax = 0.0f;
	float onsetSum = 0.0f;
	for (float v : out.bandOnset) {
		onsetMax = std::max(onsetMax, v);
		onsetSum += v;
	}
	float onsetAvg = out.bandOnset.empty() ? 0.0f : onsetSum / float(out.bandOnset.size());

	float oldBreakLevel = breakLevel;
	float breakTarget = energyNow < cfg.breakEnergyThreshold ? 1.0f : 0.0f;
	breakLevel = safeLerp(breakLevel, breakTarget, alpha);

	float pulseStep = dtSec / std::max(1e-3f, cfg.structurePulseDecaySec);
	dropPulse = std::max(0.0f, dropPulse - pulseStep);
	layerChangePulse = std::max(0.0f, layerChangePulse - pulseStep);
	isolatedHitPulse = std::max(0.0f, isolatedHitPulse - pulseStep);

	bool energeticReturn = oldBreakLevel > 0.45f && energyNow > cfg.dropEnergyThreshold;
	bool transientReturn = onsetMax > 0.35f || out.beatTriggered;
	if (energeticReturn && transientReturn) dropPulse = 1.0f;

	bool layerMoved = rawDelta > std::max(cfg.layerChangeThreshold, energyDeltaAvg * 4.0f);
	if (layerMoved && !energeticReturn) layerChangePulse = 1.0f;

	bool sparseHit = onsetMax > 0.60f && energyNow < std::max(0.55f, structureEnergyAvg * 1.2f) && onsetAvg < onsetMax * 0.45f;
	if (sparseHit) isolatedHitPulse = 1.0f;

	float rise = energyNow - previousEnergy;
	if (rise > cfg.buildUpRiseThreshold && energyNow > cfg.breakEnergyThreshold && onsetAvg > 0.03f) {
		buildUpLevel = std::min(1.0f, buildUpLevel + dtSec * 1.6f);
	} else {
		buildUpLevel = std::max(0.0f, buildUpLevel - dtSec * 1.0f);
	}

	out.energyLow = clamp01(structureLow);
	out.energyMid = clamp01(structureMid);
	out.energyHigh = clamp01(structureHigh);
	out.onsetStrength = clamp01(onsetMax);
	out.energyDelta = clamp01(normalizedDelta);
	out.dropTrigger = dropPulse;
	out.breakState = clamp01(breakLevel);
	out.buildUp = buildUpLevel;
	out.layerChange = layerChangePulse;
	out.isolatedHit = isolatedHitPulse;

	prevStructureLow = structureLow;
	prevStructureMid = structureMid;
	prevStructureHigh = structureHigh;
}

AudioAnalysis::AnalysisFrame AudioAnalysis::processInterleaved(const std::vector<float>& interleaved, unsigned int channels) {
	AnalysisFrame out;
	if (interleaved.empty()) return out;

	float dtSec = float(cfg.fftSize) / float(sampleRate);
	timeSec += dtSec;

	computeFftAndMag(interleaved, channels);
	computeBroadband(out);
	computeBands(out);
	computeOnsets(out);
	computeBeat(out, dtSec);
	computeHPSS(out);
	computeStructure(out, dtSec);
	return out;
}

AudioAnalysis::AnalysisFrame AudioAnalysis::processInterleavedStereo(const std::vector<float>& interleavedStereo) {
	return processInterleaved(interleavedStereo, 2);
}
