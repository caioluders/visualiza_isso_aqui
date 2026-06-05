#include "AudioAnalysis.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <utility>

extern "C" {
#include <kiss_fftr.h>
}

namespace {

static inline float clamp01(float x) {
	return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static inline float safeDiv(float num, float den) {
	return den > 1e-6f ? num / den : 0.0f;
}

static inline float smoothTowards(float cur, float target, float alpha) {
	return cur + (target - cur) * clamp01(alpha);
}

static inline float easeOutCubic(float t) {
	float u = 1.0f - clamp01(t);
	return 1.0f - u * u * u;
}

static inline float softCompress(float x, float knee) {
	if (x <= 0.0f) return 0.0f;
	const float k = std::max(1e-6f, knee);
	return x / (x + k);
}

static inline float perceptualLift(float x, float gamma = 0.55f) {
	return std::pow(clamp01(x), gamma);
}

static inline float normalizeAdaptive(float value, float& floorState, float& peakState, float floorRiseAlpha, float floorFallAlpha, float peakRiseAlpha, float peakFallAlpha, float minSpan) {
	if (value < floorState) floorState = smoothTowards(floorState, value, std::max(floorFallAlpha, 0.35f));
	else floorState = smoothTowards(floorState, value, floorRiseAlpha);
	if (value > peakState) peakState = smoothTowards(peakState, value, std::max(peakRiseAlpha, 0.55f));
	else peakState = smoothTowards(peakState, value, peakFallAlpha);
	peakState = std::max(peakState, floorState + minSpan);
	return clamp01(safeDiv(value - floorState, peakState - floorState));
}

static inline float fastAlphaFromTau(float dtSec, float tauSec) {
	return clamp01(dtSec / std::max(1e-4f, tauSec));
}

static inline float followEnvelope(float cur, float target, float dtSec, float attackTauSec, float releaseTauSec) {
	const float tau = target > cur ? attackTauSec : releaseTauSec;
	return smoothTowards(cur, target, fastAlphaFromTau(dtSec, tau));
}

static float peakProminence(const std::vector<float>& values, int i) {
	const int a = std::max(0, i - 1);
	const int z = std::min((int)values.size() - 1, i + 1);
	float neighborhood = 0.0f;
	int count = 0;
	for (int k = a; k <= z; ++k) {
		if (k == i) continue;
		neighborhood += values[k];
		++count;
	}
	const float base = count > 0 ? neighborhood / float(count) : 0.0f;
	return std::max(0.0f, values[i] - base);
}

} // namespace

AudioAnalysis::AudioAnalysis(unsigned int sr, const Config& c)
	: sampleRate(sr),
	  cfg(c),
	  hzPerBin(float(sr) / float(c.fftSize)),
	  fftr(nullptr),
	  hpssBlockCounter(0),
	  lastPercussiveEnergy(0.0f),
	  lastHarmonicEnergy(0.0f),
	  lastPercussiveRatio(0.0f),
	  lowBandEnergyAvg(0.0f),
	  lastBeatTimeSec(-1e9f),
	  beatEnvelope(0.0f),
	  timeSec(0.0f),
	  macroState(MacroState::Neutral) {
	window.resize(cfg.fftSize);
	for (int i = 0; i < cfg.fftSize; ++i) {
		window[i] = 0.5f * (1.0f - std::cos(2.0f * 3.1415926535f * i / (cfg.fftSize - 1)));
	}
	mono.assign(cfg.fftSize, 0.0f);
	fftIn.assign(cfg.fftSize, 0.0f);
	mag.assign(cfg.fftSize / 2 + 1, 0.0f);
	prevMag.assign(cfg.fftSize / 2 + 1, 0.0f);
	fftOut.resize(cfg.fftSize / 2 + 1);
	hpssTimeMedianBuffer.assign(cfg.fftSize / 2 + 1, 0.0f);
	hpssFreqMedianBuffer.assign(cfg.fftSize / 2 + 1, 0.0f);
	hpssScratchBuffer.reserve(std::max(cfg.hpssTimeMedian, cfg.hpssFreqMedian * 2 + 1));
	fftr = kiss_fftr_alloc(cfg.fftSize, 0, nullptr, nullptr);

	initializeBands();
	initializeRoleBands();
	bandEnergyHistoryAvg.assign(cfg.numBands, 1e-6f);
	bandOnsetHistory.assign(cfg.numBands * std::max(1, cfg.onsetHistory), 0.0f);
	bandPrevEnergy.assign(cfg.numBands, 0.0f);
	bandOnsetMean.assign(cfg.numBands, 0.0f);
	bandOnsetVar.assign(cfg.numBands, 0.0f);
	roleState.previousSemanticVector.assign(8, 0.0f);
}

AudioAnalysis::AudioAnalysis(unsigned int sr)
	: AudioAnalysis(sr, Config()) {}

AudioAnalysis::~AudioAnalysis() {
	if (fftr) free(fftr);
}

AudioAnalysis::AudioAnalysis(AudioAnalysis&& other) noexcept
	: sampleRate(other.sampleRate),
	  cfg(other.cfg),
	  hzPerBin(other.hzPerBin),
	  fftr(other.fftr),
	  window(std::move(other.window)),
	  mono(std::move(other.mono)),
	  fftIn(std::move(other.fftIn)),
	  mag(std::move(other.mag)),
	  prevMag(std::move(other.prevMag)),
	  fftOut(std::move(other.fftOut)),
	  magHistory(std::move(other.magHistory)),
	  hpssTimeMedianBuffer(std::move(other.hpssTimeMedianBuffer)),
	  hpssFreqMedianBuffer(std::move(other.hpssFreqMedianBuffer)),
	  hpssScratchBuffer(std::move(other.hpssScratchBuffer)),
	  hpssBlockCounter(other.hpssBlockCounter),
	  lastPercussiveEnergy(other.lastPercussiveEnergy),
	  lastHarmonicEnergy(other.lastHarmonicEnergy),
	  lastPercussiveRatio(other.lastPercussiveRatio),
	  bandEdgesHz(std::move(other.bandEdgesHz)),
	  bandEdgeBins(std::move(other.bandEdgeBins)),
	  bandEnergyHistoryAvg(std::move(other.bandEnergyHistoryAvg)),
	  bandOnsetHistory(std::move(other.bandOnsetHistory)),
	  bandPrevEnergy(std::move(other.bandPrevEnergy)),
	  bandOnsetMean(std::move(other.bandOnsetMean)),
	  bandOnsetVar(std::move(other.bandOnsetVar)),
	  lowBandEnergyAvg(other.lowBandEnergyAvg),
	  lowBandEnergyHistory(std::move(other.lowBandEnergyHistory)),
	  lastBeatTimeSec(other.lastBeatTimeSec),
	  ibiHistorySec(std::move(other.ibiHistorySec)),
	  beatEnvelope(other.beatEnvelope),
	  timeSec(other.timeSec),
	  roleState(std::move(other.roleState)),
	  macroState(other.macroState) {
	other.fftr = nullptr;
}

AudioAnalysis& AudioAnalysis::operator=(AudioAnalysis&& other) noexcept {
	if (this != &other) {
		if (fftr) free(fftr);
		sampleRate = other.sampleRate;
		cfg = other.cfg;
		hzPerBin = other.hzPerBin;
		fftr = other.fftr;
		window = std::move(other.window);
		mono = std::move(other.mono);
		fftIn = std::move(other.fftIn);
		mag = std::move(other.mag);
		prevMag = std::move(other.prevMag);
		fftOut = std::move(other.fftOut);
		magHistory = std::move(other.magHistory);
		hpssTimeMedianBuffer = std::move(other.hpssTimeMedianBuffer);
		hpssFreqMedianBuffer = std::move(other.hpssFreqMedianBuffer);
		hpssScratchBuffer = std::move(other.hpssScratchBuffer);
		hpssBlockCounter = other.hpssBlockCounter;
		lastPercussiveEnergy = other.lastPercussiveEnergy;
		lastHarmonicEnergy = other.lastHarmonicEnergy;
		lastPercussiveRatio = other.lastPercussiveRatio;
		bandEdgesHz = std::move(other.bandEdgesHz);
		bandEdgeBins = std::move(other.bandEdgeBins);
		bandEnergyHistoryAvg = std::move(other.bandEnergyHistoryAvg);
		bandOnsetHistory = std::move(other.bandOnsetHistory);
		bandPrevEnergy = std::move(other.bandPrevEnergy);
		bandOnsetMean = std::move(other.bandOnsetMean);
		bandOnsetVar = std::move(other.bandOnsetVar);
		lowBandEnergyAvg = other.lowBandEnergyAvg;
		lowBandEnergyHistory = std::move(other.lowBandEnergyHistory);
		lastBeatTimeSec = other.lastBeatTimeSec;
		ibiHistorySec = std::move(other.ibiHistorySec);
		beatEnvelope = other.beatEnvelope;
		timeSec = other.timeSec;
		roleState = std::move(other.roleState);
		macroState = other.macroState;
		other.fftr = nullptr;
	}
	return *this;
}

void AudioAnalysis::initializeLogBands(int numBands, float lowHz, float highHz, std::vector<float>& edgesHz, std::vector<int>& edgeBins) {
	edgesHz.resize(numBands + 1);
	const float logLow = std::log10(std::max(1.0f, lowHz));
	const float logHigh = std::log10(std::max(lowHz + 1.0f, highHz));
	for (int i = 0; i <= numBands; ++i) {
		const float t = float(i) / float(numBands);
		edgesHz[i] = std::pow(10.0f, logLow + t * (logHigh - logLow));
	}
	edgeBins.resize(numBands + 1);
	for (int i = 0; i <= numBands; ++i) {
		int bin = int(std::round(edgesHz[i] / hzPerBin));
		bin = std::max(0, std::min(bin, cfg.fftSize / 2));
		if (i > 0) {
			bin = std::max(bin, edgeBins[i - 1] + 1);
			bin = std::min(bin, cfg.fftSize / 2);
		}
		edgeBins[i] = bin;
	}
	edgeBins.back() = std::max(edgeBins.back(), cfg.fftSize / 2);
}

void AudioAnalysis::initializeBands() {
	initializeLogBands(cfg.numBands, cfg.lowBandHz, cfg.highBandHz, bandEdgesHz, bandEdgeBins);
}

void AudioAnalysis::initializeRoleBands() {
	const int roleBandCount = std::max(cfg.numBands, cfg.roleBands);
	initializeLogBands(roleBandCount, cfg.lowBandHz, cfg.highBandHz, roleState.bandEdgesHz, roleState.bandEdgeBins);
	roleState.energy.assign(roleBandCount, 0.0f);
	roleState.prevEnergy.assign(roleBandCount, 0.0f);
	roleState.flux.assign(roleBandCount, 0.0f);
	roleState.fastEnergy.assign(roleBandCount, 0.0f);
	roleState.slowEnergy.assign(roleBandCount, 1e-4f);
	roleState.fastFlux.assign(roleBandCount, 0.0f);
	roleState.transientEma.assign(roleBandCount, 0.0f);
	roleState.occupancyFast.assign(roleBandCount, 0.0f);
	roleState.occupancySlow.assign(roleBandCount, 0.0f);
	roleState.prominence.assign(roleBandCount, 0.0f);
	roleState.harmonicity.assign(roleBandCount, 0.5f);
	roleState.percussiveWeight.assign(roleBandCount, 0.5f);
	roleState.longSpectralAvg.assign(roleBandCount, 1e-4f);
	roleState.transientConcentration.assign(roleBandCount, 0.0f);

	const float nyq = float(sampleRate) * 0.5f;
	roleState.lowImpactAnchor = 85.0f / nyq;
	roleState.lowSustainAnchor = 120.0f / nyq;
	roleState.midBodyAnchor = 1400.0f / nyq;
	roleState.leadAnchor = 2800.0f / nyq;
	roleState.airAnchor = 9000.0f / nyq;
	roleState.tickAnchor = 10500.0f / nyq;

	roleState.lowImpactWidth = 55.0f / nyq;
	roleState.lowSustainWidth = 120.0f / nyq;
	roleState.midBodyWidth = 900.0f / nyq;
	roleState.leadWidth = 1400.0f / nyq;
	roleState.airWidth = 4200.0f / nyq;
	roleState.tickWidth = 2600.0f / nyq;
}

void AudioAnalysis::computeFftAndMag(const std::vector<float>& interleaved, unsigned int channels) {
	const size_t ch = std::max(1u, channels);
	const size_t frames = interleaved.size() / ch;
	const size_t use = std::min<size_t>(frames, (size_t)cfg.fftSize);
	for (size_t i = 0; i < use; ++i) {
		float sum = 0.0f;
		for (size_t c = 0; c < ch; ++c) sum += interleaved[i * ch + c];
		mono[i] = sum / float(ch);
	}
	for (size_t i = use; i < (size_t)cfg.fftSize; ++i) mono[i] = 0.0f;

	for (int i = 0; i < cfg.fftSize; ++i) fftIn[i] = mono[i] * window[i];
	kiss_fftr(fftr, fftIn.data(), fftOut.data());
	for (int i = 0; i <= cfg.fftSize / 2; ++i) {
		const float re = fftOut[i].r;
		const float im = fftOut[i].i;
		mag[i] = std::sqrt(re * re + im * im);
	}
}

void AudioAnalysis::computeBroadband(AnalysisFrame& out) {
	float sumSq = 0.0f;
	for (int i = 0; i < cfg.fftSize; ++i) sumSq += mono[i] * mono[i];
	out.rms = std::sqrt(sumSq / float(cfg.fftSize));

	float num = 0.0f;
	float den = 0.0f;
	float flux = 0.0f;
	for (int i = 0; i <= cfg.fftSize / 2; ++i) {
		const float f = i * hzPerBin;
		num += f * mag[i];
		den += mag[i];
		const float d = mag[i] - prevMag[i];
		if (d > 0.0f) flux += d;
		prevMag[i] = mag[i];
	}
	out.spectralCentroidHz = den > 1e-6f ? num / den : 0.0f;
	out.spectralFlux = flux;
}

void AudioAnalysis::computeBands(AnalysisFrame& out) {
	out.bandEnergy.assign(cfg.numBands, 0.0f);
	for (int b = 0; b < cfg.numBands; ++b) {
		int a = bandEdgeBins[b];
		int z = bandEdgeBins[b + 1];
		if (z <= a) z = a + 1;
		float e = 0.0f;
		for (int i = a; i < z; ++i) e += mag[i];
		out.bandEnergy[b] = e / float(std::max(1, z - a));
	}

	out.bandEnergyNorm = out.bandEnergy;
	for (int b = 0; b < cfg.numBands; ++b) {
		const float ema = bandEnergyHistoryAvg[b] = 0.98f * bandEnergyHistoryAvg[b] + 0.02f * (out.bandEnergy[b] + 1e-6f);
		out.bandEnergyNorm[b] = clamp01(out.bandEnergy[b] / ema);
	}
}

void AudioAnalysis::computeOnsets(AnalysisFrame& out, float /*dtSec*/) {
	out.bandOnset.assign(cfg.numBands, 0.0f);
	for (int b = 0; b < cfg.numBands; ++b) {
		const float e = out.bandEnergy[b];
		const float prev = bandPrevEnergy[b];
		const float inc = std::max(0.0f, e - prev);
		bandPrevEnergy[b] = e;

		const float mean = bandOnsetMean[b] = 0.97f * bandOnsetMean[b] + 0.03f * inc;
		const float diff = inc - mean;
		const float var = bandOnsetVar[b] = 0.97f * bandOnsetVar[b] + 0.03f * (diff * diff);
		const float stdv = std::sqrt(std::max(0.0f, var));
		const float threshold = cfg.onsetThresholdBase * (mean + cfg.onsetVarianceWeight * stdv + 1e-6f);
		float strength = 0.0f;
		if (inc > threshold) strength = clamp01((inc - threshold) / (threshold + 1e-6f));
		out.bandOnset[b] = strength;
	}
}

void AudioAnalysis::computeBeat(AnalysisFrame& out, float dtSec) {
	const int lowEnd = std::max(1, cfg.numBands / 6);
	float lowSum = 0.0f;
	for (int b = 0; b < lowEnd; ++b) lowSum += out.bandEnergy[b];
	const float lowAvg = lowSum / float(lowEnd);

	float mean = 0.0f;
	float var = 0.0f;
	for (float v : lowBandEnergyHistory) mean += v;
	if (!lowBandEnergyHistory.empty()) mean /= float(lowBandEnergyHistory.size());
	for (float v : lowBandEnergyHistory) {
		const float d = v - mean;
		var += d * d;
	}
	if (!lowBandEnergyHistory.empty()) var /= float(lowBandEnergyHistory.size());
	const float stdv = std::sqrt(std::max(0.0f, var));
	const float threshold = mean + std::max(cfg.onsetVarianceWeight * stdv, mean * 0.08f);

	const bool canTrigger = (timeSec - lastBeatTimeSec) >= cfg.beatMinIntervalSec;
	const bool hasHistory = (int)lowBandEnergyHistory.size() >= std::min(8, cfg.onsetHistory);
	const bool trigger = hasHistory && canTrigger && (lowAvg > threshold) && (lowAvg > 1e-4f);
	out.beatTriggered = trigger;
	if (trigger) {
		const float ibi = (lastBeatTimeSec > 0.0f) ? (timeSec - lastBeatTimeSec) : 0.0f;
		lastBeatTimeSec = timeSec;
		beatEnvelope = 1.0f;
		if (ibi > 0.0f) {
			ibiHistorySec.push_back(ibi);
			if (ibiHistorySec.size() > 16) ibiHistorySec.pop_front();
		}
	}

	if (beatEnvelope > 0.0f) {
		const float step = dtSec / std::max(1e-3f, cfg.beatEnvelopeHoldSec);
		beatEnvelope = std::max(0.0f, beatEnvelope - step);
	}
	out.beatEnvelope = easeOutCubic(beatEnvelope);

	lowBandEnergyHistory.push_back(lowAvg);
	if ((int)lowBandEnergyHistory.size() > cfg.onsetHistory) lowBandEnergyHistory.pop_back();

	if (ibiHistorySec.size() >= 5) {
		std::vector<float> scratch(ibiHistorySec.begin(), ibiHistorySec.end());
		std::sort(scratch.begin(), scratch.end());
		const float medianIbi = scratch[scratch.size() / 2];
		out.bpm = medianIbi > 1e-3f ? 60.0f / medianIbi : 0.0f;
	} else {
		out.bpm = 0.0f;
	}
}

void AudioAnalysis::computeHPSS(AnalysisFrame& out) {
	magHistory.push_front(mag);
	if ((int)magHistory.size() > cfg.hpssTimeMedian) magHistory.pop_back();

	const int updateInterval = std::max(1, cfg.hpssUpdateInterval);
	++hpssBlockCounter;
	if (hpssBlockCounter < updateInterval) {
		out.percussiveEnergy = lastPercussiveEnergy;
		out.harmonicEnergy = lastHarmonicEnergy;
		out.percussiveRatio = lastPercussiveRatio;
		return;
	}
	hpssBlockCounter = 0;

	const int Wt = std::min<int>((int)magHistory.size(), cfg.hpssTimeMedian);
	for (size_t i = 0; i < mag.size(); ++i) {
		hpssScratchBuffer.clear();
		for (int t = 0; t < Wt; ++t) hpssScratchBuffer.push_back(magHistory[t][i]);
		std::nth_element(hpssScratchBuffer.begin(), hpssScratchBuffer.begin() + hpssScratchBuffer.size() / 2, hpssScratchBuffer.end());
		hpssTimeMedianBuffer[i] = hpssScratchBuffer[hpssScratchBuffer.size() / 2];
	}

	const int Wf = std::max(1, cfg.hpssFreqMedian);
	for (size_t i = 0; i < mag.size(); ++i) {
		const int a = std::max<int>(0, (int)i - Wf);
		const int z = std::min<int>((int)mag.size() - 1, (int)i + Wf);
		hpssScratchBuffer.clear();
		for (int k = a; k <= z; ++k) hpssScratchBuffer.push_back(mag[k]);
		std::nth_element(hpssScratchBuffer.begin(), hpssScratchBuffer.begin() + hpssScratchBuffer.size() / 2, hpssScratchBuffer.end());
		hpssFreqMedianBuffer[i] = hpssScratchBuffer[hpssScratchBuffer.size() / 2];
	}

	float perE = 0.0f;
	float harE = 0.0f;
	for (size_t i = 0; i < mag.size(); ++i) {
		const float p = std::max(0.0f, mag[i] - hpssFreqMedianBuffer[i]);
		const float h = std::max(0.0f, mag[i] - hpssTimeMedianBuffer[i]);
		perE += p;
		harE += h;
	}
	lastPercussiveEnergy = perE;
	lastHarmonicEnergy = harE;
	lastPercussiveRatio = (perE + harE) > 1e-6f ? perE / (perE + harE) : 0.0f;
	out.percussiveEnergy = lastPercussiveEnergy;
	out.harmonicEnergy = lastHarmonicEnergy;
	out.percussiveRatio = lastPercussiveRatio;
}

float AudioAnalysis::weightedCenterNorm(const std::vector<float>& weights, int startBand, int endBand) const {
	float num = 0.0f;
	float den = 0.0f;
	for (int i = startBand; i <= endBand && i < (int)weights.size(); ++i) {
		const float centerHz = 0.5f * (roleState.bandEdgesHz[i] + roleState.bandEdgesHz[i + 1]);
		const float norm = centerHz / (float(sampleRate) * 0.5f);
		num += norm * weights[i];
		den += weights[i];
	}
	return den > 1e-6f ? clamp01(num / den) : 0.0f;
}

float AudioAnalysis::weightedSpreadNorm(const std::vector<float>& weights, int startBand, int endBand, float centerNorm) const {
	float num = 0.0f;
	float den = 0.0f;
	for (int i = startBand; i <= endBand && i < (int)weights.size(); ++i) {
		const float centerHz = 0.5f * (roleState.bandEdgesHz[i] + roleState.bandEdgesHz[i + 1]);
		const float norm = centerHz / (float(sampleRate) * 0.5f);
		const float d = norm - centerNorm;
		num += d * d * weights[i];
		den += weights[i];
	}
	return den > 1e-6f ? std::sqrt(std::max(1e-5f, num / den)) : 0.08f;
}

void AudioAnalysis::updateAnchor(float& anchor, float& width, float& confidence, float targetNorm, float targetWidth, float targetConfidence, float fastAlpha, float slowAlpha) {
	const float adaptAlpha = targetConfidence > confidence ? fastAlpha : slowAlpha;
	anchor = smoothTowards(anchor, targetNorm, adaptAlpha);
	width = smoothTowards(width, std::max(0.035f, std::min(0.30f, targetWidth)), adaptAlpha * 0.75f);
	confidence = smoothTowards(confidence, clamp01(targetConfidence), adaptAlpha * 0.9f);
}

float AudioAnalysis::roleWindow(float centerNorm, float widthNorm, float bandCenterNorm) const {
	const float safeWidth = std::max(0.02f, widthNorm);
	const float d = (bandCenterNorm - centerNorm) / safeWidth;
	return std::exp(-0.5f * d * d);
}

float AudioAnalysis::regionBias(float centerNorm, float bandCenterNorm, float widthNorm) const {
	return roleWindow(centerNorm, widthNorm, bandCenterNorm);
}

void AudioAnalysis::computeAdaptiveRoleBands(float dtSec) {
	const float fastAlpha = fastAlphaFromTau(dtSec, 0.18f);
	const float slowAlpha = fastAlphaFromTau(dtSec, 1.0f);
	const int n = (int)roleState.energy.size();
	float sumFastEnergy = 0.0f;
	for (int b = 0; b < n; ++b) {
		int a = roleState.bandEdgeBins[b];
		int z = roleState.bandEdgeBins[b + 1];
		if (z <= a) z = a + 1;
		float e = 0.0f;
		for (int i = a; i < z; ++i) e += mag[i];
		e /= float(std::max(1, z - a));
		roleState.energy[b] = e;
		const float delta = e - roleState.prevEnergy[b];
		roleState.prevEnergy[b] = e;
		roleState.flux[b] = std::max(0.0f, delta);
		roleState.fastEnergy[b] = smoothTowards(roleState.fastEnergy[b], e, fastAlpha);
		roleState.slowEnergy[b] = smoothTowards(roleState.slowEnergy[b], e, slowAlpha);
		roleState.fastFlux[b] = smoothTowards(roleState.fastFlux[b], roleState.flux[b], fastAlpha);
		roleState.longSpectralAvg[b] = smoothTowards(roleState.longSpectralAvg[b], roleState.fastEnergy[b], slowAlpha * 0.5f);
		sumFastEnergy += roleState.fastEnergy[b];
	}

	const float meanFastEnergy = sumFastEnergy / float(std::max(1, n));
	const float occupancyKnee = std::max(1e-4f, meanFastEnergy * 0.65f);
	const float prominenceKnee = std::max(1e-4f, meanFastEnergy * 0.35f);
	for (int b = 0; b < n; ++b) {
		const float transient = safeDiv(roleState.flux[b], roleState.slowEnergy[b] + 1e-4f);
		roleState.transientEma[b] = smoothTowards(roleState.transientEma[b], transient, fastAlpha);
		const float occupancyRatio = safeDiv(roleState.fastEnergy[b], roleState.slowEnergy[b] + 1e-4f);
		const float absoluteGate = softCompress(roleState.fastEnergy[b], occupancyKnee);
		roleState.occupancyFast[b] = softCompress(std::max(0.0f, occupancyRatio - 0.72f), 0.30f) * absoluteGate;
		roleState.occupancySlow[b] = smoothTowards(roleState.occupancySlow[b], roleState.occupancyFast[b], slowAlpha);
		roleState.prominence[b] = softCompress(peakProminence(roleState.fastEnergy, b), prominenceKnee) * absoluteGate;
		const float harm = safeDiv(roleState.slowEnergy[b], roleState.slowEnergy[b] + roleState.fastFlux[b] + 1e-4f);
		roleState.harmonicity[b] = smoothTowards(roleState.harmonicity[b], clamp01(harm), fastAlpha);
		roleState.percussiveWeight[b] = clamp01((0.58f * roleState.transientEma[b] + 0.42f * (1.0f - roleState.harmonicity[b])) * (0.35f + 0.65f * absoluteGate));
		roleState.transientConcentration[b] = smoothTowards(roleState.transientConcentration[b], roleState.transientEma[b] * absoluteGate, slowAlpha);
	}
}

void AudioAnalysis::computeSemanticRoles(AnalysisFrame& out, float dtSec) {
	const float fastAlpha = fastAlphaFromTau(dtSec, 0.12f);
	const float slowAlpha = fastAlphaFromTau(dtSec, 2.2f);
	const float nyq = float(sampleRate) * 0.5f;
	const int n = (int)roleState.energy.size();

	std::vector<float> kickWeights(n, 0.0f);
	std::vector<float> bassWeights(n, 0.0f);
	std::vector<float> bodyWeights(n, 0.0f);
	std::vector<float> leadWeights(n, 0.0f);
	std::vector<float> airWeights(n, 0.0f);
	std::vector<float> tickWeights(n, 0.0f);
	auto meanOf = [](const std::vector<float>& values) {
		if (values.empty()) return 0.0f;
		float s = std::accumulate(values.begin(), values.end(), 0.0f);
		return s / float(values.size());
	};
	auto maxOf = [](const std::vector<float>& values) {
		return values.empty() ? 0.0f : *std::max_element(values.begin(), values.end());
	};

	float totalTransient = 0.0f;
	float lowEnergy = 0.0f;
	float midEnergy = 0.0f;
	float leadEnergy = 0.0f;
	float airEnergy = 0.0f;

	for (int i = 0; i < n; ++i) {
		const float centerHz = 0.5f * (roleState.bandEdgesHz[i] + roleState.bandEdgesHz[i + 1]);
		const float centerNorm = centerHz / nyq;
		const float kickMask = roleWindow(85.0f / nyq, 55.0f / nyq, centerNorm);
		const float lowSustainMask = roleWindow(120.0f / nyq, 120.0f / nyq, centerNorm);
		const float midMask = 0.72f * roleWindow(1400.0f / nyq, 900.0f / nyq, centerNorm)
			+ 0.28f * roleWindow(700.0f / nyq, 450.0f / nyq, centerNorm);
		const float leadMask = roleWindow(2800.0f / nyq, 1400.0f / nyq, centerNorm);
		const float airMask = clamp01((centerHz - 5000.0f) / 5000.0f)
			* (0.65f * roleWindow(9000.0f / nyq, 4200.0f / nyq, centerNorm) + 0.35f);
		const float tickMask = clamp01((centerHz - 5500.0f) / 3500.0f)
			* roleWindow(10500.0f / nyq, 2600.0f / nyq, centerNorm);

		kickWeights[i] = kickMask * (0.72f * roleState.transientEma[i] + 0.28f * roleState.percussiveWeight[i]);
		bassWeights[i] = lowSustainMask * (0.58f * roleState.occupancyFast[i] + 0.42f * roleState.occupancySlow[i]) * (0.62f + 0.38f * roleState.harmonicity[i]);
		bodyWeights[i] = midMask * (0.60f * roleState.occupancyFast[i] + 0.40f * roleState.occupancySlow[i])
			* (0.10f + 0.90f * roleState.harmonicity[i])
			* (0.45f + 0.55f * (1.0f - roleState.percussiveWeight[i]));
		leadWeights[i] = leadMask * (0.88f * roleState.prominence[i] + 0.12f * roleState.occupancyFast[i])
			* (0.02f + 0.98f * roleState.harmonicity[i])
			* (0.18f + 0.82f * (1.0f - roleState.percussiveWeight[i]));
		airWeights[i] = airMask * (0.36f * roleState.occupancyFast[i] + 0.22f * roleState.occupancySlow[i] + 0.18f * roleState.fastEnergy[i] + 0.24f * roleState.transientConcentration[i])
			* (0.25f + 0.75f * (1.0f - roleState.harmonicity[i]));
		tickWeights[i] = tickMask * (0.82f * roleState.transientEma[i] + 0.18f * roleState.fastFlux[i])
			* (0.15f + 0.85f * (1.0f - roleState.harmonicity[i]));

		totalTransient += roleState.transientEma[i];
		if (centerHz < 280.0f) lowEnergy += roleState.fastEnergy[i];
		if (centerHz >= 250.0f && centerHz <= 2600.0f) midEnergy += roleState.fastEnergy[i];
		if (centerHz >= 1000.0f && centerHz <= 4500.0f) leadEnergy += roleState.fastEnergy[i];
		if (centerHz > 3500.0f) airEnergy += roleState.fastEnergy[i];
	}

	const float kickTarget = clamp01(0.65f * maxOf(kickWeights) + 0.70f * meanOf(kickWeights));
	const float bassTarget = clamp01(0.45f * maxOf(bassWeights) + 1.20f * meanOf(bassWeights));
	const float bodyTarget = clamp01(0.35f * maxOf(bodyWeights) + 1.10f * meanOf(bodyWeights));
	const float leadTarget = clamp01(0.45f * maxOf(leadWeights) + 1.00f * meanOf(leadWeights));
	const float airTarget = clamp01(0.40f * maxOf(airWeights) + 1.10f * meanOf(airWeights));
	const float tickTarget = clamp01(0.75f * maxOf(tickWeights) + 0.60f * meanOf(tickWeights));

	const float kickCenterTarget = weightedCenterNorm(kickWeights, 0, std::min(n - 1, n / 3));
	const float bassCenterTarget = weightedCenterNorm(bassWeights, 0, std::min(n - 1, n / 2));
	const float leadCenterTarget = weightedCenterNorm(leadWeights, std::max(0, n / 6), n - 1);
	const float airCenterTarget = weightedCenterNorm(airWeights, std::max(0, n / 2), n - 1);

	updateAnchor(roleState.lowImpactAnchor, roleState.lowImpactWidth, roleState.lowImpactConfidence,
		kickCenterTarget, weightedSpreadNorm(kickWeights, 0, std::min(n - 1, n / 3), kickCenterTarget),
		kickTarget, 0.25f, 0.05f);
	updateAnchor(roleState.lowSustainAnchor, roleState.lowSustainWidth, roleState.lowSustainConfidence,
		bassCenterTarget, weightedSpreadNorm(bassWeights, 0, std::min(n - 1, n / 2), bassCenterTarget),
		bassTarget, 0.18f, 0.04f);
	updateAnchor(roleState.leadAnchor, roleState.leadWidth, roleState.leadConfidence,
		leadCenterTarget, weightedSpreadNorm(leadWeights, std::max(0, n / 6), n - 1, leadCenterTarget),
		leadTarget, 0.16f, 0.03f);
	updateAnchor(roleState.airAnchor, roleState.airWidth, roleState.airConfidence,
		airCenterTarget, weightedSpreadNorm(airWeights, std::max(0, n / 2), n - 1, airCenterTarget),
		airTarget, 0.14f, 0.03f);
	updateAnchor(roleState.tickAnchor, roleState.tickWidth, roleState.airConfidence,
		weightedCenterNorm(tickWeights, std::max(0, n / 2), n - 1),
		weightedSpreadNorm(tickWeights, std::max(0, n / 2), n - 1, roleState.tickAnchor),
		tickTarget, 0.20f, 0.05f);
	updateAnchor(roleState.midBodyAnchor, roleState.midBodyWidth, roleState.leadConfidence,
		weightedCenterNorm(bodyWeights, std::max(0, n / 8), std::max(0, (2 * n) / 3)),
		weightedSpreadNorm(bodyWeights, std::max(0, n / 8), std::max(0, (2 * n) / 3), roleState.midBodyAnchor),
		bodyTarget, 0.10f, 0.02f);

	const float lowSustainNorm = softCompress(lowEnergy, 18.0f);
	const float midEnergyNorm = softCompress(midEnergy, 4.0f);
	const float leadEnergyNorm = softCompress(leadEnergy, 2.0f);
	const float airEnergyNorm = softCompress(airEnergy, 1.2f);
	const float brightnessRaw = clamp01(0.38f * (out.spectralCentroidHz / std::max(1.0f, nyq)) + 0.62f * airEnergyNorm);
	const float lowPercussiveGate = clamp01(lowSustainNorm * (0.20f + 0.80f * out.percussiveRatio));
	const float kickTransientRaw = lowPercussiveGate * clamp01((kickTarget - (0.20f + 0.70f * bassTarget + 0.14f * bodyTarget)) / 0.30f);
	const float bassSustainRaw = clamp01(0.56f * bassTarget + 0.52f * lowSustainNorm - 0.18f * kickTransientRaw);
	const float transientDensityRaw = clamp01(softCompress(totalTransient / std::max(1, n), 6.0f));
	const float percussiveFocusRaw = clamp01(
		(0.40f * out.percussiveRatio + 0.18f * transientDensityRaw + 0.08f * tickTarget) *
		(0.20f + 0.80f * transientDensityRaw));
	const float airStableRaw = clamp01(std::max(0.0f,
		0.52f * airTarget +
		0.32f * airEnergyNorm +
		0.10f * brightnessRaw -
		0.06f * leadTarget -
		0.08f * tickTarget));
	const float leadStableRaw = clamp01(std::max(0.0f,
		0.78f * leadTarget +
		0.18f * leadEnergyNorm -
		0.16f * bodyTarget -
		0.12f * airTarget -
		0.12f * percussiveFocusRaw));
	const float energyLevelRaw = clamp01(0.16f * clamp01(out.rms * 2.4f) + 0.18f * kickTransientRaw + 0.28f * bassSustainRaw + 0.23f * bodyTarget + 0.15f * airStableRaw);

	const float floorRiseAlpha = fastAlphaFromTau(dtSec, 14.0f);
	const float floorFallAlpha = fastAlphaFromTau(dtSec, 1.8f);
	const float peakRiseAlpha = fastAlphaFromTau(dtSec, 0.15f);
	const float peakFallAlpha = fastAlphaFromTau(dtSec, 1.8f);

	const float kickTransientNorm = normalizeAdaptive(kickTransientRaw, roleState.kickFloor, roleState.kickPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.20f);
	normalizeAdaptive(bassSustainRaw, roleState.bassFloor, roleState.bassPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.16f);
	normalizeAdaptive(bodyTarget, roleState.bodyFloor, roleState.bodyPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.16f);
	normalizeAdaptive(leadStableRaw, roleState.leadFloor, roleState.leadPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.14f);
	normalizeAdaptive(airStableRaw, roleState.airFloor, roleState.airPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.14f);
	const float transientNorm = normalizeAdaptive(transientDensityRaw, roleState.transientFloor, roleState.transientPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.12f);
	const float brightnessNorm = normalizeAdaptive(brightnessRaw, roleState.brightnessFloor, roleState.brightnessPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.12f);
	const float percussiveNorm = normalizeAdaptive(percussiveFocusRaw, roleState.percussiveFloor, roleState.percussivePeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.12f);

	// Fire on a stronger transient with a longer refractory window to cut false
	// triggers, and decay the envelope faster (linear, not eased) so the kick
	// reads as a snappy hit instead of a lingering swell.
	if (kickTransientRaw > 0.68f && (timeSec - roleState.lastKickTriggerTime) > 0.13f) {
		roleState.lastKickTriggerTime = timeSec;
		roleState.kickEnv = 1.0f;
	}
	roleState.kickEnv = std::max(0.0f, roleState.kickEnv - dtSec / 0.075f);
	out.kickImpact = std::max(kickTransientNorm, roleState.kickEnv);

	const float snareRaw = clamp01(softCompress(
		bodyTarget *
		(0.25f + 0.75f * transientDensityRaw) *
		(0.25f + 0.75f * out.percussiveRatio),
		0.85f));
	if (snareRaw > 0.58f && (timeSec - roleState.lastSnareTriggerTime) > 0.14f) {
		roleState.lastSnareTriggerTime = timeSec;
		roleState.snareEnv = 1.0f;
	}
	roleState.snareEnv = std::max(0.0f, roleState.snareEnv - dtSec / 0.15f);
	out.snareImpact = std::max(normalizeAdaptive(snareRaw, roleState.bodyFloor, roleState.bodyPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.16f), easeOutCubic(roleState.snareEnv));

	if (tickTarget > 0.52f && (timeSec - roleState.lastHatTriggerTime) > 0.05f) {
		roleState.lastHatTriggerTime = timeSec;
		roleState.hatEnv = 1.0f;
	}
	roleState.hatEnv = std::max(0.0f, roleState.hatEnv - dtSec / 0.08f);
	out.hatTick = std::max(normalizeAdaptive(tickTarget, roleState.airFloor, roleState.airPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.14f), easeOutCubic(roleState.hatEnv));

	const float lowBodyNorm = normalizeAdaptive(lowSustainNorm, roleState.bassFloor, roleState.bassPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.16f);
	const float bassPresenceRaw = clamp01(bassSustainRaw * clamp01(1.2f * lowSustainNorm * lowSustainNorm));
	const float harmonicPresenceRaw = clamp01(std::max(0.0f, 0.88f * bodyTarget - 0.12f * out.percussiveRatio) * (0.18f + 0.82f * midEnergyNorm));
	const float leadPresenceRaw = clamp01(leadStableRaw * (0.12f + 0.88f * leadEnergyNorm));
	const float airPresenceRaw = clamp01(airStableRaw * (0.10f + 0.90f * airEnergyNorm));

	const float bassPresenceNorm = normalizeAdaptive(bassPresenceRaw, roleState.bassOutFloor, roleState.bassOutPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.08f);
	const float harmonicPresenceNorm = normalizeAdaptive(harmonicPresenceRaw, roleState.harmonicOutFloor, roleState.harmonicOutPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.08f);
	const float leadPresenceNorm = normalizeAdaptive(leadPresenceRaw, roleState.leadOutFloor, roleState.leadOutPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.08f);
	const float airPresenceNorm = normalizeAdaptive(airPresenceRaw, roleState.airOutFloor, roleState.airOutPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.08f);

	const float subBodyTarget = clamp01(0.58f * bassPresenceNorm + 0.42f * lowBodyNorm);
	const float bassBodyTarget = clamp01(perceptualLift(bassPresenceNorm, 0.95f));
	const float harmonicBodyTarget = clamp01(perceptualLift(harmonicPresenceNorm, 0.98f));
	const float leadPresenceTarget = clamp01(perceptualLift(leadPresenceNorm, 1.00f));
	const float airPresenceTarget = clamp01(perceptualLift(airPresenceNorm, 1.00f));

	roleState.subBodyOut = clamp01(followEnvelope(roleState.subBodyOut, subBodyTarget, dtSec, 0.028f, 0.10f));
	roleState.bassBodyOut = clamp01(followEnvelope(roleState.bassBodyOut, bassBodyTarget, dtSec, 0.032f, 0.090f));
	roleState.harmonicBodyOut = clamp01(followEnvelope(roleState.harmonicBodyOut, harmonicBodyTarget, dtSec, 0.026f, 0.070f));
	roleState.leadPresenceOut = clamp01(followEnvelope(roleState.leadPresenceOut, leadPresenceTarget, dtSec, 0.018f, 0.055f));
	roleState.airPresenceOut = clamp01(followEnvelope(roleState.airPresenceOut, airPresenceTarget, dtSec, 0.018f, 0.055f));

	out.subBody = roleState.subBodyOut;
	out.bassBody = roleState.bassBodyOut;
	out.harmonicBody = roleState.harmonicBodyOut;
	out.leadPresence = roleState.leadPresenceOut;
	out.airPresence = roleState.airPresenceOut;

	std::vector<float> semanticVector = {
		out.kickImpact, out.bassBody, out.harmonicBody, out.leadPresence,
		out.airPresence, transientDensityRaw, brightnessRaw, percussiveFocusRaw
	};
	float noveltyDistance = 0.0f;
	for (size_t i = 0; i < semanticVector.size(); ++i) {
		const float d = semanticVector[i] - roleState.previousSemanticVector[i];
		noveltyDistance += d * d;
		roleState.previousSemanticVector[i] = smoothTowards(roleState.previousSemanticVector[i], semanticVector[i], fastAlpha);
	}
	noveltyDistance = std::sqrt(noveltyDistance / float(semanticVector.size()));
	roleState.fastNovelty = smoothTowards(roleState.fastNovelty, noveltyDistance, fastAlpha);
	roleState.slowNovelty = smoothTowards(roleState.slowNovelty, noveltyDistance, slowAlpha);
	roleState.fastBrightness = smoothTowards(roleState.fastBrightness, brightnessRaw, fastAlpha);
	roleState.slowBrightness = smoothTowards(roleState.slowBrightness, brightnessRaw, slowAlpha);

	out.transientDensity = transientNorm;
	out.novelty = clamp01(normalizeAdaptive(
		clamp01(softCompress(std::max(0.0f, roleState.fastNovelty - 0.35f * roleState.slowNovelty) + 0.5f * noveltyDistance, 0.20f)),
		roleState.noveltyFloor, roleState.noveltyPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.10f));
	out.brightness = brightnessNorm;
	out.percussiveFocus = percussiveNorm;

	roleState.energyLevel = smoothTowards(roleState.energyLevel, energyLevelRaw, fastAlphaFromTau(dtSec, 0.24f));
	out.energyLevel = clamp01(perceptualLift(
		normalizeAdaptive(roleState.energyLevel, roleState.energyFloor, roleState.energyPeak, floorRiseAlpha, floorFallAlpha, peakRiseAlpha, peakFallAlpha, 0.22f),
		0.85f));

	const float energySlope = std::max(0.0f, out.energyLevel - roleState.lastEnergyLevel);
	const float brightnessSlope = std::max(0.0f, out.brightness - roleState.slowBrightness);
	const float airRise = std::max(0.0f, out.airPresence - roleState.lastAirPresence);
	const float leadRise = std::max(0.0f, out.leadPresence - roleState.lastLeadPresence);
	const float lowHoldback = clamp01(1.0f - out.bassBody);
	const float tensionTarget = clamp01(
		0.16f * out.energyLevel +
		0.14f * out.brightness +
		0.12f * out.transientDensity +
		0.14f * out.novelty +
		0.12f * airRise +
		0.08f * leadRise +
		0.08f * lowHoldback +
		0.22f * energySlope +
		0.16f * brightnessSlope
	);
	roleState.tension = followEnvelope(roleState.tension, tensionTarget, dtSec, 0.45f, 1.20f);
	out.tension = clamp01(roleState.tension);

	switch (macroState) {
		case MacroState::Neutral:
			if (out.tension > 0.46f && energySlope > 0.005f) macroState = MacroState::Building;
			break;
		case MacroState::Building:
			roleState.buildStrength = smoothTowards(roleState.buildStrength, out.tension, fastAlphaFromTau(dtSec, 0.7f));
			if (out.tension > 0.72f) macroState = MacroState::PreDrop;
			if (out.tension < 0.30f) macroState = MacroState::Neutral;
			break;
		case MacroState::PreDrop:
			if ((out.kickImpact > 0.72f || out.bassBody > 0.76f) && out.novelty > 0.28f && (timeSec - roleState.lastDropTriggerTime) > 0.6f) {
				roleState.lastDropTriggerTime = timeSec;
				roleState.dropEventEnv = 1.0f;
				macroState = MacroState::DropCooldown;
				roleState.release = 0.0f;
			}
			if (out.tension < 0.42f) macroState = MacroState::Building;
			break;
		case MacroState::DropCooldown:
			if (timeSec - roleState.lastDropTriggerTime > 0.8f) {
				macroState = MacroState::Release;
				roleState.release = 1.0f;
			}
			break;
		case MacroState::Release:
			roleState.release = std::max(0.0f, roleState.release - dtSec / 1.1f);
			if (roleState.release <= 0.01f) macroState = MacroState::Neutral;
			break;
	}

	const float releaseTransient = clamp01(
		(roleState.lastTension - out.tension) * 2.8f +
		(roleState.lastEnergyLevel - out.energyLevel) * 2.0f +
		(roleState.lastLowEnergy - out.bassBody) * 1.0f);
	const float releaseTexture = clamp01(
		(1.0f - out.energyLevel) * 0.42f +
		(1.0f - out.tension) * 0.33f +
		(1.0f - out.transientDensity) * 0.15f +
		(1.0f - out.kickImpact) * 0.10f);
	const float releaseTarget = std::max(roleState.release, clamp01(1.15f * releaseTransient + 0.45f * releaseTexture));

	roleState.dropEventEnv = std::max(0.0f, roleState.dropEventEnv - dtSec / 0.28f);
	out.dropEvent = easeOutCubic(roleState.dropEventEnv);

	const float sectionTrigger = clamp01(softCompress(std::max(0.0f, noveltyDistance - roleState.slowNovelty) + 0.5f * std::fabs(out.energyLevel - roleState.lastEnergyLevel), 0.16f));
	if (sectionTrigger > 0.62f && out.kickImpact < 0.85f && (timeSec - roleState.lastSectionTriggerTime) > 0.45f) {
		roleState.lastSectionTriggerTime = timeSec;
		roleState.sectionChangeEnv = 1.0f;
	}
	roleState.sectionChangeEnv = std::max(0.0f, roleState.sectionChangeEnv - dtSec / 0.32f);
	out.sectionChange = easeOutCubic(roleState.sectionChangeEnv);

	roleState.releaseOut = clamp01(followEnvelope(roleState.releaseOut, releaseTarget, dtSec, 0.10f, 0.26f));
	out.release = roleState.releaseOut;
	out.beatPulse = clamp01(0.60f * out.beatEnvelope + 0.40f * out.kickImpact);

	out.roleKickCenterNorm = roleState.lowImpactAnchor;
	out.roleBassCenterNorm = roleState.lowSustainAnchor;
	out.roleLeadCenterNorm = roleState.leadAnchor;
	out.roleAirCenterNorm = roleState.airAnchor;
	out.roleKickConfidence = roleState.lowImpactConfidence;
	out.roleBassConfidence = roleState.lowSustainConfidence;
	out.roleLeadConfidence = roleState.leadConfidence;
	out.roleAirConfidence = roleState.airConfidence;

	roleState.lastEnergyLevel = out.energyLevel;
	roleState.lastTension = out.tension;
	roleState.lastLowEnergy = out.bassBody;
	roleState.lastLeadPresence = out.leadPresence;
	roleState.lastAirPresence = out.airPresence;
}

AudioAnalysis::AnalysisFrame AudioAnalysis::processInterleaved(const std::vector<float>& interleaved, unsigned int channels) {
	AnalysisFrame out;
	if (interleaved.empty()) return out;

	const unsigned int safeChannels = std::max(1u, channels);
	const size_t frameCount = interleaved.size() / safeChannels;
	const float dtSec = frameCount > 0 ? float(frameCount) / float(sampleRate) : float(cfg.fftSize) / float(sampleRate);
	timeSec += dtSec;

	computeFftAndMag(interleaved, channels);
	computeBroadband(out);
	computeBands(out);
	computeOnsets(out, dtSec);
	computeBeat(out, dtSec);
	computeHPSS(out);
	computeAdaptiveRoleBands(dtSec);
	computeSemanticRoles(out, dtSec);
	return out;
}

AudioAnalysis::AnalysisFrame AudioAnalysis::processInterleavedStereo(const std::vector<float>& interleavedStereo) {
	return processInterleaved(interleavedStereo, 2);
}

float AudioAnalysis::computeMedian(std::vector<float>& scratch) {
	if (scratch.empty()) return 0.0f;
	std::nth_element(scratch.begin(), scratch.begin() + scratch.size() / 2, scratch.end());
	return scratch[scratch.size() / 2];
}
