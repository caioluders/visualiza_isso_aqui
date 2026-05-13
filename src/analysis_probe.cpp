#include "AudioAnalysis.h"
#include "AudioFilePlayer.h"
#include "FeatureExtractor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Summary {
	unsigned int sampleRate = 0;
	unsigned int channels = 0;
	size_t blocks = 0;
	size_t frames = 0;
	double audioSeconds = 0.0;
	double processingSeconds = 0.0;
	double maxBlockProcessingSeconds = 0.0;
	double p95BlockProcessingSeconds = 0.0;
	double blockAudioSeconds = 0.0;
	double avgRms = 0.0;
	double maxRms = 0.0;
	double avgLow = 0.0;
	double avgMid = 0.0;
	double avgHigh = 0.0;
	double maxOnset = 0.0;
	int beatCount = 0;
	double avgBeatEnvelope = 0.0;
	double lastBpm = 0.0;
	double maxFlux = 0.0;
	double avgPercussiveRatio = 0.0;
	double avgKickImpact = 0.0;
	double avgBassBody = 0.0;
	double avgLeadPresence = 0.0;
	double avgAirPresence = 0.0;
	double maxTension = 0.0;
	int dropEvents = 0;
};

void printUsage(const char* argv0) {
	std::fprintf(
		stderr,
		"Usage: %s --wav path [--block 1024] [--limit-seconds N] [--frames-jsonl]\n",
		argv0
	);
}

double secondsNow() {
	using clock = std::chrono::steady_clock;
	return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void printFloatArrayJson(const std::vector<float>& values) {
	std::printf("[");
	for (size_t i = 0; i < values.size(); ++i) {
		if (i > 0) std::printf(", ");
		std::printf("%.8f", values[i]);
	}
	std::printf("]");
}

void printFrameJsonLine(
	size_t blockIndex,
	double timestampSec,
	double blockDurationSec,
	const FeatureExtractor::Features& f,
	const AudioAnalysis::AnalysisFrame& af
) {
	std::printf("{");
	std::printf("\"block_index\": %zu, ", blockIndex);
	std::printf("\"timestamp_sec\": %.8f, ", timestampSec);
	std::printf("\"block_duration_sec\": %.8f, ", blockDurationSec);
	std::printf("\"rms\": %.8f, ", f.rms);
	std::printf("\"band_low\": %.8f, ", f.bandLow);
	std::printf("\"band_mid\": %.8f, ", f.bandMid);
	std::printf("\"band_high\": %.8f, ", f.bandHigh);
	std::printf("\"onset\": %.8f, ", f.onset);
	std::printf("\"spectral_centroid_hz\": %.8f, ", af.spectralCentroidHz);
	std::printf("\"spectral_flux\": %.8f, ", af.spectralFlux);
	std::printf("\"beat_triggered\": %s, ", af.beatTriggered ? "true" : "false");
	std::printf("\"beat_envelope\": %.8f, ", af.beatEnvelope);
	std::printf("\"bpm\": %.8f, ", af.bpm);
	std::printf("\"percussive_energy\": %.8f, ", af.percussiveEnergy);
	std::printf("\"harmonic_energy\": %.8f, ", af.harmonicEnergy);
	std::printf("\"percussive_ratio\": %.8f, ", af.percussiveRatio);
	std::printf("\"kick_impact\": %.8f, ", af.kickImpact);
	std::printf("\"snare_impact\": %.8f, ", af.snareImpact);
	std::printf("\"hat_tick\": %.8f, ", af.hatTick);
	std::printf("\"beat_pulse\": %.8f, ", af.beatPulse);
	std::printf("\"sub_body\": %.8f, ", af.subBody);
	std::printf("\"bass_body\": %.8f, ", af.bassBody);
	std::printf("\"harmonic_body\": %.8f, ", af.harmonicBody);
	std::printf("\"lead_presence\": %.8f, ", af.leadPresence);
	std::printf("\"air_presence\": %.8f, ", af.airPresence);
	std::printf("\"transient_density\": %.8f, ", af.transientDensity);
	std::printf("\"novelty\": %.8f, ", af.novelty);
	std::printf("\"brightness\": %.8f, ", af.brightness);
	std::printf("\"percussive_focus\": %.8f, ", af.percussiveFocus);
	std::printf("\"energy_level\": %.8f, ", af.energyLevel);
	std::printf("\"tension\": %.8f, ", af.tension);
	std::printf("\"release\": %.8f, ", af.release);
	std::printf("\"drop_event\": %.8f, ", af.dropEvent);
	std::printf("\"section_change\": %.8f, ", af.sectionChange);
	std::printf("\"role_kick_center_norm\": %.8f, ", af.roleKickCenterNorm);
	std::printf("\"role_bass_center_norm\": %.8f, ", af.roleBassCenterNorm);
	std::printf("\"role_lead_center_norm\": %.8f, ", af.roleLeadCenterNorm);
	std::printf("\"role_air_center_norm\": %.8f, ", af.roleAirCenterNorm);
	std::printf("\"band_energy\": ");
	printFloatArrayJson(af.bandEnergy);
	std::printf(", \"band_energy_norm\": ");
	printFloatArrayJson(af.bandEnergyNorm);
	std::printf(", \"band_onset\": ");
	printFloatArrayJson(af.bandOnset);
	std::printf("}\n");
}

} // namespace

