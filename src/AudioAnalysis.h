#pragma once

#include <deque>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif
#include <kiss_fft.h>
#ifdef __cplusplus
}
#endif

struct kiss_fftr_state;

class AudioAnalysis {
public:
	struct Config {
		int fftSize = 1024;
		int numBands = 16;
		int roleBands = 28;
		float lowBandHz = 20.0f;
		float highBandHz = 20000.0f;
		int onsetHistory = 43;
		float onsetThresholdBase = 1.35f;
		float onsetVarianceWeight = 0.25f;
		float beatMinIntervalSec = 0.35f;
		float beatEnvelopeHoldSec = 0.22f;
		int hpssTimeMedian = 25;
		int hpssFreqMedian = 15;
		int hpssUpdateInterval = 4;
	};

	struct AnalysisFrame {
		float rms = 0.0f;
		float spectralCentroidHz = 0.0f;
		float spectralFlux = 0.0f;

		std::vector<float> bandEnergy;
		std::vector<float> bandEnergyNorm;
		std::vector<float> bandOnset;

		bool beatTriggered = false;
		float beatEnvelope = 0.0f;
		float bpm = 0.0f;

		float percussiveEnergy = 0.0f;
		float harmonicEnergy = 0.0f;
		float percussiveRatio = 0.0f;

		float kickImpact = 0.0f;
		float snareImpact = 0.0f;
		float hatTick = 0.0f;
		float beatPulse = 0.0f;

		float subBody = 0.0f;
		float bassBody = 0.0f;
		float harmonicBody = 0.0f;
		float leadPresence = 0.0f;
		float airPresence = 0.0f;

		float transientDensity = 0.0f;
		float novelty = 0.0f;
		float brightness = 0.0f;
		float percussiveFocus = 0.0f;

		float energyLevel = 0.0f;
		float tension = 0.0f;
		float release = 0.0f;
		float dropEvent = 0.0f;
		float sectionChange = 0.0f;

		float roleKickCenterNorm = 0.0f;
		float roleBassCenterNorm = 0.0f;
		float roleLeadCenterNorm = 0.0f;
		float roleAirCenterNorm = 0.0f;

		float roleKickConfidence = 0.0f;
		float roleBassConfidence = 0.0f;
		float roleLeadConfidence = 0.0f;
		float roleAirConfidence = 0.0f;
	};

	explicit AudioAnalysis(unsigned int sampleRate);
	AudioAnalysis(unsigned int sampleRate, const Config& config);
	~AudioAnalysis();
	AudioAnalysis(const AudioAnalysis&) = delete;
	AudioAnalysis& operator=(const AudioAnalysis&) = delete;
	AudioAnalysis(AudioAnalysis&& other) noexcept;
	AudioAnalysis& operator=(AudioAnalysis&& other) noexcept;

	AnalysisFrame processInterleaved(const std::vector<float>& interleaved, unsigned int channels = 2);
	AnalysisFrame processInterleavedStereo(const std::vector<float>& interleavedStereo);

	const std::vector<float>& getBandEdgesHz() const { return bandEdgesHz; }
	int getFftSize() const { return cfg.fftSize; }
	int getNumBands() const { return cfg.numBands; }
	unsigned int getSampleRate() const { return sampleRate; }

private:
	struct AdaptiveRoleState {
		std::vector<float> bandEdgesHz;
		std::vector<int> bandEdgeBins;
		std::vector<float> energy;
		std::vector<float> prevEnergy;
		std::vector<float> flux;
		std::vector<float> fastEnergy;
		std::vector<float> slowEnergy;
		std::vector<float> fastFlux;
		std::vector<float> transientEma;
		std::vector<float> occupancyFast;
		std::vector<float> occupancySlow;
		std::vector<float> prominence;
		std::vector<float> harmonicity;
		std::vector<float> percussiveWeight;
		std::vector<float> longSpectralAvg;
		std::vector<float> transientConcentration;

		float lowImpactAnchor = 0.07f;
		float lowSustainAnchor = 0.11f;
		float midBodyAnchor = 0.28f;
		float leadAnchor = 0.46f;
		float airAnchor = 0.78f;
		float tickAnchor = 0.84f;

