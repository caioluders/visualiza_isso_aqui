#pragma once

/*
AudioAnalysis - Real-time Music Feature Extraction Library

Purpose
  Provide rich, musically meaningful features for visual shaders:
  - Broadband features: RMS, spectral centroid, spectral flux
  - Multi-band energies: logarithmic 16-band energies and normalized versions
  - Multi-band onset strengths (detects transients in each band)
  - Beat detection with adaptive threshold and refractory window
  - Online BPM estimation from recent inter-beat intervals
  - Simple HPSS (Harmonic-Percussive Source Separation) masks and energies

Design notes
  - The class processes interleaved stereo input and internally mixes to mono
  - Uses Hann window + KissFFT real FFT (fftr) for spectrum
  - Maintains short histories for adaptive thresholds, spectral flux, HPSS
  - Emphasizes robustness and clarity over micro-optimizations

Integration
  1) Construct with sampleRate
  2) Call processInterleavedStereo(...) each frame with latest audio frames
  3) Read returned AnalysisFrame and send to shaders as uniforms

Threading
  - Not thread-safe by itself; call from the audio/render thread in sequence

Limitations
  - HPSS is approximate (median filters); good enough for visualization
  - BPM estimate stabilizes after a few beats (uses median IBI)
*/

#include <vector>
#include <deque>
#include <string>

struct kiss_fftr_state;

class AudioAnalysis {
public:
	// Tunable configuration for analysis
	struct Config {
		int fftSize = 1024;                 // Power of two, affects time/freq resolution
		int numBands = 16;                  // Log-spaced bands for energies/onsets
		float lowBandHz = 20.0f;            // Lowest band edge
		float highBandHz = 20000.0f;        // Highest band edge
		int onsetHistory = 43;              // ~900 ms at 48k/1024 hop; for adaptive threshold
		float onsetThresholdBase = 1.35f;   // Base onset threshold multiplier
		float onsetVarianceWeight = 0.25f;  // Add variance-weighted bias to threshold
		float beatMinIntervalSec = 0.25f;   // Refractory; ignore beats faster than 240 BPM
		float beatEnvelopeHoldSec = 0.22f;  // Beat envelope hold/decay window
		int hpssTimeMedian = 25;            // Frames for time median (percussive emphasis)
		int hpssFreqMedian = 15;            // Bins for freq median (harmonic emphasis)
	};

	// Outputs per frame
	struct AnalysisFrame {
		// Broadband
		float rms = 0.0f;                   // Root-mean-square amplitude
		float spectralCentroidHz = 0.0f;    // Brightness proxy
		float spectralFlux = 0.0f;          // Change in spectrum (>=0)

		// Bands (size == numBands)
		std::vector<float> bandEnergy;      // Linear energy per band
		std::vector<float> bandEnergyNorm;  // Normalized 0..1 per band (AGC over short history)
		std::vector<float> bandOnset;       // Onset strength 0..1 per band

		// Beat / tempo
		bool beatTriggered = false;         // True when a new beat is detected this frame
		float beatEnvelope = 0.0f;          // Decaying envelope for visual pulse 0..1
		float bpm = 0.0f;                    // Online BPM estimate (0 if unknown)

		// HPSS (broad energies and ratio)
		float percussiveEnergy = 0.0f;
		float harmonicEnergy = 0.0f;
		float percussiveRatio = 0.0f;       // P / (H + P)
	};

    explicit AudioAnalysis(unsigned int sampleRate);
    AudioAnalysis(unsigned int sampleRate, const Config& config);
	~AudioAnalysis();

	// Process a block of interleaved stereo frames. Returns computed features.
	AnalysisFrame processInterleavedStereo(const std::vector<float>& interleavedStereo);

	// Precomputed band edges in Hz (size = numBands+1). Useful for UI/debug.
	const std::vector<float>& getBandEdgesHz() const { return bandEdgesHz; }
	int getFftSize() const { return cfg.fftSize; }
	int getNumBands() const { return cfg.numBands; }
	unsigned int getSampleRate() const { return sampleRate; }

private:
	// Configuration and constants
	unsigned int sampleRate;
	Config cfg;
	float hzPerBin;

	// Windows and FFT buffers
	kiss_fftr_state* fftr;
	std::vector<float> window;
	std::vector<float> mono;      // Time-domain mono windowed
	std::vector<float> fftIn;     // Time-domain windowed
	std::vector<float> mag;       // Magnitude spectrum (fftSize/2+1)
	std::vector<float> prevMag;   // Previous magnitude for spectral flux

	// HPSS buffers
	std::deque< std::vector<float> > magHistory; // Last N magnitude frames

	// Bands
	std::vector<float> bandEdgesHz;   // Size numBands+1
	std::vector<int> bandEdgeBins;    // Corresponding bin indices
	std::vector<float> bandEnergyHistoryAvg; // Running averages for normalization (AGC)
	std::vector<float> bandOnsetHistory;     // For adaptive thresholds (per band)
	std::vector<float> bandPrevEnergy;       // For onset strength
	std::vector<float> bandOnsetMean;        // EMA mean of onset energy per band
	std::vector<float> bandOnsetVar;         // EMA variance proxy per band

	// Beat detection
	float lowBandEnergyAvg;
	std::deque<float> lowBandEnergyHistory;
	float lastBeatTimeSec;
	std::deque<float> ibiHistorySec; // Inter-beat intervals for BPM
	float beatEnvelope;
	float timeSec;                           // Running time from processed frames

	// Helpers
	void initializeBands();
	void computeFftAndMag(const std::vector<float>& interleavedStereo);
	void computeBroadband(AnalysisFrame& out);
	void computeBands(AnalysisFrame& out);
	void computeOnsets(AnalysisFrame& out, float dtSec);
	void computeBeat(AnalysisFrame& out, float dtSec);
	void computeHPSS(AnalysisFrame& out);
	static float computeMedian(std::vector<float>& scratch);
};