int main(int argc, char** argv) {
	std::string wavPath;
	size_t blockFrames = 1024;
	double limitSeconds = 0.0;
	bool framesJsonl = false;

	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
			wavPath = argv[++i];
		} else if (std::strcmp(argv[i], "--block") == 0 && i + 1 < argc) {
			blockFrames = std::max<size_t>(1, (size_t)std::strtoull(argv[++i], nullptr, 10));
		} else if (std::strcmp(argv[i], "--limit-seconds") == 0 && i + 1 < argc) {
			limitSeconds = std::max(0.0, std::atof(argv[++i]));
		} else if (std::strcmp(argv[i], "--frames-jsonl") == 0) {
			framesJsonl = true;
		} else {
			printUsage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (wavPath.empty()) {
		printUsage(argv[0]);
		return EXIT_FAILURE;
	}

	AudioFilePlayer player;
	if (!player.loadWav(wavPath)) {
		std::fprintf(stderr, "Failed to load WAV: %s\n", wavPath.c_str());
		return EXIT_FAILURE;
	}
	player.setLoop(false);

	const unsigned int sampleRate = player.getSampleRate();
	const unsigned int channels = std::max(1u, player.getChannels());
	FeatureExtractor featureExtractor(sampleRate);
	AudioAnalysis::Config cfg;
	cfg.fftSize = (int)blockFrames;
	cfg.numBands = 16;
	AudioAnalysis analyzer(sampleRate, cfg);

	Summary s;
	s.sampleRate = sampleRate;
	s.channels = channels;
	const double blockDurationSec = (double)blockFrames / (double)sampleRate;

	std::vector<float> block;
	std::vector<double> blockProcessingSeconds;
	while (player.isPlaying()) {
		if (limitSeconds > 0.0 && s.audioSeconds >= limitSeconds) break;
		const double t0 = secondsNow();
		size_t got = player.getNext(blockFrames, block);
		if (got == 0 || block.empty()) break;
		auto f = featureExtractor.compute(block, channels);
		auto af = analyzer.processInterleaved(block, channels);
		const double t1 = secondsNow();
		const double blockProcessing = t1 - t0;

		s.blocks += 1;
		s.frames += got;
		s.audioSeconds = (double)s.frames / (double)sampleRate;
		s.processingSeconds += blockProcessing;
		s.maxBlockProcessingSeconds = std::max(s.maxBlockProcessingSeconds, blockProcessing);
		blockProcessingSeconds.push_back(blockProcessing);
		s.avgRms += f.rms;
		s.maxRms = std::max(s.maxRms, (double)f.rms);
		s.avgLow += f.bandLow;
		s.avgMid += f.bandMid;
		s.avgHigh += f.bandHigh;
		s.maxOnset = std::max(s.maxOnset, (double)f.onset);
		if (af.beatTriggered) s.beatCount += 1;
		s.avgBeatEnvelope += af.beatEnvelope;
		if (af.bpm > 0.0f) s.lastBpm = af.bpm;
		s.maxFlux = std::max(s.maxFlux, (double)af.spectralFlux);
		s.avgPercussiveRatio += af.percussiveRatio;
		s.avgKickImpact += af.kickImpact;
		s.avgBassBody += af.bassBody;
		s.avgLeadPresence += af.leadPresence;
		s.avgAirPresence += af.airPresence;
		s.maxTension = std::max(s.maxTension, (double)af.tension);
		if (af.dropEvent > 0.6f) s.dropEvents += 1;

		if (framesJsonl) {
			double timestampSec = (double)(s.frames - got) / (double)sampleRate;
			printFrameJsonLine(s.blocks - 1, timestampSec, blockDurationSec, f, af);
		}
	}

	if (s.blocks > 0) {
		double denom = (double)s.blocks;
		s.avgRms /= denom;
		s.avgLow /= denom;
		s.avgMid /= denom;
		s.avgHigh /= denom;
		s.avgBeatEnvelope /= denom;
		s.avgPercussiveRatio /= denom;
		s.avgKickImpact /= denom;
		s.avgBassBody /= denom;
		s.avgLeadPresence /= denom;
		s.avgAirPresence /= denom;
		s.blockAudioSeconds = blockDurationSec;
		std::sort(blockProcessingSeconds.begin(), blockProcessingSeconds.end());
		size_t p95Index = (size_t)std::min<double>((double)blockProcessingSeconds.size() - 1.0, std::ceil((double)blockProcessingSeconds.size() * 0.95) - 1.0);
		s.p95BlockProcessingSeconds = blockProcessingSeconds[p95Index];
	}

	if (framesJsonl) {
		return s.blocks > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	double realtimeRatio = s.audioSeconds > 0.0 ? s.processingSeconds / s.audioSeconds : 0.0;
	double p95BlockRealtimeRatio = s.blockAudioSeconds > 0.0 ? s.p95BlockProcessingSeconds / s.blockAudioSeconds : 0.0;
	double maxBlockRealtimeRatio = s.blockAudioSeconds > 0.0 ? s.maxBlockProcessingSeconds / s.blockAudioSeconds : 0.0;
	std::printf("{\n");
	std::printf("  \"file\": \"%s\",\n", wavPath.c_str());
	std::printf("  \"sample_rate\": %u,\n", s.sampleRate);
	std::printf("  \"channels\": %u,\n", s.channels);
	std::printf("  \"blocks\": %zu,\n", s.blocks);
	std::printf("  \"audio_seconds\": %.6f,\n", s.audioSeconds);
	std::printf("  \"processing_seconds\": %.6f,\n", s.processingSeconds);
	std::printf("  \"realtime_ratio\": %.8f,\n", realtimeRatio);
	std::printf("  \"block_audio_seconds\": %.8f,\n", s.blockAudioSeconds);
	std::printf("  \"p95_block_processing_seconds\": %.8f,\n", s.p95BlockProcessingSeconds);
	std::printf("  \"max_block_processing_seconds\": %.8f,\n", s.maxBlockProcessingSeconds);
	std::printf("  \"p95_block_realtime_ratio\": %.8f,\n", p95BlockRealtimeRatio);
	std::printf("  \"max_block_realtime_ratio\": %.8f,\n", maxBlockRealtimeRatio);
	std::printf("  \"avg_rms\": %.8f,\n", s.avgRms);
	std::printf("  \"max_rms\": %.8f,\n", s.maxRms);
	std::printf("  \"avg_low\": %.8f,\n", s.avgLow);
	std::printf("  \"avg_mid\": %.8f,\n", s.avgMid);
	std::printf("  \"avg_high\": %.8f,\n", s.avgHigh);
	std::printf("  \"max_onset\": %.8f,\n", s.maxOnset);
	std::printf("  \"beat_count\": %d,\n", s.beatCount);
	std::printf("  \"avg_beat_envelope\": %.8f,\n", s.avgBeatEnvelope);
	std::printf("  \"last_bpm\": %.8f,\n", s.lastBpm);
	std::printf("  \"max_flux\": %.8f,\n", s.maxFlux);
	std::printf("  \"avg_percussive_ratio\": %.8f,\n", s.avgPercussiveRatio);
	std::printf("  \"avg_kick_impact\": %.8f,\n", s.avgKickImpact);
	std::printf("  \"avg_bass_body\": %.8f,\n", s.avgBassBody);
	std::printf("  \"avg_lead_presence\": %.8f,\n", s.avgLeadPresence);
	std::printf("  \"avg_air_presence\": %.8f,\n", s.avgAirPresence);
	std::printf("  \"max_tension\": %.8f,\n", s.maxTension);
	std::printf("  \"drop_event_count\": %d\n", s.dropEvents);
	std::printf("}\n");

	return s.blocks > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