		float lowImpactWidth = 0.05f;
		float lowSustainWidth = 0.08f;
		float midBodyWidth = 0.12f;
		float leadWidth = 0.10f;
		float airWidth = 0.12f;
		float tickWidth = 0.08f;

		float lowImpactConfidence = 0.0f;
		float lowSustainConfidence = 0.0f;
		float leadConfidence = 0.0f;
		float airConfidence = 0.0f;

		float fastBrightness = 0.0f;
		float slowBrightness = 0.0f;
		float fastNovelty = 0.0f;
		float slowNovelty = 0.0f;
		float energyLevel = 0.0f;
		float tension = 0.0f;
		float release = 0.0f;
		float dropEventEnv = 0.0f;
		float sectionChangeEnv = 0.0f;
		float kickEnv = 0.0f;
		float snareEnv = 0.0f;
		float hatEnv = 0.0f;

		float lastKickTriggerTime = -1e9f;
		float lastSnareTriggerTime = -1e9f;
		float lastHatTriggerTime = -1e9f;
		float lastDropTriggerTime = -1e9f;
		float lastSectionTriggerTime = -1e9f;
		float buildStrength = 0.0f;
		float bassSuppression = 0.0f;
		float lastEnergyLevel = 0.0f;
		float lastTension = 0.0f;
		float lastLowEnergy = 0.0f;
		float lastAirPresence = 0.0f;

		std::vector<float> previousSemanticVector;
	};

	enum class MacroState {
		Neutral,
		Building,
		PreDrop,
		DropCooldown,
		Release
	};

	unsigned int sampleRate;
	Config cfg;
	float hzPerBin;

	kiss_fftr_state* fftr;
	std::vector<float> window;
	std::vector<float> mono;
	std::vector<float> fftIn;
	std::vector<float> mag;
	std::vector<float> prevMag;
	std::vector<kiss_fft_cpx> fftOut;

	std::deque<std::vector<float>> magHistory;
	std::vector<float> hpssTimeMedianBuffer;
	std::vector<float> hpssFreqMedianBuffer;
	std::vector<float> hpssScratchBuffer;
	int hpssBlockCounter;
	float lastPercussiveEnergy;
	float lastHarmonicEnergy;
	float lastPercussiveRatio;

	std::vector<float> bandEdgesHz;
	std::vector<int> bandEdgeBins;
	std::vector<float> bandEnergyHistoryAvg;
	std::vector<float> bandOnsetHistory;
	std::vector<float> bandPrevEnergy;
	std::vector<float> bandOnsetMean;
	std::vector<float> bandOnsetVar;

	float lowBandEnergyAvg;
	std::deque<float> lowBandEnergyHistory;
	float lastBeatTimeSec;
	std::deque<float> ibiHistorySec;
	float beatEnvelope;
	float timeSec;

	AdaptiveRoleState roleState;
	MacroState macroState;

	void initializeBands();
	void initializeRoleBands();
	void initializeLogBands(int numBands, float lowHz, float highHz, std::vector<float>& edgesHz, std::vector<int>& edgeBins);
	void computeFftAndMag(const std::vector<float>& interleaved, unsigned int channels);
	void computeBroadband(AnalysisFrame& out);
	void computeBands(AnalysisFrame& out);
	void computeOnsets(AnalysisFrame& out, float dtSec);
	void computeBeat(AnalysisFrame& out, float dtSec);
	void computeHPSS(AnalysisFrame& out);
	void computeAdaptiveRoleBands(float dtSec);
	void computeSemanticRoles(AnalysisFrame& out, float dtSec);
	float weightedCenterNorm(const std::vector<float>& weights, int startBand, int endBand) const;
	float weightedSpreadNorm(const std::vector<float>& weights, int startBand, int endBand, float centerNorm) const;
	void updateAnchor(float& anchor, float& width, float& confidence, float targetNorm, float targetWidth, float targetConfidence, float fastAlpha, float slowAlpha);
	float roleWindow(float centerNorm, float widthNorm, float bandCenterNorm) const;
	float regionBias(float centerNorm, float bandCenterNorm, float widthNorm) const;
	static float computeMedian(std::vector<float>& scratch);
};
