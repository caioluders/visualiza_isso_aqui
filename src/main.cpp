#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <array>
#include <csignal>
#include <cstdint>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#if defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif
#if defined(__linux__) || defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "ShaderProgram.h"
#include "AudioInput.h"
#include "AudioFilePlayer.h"
#include "AudioAnalysis.h"
#include "FileDialog.h"
#include "FFglitchPlayer.h"

#ifndef VISUALIZA_HAS_FFGLITCH
#define VISUALIZA_HAS_FFGLITCH 0
#endif

static void glfwErrorCallback(int error, const char* description) {
	std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

namespace {

static float clamp01(float x) {
	return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static float softCompress(float value, float knee) {
	if (value <= 0.0f) return 0.0f;
	const float safeKnee = std::max(1e-6f, knee);
	return value / (value + safeKnee);
}

static std::string formatInputDeviceLabel(const AudioInput::DeviceInfo& d) {
	std::string label = d.name;
	label += " [";
	label += std::to_string(d.inputChannels);
	label += (d.inputChannels == 1 ? " ch" : " ch");
	if (d.inputChannels > 0 && d.outputChannels > 0) {
		label += ", playback monitor";
	} else if (d.inputChannels == 1) {
		label += ", mono input";
	} else {
		label += ", input";
	}
	if (d.isDefault) label += ", default";
	label += "]";
	return label;
}

struct ShaderAudioMetrics {
	float rms = 0.0f;
	float bandLow = 0.0f;
	float bandMid = 0.0f;
	float bandHigh = 0.0f;
	float onset = 0.0f;
	float flux = 0.0f;
	float percE = 0.0f;
	float harmE = 0.0f;
};

struct ShaderUiMeta {
	bool hasMeta = false;
	bool hidden = false;
	bool hasRange = false;
	bool hasDefault = false;
	float minValue = -10.0f;
	float maxValue = 10.0f;
	float defaultValue = 0.0f;
	std::string label;
	std::string group;
};

static std::string trimCopy(const std::string& s) {
	size_t start = 0;
	while (start < s.size() && std::isspace((unsigned char)s[start])) ++start;
	size_t end = s.size();
	while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
	return s.substr(start, end - start);
}

static std::unordered_map<std::string, ShaderUiMeta> parseShaderUiMetadata(const std::string& shaderPath) {
	std::unordered_map<std::string, ShaderUiMeta> meta;
	std::ifstream in(shaderPath);
	if (!in) return meta;
	std::string line;
	while (std::getline(in, line)) {
		const size_t uiPos = line.find("@ui");
		if (uiPos == std::string::npos) continue;
		const size_t uniformPos = line.find("uniform");
		if (uniformPos == std::string::npos || uniformPos > uiPos) continue;
		std::string decl = trimCopy(line.substr(uniformPos, uiPos - uniformPos));
		std::istringstream declStream(decl);
		std::string uniformKeyword, typeName, uniformName;
		declStream >> uniformKeyword >> typeName >> uniformName;
		if (uniformKeyword != "uniform" || uniformName.empty()) continue;
		size_t semicolon = uniformName.find(';');
		if (semicolon != std::string::npos) uniformName = uniformName.substr(0, semicolon);
		size_t bracket = uniformName.find('[');
		if (bracket != std::string::npos) uniformName = uniformName.substr(0, bracket);
		if (uniformName.empty()) continue;

		ShaderUiMeta item;
		item.hasMeta = true;
		std::string spec = line.substr(uiPos + 3);
		std::istringstream specStream(spec);
		std::string token;
		while (specStream >> token) {
			const size_t eq = token.find('=');
			if (eq == std::string::npos) {
				if (token == "hide") item.hidden = true;
				continue;
			}
			const std::string key = token.substr(0, eq);
			const std::string value = token.substr(eq + 1);
			if (key == "min") {
				item.hasRange = true;
				item.minValue = std::strtof(value.c_str(), nullptr);
			} else if (key == "max") {
				item.hasRange = true;
				item.maxValue = std::strtof(value.c_str(), nullptr);
			} else if (key == "default") {
				item.hasDefault = true;
				item.defaultValue = std::strtof(value.c_str(), nullptr);
			} else if (key == "label") {
				item.label = value;
				std::replace(item.label.begin(), item.label.end(), '_', ' ');
			} else if (key == "group") {
				item.group = value;
				std::replace(item.group.begin(), item.group.end(), '_', ' ');
			}
		}
		meta[uniformName] = item;
	}
	return meta;
}

static ShaderAudioMetrics deriveShaderAudioMetrics(const AudioAnalysis::AnalysisFrame& af) {
	ShaderAudioMetrics out{};
	out.rms = clamp01(0.65f * af.energyLevel + 0.35f * af.rms * 2.0f);
	out.bandLow = clamp01(0.55f * af.subBody + 0.45f * af.bassBody);
	out.bandMid = clamp01(0.60f * af.harmonicBody + 0.40f * af.leadPresence);
	out.bandHigh = clamp01(0.55f * af.airPresence + 0.45f * af.brightness);
	out.onset = clamp01(0.45f * af.kickImpact + 0.30f * af.snareImpact + 0.25f * af.hatTick);
	out.flux = clamp01(0.55f * af.novelty + 0.45f * softCompress(af.spectralFlux, 75.0f));
	const float totalHpssEnergy = af.percussiveEnergy + af.harmonicEnergy;
	const float totalEnergyNorm = softCompress(totalHpssEnergy, 120.0f);
	out.percE = clamp01(0.55f * af.percussiveFocus + 0.45f * totalEnergyNorm * clamp01(af.percussiveRatio));
	out.harmE = clamp01(0.65f * af.harmonicBody + 0.35f * totalEnergyNorm * (1.0f - clamp01(af.percussiveRatio)));
	return out;
}

static void drawCompactWaveform(const char* label, const std::vector<float>& samples, float height) {
	if (samples.empty()) return;
	ImGui::TextUnformatted(label);
	ImVec2 avail = ImGui::GetContentRegionAvail();
	ImVec2 size(avail.x, height);
	ImVec2 p0 = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(label, size);
	ImVec2 p1(p0.x + size.x, p0.y + size.y);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 18, 255), 4.0f);
	dl->AddRect(p0, p1, IM_COL32(48, 54, 66, 255), 4.0f);
	dl->AddLine(ImVec2(p0.x, (p0.y + p1.y) * 0.5f), ImVec2(p1.x, (p0.y + p1.y) * 0.5f), IM_COL32(60, 66, 80, 255), 1.0f);

	std::vector<ImVec2> points;
	points.reserve(samples.size());
	const float midY = (p0.y + p1.y) * 0.5f;
	const float ampY = (size.y * 0.5f) - 4.0f;
	const float dx = samples.size() > 1 ? size.x / float(samples.size() - 1) : 0.0f;
	for (size_t i = 0; i < samples.size(); ++i) {
		const float x = p0.x + dx * float(i);
		const float clamped = std::max(-1.0f, std::min(1.0f, samples[i]));
		points.emplace_back(x, midY - clamped * ampY);
	}
	dl->AddPolyline(points.data(), (int)points.size(), IM_COL32(122, 200, 255, 255), 0, 1.5f);
}

static void drawCompactBandBars(const char* label, const std::vector<float>& values, ImU32 color) {
	if (values.empty()) return;
	ImGui::TextUnformatted(label);
	ImVec2 avail = ImGui::GetContentRegionAvail();
	ImVec2 size(avail.x, 72.0f);
	ImVec2 p0 = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(label, size);
	ImVec2 p1(p0.x + size.x, p0.y + size.y);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 18, 255), 4.0f);
	dl->AddRect(p0, p1, IM_COL32(48, 54, 66, 255), 4.0f);
	const float gap = 3.0f;
	const float innerW = size.x - gap * float(values.size() + 1);
	const float barW = innerW / float(values.size());
	for (size_t i = 0; i < values.size(); ++i) {
		const float v = clamp01(values[i]);
		const float x0 = p0.x + gap + float(i) * (barW + gap);
		const float x1 = x0 + barW;
		const float y1 = p1.y - gap;
		const float y0 = y1 - v * (size.y - gap * 2.0f);
		dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), color, 2.0f);
	}
}

static void pushContentWrapPos() {
	ImGui::PushTextWrapPos(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x);
}

struct AudioTuningProfile {
	float pulseGain = 1.00f;
	float bodyGain = 1.00f;
	float detailGain = 1.00f;
	float macroGain = 1.00f;
	float bassGain = 1.00f;
	float harmonicGain = 1.00f;
	float leadGain = 1.00f;
	float airGain = 1.00f;
	float noveltyGain = 1.00f;
	float brightnessGain = 1.00f;
	float percussiveGain = 1.00f;
	float tensionGain = 1.00f;
	float releaseGain = 1.00f;
	float pulseKnee = 0.90f;
	float bodyKnee = 0.90f;
	float detailKnee = 0.85f;
	float macroKnee = 0.95f;
	float pulseFloor = 0.00f;
	float bodyFloor = 0.00f;
	float detailFloor = 0.00f;
	float macroFloor = 0.00f;
	bool autoStyleEnabled = false;
	float autoStyleBlend = 0.60f;
	float autoStyleResponseSec = 1.75f;
};

static AudioTuningProfile makeDefaultAudioTuningProfile() {
	return AudioTuningProfile{};
}

struct StyleProfileWeights {
	float houseGroove = 0.25f;
	float technoRumble = 0.25f;
	float tranceLift = 0.25f;
	float breakdownSparse = 0.25f;
	float activity = 0.0f;
};

struct StyleAutoTuningState {
	StyleProfileWeights raw{};
	StyleProfileWeights smoothed{};
};

static float smoothTowards(float current, float target, float dtSec, float responseSec) {
	if (dtSec <= 0.0f) return current;
	const float safeResponse = std::max(0.05f, responseSec);
	const float alpha = 1.0f - std::exp(-dtSec / safeResponse);
	return current + (target - current) * clamp01(alpha);
}

static void normalizeStyleWeights(StyleProfileWeights& weights) {
	weights.houseGroove = std::max(0.0f, weights.houseGroove);
	weights.technoRumble = std::max(0.0f, weights.technoRumble);
	weights.tranceLift = std::max(0.0f, weights.tranceLift);
	weights.breakdownSparse = std::max(0.0f, weights.breakdownSparse);
	weights.activity = clamp01(weights.activity);
	const float sum = weights.houseGroove + weights.technoRumble + weights.tranceLift + weights.breakdownSparse;
	if (sum <= 1e-6f) {
		weights.houseGroove = 0.25f;
		weights.technoRumble = 0.25f;
		weights.tranceLift = 0.25f;
		weights.breakdownSparse = 0.25f;
		return;
	}
	const float inv = 1.0f / sum;
	weights.houseGroove *= inv;
	weights.technoRumble *= inv;
	weights.tranceLift *= inv;
	weights.breakdownSparse *= inv;
}

static const char* dominantStyleLabel(const StyleProfileWeights& weights) {
	float best = weights.houseGroove;
	const char* label = "House Groove";
	if (weights.technoRumble > best) {
		best = weights.technoRumble;
		label = "Techno Rumble";
	}
	if (weights.tranceLift > best) {
		best = weights.tranceLift;
		label = "Trance Lift";
	}
	if (weights.breakdownSparse > best) {
		label = "Breakdown Sparse";
	}
	return label;
}

static StyleProfileWeights detectStyleProfile(
	const AudioAnalysis::AnalysisFrame& af,
	StyleAutoTuningState& state,
	float dtSec,
	float responseSec
) {
	StyleProfileWeights raw{};
	const float kick = clamp01(std::max(af.kickImpact, af.beatPulse));
	const float bass = clamp01(std::max(af.subBody, af.bassBody));
	const float harmonic = clamp01(af.harmonicBody);
	const float lead = clamp01(af.leadPresence);
	const float air = clamp01(af.airPresence);
	const float perc = clamp01(std::max(af.hatTick, af.percussiveFocus));
	const float energy = clamp01(af.energyLevel);
	const float tension = clamp01(af.tension);
	const float release = clamp01(af.release);
	const float brightness = clamp01(af.brightness);
	const float transient = clamp01(af.transientDensity);

	const float lowEndWeight = clamp01(0.58f * bass + 0.27f * kick + 0.15f * energy);
	const float harmonicWall = clamp01(0.55f * harmonic + 0.20f * lead + 0.15f * energy + 0.10f * tension);
	const float topEndLift = clamp01(0.45f * air + 0.30f * brightness + 0.25f * perc);
	const float sparseBreakdown = clamp01(
		0.35f * release +
		0.25f * (1.0f - kick) +
		0.20f * (1.0f - bass) +
		0.20f * (1.0f - transient)
	);
	const float fourOnFloor = clamp01(
		0.40f * kick +
		0.25f * clamp01(af.beatPulse) +
		0.20f * energy +
		0.15f * clamp01(1.0f - release)
	);

	raw.activity = clamp01(
		0.30f * energy +
		0.25f * lowEndWeight +
		0.20f * harmonic +
		0.15f * topEndLift +
		0.10f * transient
	);
	raw.houseGroove = clamp01(
		0.32f * fourOnFloor +
		0.24f * bass +
		0.20f * harmonic +
		0.14f * perc +
		0.10f * clamp01(1.0f - tension)
	);
	raw.technoRumble = clamp01(
		0.33f * lowEndWeight +
		0.22f * kick +
		0.18f * perc +
		0.15f * clamp01(1.0f - lead) +
		0.12f * clamp01(1.0f - brightness)
	);
	raw.tranceLift = clamp01(
		0.28f * harmonicWall +
		0.24f * lead +
		0.18f * air +
		0.16f * tension +
		0.14f * brightness
	);
	raw.breakdownSparse = clamp01(
		0.38f * sparseBreakdown +
		0.20f * harmonic +
		0.16f * air +
		0.14f * release +
		0.12f * clamp01(1.0f - kick)
	);

	raw.technoRumble *= (1.0f - 0.18f * lead);
	raw.tranceLift *= (0.80f + 0.20f * clamp01(1.0f - lowEndWeight));
	raw.breakdownSparse *= (1.0f - 0.25f * fourOnFloor);
	normalizeStyleWeights(raw);

	state.raw = raw;
	state.smoothed.houseGroove = smoothTowards(state.smoothed.houseGroove, raw.houseGroove, dtSec, responseSec);
	state.smoothed.technoRumble = smoothTowards(state.smoothed.technoRumble, raw.technoRumble, dtSec, responseSec);
	state.smoothed.tranceLift = smoothTowards(state.smoothed.tranceLift, raw.tranceLift, dtSec, responseSec);
	state.smoothed.breakdownSparse = smoothTowards(state.smoothed.breakdownSparse, raw.breakdownSparse, dtSec, responseSec);
	state.smoothed.activity = smoothTowards(state.smoothed.activity, raw.activity, dtSec, responseSec);
	normalizeStyleWeights(state.smoothed);
	return state.smoothed;
}

static AudioTuningProfile makeHouseGrooveStyleTuningProfile() {
	AudioTuningProfile out = makeDefaultAudioTuningProfile();
	out.pulseGain = 1.08f;
	out.bodyGain = 1.05f;
	out.detailGain = 0.92f;
	out.macroGain = 0.95f;
	out.bassGain = 1.18f;
	out.harmonicGain = 1.05f;
	out.leadGain = 0.92f;
	out.airGain = 0.94f;
	out.percussiveGain = 1.06f;
	out.tensionGain = 0.88f;
	out.releaseGain = 0.92f;
	out.pulseKnee = 0.80f;
	out.bodyKnee = 0.86f;
	return out;
}

static AudioTuningProfile makeTechnoRumbleStyleTuningProfile() {
	AudioTuningProfile out = makeDefaultAudioTuningProfile();
	out.pulseGain = 1.14f;
	out.bodyGain = 1.12f;
	out.detailGain = 0.86f;
	out.macroGain = 0.98f;
	out.bassGain = 1.34f;
	out.harmonicGain = 0.92f;
	out.leadGain = 0.78f;
	out.airGain = 0.82f;
	out.percussiveGain = 1.10f;
	out.brightnessGain = 0.88f;
	out.tensionGain = 0.92f;
	out.releaseGain = 0.84f;
	out.pulseKnee = 0.74f;
	out.bodyKnee = 0.82f;
	out.detailKnee = 0.92f;
	return out;
}

static AudioTuningProfile makeTranceLiftStyleTuningProfile() {
	AudioTuningProfile out = makeDefaultAudioTuningProfile();
	out.pulseGain = 0.94f;
	out.bodyGain = 1.18f;
	out.detailGain = 1.14f;
	out.macroGain = 1.16f;
	out.bassGain = 0.96f;
	out.harmonicGain = 1.22f;
	out.leadGain = 1.34f;
	out.airGain = 1.26f;
	out.noveltyGain = 1.08f;
	out.brightnessGain = 1.14f;
	out.percussiveGain = 0.90f;
	out.tensionGain = 1.28f;
	out.releaseGain = 1.04f;
	out.bodyKnee = 0.78f;
	out.detailKnee = 0.76f;
	out.macroKnee = 0.82f;
	return out;
}

static AudioTuningProfile makeBreakdownSparseStyleTuningProfile() {
	AudioTuningProfile out = makeDefaultAudioTuningProfile();
	out.pulseGain = 0.78f;
	out.bodyGain = 1.14f;
	out.detailGain = 1.06f;
	out.macroGain = 1.02f;
	out.bassGain = 0.82f;
	out.harmonicGain = 1.18f;
	out.leadGain = 1.12f;
	out.airGain = 1.18f;
	out.brightnessGain = 1.10f;
	out.percussiveGain = 0.82f;
	out.tensionGain = 0.94f;
	out.releaseGain = 1.30f;
	out.pulseKnee = 0.98f;
	out.bodyKnee = 0.84f;
	out.detailKnee = 0.82f;
	out.macroKnee = 0.90f;
	return out;
}

static void applyScaledAudioTuningDelta(AudioTuningProfile& dst, const AudioTuningProfile& target, const AudioTuningProfile& base, float scale) {
	if (scale <= 0.0f) return;
	dst.pulseGain += (target.pulseGain - base.pulseGain) * scale;
	dst.bodyGain += (target.bodyGain - base.bodyGain) * scale;
	dst.detailGain += (target.detailGain - base.detailGain) * scale;
	dst.macroGain += (target.macroGain - base.macroGain) * scale;
	dst.bassGain += (target.bassGain - base.bassGain) * scale;
	dst.harmonicGain += (target.harmonicGain - base.harmonicGain) * scale;
	dst.leadGain += (target.leadGain - base.leadGain) * scale;
	dst.airGain += (target.airGain - base.airGain) * scale;
	dst.noveltyGain += (target.noveltyGain - base.noveltyGain) * scale;
	dst.brightnessGain += (target.brightnessGain - base.brightnessGain) * scale;
	dst.percussiveGain += (target.percussiveGain - base.percussiveGain) * scale;
	dst.tensionGain += (target.tensionGain - base.tensionGain) * scale;
	dst.releaseGain += (target.releaseGain - base.releaseGain) * scale;
	dst.pulseKnee += (target.pulseKnee - base.pulseKnee) * scale;
	dst.bodyKnee += (target.bodyKnee - base.bodyKnee) * scale;
	dst.detailKnee += (target.detailKnee - base.detailKnee) * scale;
	dst.macroKnee += (target.macroKnee - base.macroKnee) * scale;
	dst.pulseFloor += (target.pulseFloor - base.pulseFloor) * scale;
	dst.bodyFloor += (target.bodyFloor - base.bodyFloor) * scale;
	dst.detailFloor += (target.detailFloor - base.detailFloor) * scale;
	dst.macroFloor += (target.macroFloor - base.macroFloor) * scale;
}

static AudioTuningProfile clampAudioTuningProfile(const AudioTuningProfile& source) {
	AudioTuningProfile out = source;
	out.pulseGain = std::clamp(out.pulseGain, 0.2f, 3.0f);
	out.bodyGain = std::clamp(out.bodyGain, 0.2f, 3.0f);
	out.detailGain = std::clamp(out.detailGain, 0.2f, 3.0f);
	out.macroGain = std::clamp(out.macroGain, 0.2f, 3.0f);
	out.bassGain = std::clamp(out.bassGain, 0.2f, 3.0f);
	out.harmonicGain = std::clamp(out.harmonicGain, 0.2f, 3.0f);
	out.leadGain = std::clamp(out.leadGain, 0.2f, 3.0f);
	out.airGain = std::clamp(out.airGain, 0.2f, 3.0f);
	out.noveltyGain = std::clamp(out.noveltyGain, 0.2f, 3.0f);
	out.brightnessGain = std::clamp(out.brightnessGain, 0.2f, 3.0f);
	out.percussiveGain = std::clamp(out.percussiveGain, 0.2f, 3.0f);
	out.tensionGain = std::clamp(out.tensionGain, 0.2f, 3.0f);
	out.releaseGain = std::clamp(out.releaseGain, 0.2f, 3.0f);
	out.pulseKnee = std::clamp(out.pulseKnee, 0.2f, 2.0f);
	out.bodyKnee = std::clamp(out.bodyKnee, 0.2f, 2.0f);
	out.detailKnee = std::clamp(out.detailKnee, 0.2f, 2.0f);
	out.macroKnee = std::clamp(out.macroKnee, 0.2f, 2.0f);
	out.pulseFloor = std::clamp(out.pulseFloor, 0.0f, 0.6f);
	out.bodyFloor = std::clamp(out.bodyFloor, 0.0f, 0.6f);
	out.detailFloor = std::clamp(out.detailFloor, 0.0f, 0.6f);
	out.macroFloor = std::clamp(out.macroFloor, 0.0f, 0.6f);
	out.autoStyleBlend = clamp01(out.autoStyleBlend);
	out.autoStyleResponseSec = std::clamp(out.autoStyleResponseSec, 0.15f, 6.0f);
	return out;
}

static AudioTuningProfile buildEffectiveAudioTuning(const AudioTuningProfile& manual, const StyleProfileWeights& weights) {
	AudioTuningProfile effective = manual;
	if (!manual.autoStyleEnabled) return effective;
	const AudioTuningProfile base = makeDefaultAudioTuningProfile();
	const float styleStrength = clamp01(manual.autoStyleBlend * weights.activity);
	applyScaledAudioTuningDelta(effective, makeHouseGrooveStyleTuningProfile(), base, styleStrength * weights.houseGroove);
	applyScaledAudioTuningDelta(effective, makeTechnoRumbleStyleTuningProfile(), base, styleStrength * weights.technoRumble);
	applyScaledAudioTuningDelta(effective, makeTranceLiftStyleTuningProfile(), base, styleStrength * weights.tranceLift);
	applyScaledAudioTuningDelta(effective, makeBreakdownSparseStyleTuningProfile(), base, styleStrength * weights.breakdownSparse);
	effective.autoStyleEnabled = manual.autoStyleEnabled;
	effective.autoStyleBlend = manual.autoStyleBlend;
	effective.autoStyleResponseSec = manual.autoStyleResponseSec;
	return clampAudioTuningProfile(effective);
}

struct LookPreset {
	std::string fragmentPath;
	std::string processingSketchPath;
	bool useFFglitch = false;
	bool useProcessing = false;
	bool shaderFeedback = false;
	bool hotReloadShaders = true;
	int previewSize = 512;
	AudioTuningProfile audioTuning = makeDefaultAudioTuningProfile();
};

struct WorkspacePreset {
	LookPreset look;
	float smoothing = 0.35f;
	float bandsGain = 1.0f;
	float onsetsGain = 1.0f;
	int selectedMonitor = 0;
	bool showSource = true;
	bool showVisual = true;
	bool showDiagnostics = true;
	bool showExpert = true;
	bool showOutput = true;
	bool showViewport = true;
	std::string layoutFile;
};

struct UiPreferences {
	bool restoreLastSessionOnStartup = false;
	std::string startupWorkspace;
};

static float shapeTunedSignal(float value, float gain, float knee, float floorValue) {
	const float lifted = std::max(0.0f, value - floorValue);
	return clamp01(softCompress(lifted * std::max(0.0f, gain), std::max(0.05f, knee)));
}

static AudioAnalysis::AnalysisFrame applyAudioTuning(const AudioAnalysis::AnalysisFrame& source, const AudioTuningProfile& tuning) {
	AudioAnalysis::AnalysisFrame out = source;
	out.kickImpact = shapeTunedSignal(source.kickImpact, tuning.pulseGain, tuning.pulseKnee, tuning.pulseFloor);
	out.snareImpact = shapeTunedSignal(source.snareImpact, tuning.pulseGain, tuning.pulseKnee, tuning.pulseFloor);
	out.hatTick = shapeTunedSignal(source.hatTick, tuning.detailGain, tuning.detailKnee, tuning.detailFloor);
	out.beatPulse = shapeTunedSignal(source.beatPulse, tuning.pulseGain, tuning.pulseKnee, tuning.pulseFloor);
	out.subBody = shapeTunedSignal(source.subBody, tuning.bodyGain * tuning.bassGain, tuning.bodyKnee, tuning.bodyFloor);
	out.bassBody = shapeTunedSignal(source.bassBody, tuning.bodyGain * tuning.bassGain, tuning.bodyKnee, tuning.bodyFloor);
	out.harmonicBody = shapeTunedSignal(source.harmonicBody, tuning.bodyGain * tuning.harmonicGain, tuning.bodyKnee, tuning.bodyFloor);
	out.leadPresence = shapeTunedSignal(source.leadPresence, tuning.bodyGain * tuning.leadGain, tuning.bodyKnee, tuning.bodyFloor);
	out.airPresence = shapeTunedSignal(source.airPresence, tuning.bodyGain * tuning.airGain, tuning.bodyKnee, tuning.bodyFloor);
	out.transientDensity = shapeTunedSignal(source.transientDensity, tuning.detailGain, tuning.detailKnee, tuning.detailFloor);
	out.novelty = shapeTunedSignal(source.novelty, tuning.detailGain * tuning.noveltyGain, tuning.detailKnee, tuning.detailFloor);
	out.brightness = shapeTunedSignal(source.brightness, tuning.detailGain * tuning.brightnessGain, tuning.detailKnee, tuning.detailFloor);
	out.percussiveFocus = shapeTunedSignal(source.percussiveFocus, tuning.detailGain * tuning.percussiveGain, tuning.detailKnee, tuning.detailFloor);
	out.energyLevel = shapeTunedSignal(source.energyLevel, tuning.macroGain, tuning.macroKnee, tuning.macroFloor);
	out.tension = shapeTunedSignal(source.tension, tuning.macroGain * tuning.tensionGain, tuning.macroKnee, tuning.macroFloor);
	out.release = shapeTunedSignal(source.release, tuning.macroGain * tuning.releaseGain, tuning.macroKnee, tuning.macroFloor);
	out.dropEvent = shapeTunedSignal(source.dropEvent, tuning.pulseGain, tuning.pulseKnee, tuning.pulseFloor);
	out.sectionChange = shapeTunedSignal(source.sectionChange, tuning.detailGain, tuning.detailKnee, tuning.detailFloor);
	out.percussiveEnergy = shapeTunedSignal(source.percussiveEnergy, tuning.detailGain, 40.0f, 0.0f);
	out.harmonicEnergy = shapeTunedSignal(source.harmonicEnergy, tuning.bodyGain, 40.0f, 0.0f);
	out.percussiveRatio = clamp01(shapeTunedSignal(source.percussiveRatio, tuning.percussiveGain, 0.8f, 0.0f));
	return out;
}

static bool saveAudioTuningProfile(const std::filesystem::path& path, const AudioTuningProfile& t) {
	std::ofstream out(path);
	if (!out) return false;
	out << "pulseGain=" << t.pulseGain << "\n";
	out << "bodyGain=" << t.bodyGain << "\n";
	out << "detailGain=" << t.detailGain << "\n";
	out << "macroGain=" << t.macroGain << "\n";
	out << "bassGain=" << t.bassGain << "\n";
	out << "harmonicGain=" << t.harmonicGain << "\n";
	out << "leadGain=" << t.leadGain << "\n";
	out << "airGain=" << t.airGain << "\n";
	out << "noveltyGain=" << t.noveltyGain << "\n";
	out << "brightnessGain=" << t.brightnessGain << "\n";
	out << "percussiveGain=" << t.percussiveGain << "\n";
	out << "tensionGain=" << t.tensionGain << "\n";
	out << "releaseGain=" << t.releaseGain << "\n";
	out << "pulseKnee=" << t.pulseKnee << "\n";
	out << "bodyKnee=" << t.bodyKnee << "\n";
	out << "detailKnee=" << t.detailKnee << "\n";
	out << "macroKnee=" << t.macroKnee << "\n";
	out << "pulseFloor=" << t.pulseFloor << "\n";
	out << "bodyFloor=" << t.bodyFloor << "\n";
	out << "detailFloor=" << t.detailFloor << "\n";
	out << "macroFloor=" << t.macroFloor << "\n";
	out << "autoStyleEnabled=" << (t.autoStyleEnabled ? 1 : 0) << "\n";
	out << "autoStyleBlend=" << t.autoStyleBlend << "\n";
	out << "autoStyleResponseSec=" << t.autoStyleResponseSec << "\n";
	return true;
}

static bool loadAudioTuningProfile(const std::filesystem::path& path, AudioTuningProfile& t) {
	std::ifstream in(path);
	if (!in) return false;
	std::string line;
	while (std::getline(in, line)) {
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = line.substr(0, eq);
		const float value = std::strtof(line.c_str() + eq + 1, nullptr);
		if (key == "pulseGain") t.pulseGain = value;
		else if (key == "bodyGain") t.bodyGain = value;
		else if (key == "detailGain") t.detailGain = value;
		else if (key == "macroGain") t.macroGain = value;
		else if (key == "bassGain") t.bassGain = value;
		else if (key == "harmonicGain") t.harmonicGain = value;
		else if (key == "leadGain") t.leadGain = value;
		else if (key == "airGain") t.airGain = value;
		else if (key == "noveltyGain") t.noveltyGain = value;
		else if (key == "brightnessGain") t.brightnessGain = value;
		else if (key == "percussiveGain") t.percussiveGain = value;
		else if (key == "tensionGain") t.tensionGain = value;
		else if (key == "releaseGain") t.releaseGain = value;
		else if (key == "pulseKnee") t.pulseKnee = value;
		else if (key == "bodyKnee") t.bodyKnee = value;
		else if (key == "detailKnee") t.detailKnee = value;
		else if (key == "macroKnee") t.macroKnee = value;
		else if (key == "pulseFloor") t.pulseFloor = value;
		else if (key == "bodyFloor") t.bodyFloor = value;
		else if (key == "detailFloor") t.detailFloor = value;
		else if (key == "macroFloor") t.macroFloor = value;
		else if (key == "autoStyleEnabled") t.autoStyleEnabled = value >= 0.5f;
		else if (key == "autoStyleBlend") t.autoStyleBlend = value;
		else if (key == "autoStyleResponseSec") t.autoStyleResponseSec = value;
	}
	t = clampAudioTuningProfile(t);
	return true;
}

static void writeAudioTuningProfile(std::ostream& out, const AudioTuningProfile& t) {
	out << "pulseGain=" << t.pulseGain << "\n";
	out << "bodyGain=" << t.bodyGain << "\n";
	out << "detailGain=" << t.detailGain << "\n";
	out << "macroGain=" << t.macroGain << "\n";
	out << "bassGain=" << t.bassGain << "\n";
	out << "harmonicGain=" << t.harmonicGain << "\n";
	out << "leadGain=" << t.leadGain << "\n";
	out << "airGain=" << t.airGain << "\n";
	out << "noveltyGain=" << t.noveltyGain << "\n";
	out << "brightnessGain=" << t.brightnessGain << "\n";
	out << "percussiveGain=" << t.percussiveGain << "\n";
	out << "tensionGain=" << t.tensionGain << "\n";
	out << "releaseGain=" << t.releaseGain << "\n";
	out << "pulseKnee=" << t.pulseKnee << "\n";
	out << "bodyKnee=" << t.bodyKnee << "\n";
	out << "detailKnee=" << t.detailKnee << "\n";
	out << "macroKnee=" << t.macroKnee << "\n";
	out << "pulseFloor=" << t.pulseFloor << "\n";
	out << "bodyFloor=" << t.bodyFloor << "\n";
	out << "detailFloor=" << t.detailFloor << "\n";
	out << "macroFloor=" << t.macroFloor << "\n";
	out << "autoStyleEnabled=" << (t.autoStyleEnabled ? 1 : 0) << "\n";
	out << "autoStyleBlend=" << t.autoStyleBlend << "\n";
	out << "autoStyleResponseSec=" << t.autoStyleResponseSec << "\n";
}

static void parseAudioTuningKey(const std::string& key, float value, AudioTuningProfile& t) {
	if (key == "pulseGain") t.pulseGain = value;
	else if (key == "bodyGain") t.bodyGain = value;
	else if (key == "detailGain") t.detailGain = value;
	else if (key == "macroGain") t.macroGain = value;
	else if (key == "bassGain") t.bassGain = value;
	else if (key == "harmonicGain") t.harmonicGain = value;
	else if (key == "leadGain") t.leadGain = value;
	else if (key == "airGain") t.airGain = value;
	else if (key == "noveltyGain") t.noveltyGain = value;
	else if (key == "brightnessGain") t.brightnessGain = value;
	else if (key == "percussiveGain") t.percussiveGain = value;
	else if (key == "tensionGain") t.tensionGain = value;
	else if (key == "releaseGain") t.releaseGain = value;
	else if (key == "pulseKnee") t.pulseKnee = value;
	else if (key == "bodyKnee") t.bodyKnee = value;
	else if (key == "detailKnee") t.detailKnee = value;
	else if (key == "macroKnee") t.macroKnee = value;
	else if (key == "pulseFloor") t.pulseFloor = value;
	else if (key == "bodyFloor") t.bodyFloor = value;
	else if (key == "detailFloor") t.detailFloor = value;
	else if (key == "macroFloor") t.macroFloor = value;
	else if (key == "autoStyleEnabled") t.autoStyleEnabled = value >= 0.5f;
	else if (key == "autoStyleBlend") t.autoStyleBlend = value;
	else if (key == "autoStyleResponseSec") t.autoStyleResponseSec = value;
}

static bool saveLookPreset(const std::filesystem::path& path, const LookPreset& preset) {
	std::ofstream out(path);
	if (!out) return false;
	out << "fragmentPath=" << preset.fragmentPath << "\n";
	out << "processingSketchPath=" << preset.processingSketchPath << "\n";
	out << "useFFglitch=" << (preset.useFFglitch ? 1 : 0) << "\n";
	out << "useProcessing=" << (preset.useProcessing ? 1 : 0) << "\n";
	out << "shaderFeedback=" << (preset.shaderFeedback ? 1 : 0) << "\n";
	out << "hotReloadShaders=" << (preset.hotReloadShaders ? 1 : 0) << "\n";
	out << "previewSize=" << preset.previewSize << "\n";
	writeAudioTuningProfile(out, preset.audioTuning);
	return true;
}

static bool loadLookPreset(const std::filesystem::path& path, LookPreset& preset) {
	std::ifstream in(path);
	if (!in) return false;
	std::string line;
	while (std::getline(in, line)) {
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = line.substr(0, eq);
		const std::string raw = line.substr(eq + 1);
		if (key == "fragmentPath") preset.fragmentPath = raw;
		else if (key == "processingSketchPath") preset.processingSketchPath = raw;
		else if (key == "useFFglitch") preset.useFFglitch = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "useProcessing") preset.useProcessing = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "shaderFeedback") preset.shaderFeedback = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "hotReloadShaders") preset.hotReloadShaders = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "previewSize") preset.previewSize = std::max(256, std::min(1024, (int)std::strtol(raw.c_str(), nullptr, 10)));
		else parseAudioTuningKey(key, std::strtof(raw.c_str(), nullptr), preset.audioTuning);
	}
	preset.audioTuning = clampAudioTuningProfile(preset.audioTuning);
	return true;
}

static bool saveWorkspacePreset(const std::filesystem::path& path, const WorkspacePreset& preset, const char* imguiIniPath) {
	std::ofstream out(path);
	if (!out) return false;
	out << "fragmentPath=" << preset.look.fragmentPath << "\n";
	out << "processingSketchPath=" << preset.look.processingSketchPath << "\n";
	out << "useFFglitch=" << (preset.look.useFFglitch ? 1 : 0) << "\n";
	out << "useProcessing=" << (preset.look.useProcessing ? 1 : 0) << "\n";
	out << "shaderFeedback=" << (preset.look.shaderFeedback ? 1 : 0) << "\n";
	out << "hotReloadShaders=" << (preset.look.hotReloadShaders ? 1 : 0) << "\n";
	out << "previewSize=" << preset.look.previewSize << "\n";
	out << "smoothing=" << preset.smoothing << "\n";
	out << "bandsGain=" << preset.bandsGain << "\n";
	out << "onsetsGain=" << preset.onsetsGain << "\n";
	out << "selectedMonitor=" << preset.selectedMonitor << "\n";
	out << "showSource=" << (preset.showSource ? 1 : 0) << "\n";
	out << "showVisual=" << (preset.showVisual ? 1 : 0) << "\n";
	out << "showDiagnostics=" << (preset.showDiagnostics ? 1 : 0) << "\n";
	out << "showExpert=" << (preset.showExpert ? 1 : 0) << "\n";
	out << "showOutput=" << (preset.showOutput ? 1 : 0) << "\n";
	out << "showViewport=" << (preset.showViewport ? 1 : 0) << "\n";
	const std::string layoutFile = path.stem().string() + ".layout.ini";
	out << "layoutFile=" << layoutFile << "\n";
	writeAudioTuningProfile(out, preset.look.audioTuning);
	out.close();

	if (imguiIniPath && *imguiIniPath) {
		std::ifstream src(imguiIniPath, std::ios::binary);
		std::ofstream dst(path.parent_path() / layoutFile, std::ios::binary | std::ios::trunc);
		if (src && dst) dst << src.rdbuf();
	}
	return true;
}

static bool loadWorkspacePreset(const std::filesystem::path& path, WorkspacePreset& preset) {
	std::ifstream in(path);
	if (!in) return false;
	std::string line;
	while (std::getline(in, line)) {
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = line.substr(0, eq);
		const std::string raw = line.substr(eq + 1);
		if (key == "fragmentPath") preset.look.fragmentPath = raw;
		else if (key == "processingSketchPath") preset.look.processingSketchPath = raw;
		else if (key == "useFFglitch") preset.look.useFFglitch = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "useProcessing") preset.look.useProcessing = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "shaderFeedback") preset.look.shaderFeedback = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "hotReloadShaders") preset.look.hotReloadShaders = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "previewSize") preset.look.previewSize = std::max(256, std::min(1024, (int)std::strtol(raw.c_str(), nullptr, 10)));
		else if (key == "smoothing") preset.smoothing = std::strtof(raw.c_str(), nullptr);
		else if (key == "bandsGain") preset.bandsGain = std::strtof(raw.c_str(), nullptr);
		else if (key == "onsetsGain") preset.onsetsGain = std::strtof(raw.c_str(), nullptr);
		else if (key == "selectedMonitor") preset.selectedMonitor = (int)std::strtol(raw.c_str(), nullptr, 10);
		else if (key == "showSource") preset.showSource = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "showVisual") preset.showVisual = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "showDiagnostics") preset.showDiagnostics = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "showExpert") preset.showExpert = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "showOutput") preset.showOutput = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "showViewport") preset.showViewport = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "layoutFile") preset.layoutFile = raw;
		else parseAudioTuningKey(key, std::strtof(raw.c_str(), nullptr), preset.look.audioTuning);
	}
	preset.look.audioTuning = clampAudioTuningProfile(preset.look.audioTuning);
	return true;
}

static bool saveUiPreferences(const std::filesystem::path& path, const UiPreferences& prefs) {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::ofstream out(path);
	if (!out) return false;
	out << "restoreLastSessionOnStartup=" << (prefs.restoreLastSessionOnStartup ? 1 : 0) << "\n";
	out << "startupWorkspace=" << prefs.startupWorkspace << "\n";
	return true;
}

static bool loadUiPreferences(const std::filesystem::path& path, UiPreferences& prefs) {
	std::ifstream in(path);
	if (!in) return false;
	std::string line;
	while (std::getline(in, line)) {
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = line.substr(0, eq);
		const std::string raw = line.substr(eq + 1);
		if (key == "restoreLastSessionOnStartup") prefs.restoreLastSessionOnStartup = std::strtol(raw.c_str(), nullptr, 10) != 0;
		else if (key == "startupWorkspace") prefs.startupWorkspace = raw;
	}
	return true;
}

static std::string getenvOrUnset(const char* name) {
	const char* value = std::getenv(name);
	return value ? std::string(value) : std::string("<unset>");
}

static void normalizeGraphicsEnvironment() {
#if defined(__linux__)
	const char* keepSoftware = std::getenv("VISUALIZA_KEEP_SOFTWARE_GL");
	const bool preserve = keepSoftware && std::strcmp(keepSoftware, "0") != 0;
	if (!preserve) {
		unsetenv("LIBGL_ALWAYS_SOFTWARE");
	}
	const char* keepVsync = std::getenv("VISUALIZA_KEEP_SYSTEM_VSYNC");
	const bool preserveVsync = keepVsync && std::strcmp(keepVsync, "0") != 0;
	if (!preserveVsync) {
		setenv("vblank_mode", "0", 1);
		setenv("__GL_SYNC_TO_VBLANK", "0", 1);
	}
#endif
}

static void logStartupEnvironment() {
	const std::array<const char*, 13> keys = {
		"DISPLAY",
		"WAYLAND_DISPLAY",
		"XDG_SESSION_TYPE",
		"XDG_CURRENT_DESKTOP",
		"DESKTOP_SESSION",
		"PULSE_SERVER",
		"PIPEWIRE_REMOTE",
		"SDL_VIDEODRIVER",
		"LIBGL_ALWAYS_SOFTWARE",
		"vblank_mode",
		"__GL_SYNC_TO_VBLANK",
		"MESA_LOADER_DRIVER_OVERRIDE",
		"__GLX_VENDOR_LIBRARY_NAME"
	};

	std::ostringstream out;
	out << "visualiza startup environment\n";
	out << "cwd=" << std::filesystem::current_path().string() << "\n";
	for (const char* key : keys) {
		out << key << "=" << getenvOrUnset(key) << "\n";
	}
	const std::string text = out.str();
	std::fprintf(stderr, "%s", text.c_str());

	std::error_code ec;
	std::filesystem::create_directories("build", ec);
	std::ofstream file("build/startup_env.log", std::ios::trunc);
	if (file) file << text;
}

static void logStartupStage(const char* stage) {
	const char* enabled = std::getenv("VISUALIZA_STARTUP_TRACE");
	if (!enabled || std::strcmp(enabled, "0") == 0) return;
	std::fprintf(stderr, "startup stage: %s\n", stage);
	std::fflush(stderr);
}

static bool useGlfwSwapInterval() {
	const char* enabled = std::getenv("VISUALIZA_USE_GLFW_SWAP_INTERVAL");
	return enabled && std::strcmp(enabled, "0") != 0;
}

#if defined(__linux__)
static int visualizaXErrorHandler(Display* display, XErrorEvent* event) {
	if (event && event->error_code != BadWindow) {
		char message[256] = {};
		if (display) XGetErrorText(display, event->error_code, message, sizeof(message));
		std::fprintf(stderr, "X11 warning: %s (opcode %u, resource 0x%lx)\n",
			message[0] ? message : "unknown X error",
			event->request_code,
			event->resourceid);
	}
	return 0;
}

static std::string readXWindowTextProperty(Display* display, Window window, Atom atom) {
	Atom actualType = None;
	int actualFormat = 0;
	unsigned long itemCount = 0;
	unsigned long bytesAfter = 0;
	unsigned char* data = nullptr;
	std::string out;
	if (XGetWindowProperty(display, window, atom, 0, 1024, False, AnyPropertyType, &actualType, &actualFormat, &itemCount, &bytesAfter, &data) == Success && data) {
		if (actualFormat == 8 && itemCount > 0) out.assign(reinterpret_cast<char*>(data), itemCount);
		XFree(data);
	}
	return out;
}

static long readXWindowLongProperty(Display* display, Window window, Atom atom) {
	Atom actualType = None;
	int actualFormat = 0;
	unsigned long itemCount = 0;
	unsigned long bytesAfter = 0;
	unsigned char* data = nullptr;
	long out = -1;
	if (XGetWindowProperty(display, window, atom, 0, 1, False, AnyPropertyType, &actualType, &actualFormat, &itemCount, &bytesAfter, &data) == Success && data) {
		if (itemCount > 0) {
			if (actualFormat == 32) out = (long)*reinterpret_cast<unsigned long*>(data);
			else if (actualFormat == 16) out = (long)*reinterpret_cast<unsigned short*>(data);
			else if (actualFormat == 8) out = (long)*data;
		}
		XFree(data);
	}
	return out;
}

static std::string readXWindowTitle(Display* display, Window window) {
	const Atom netWmName = XInternAtom(display, "_NET_WM_NAME", True);
	if (netWmName != None) {
		std::string title = readXWindowTextProperty(display, window, netWmName);
		if (!title.empty()) return title;
	}
	char* wmName = nullptr;
	if (XFetchName(display, window, &wmName) > 0 && wmName) {
		std::string title = wmName;
		XFree(wmName);
		return title;
	}
	return {};
}

static Window findXWindowByPidOrTitle(Display* display, Window root, long pid, const std::string& titleNeedle) {
	if (!display || root == None) return None;
	const Atom netWmPid = XInternAtom(display, "_NET_WM_PID", True);
	if (pid > 0 && netWmPid != None && readXWindowLongProperty(display, root, netWmPid) == pid) return root;
	if (!titleNeedle.empty()) {
		const std::string title = readXWindowTitle(display, root);
		if (title.find(titleNeedle) != std::string::npos) return root;
	}

	Window rootReturn = None;
	Window parentReturn = None;
	Window* children = nullptr;
	unsigned int childCount = 0;
	Window found = None;
	if (XQueryTree(display, root, &rootReturn, &parentReturn, &children, &childCount) != 0 && children) {
		for (unsigned int i = 0; i < childCount && found == None; ++i) {
			found = findXWindowByPidOrTitle(display, children[i], pid, titleNeedle);
		}
		XFree(children);
	}
	return found;
}
#endif

} // namespace

// Minimal shader helpers for blitting a texture (GL 3.2 core)
static const char* kBlitVS = R"GLSL(
#version 150
out vec2 vUV;
void main(){
    float x = (gl_VertexID == 1) ? 3.0 : -1.0;
    float y = (gl_VertexID == 2) ? 3.0 : -1.0;
    vec2 pos = vec2(x, y);
    vUV = pos * 0.5 + 0.5; // map clip [-1,3] -> [0,2] -> [0,1] inside screen
    gl_Position = vec4(pos, 0.0, 1.0);
}
)GLSL";
static const char* kBlitFS = R"GLSL(
#version 150
precision highp float;
in vec2 vUV;
uniform sampler2D u_tex;
out vec4 FragColor;
void main(){
    // vUV ~ [0,1] in-screen; clamp and flip Y to account for upload origin
    vec2 uv = clamp(vUV, 0.0, 1.0);
    uv.y = 1.0 - uv.y;
	FragColor = texture(u_tex, uv);
}
)GLSL";
static GLuint createShader(GLenum type, const char* src){
	GLuint id = glCreateShader(type);
	glShaderSource(id, 1, &src, nullptr);
	glCompileShader(id);
	GLint ok = 0; glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
	if (!ok){ char log[1024]; GLsizei n=0; glGetShaderInfoLog(id, sizeof(log), &n, log); std::fprintf(stderr, "Blit %s compile error: %s\n", type==GL_VERTEX_SHADER?"VS":"FS", log); glDeleteShader(id); return 0; }
	return id;
}
static GLuint createProgram(const char* vs, const char* fs){
	GLuint v = createShader(GL_VERTEX_SHADER, vs); if(!v) return 0;
	GLuint f = createShader(GL_FRAGMENT_SHADER, fs); if(!f){ glDeleteShader(v); return 0; }
	GLuint p = glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
	glDeleteShader(v); glDeleteShader(f);
	GLint linked=0; glGetProgramiv(p, GL_LINK_STATUS, &linked);
	if(!linked){ char log[1024]; GLsizei n=0; glGetProgramInfoLog(p,sizeof(log),&n,log); std::fprintf(stderr, "Blit program link error: %s\n", log); glDeleteProgram(p); return 0; }
	return p;
}

int main() {
#ifndef _WIN32
	std::signal(SIGPIPE, SIG_IGN);
#endif
#if defined(__linux__)
	XSetErrorHandler(visualizaXErrorHandler);
#endif
	normalizeGraphicsEnvironment();
	logStartupEnvironment();
	glfwSetErrorCallback(glfwErrorCallback);
	logStartupStage("glfwInit begin");
	if (!glfwInit()) {
		std::fprintf(stderr, "Failed to initialize GLFW\n");
		return EXIT_FAILURE;
	}
	logStartupStage("glfwInit ok");

	// macOS prefers 3.2 core
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	#ifdef __APPLE__
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	#endif

	logStartupStage("glfwCreateWindow begin");
	GLFWwindow* window = glfwCreateWindow(1280, 720, "visualiza_isso_aqui", nullptr, nullptr);
	if (!window) {
		std::fprintf(stderr, "Failed to create window\n");
		glfwTerminate();
		return EXIT_FAILURE;
	}
	logStartupStage("glfwCreateWindow ok");
	logStartupStage("glfwMakeContextCurrent begin");
	glfwMakeContextCurrent(window);
	logStartupStage("glfwMakeContextCurrent ok");
	if (useGlfwSwapInterval()) {
		logStartupStage("glfwSwapInterval begin");
		glfwSwapInterval(1);
		logStartupStage("glfwSwapInterval ok");
	}

	logStartupStage("gladLoadGLLoader begin");
	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
		std::fprintf(stderr, "Failed to load OpenGL via GLAD\n");
		return EXIT_FAILURE;
	}
	logStartupStage("gladLoadGLLoader ok");

	logStartupStage("imgui init begin");
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	(void)io;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.IniFilename = "imgui.ini";
	ImGui::StyleColorsDark();

	const char* glsl_version = "#version 150"; // GL 3.2 core
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init(glsl_version);
	logStartupStage("imgui init ok");

	// Audio
	logStartupStage("audio init begin");
	unsigned int sampleRate = 48000;
	unsigned int channelCount = 2;
	AudioInput audioInput;
	AudioFilePlayer filePlayer;
	bool useFile = false;
	char filePathBuf[512] = "";
	std::string audioFileStatus;
	std::string inputStatus;
	if (!audioInput.startStream(sampleRate, channelCount)) {
		std::fprintf(stderr, "Failed to start audio input\n");
		inputStatus = audioInput.getLastError().empty() ? "Failed to start audio input" : audioInput.getLastError();
	} else {
		inputStatus = "Live input ready";
	}
	logStartupStage("audio init ok");

    // Advanced analysis
	logStartupStage("analysis init begin");
    AudioAnalysis::Config aaCfg;
    aaCfg.fftSize = 512;
    aaCfg.numBands = 16;
    AudioAnalysis analyzer(sampleRate, aaCfg);
    AudioAnalysis::AnalysisFrame af{};
    AudioAnalysis::AnalysisFrame shaderAf{};
    const unsigned int analysisBlockFrames = (unsigned int)aaCfg.fftSize;
	ShaderAudioMetrics shaderMetrics{};
    AudioTuningProfile audioTuning = makeDefaultAudioTuningProfile();
    AudioTuningProfile effectiveAudioTuning = audioTuning;
    StyleAutoTuningState styleAutoState{};
    StyleProfileWeights styleWeights{};
    struct SemanticDebugRange {
        float minValue = 1.0f;
        float maxValue = 0.0f;
        bool seen = false;
        void update(float v) {
            if (!seen) {
                minValue = v;
                maxValue = v;
                seen = true;
                return;
            }
            minValue = std::min(minValue, v);
            maxValue = std::max(maxValue, v);
        }
        void reset() {
            minValue = 1.0f;
            maxValue = 0.0f;
            seen = false;
        }
    };
    SemanticDebugRange dbgBassBody, dbgHarmonicBody, dbgAirPresence, dbgLeadPresence, dbgRelease, dbgBodyComposite;
    std::filesystem::path audioTuningDir = "presets/audio_tuning";
    std::filesystem::path lookPresetDir = "presets/looks";
    std::filesystem::path workspacePresetDir = "presets/workspaces";
    std::filesystem::path uiPreferencesPath = "presets/ui/preferences.cfg";
    std::filesystem::path lastSessionWorkspacePath = workspacePresetDir / "__last_session.cfg";
    std::vector<std::string> audioTuningPresetFiles;
    std::vector<std::string> lookPresetFiles;
    std::vector<std::string> workspacePresetFiles;
    char audioTuningPresetName[128] = "live_default";
    char lookPresetName[128] = "default_look";
    char workspacePresetName[128] = "default_workspace";
    std::string audioTuningStatus;
    std::string presetStatus;
    std::string currentLookPresetName;
    std::string currentWorkspacePresetName;
    UiPreferences uiPreferences;
    loadUiPreferences(uiPreferencesPath, uiPreferences);
    auto refreshAudioTuningPresets = [&]() {
        audioTuningPresetFiles.clear();
        std::error_code ec;
        std::filesystem::create_directories(audioTuningDir, ec);
        if (ec || !std::filesystem::exists(audioTuningDir, ec)) return;
        for (const auto& entry : std::filesystem::directory_iterator(audioTuningDir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() == ".cfg") audioTuningPresetFiles.push_back(entry.path().filename().string());
        }
        std::sort(audioTuningPresetFiles.begin(), audioTuningPresetFiles.end());
    };
    auto refreshPresetFiles = [&](const std::filesystem::path& dir, std::vector<std::string>& outFiles) {
        outFiles.clear();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec || !std::filesystem::exists(dir, ec)) return;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".cfg") continue;
            const std::string filename = entry.path().filename().string();
            if (filename == "__last_session.cfg") continue;
            outFiles.push_back(filename);
        }
        std::sort(outFiles.begin(), outFiles.end());
    };
    refreshAudioTuningPresets();
    refreshPresetFiles(lookPresetDir, lookPresetFiles);
    refreshPresetFiles(workspacePresetDir, workspacePresetFiles);

    // Analysis UI controllers (scalers)
    float bandsGain = 1.0f;
    float onsetsGain = 1.0f;
    bool showPerBandControls = false;
    float bandScale[16];
    float onsetScale[16];
    for (int i = 0; i < 16; ++i) { bandScale[i] = 1.0f; onsetScale[i] = 1.0f; }
	logStartupStage("analysis init ok");

    // RM spheres shader controls
    float ui_camVel = 2.2f;
    bool  ui_useCamVel = true;
    float ui_warpIntensity = 0.6f;
    float ui_stepFactor = 0.7f;
    float ui_epsilon = 0.0015f;
    int   ui_maxSteps = 96;
    float ui_fogDensity = 0.015f;
    float ui_barrelK = 0.08f;
    float ui_colorGain = 1.0f;
    float ui_cellScale = 1.0f;
    float ui_r1Base = 0.7f;
    float ui_r2Base = 0.42f;
    float ui_flashGain = 0.35f;

    // Dynamic current-shader uniform controls (auto-generated)
    struct UIUniform {
        GLenum type;
        int components;
        GLint location;
        std::vector<float> values; // size == components
    };
    std::unordered_map<std::string, UIUniform> uiUniforms;
    std::vector<std::string> uiUniformOrder;
    std::string lastUniformSignature;
    std::unordered_map<std::string, ShaderUiMeta> shaderUiMeta;
    std::string uiMetadataPath;
    GLuint uiTargetProgramId = 0;
    auto invalidateUniformUI = [&]() {
        lastUniformSignature.clear();
        uiUniforms.clear();
        uiUniformOrder.clear();
        shaderUiMeta.clear();
    };

    auto isIgnoredUniform = [](const std::string& n) {
        static const char* ignored[] = {
            "u_time","u_resolution",
            "u_rms","u_bandLow","u_bandMid","u_bandHigh","u_onset",
            "u_bands","u_onsets","u_centroidNorm","u_flux","u_beat","u_beatEnv","u_bpm","u_percE","u_harmE","u_percRatio",
            "u_kickImpact","u_snareImpact","u_hatTick","u_beatPulse",
            "u_subBody","u_bassBody","u_harmonicBody","u_leadPresence","u_airPresence",
            "u_transientDensity","u_novelty","u_brightness","u_percussiveFocus",
            "u_energyLevel","u_tension","u_release","u_dropEvent","u_sectionChange",
            "u_roleKickCenterNorm","u_roleBassCenterNorm","u_roleLeadCenterNorm","u_roleAirCenterNorm",
            "u_roleKickConfidence","u_roleBassConfidence","u_roleLeadConfidence","u_roleAirConfidence",
            // don't ignore RM-specific params here so dynamic UI can show them
        };
        for (auto* s : ignored) if (n == s) return true;
        return false;
    };

    auto rebuildUniformUI = [&](const ShaderProgram& sp) {
        const_cast<ShaderProgram&>(sp).use();
        shaderUiMeta = parseShaderUiMetadata(uiMetadataPath);
        uiTargetProgramId = sp.getProgramId();
        auto infos = sp.getActiveUniforms();
        // Signature to detect changes
        std::string sig;
        sig.reserve(infos.size() * 16);
        for (auto& u : infos) { sig += u.name; sig += ";"; }
        // If signature is same but we currently have nothing, force rebuild
        if (sig == lastUniformSignature && !uiUniforms.empty()) return;
        lastUniformSignature = sig;
        uiUniforms.clear();
        uiUniformOrder.clear();
        for (auto& u : infos) {
            if (u.location < 0) continue;
            if (u.size != 1) continue; // skip arrays for now
            if (isIgnoredUniform(u.name)) continue;
            int comps = 0;
            switch (u.type) {
                case GL_FLOAT: comps = 1; break;
                case GL_FLOAT_VEC2: comps = 2; break;
                case GL_FLOAT_VEC3: comps = 3; break;
                case GL_FLOAT_VEC4: comps = 4; break;
                case GL_INT:
                case GL_BOOL: comps = 1; break;
                default: comps = 0; break; // skip samplers/mats
            }
            if (comps == 0) continue;
            UIUniform ui{}; ui.type = u.type; ui.components = comps; ui.location = u.location; ui.values.assign(comps, 0.0f);
            // Try to read current values
            if (u.type == GL_INT || u.type == GL_BOOL) {
                GLint iv[4] = {0,0,0,0};
                glGetUniformiv(sp.getProgramId(), u.location, iv);
                for (int i = 0; i < comps; ++i) ui.values[i] = (float)iv[i];
            } else {
                GLfloat fv[4] = {0,0,0,0};
                glGetUniformfv(sp.getProgramId(), u.location, fv);
                for (int i = 0; i < comps; ++i) ui.values[i] = fv[i];
            }
            if (u.type == GL_FLOAT && comps == 1) {
                auto mit = shaderUiMeta.find(u.name);
                if (mit != shaderUiMeta.end() && mit->second.hasDefault) {
                    ui.values[0] = mit->second.defaultValue;
                    glUniform1f(u.location, ui.values[0]);
                }
            }
            uiUniforms[u.name] = ui;
            uiUniformOrder.push_back(u.name);
        }
    };

    auto drawShaderUniformEditor = [&](ShaderProgram& program, const char* childId, float height, bool metadataOnly) {
        if (uiUniforms.empty()) rebuildUniformUI(program);
        if (uiUniformOrder.empty()) {
            ImGui::TextDisabled("No adjustable uniforms detected in current shader.");
            return;
        }
        ImGui::BeginChild(childId, ImVec2(0.0f, height), true);
        std::string currentGroup;
        for (const auto& uname : uiUniformOrder) {
            auto it = uiUniforms.find(uname);
            if (it == uiUniforms.end()) continue;
            auto mit = shaderUiMeta.find(uname);
            const ShaderUiMeta* meta = mit != shaderUiMeta.end() ? &mit->second : nullptr;
            if (metadataOnly) {
                if (!meta || !meta->hasMeta || meta->hidden) continue;
                if (!meta->group.empty() && meta->group != currentGroup) {
                    currentGroup = meta->group;
                    ImGui::SeparatorText(currentGroup.c_str());
                }
            }
            UIUniform& u = it->second;
            const char* displayName = (meta && !meta->label.empty()) ? meta->label.c_str() : uname.c_str();
            if (u.components == 1) {
                if (u.type == GL_INT || u.type == GL_BOOL) {
                    int v = (int)u.values[0];
                    if (ImGui::InputInt(displayName, &v)) { u.values[0] = (float)v; program.setUniformi(uname.c_str(), v); }
                } else {
                    float minv = -10.0f;
                    float maxv = 10.0f;
                    if (meta && meta->hasRange) { minv = meta->minValue; maxv = meta->maxValue; }
                    if (ImGui::SliderFloat(displayName, &u.values[0], minv, maxv)) program.setUniform(uname.c_str(), u.values[0]);
                }
            } else if (u.components == 2) {
                float v2[2] = {u.values[0], u.values[1]};
                if (ImGui::DragFloat2(displayName, v2, 0.01f)) { u.values[0]=v2[0]; u.values[1]=v2[1]; program.setUniform(uname.c_str(), v2[0], v2[1]); }
            } else if (u.components == 3) {
                float v3[3] = {u.values[0], u.values[1], u.values[2]};
                if (ImGui::DragFloat3(displayName, v3, 0.01f)) { u.values[0]=v3[0]; u.values[1]=v3[1]; u.values[2]=v3[2]; program.setUniform(uname.c_str(), v3[0], v3[1], v3[2]); }
            } else if (u.components == 4) {
                float v4[4] = {u.values[0], u.values[1], u.values[2], u.values[3]};
                if (ImGui::DragFloat4(displayName, v4, 0.01f)) { u.values[0]=v4[0]; u.values[1]=v4[1]; u.values[2]=v4[2]; u.values[3]=v4[3]; program.setUniform(uname.c_str(), v4[0], v4[1], v4[2], v4[3]); }
            }
        }
        ImGui::EndChild();
    };

	logStartupStage("ui uniform setup ok");

    // Graphics and shader
	logStartupStage("graphics discovery begin");
    auto collectShaders = [](const std::vector<std::string>& dirs){
        std::vector<std::string> out;
        std::vector<std::string> names; // to de-dup by filename (prefer earlier dirs)
        for (const auto& d : dirs) {
            std::error_code ec;
            if (!std::filesystem::exists(d, ec)) continue;
            for (auto &p : std::filesystem::directory_iterator(d)) {
                if (p.is_regular_file()) {
                    auto path = p.path();
                    if (path.extension() == ".frag") {
                        std::string name = path.filename().string();
                        const std::string stem = path.stem().string();
                        if (stem.size() > 8) {
                            const std::string suffix = stem.substr(stem.size() - 8);
                            if (suffix == "_bufferA" || suffix == "_bufferB" || suffix == "_bufferC" || suffix == "_bufferD" || suffix == "_bufferE") {
                                continue;
                            }
                        }
                        if (std::find(names.begin(), names.end(), name) == names.end()) {
                            names.push_back(name);
                            out.push_back(path.string());
                        }
                    }
                }
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    // Prefer source tree shaders; fall back to app bundle Resources when running .app
    auto bundleAssets = [](){
        std::string p = "assets/shaders"; // default relative
    #ifdef __APPLE__
        // Try macOS bundle Resources
        std::string r1 = "../Resources/assets/shaders";   // when cwd is MacOS/
        std::string r2 = "../../Resources/assets/shaders"; // alternate
        if (std::filesystem::exists(r1)) return r1;
        if (std::filesystem::exists(r2)) return r2;
    #endif
        return p;
    };
    std::string defaultDir = std::filesystem::exists("../assets/shaders") ? "../assets/shaders" : bundleAssets();
    std::vector<std::string> shaderFiles = collectShaders({"../assets/shaders", bundleAssets(), "assets/shaders"});
    std::string shadersDir = defaultDir;
    auto collectProcessingSketches = [](const std::vector<std::string>& dirs){
        std::vector<std::string> out;
        std::vector<std::string> names;
        for (const auto& d : dirs) {
            std::error_code ec;
            if (!std::filesystem::exists(d, ec)) continue;
            for (auto& p : std::filesystem::directory_iterator(d)) {
                if (!p.is_regular_file()) continue;
                const auto path = p.path();
                const std::string filename = path.filename().string();
                const bool isSketch = path.extension() == ".js";
                if (!isSketch) continue;
                if (std::find(names.begin(), names.end(), filename) == names.end()) {
                    names.push_back(filename);
                    out.push_back(path.string());
                }
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto processingBundle = [](){
        std::string p = "assets/processing";
    #ifdef __APPLE__
        std::string r1 = "../Resources/assets/processing";
        std::string r2 = "../../Resources/assets/processing";
        if (std::filesystem::exists(r1)) return r1;
        if (std::filesystem::exists(r2)) return r2;
#endif
        return p;
    };
    std::vector<std::string> processingSketchFiles = collectProcessingSketches({
		"../assets/processing/sketches",
		processingBundle() + "/sketches",
		"assets/processing/sketches"
	});
    std::string processingSketchPath = processingSketchFiles.empty() ? std::string() : processingSketchFiles[0];
    // Collect ffglitch built-in scripts (optional)
    auto collectScripts = [](const std::vector<std::string>& dirs){
        std::vector<std::string> out;
        std::vector<std::string> names;
        for (const auto& d : dirs) {
            std::error_code ec;
            if (!std::filesystem::exists(d, ec)) continue;
            for (auto &p : std::filesystem::directory_iterator(d)) {
                if (p.is_regular_file()) {
                    auto path = p.path();
                    if (path.extension() == ".js") {
                        std::string name = path.filename().string();
                        if (std::find(names.begin(), names.end(), name) == names.end()) { names.push_back(name); out.push_back(path.string()); }
                    }
                }
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto scriptsBundle = [](){
        std::string p = "assets/ffglitch";
    #ifdef __APPLE__
        std::string r1 = "../Resources/assets/ffglitch";
        std::string r2 = "../../Resources/assets/ffglitch";
        if (std::filesystem::exists(r1)) return r1;
        if (std::filesystem::exists(r2)) return r2;
    #endif
        return p;
    };
    std::vector<std::string> scriptFiles = collectScripts({"../assets/ffglitch", scriptsBundle(), "assets/ffglitch"});
    std::string scriptsDir = std::filesystem::exists("../assets/ffglitch") ? "../assets/ffglitch" : scriptsBundle();
	logStartupStage("graphics discovery ok");
    // External shader path input (user-provided absolute or relative file)
    static char shaderPathBuf[1024] = "";
	std::string fragmentPath = shaderFiles.empty() ? std::string("assets/shaders/visual.frag") : shaderFiles[0];
    uiMetadataPath = fragmentPath;
	ShaderProgram shaderProgram;
	FFglitchPlayer ffgPlayer;
	bool useFFglitch = false;
	bool useProcessing = false;
	int processingOutputTarget = 0; // 0 = docked Viewport, 1 = fullscreen Output window
	std::error_code processingTempEc;
	std::filesystem::path processingTempDir = std::filesystem::temp_directory_path(processingTempEc);
	if (processingTempDir.empty()) processingTempDir = "/tmp";
	#if defined(__linux__) || defined(__APPLE__)
	const int processingInstanceId = (int)getpid();
	#else
	const int processingInstanceId = 0;
	#endif
	const std::string processingInstanceSuffix = std::to_string(std::max(0, processingInstanceId));
	const std::string processingFrameFilePath = (processingTempDir / ("visualiza_p5_frame_" + processingInstanceSuffix + ".bin")).string();
	const std::string processingEngineLogPath = (processingTempDir / ("visualiza_processing_engine_" + processingInstanceSuffix + ".log")).string();
	const std::string processingBrowserLogPath = (processingTempDir / ("visualiza_p5_chromium_" + processingInstanceSuffix + ".log")).string();
	int processingCaptureWidth = 1280;
	int processingCaptureHeight = 720;
	int processingCaptureFps = 60;
	bool uiShaderFeedback = false; // single-file shader feedback toggle
	bool uiHotReloadShaders = true;
	struct CompanionPass {
		ShaderProgram program;
		std::string path;
		bool active = false;
		GLuint tex[2] = {0, 0};
		int readIndex = 0;
		int writeIndex = 1;
		int iterations = 1;
	};
	std::array<CompanionPass, 5> companionPasses;
	logStartupStage("initial shader compile begin");
	if (!shaderProgram.buildFromFiles(fragmentPath.c_str())) {
		std::fprintf(stderr, "Initial shader compile failed. Fix the shader file and it will hot-reload.\n");
	}
	logStartupStage("initial shader compile ok");

	// Fullscreen triangle (no attributes needed; vertex shader generates positions)
	logStartupStage("gl resources begin");
	GLuint vao = 0;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	// Offscreen preview framebuffer (square) for centered viewport
	GLuint previewFbo = 0, previewTex = 0;
	GLuint previewDisplayTex = 0;
	int previewSize = 512;
    // Shader feedback ping-pong resources
    GLuint feedbackTex[2] = {0,0};
    int fbRead = 0, fbWrite = 1;
	auto createPreviewTargets = [&]() {
		if (previewTex) glDeleteTextures(1, &previewTex);
		if (previewFbo) glDeleteFramebuffers(1, &previewFbo);
        if (feedbackTex[0]) { glDeleteTextures(1, &feedbackTex[0]); feedbackTex[0] = 0; }
        if (feedbackTex[1]) { glDeleteTextures(1, &feedbackTex[1]); feedbackTex[1] = 0; }
		glGenTextures(1, &previewTex);
		previewDisplayTex = previewTex;
		glBindTexture(GL_TEXTURE_2D, previewTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, previewSize, previewSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
        // Feedback ping-pong textures
        glGenTextures(2, feedbackTex);
        for (int i = 0; i < 2; ++i) {
            glBindTexture(GL_TEXTURE_2D, feedbackTex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, previewSize, previewSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            // Seed a single pixel at (0,0) to kickstart feedback shaders that depend on prior state
            unsigned char seed[4] = { 32, 16, 240, 255 };
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, seed);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
		fbRead = 0; fbWrite = 1;
		glGenFramebuffers(1, &previewFbo);
		glBindFramebuffer(GL_FRAMEBUFFER, previewFbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, previewTex, 0);
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			std::fprintf(stderr, "Preview FBO incomplete: 0x%x\n", status);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	};
	createPreviewTargets();
	logStartupStage("preview targets ok");
	auto clearCompanionTextures = [&]() {
		for (auto& pass : companionPasses) {
			if (pass.tex[0]) { glDeleteTextures(1, &pass.tex[0]); pass.tex[0] = 0; }
			if (pass.tex[1]) { glDeleteTextures(1, &pass.tex[1]); pass.tex[1] = 0; }
			pass.readIndex = 0;
			pass.writeIndex = 1;
		}
	};
	auto createCompanionTargets = [&](int size) {
		clearCompanionTextures();
		for (auto& pass : companionPasses) {
			if (!pass.active) continue;
			glGenTextures(2, pass.tex);
			for (int i = 0; i < 2; ++i) {
				glBindTexture(GL_TEXTURE_2D, pass.tex[i]);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, size, size, 0, GL_RGBA, GL_FLOAT, nullptr);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				const std::vector<float> seed((size_t)size * (size_t)size * 4u, 0.0f);
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RGBA, GL_FLOAT, seed.data());
			}
		}
		glBindTexture(GL_TEXTURE_2D, 0);
	};
	GLuint processingFrameTex = 0;
	int processingFrameWidth = 0;
	int processingFrameHeight = 0;
	uint32_t processingFrameSeq = 0;
	bool processingFrameReady = false;
	auto updateProcessingFrameTexture = [&]() {
		std::ifstream in(processingFrameFilePath, std::ios::binary);
		if (!in) return false;
		std::array<unsigned char, 24> header{};
		in.read(reinterpret_cast<char*>(header.data()), (std::streamsize)header.size());
		if (in.gcount() != (std::streamsize)header.size()) return false;
		if (std::memcmp(header.data(), "VZP5FRM1", 8) != 0) return false;
		auto readU32 = [&](size_t offset) -> uint32_t {
			return (uint32_t)header[offset]
				| ((uint32_t)header[offset + 1] << 8)
				| ((uint32_t)header[offset + 2] << 16)
				| ((uint32_t)header[offset + 3] << 24);
		};
		const uint32_t width = readU32(8);
		const uint32_t height = readU32(12);
		const uint32_t seq = readU32(16);
		const uint32_t bytes = readU32(20);
		if (seq == processingFrameSeq || width < 1 || height < 1 || width > 4096 || height > 4096 || bytes != width * height * 4u) return processingFrameReady;
		std::vector<unsigned char> pixels(bytes);
		in.read(reinterpret_cast<char*>(pixels.data()), (std::streamsize)pixels.size());
		if (in.gcount() != (std::streamsize)pixels.size()) return processingFrameReady;
		if (!processingFrameTex) {
			glGenTextures(1, &processingFrameTex);
			glBindTexture(GL_TEXTURE_2D, processingFrameTex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		} else {
			glBindTexture(GL_TEXTURE_2D, processingFrameTex);
		}
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		if (processingFrameWidth != (int)width || processingFrameHeight != (int)height) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
			processingFrameWidth = (int)width;
			processingFrameHeight = (int)height;
		} else {
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		}
		glBindTexture(GL_TEXTURE_2D, 0);
		processingFrameSeq = seq;
		processingFrameReady = true;
		return true;
	};
	auto refreshCompanionPasses = [&]() {
		const std::filesystem::path basePath(fragmentPath);
		const std::filesystem::path dir = basePath.parent_path();
		const std::string stem = basePath.stem().string();
		static const std::array<const char*, 5> suffixes = { "_bufferA.frag", "_bufferB.frag", "_bufferC.frag", "_bufferD.frag", "_bufferE.frag" };
		static const std::array<int, 5> defaultIterations = { 1, 1, 10, 1, 1 };
		bool anyActive = false;
		for (size_t i = 0; i < companionPasses.size(); ++i) {
			auto& pass = companionPasses[i];
			pass.active = false;
			pass.path.clear();
			pass.iterations = defaultIterations[i];
			const std::filesystem::path candidate = dir / (stem + suffixes[i]);
			if (std::filesystem::exists(candidate)) {
				pass.path = candidate.string();
				pass.active = pass.program.buildFromFiles(pass.path.c_str());
				anyActive = anyActive || pass.active;
			}
		}
		if (anyActive) createCompanionTargets(previewSize);
		else clearCompanionTextures();
		uiMetadataPath = fragmentPath;
		for (const auto& pass : companionPasses) {
			if (pass.active) {
				uiMetadataPath = pass.path;
				break;
			}
		}
		invalidateUniformUI();
	};
	refreshCompanionPasses();
	logStartupStage("companion passes ok");
	auto getUiControlProgram = [&]() -> ShaderProgram* {
		for (auto& pass : companionPasses) {
			if (pass.active) return &pass.program;
		}
		return &shaderProgram;
	};

	// Create blit program for FFglitch texture
	GLuint blitProgram = createProgram(kBlitVS, kBlitFS);
	GLint blitTexLoc = blitProgram ? glGetUniformLocation(blitProgram, "u_tex") : -1;
	logStartupStage("gl resources ok");

	auto lastTime = std::chrono::high_resolution_clock::now();
	float timeSeconds = 0.0f;
	float frameTimeMs = 0.0f;
	float fpsDisplay = 0.0f;
	float onsetDisplay = 0.0f;
	float beatRateDisplay = 0.0f;
	float analysisUpdateRateHz = 0.0f;
	auto lastAnalysisUpdateTime = std::chrono::steady_clock::now();
	auto lastLiveStatsLogTime = std::chrono::steady_clock::now();
	bool showDemoWindow = false;
	bool freezeAudio = false;
	float smoothing = 0.35f;
    bool showSourceWindow = true;
    bool showVisualWindow = true;
    bool showDiagnosticsWindow = true;
    bool showExpertWindow = true;
	bool showOutputWindow = true;
    bool showViewportWindow = true;
    bool requestDockLayoutReset = false;
	unsigned int selectedDeviceId = (unsigned int)-1;
	logStartupStage("audio device list begin");
	std::vector<AudioInput::DeviceInfo> devices = audioInput.listInputDevices();
	logStartupStage("audio device list ok");
	if (audioInput.getCurrentDeviceId() != 0) selectedDeviceId = audioInput.getCurrentDeviceId();
	if (selectedDeviceId == (unsigned int)-1) {
		for (const auto& d : devices) { if (d.isDefault) { selectedDeviceId = d.id; break; } }
	}
	std::vector<float> waveform; waveform.reserve(2048);
	logStartupStage("external integration setup begin");

	auto whichCmd = [&](const char* bin){
		std::string cmd = std::string("/usr/bin/env which ") + bin + std::string(" 2>/dev/null");
		FILE* fp = popen(cmd.c_str(), "r");
		if (!fp) return std::string();
		char buf[512]; std::string out;
		size_t n = fread(buf, 1, sizeof(buf)-1, fp); if (n>0) { buf[n]=0; out = buf; }
		pclose(fp);
		// trim
		auto trim = [](std::string& s){ while(!s.empty() && (s.back()=='\n' || s.back()=='\r' || s.back()==' ')) s.pop_back(); while(!s.empty() && (s.front()=='\n' || s.front()=='\r' || s.front()==' ')) s.erase(s.begin()); };
		trim(out);
		return out;
	};

		auto shellQuote = [](const std::string& value) {
			std::string out = "'";
			for (char c : value) {
				if (c == '\'') out += "'\\''";
			else out += c;
		}
			out += "'";
			return out;
		};
		auto isExecutablePath = [](const std::string& path) {
			if (path.empty()) return false;
			std::error_code ec;
			if (!std::filesystem::exists(path, ec) || ec) return false;
	#if defined(__linux__) || defined(__APPLE__)
			return access(path.c_str(), X_OK) == 0;
	#else
			return true;
	#endif
		};
		auto firstExecutable = [&](const std::vector<std::string>& candidates) {
			for (const std::string& candidate : candidates) {
				if (isExecutablePath(candidate)) return candidate;
			}
			return std::string();
		};
		auto findNodeBinary = [&]() {
			std::vector<std::string> candidates = {
				whichCmd("node"),
				"/opt/homebrew/bin/node",
				"/usr/local/bin/node",
				"/opt/local/bin/node",
				"/usr/bin/node"
			};
	#if defined(__APPLE__)
			const char* homeEnv = std::getenv("HOME");
			if (homeEnv && homeEnv[0]) {
				const std::string home(homeEnv);
				candidates.push_back(home + "/.nvm/current/bin/node");
				candidates.push_back(home + "/.volta/bin/node");
				candidates.push_back(home + "/.asdf/shims/node");
			}
	#endif
			return firstExecutable(candidates);
		};
		auto urlEncode = [](const std::string& value) {
			std::ostringstream out;
			out << std::hex << std::uppercase;
		for (unsigned char c : value) {
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
				out << (char)c;
			} else {
				out << '%' << std::setw(2) << std::setfill('0') << (int)c;
			}
		}
		return out.str();
	};

		std::string processingRootPath = std::filesystem::exists("../assets/processing") ? "../assets/processing" : processingBundle();
		const int processingPort = 18181 + (processingInstanceId > 0 ? (processingInstanceId % 1000) : 0);
		std::string processingStatus;
		FILE* processingPipe = nullptr;
	auto processingEngineStartTime = std::chrono::steady_clock::time_point{};
	auto currentProcessingSketchName = [&]() {
		if (processingSketchPath.empty()) return std::string("audio_spirograph.js");
		return std::filesystem::path(processingSketchPath).filename().string();
	};
	auto processingEngineUrl = [&]() {
		return std::string("http://127.0.0.1:") + std::to_string(processingPort) + "/?sketch=" + urlEncode(currentProcessingSketchName());
	};
	auto processingCaptureUrl = [&]() {
		return processingEngineUrl()
			+ "&capture=1"
			+ "&captureFps=" + std::to_string(std::max(1, processingCaptureFps))
			+ "&captureWidth=" + std::to_string(std::max(128, processingCaptureWidth))
			+ "&captureHeight=" + std::to_string(std::max(128, processingCaptureHeight));
	};
	auto startProcessingEngine = [&]() {
		if (processingPipe) {
			processingStatus = "p5.js engine already running at " + processingEngineUrl();
			return;
		}
			std::string nodeBin = findNodeBinary();
			if (nodeBin.empty()) {
				processingStatus = "Node executable not found. Install node or add it to /opt/homebrew/bin, /usr/local/bin, or PATH.";
				return;
			}
			const std::filesystem::path serverPath = std::filesystem::path(processingRootPath) / "p5_engine_server.js";
			if (!std::filesystem::exists(serverPath)) {
				processingStatus = "Missing p5 engine server: " + serverPath.string();
				return;
			}
			std::error_code frameEc;
			std::filesystem::remove(processingFrameFilePath, frameEc);
			std::filesystem::remove(processingEngineLogPath, frameEc);
			std::string cmd = shellQuote(nodeBin)
				+ " " + shellQuote(serverPath.string())
				+ " --port " + std::to_string(processingPort)
				+ " --root " + shellQuote(processingRootPath)
				+ " --sketch " + shellQuote(currentProcessingSketchName())
				+ " --frame-file " + shellQuote(processingFrameFilePath)
				+ " > " + shellQuote(processingEngineLogPath) + " 2>&1";
			processingPipe = popen(cmd.c_str(), "w");
			processingEngineStartTime = processingPipe ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			processingStatus = processingPipe
				? ("Started p5.js engine with " + nodeBin + " at " + processingEngineUrl())
				: "Failed to start p5.js engine";
	};
	auto stopProcessingEngine = [&]() {
		if (processingPipe) {
			pclose(processingPipe);
			processingPipe = nullptr;
		}
		processingEngineStartTime = std::chrono::steady_clock::time_point{};
		processingStatus = "Stopped p5.js engine";
	};
	auto openProcessingEngine = [&]() {
		const std::string url = processingEngineUrl();
#ifdef __APPLE__
		const std::string cmd = "open " + shellQuote(url) + " >/dev/null 2>&1 &";
#elif defined(_WIN32)
		const std::string cmd = "start \"\" " + shellQuote(url);
#else
		const std::string cmd = "xdg-open " + shellQuote(url) + " >/dev/null 2>&1 &";
#endif
		std::system(cmd.c_str());
		processingStatus = "Opened " + url;
	};

#if defined(__linux__) || defined(__APPLE__)
#if !defined(__linux__)
	using Window = unsigned long;
#endif
	pid_t processingBrowserPid = -1;
	std::string processingBrowserStatus;
	auto reapProcessingBrowser = [&]() {
		if (processingBrowserPid <= 0) return;
		int status = 0;
		const pid_t result = waitpid(processingBrowserPid, &status, WNOHANG);
			if (result == processingBrowserPid) {
				std::ostringstream msg;
				if (WIFEXITED(status)) {
					msg << "Chromium exited with code " << WEXITSTATUS(status);
				} else if (WIFSIGNALED(status)) {
					msg << "Chromium terminated by signal " << WTERMSIG(status);
				} else {
					msg << "Chromium viewport process exited";
				}
				msg << ". Browser log: " << processingBrowserLogPath;
				processingBrowserPid = -1;
				processingBrowserStatus = msg.str();
			}
		};
	auto findProcessingChromiumBinary = [&]() {
		std::vector<std::string> candidates = {
			whichCmd("chromium"),
			whichCmd("chromium-browser"),
			whichCmd("google-chrome-stable"),
			whichCmd("google-chrome"),
			whichCmd("chrome")
		};
#if defined(__APPLE__)
		const char* homeEnv = std::getenv("HOME");
		const std::string home = homeEnv ? std::string(homeEnv) : std::string();
		const std::vector<std::string> macApps = {
			"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
			"/Applications/Chromium.app/Contents/MacOS/Chromium",
			"/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
			"/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
			home + "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
			home + "/Applications/Chromium.app/Contents/MacOS/Chromium",
			home + "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
			home + "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser"
		};
		candidates.insert(candidates.end(), macApps.begin(), macApps.end());
#endif
			return firstExecutable(candidates);
		};
		auto launchProcessingViewportBrowser = [&]() {
			reapProcessingBrowser();
			if (processingBrowserPid > 0) return;
			if (!processingPipe) startProcessingEngine();
			if (!processingPipe) {
				processingBrowserStatus = processingStatus.empty() ? "p5 engine is not running" : processingStatus;
				return;
			}
			if (processingPipe && processingEngineStartTime != std::chrono::steady_clock::time_point{}) {
				const double ageSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - processingEngineStartTime).count();
				if (ageSec < 0.25) {
				processingBrowserStatus = "Waiting for p5 server";
				return;
			}
		}
			std::string chromiumBin = findProcessingChromiumBinary();
			if (chromiumBin.empty()) {
				processingBrowserStatus = "Chromium/Chrome not found. Install Google Chrome, Chromium, Edge, or Brave.";
				return;
			}
			const std::string userDataDir = (processingTempDir / ("visualiza_p5_chromium_profile_" + processingInstanceSuffix)).string();
		std::vector<std::string> args = {
			chromiumBin,
			"--headless=new",
			"--hide-scrollbars",
			"--mute-audio",
			"--autoplay-policy=no-user-gesture-required",
			"--disable-background-timer-throttling",
			"--disable-renderer-backgrounding",
			"--disable-backgrounding-occluded-windows",
			"--user-data-dir=" + userDataDir,
			"--no-first-run",
			"--disable-session-crashed-bubble",
			"--disable-infobars",
			"--disable-features=TranslateUI",
			"--window-size=" + std::to_string(std::max(128, processingCaptureWidth)) + "," + std::to_string(std::max(128, processingCaptureHeight)),
			processingCaptureUrl()
		};
		const pid_t pid = fork();
			if (pid == 0) {
				setsid();
				std::freopen(processingBrowserLogPath.c_str(), "w", stdout);
				std::freopen(processingBrowserLogPath.c_str(), "a", stderr);
			std::vector<char*> argv;
			argv.reserve(args.size() + 1);
			for (std::string& arg : args) argv.push_back(arg.data());
			argv.push_back(nullptr);
			execv(chromiumBin.c_str(), argv.data());
			_exit(127);
		}
			if (pid < 0) {
				processingBrowserStatus = "Failed to launch Chromium viewport";
				return;
			}
			processingBrowserPid = pid;
			processingBrowserStatus = "Launching " + chromiumBin;
	};
	auto hideProcessingViewportBrowser = [&]() {
		// Headless frame bridge has no visible window to hide.
	};
	auto attachProcessingViewportBrowser = [&](Window parent, int x, int y, int width, int height, const char* targetLabel) {
		(void)parent;
		(void)x;
		(void)y;
		reapProcessingBrowser();
			if (width <= 1 || height <= 1) return;
			launchProcessingViewportBrowser();
			const bool freshFrame = updateProcessingFrameTexture();
			if (freshFrame) {
				processingBrowserStatus = std::string("p5 frame bridge active for ") + (targetLabel ? targetLabel : "target");
			} else if (processingBrowserPid > 0) {
				processingBrowserStatus = "Waiting for p5 frames. Frame file: " + processingFrameFilePath;
			} else if (processingBrowserStatus.empty()) {
				processingBrowserStatus = "Waiting for p5 frames";
			}
		};
	auto stopProcessingViewportBrowser = [&]() {
		if (processingBrowserPid > 0) {
			kill(processingBrowserPid, SIGTERM);
			int status = 0;
			waitpid(processingBrowserPid, &status, 0);
			processingBrowserPid = -1;
		}
	};
#else
	std::string processingBrowserStatus;
	auto hideProcessingViewportBrowser = [&]() {};
	using Window = unsigned long;
	auto attachProcessingViewportBrowser = [&](Window, int, int, int, int, const char*) {
		processingBrowserStatus = "Embedded p5 viewport is only implemented on X11/Linux in this build";
	};
	auto stopProcessingViewportBrowser = [&]() {};
#endif
	auto restartProcessingEngineForCapture = [&](int width, int height, int fps) {
		width = std::clamp(width, 128, 4096);
		height = std::clamp(height, 128, 4096);
		fps = std::clamp(fps, 1, 60);
		if (processingPipe && processingCaptureWidth == width && processingCaptureHeight == height && processingCaptureFps == fps) return;
		processingCaptureWidth = width;
		processingCaptureHeight = height;
		processingCaptureFps = fps;
		processingFrameReady = false;
		processingFrameSeq = 0;
		stopProcessingViewportBrowser();
		stopProcessingEngine();
		startProcessingEngine();
	};

	// FFlive integration (external live player)
	static char fflivePath[256] = "fflive";
	static char ffgacPath[256] = "ffgac";
	static char fflInput[1024] = "";
	static char fflScript[1024] = "";
	static int  fflDisplayIndex = 0; // which monitor to fullscreen on
	std::string fflLog;
	std::mutex fflLogMutex;
	std::atomic<bool> fflRunning(false);
	std::thread fflThread;
	auto appendFflLog = [&](const std::string& s){ std::lock_guard<std::mutex> lk(fflLogMutex); fflLog += s; };
	auto buildFflCommand = [&](){
		// Detect binaries
		std::string ffliveBin = whichCmd(fflivePath); if (ffliveBin.empty()) ffliveBin = whichCmd("fflive"); if (ffliveBin.empty()) ffliveBin = fflivePath;
		std::string ffgacBin = whichCmd(ffgacPath); if (ffgacBin.empty()) ffgacBin = whichCmd("ffgac"); if (ffgacBin.empty()) ffgacBin = ffgacPath;
		// Build pipeline: ffgac -> rawvideo via stdout | fflive reads stdin
		// NOTE: we do not need UDP output; fflive renders directly
		std::string enc = std::string("\"") + ffgacBin + std::string("\" ") +
			"-nostats -hide_banner -i \"" + std::string(fflInput) + "\" -an "
			"-mpv_flags +nopimb+forcemv -qscale:v 0 -g max -sc_threshold max -vcodec mpeg4 "
			"-f rawvideo -";
		std::string live = std::string("SDL_VIDEO_FULLSCREEN_DISPLAY=") + std::to_string(fflDisplayIndex) +
			" SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=1 \"" + ffliveBin + "\" -fs -i - -s \"" + std::string(fflScript) + "\" -stats -blockffplaykeys -noframedropearly";
		std::string cmd = enc + std::string(" | ") + live + std::string(" 2>&1");
		return cmd;
	};

	// ZMQ bridge: spawn qjs zmq_sender.js and feed JSON metrics via stdin
	static char zmqSenderPath[1024] = "assets/plugins/zmq_sender.js";
	static char zmqEndpoint[256] = "ipc:///tmp/ffg_metrics";
	FILE* zmqPipe = nullptr;
	auto startZmqSender = [&](){
		if (zmqPipe) return;
		std::string cmd = std::string("qjs \"") + zmqSenderPath + std::string("\" \"") + zmqEndpoint + std::string("\"");
		zmqPipe = popen(cmd.c_str(), "w");
		if (!zmqPipe) appendFflLog("Failed to start zmq_sender.js\n");
	};
	auto stopZmqSender = [&](){ if (zmqPipe) { pclose(zmqPipe); zmqPipe = nullptr; } };
	auto runFflive = [&](){
		if (fflRunning.load()) return;
		if (fflThread.joinable()) fflThread.join();
		fflRunning = true;
		std::string cmd = buildFflCommand();
		appendFflLog(std::string("\n$ ")+cmd+"\n");
		printf("Running fflive: %s\n", cmd.c_str());
		fflThread = std::thread([&, cmd]{
			FILE* fp = popen(cmd.c_str(), "r");
			if (!fp) { appendFflLog("Failed to start fflive.\n"); fflRunning = false; return; }
			char line[512];
			while (fgets(line, sizeof(line), fp)) { appendFflLog(std::string(line)); }
			pclose(fp);
			fflRunning = false;
		});
	};
#if !VISUALIZA_HAS_FFGLITCH
	(void)runFflive;
#endif
	logStartupStage("external integration setup ok");
	// Output window / monitor selection
	GLFWwindow* outputWindow = nullptr;
	GLuint outputVao = 0; // VAO for the output context (VAOs are NOT shared between contexts)
	std::vector<GLFWmonitor*> monitorList;
	std::vector<std::string> monitorNames;
	int selectedMonitor = 0;
	{
		logStartupStage("monitor list begin");
		int mcount = 0;
		GLFWmonitor** mons = glfwGetMonitors(&mcount);
		GLFWmonitor* primary = glfwGetPrimaryMonitor();
		for (int i = 0; i < mcount; ++i) {
			monitorList.push_back(mons[i]);
			const GLFWvidmode* vm = glfwGetVideoMode(mons[i]);
			const char* name = glfwGetMonitorName(mons[i]);
			char buf[256];
			std::snprintf(buf, sizeof(buf), "%s - %dx%d @ %dHz", name ? name : "Display", vm ? vm->width : 0, vm ? vm->height : 0, vm ? vm->refreshRate : 0);
			monitorNames.emplace_back(buf);
			if (mons[i] == primary) selectedMonitor = i;
		}
		logStartupStage("monitor list ok");
	}
	auto closeOutputWindow = [&]() {
		if (!outputWindow) return;
		glfwMakeContextCurrent(outputWindow);
		if (outputVao) { glDeleteVertexArrays(1, &outputVao); outputVao = 0; }
		glfwDestroyWindow(outputWindow);
		outputWindow = nullptr;
		glfwMakeContextCurrent(window);
		if (useProcessing) processingOutputTarget = 0;
	};
	auto openOutputWindow = [&]() {
		if (outputWindow || monitorList.empty()) return;
		GLFWmonitor* mon = monitorList[std::max(0, std::min(selectedMonitor, (int)monitorList.size() - 1))];
		const GLFWvidmode* vm = glfwGetVideoMode(mon);
		if (!vm) return;
		int mx = 0, my = 0;
		glfwGetMonitorPos(mon, &mx, &my);
		glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
		glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
		glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
		glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
		outputWindow = glfwCreateWindow(vm->width, vm->height, "visual_output", nullptr, window);
		if (outputWindow) {
			glfwMakeContextCurrent(outputWindow);
			if (useGlfwSwapInterval()) glfwSwapInterval(1);
			if (outputVao) { glDeleteVertexArrays(1, &outputVao); outputVao = 0; }
			glGenVertexArrays(1, &outputVao);
			glBindVertexArray(outputVao);
			glfwSetWindowPos(outputWindow, mx, my);
			glfwMakeContextCurrent(window);
		}
	};

	auto applyLookPresetState = [&](const LookPreset& preset, const std::string& presetName) {
		if (!preset.fragmentPath.empty()) {
			fragmentPath = preset.fragmentPath;
            uiMetadataPath = fragmentPath;
			shaderProgram.buildFromFiles(fragmentPath.c_str());
			invalidateUniformUI();
			refreshCompanionPasses();
		}
		if (!preset.processingSketchPath.empty() && std::filesystem::path(preset.processingSketchPath).extension() == ".js") {
			processingSketchPath = preset.processingSketchPath;
		} else if (!processingSketchFiles.empty() && processingSketchPath.empty()) {
			processingSketchPath = processingSketchFiles.front();
		}
		useProcessing = preset.useProcessing && !processingSketchPath.empty();
		useFFglitch = preset.useFFglitch && !useProcessing;
		uiShaderFeedback = preset.shaderFeedback;
		uiHotReloadShaders = preset.hotReloadShaders;
		if (preset.previewSize != previewSize) {
			previewSize = preset.previewSize;
			createPreviewTargets();
			createCompanionTargets(previewSize);
		}
		audioTuning = preset.audioTuning;
		effectiveAudioTuning = audioTuning;
		styleAutoState = StyleAutoTuningState{};
		styleWeights = StyleProfileWeights{};
		uiMetadataPath = fragmentPath;
		invalidateUniformUI();
		currentLookPresetName = presetName;
	};

	auto applyWorkspacePresetState = [&](const WorkspacePreset& preset, const std::string& presetName) {
		applyLookPresetState(preset.look, preset.look.fragmentPath.empty() ? currentLookPresetName : preset.look.fragmentPath);
		smoothing = preset.smoothing;
		bandsGain = preset.bandsGain;
		onsetsGain = preset.onsetsGain;
		if (!monitorNames.empty()) {
			selectedMonitor = std::max(0, std::min(preset.selectedMonitor, (int)monitorNames.size() - 1));
		}
		showSourceWindow = preset.showSource;
		showVisualWindow = preset.showVisual;
		showDiagnosticsWindow = preset.showDiagnostics;
		showExpertWindow = preset.showExpert;
		showOutputWindow = preset.showOutput;
		showViewportWindow = preset.showViewport;
		if (!preset.layoutFile.empty()) {
			std::filesystem::path layoutPath = workspacePresetDir / preset.layoutFile;
			if (std::filesystem::exists(layoutPath)) {
				ImGui::LoadIniSettingsFromDisk(layoutPath.string().c_str());
			}
		}
		currentWorkspacePresetName = presetName;
	};

	{
		logStartupStage("workspace restore begin");
		WorkspacePreset startupPreset{};
		bool loadedStartupPreset = false;
		if (uiPreferences.restoreLastSessionOnStartup && loadWorkspacePreset(lastSessionWorkspacePath, startupPreset)) {
			applyWorkspacePresetState(startupPreset, "Last session");
			loadedStartupPreset = true;
		} else if (!uiPreferences.startupWorkspace.empty() &&
		           loadWorkspacePreset(workspacePresetDir / uiPreferences.startupWorkspace, startupPreset)) {
			applyWorkspacePresetState(startupPreset, uiPreferences.startupWorkspace);
			loadedStartupPreset = true;
		}
		if (!loadedStartupPreset) {
			currentWorkspacePresetName = uiPreferences.startupWorkspace.empty() ? "Built-in Default" : "Built-in Default";
		}
		if (useProcessing) startProcessingEngine();
		logStartupStage("workspace restore ok");
	}

	logStartupStage("main loop begin");
	bool firstFrameTrace = true;
	while (!glfwWindowShouldClose(window)) {
		const auto frameStartTime = std::chrono::steady_clock::now();
		glfwPollEvents();
		if (firstFrameTrace) logStartupStage("first frame poll ok");

		int display_w, display_h;
		glfwGetFramebufferSize(window, &display_w, &display_h);
		glViewport(0, 0, display_w, display_h);

		// Timing
		auto now = std::chrono::high_resolution_clock::now();
		std::chrono::duration<float> delta = now - lastTime;
		lastTime = now;
		timeSeconds += delta.count();
		frameTimeMs = delta.count() * 1000.0f;
		if (delta.count() > 1e-6f) {
			const float instantFps = 1.0f / delta.count();
			fpsDisplay = fpsDisplay <= 0.0f ? instantFps : (fpsDisplay * 0.90f + instantFps * 0.10f);
		}

	        // Audio -> Features
		if (firstFrameTrace) logStartupStage("first frame audio begin");
		std::vector<float> latest;
		bool analyzedFreshBlock = false;
		if (!freezeAudio) {
			if (useFile && filePlayer.isPlaying()) {
				filePlayer.getNext(analysisBlockFrames, latest);
				if (filePlayer.getSampleRate() != sampleRate) {
					sampleRate = filePlayer.getSampleRate();
					analyzer = AudioAnalysis(sampleRate, aaCfg);
				}
				channelCount = std::max(1u, filePlayer.getChannels());
				analyzedFreshBlock = !latest.empty();
			} else {
				analyzedFreshBlock = audioInput.readLatestIfNew(analysisBlockFrames, latest);
				unsigned int activeRate = audioInput.getActiveSampleRate();
				if (activeRate > 0 && activeRate != sampleRate) {
					sampleRate = activeRate;
					analyzer = AudioAnalysis(sampleRate, aaCfg);
				}
				channelCount = std::max(1u, audioInput.getActiveChannels());
			}
			if (analyzedFreshBlock && !latest.empty()) {
                af = analyzer.processInterleaved(latest, channelCount);
                dbgBassBody.update(af.bassBody);
                dbgHarmonicBody.update(af.harmonicBody);
                dbgAirPresence.update(af.airPresence);
                dbgLeadPresence.update(af.leadPresence);
                dbgRelease.update(af.release);
                dbgBodyComposite.update(std::max({af.subBody, af.bassBody, af.harmonicBody, af.leadPresence, af.airPresence}));
				const auto analysisNow = std::chrono::steady_clock::now();
				const double updateDtSec = std::chrono::duration<double>(analysisNow - lastAnalysisUpdateTime).count();
				lastAnalysisUpdateTime = analysisNow;
				if (updateDtSec > 1e-6) {
					analysisUpdateRateHz = float(1.0 / updateDtSec);
				}
			}
		}
		if (firstFrameTrace) logStartupStage("first frame audio ok");
		if (!freezeAudio) {
			styleWeights = detectStyleProfile(af, styleAutoState, delta.count(), audioTuning.autoStyleResponseSec);
		}
		effectiveAudioTuning = buildEffectiveAudioTuning(audioTuning, styleWeights);
		shaderAf = applyAudioTuning(af, effectiveAudioTuning);
		shaderMetrics = deriveShaderAudioMetrics(shaderAf);
		// Simple decay for visual onset indicator
		onsetDisplay = std::max(shaderMetrics.onset, onsetDisplay * std::pow(0.5f, delta.count() * 10.0f));
		float instantBeatRate = (af.bpm > 0.0f) ? af.bpm : (af.beatTriggered ? 60.0f / std::max(0.001f, aaCfg.beatMinIntervalSec) : 0.0f);
		beatRateDisplay = beatRateDisplay * 0.92f + instantBeatRate * 0.08f;
		if (!useFile && audioInput.isStreamRunning()) {
			const auto statsNow = std::chrono::steady_clock::now();
			const double statsDtSec = std::chrono::duration<double>(statsNow - lastLiveStatsLogTime).count();
			if (statsDtSec >= 1.0) {
				lastLiveStatsLogTime = statsNow;
				std::fprintf(stderr, "Live stats: capture %.1f ms | source %.1f Hz | shader %.1f Hz | rms %.3f\n",
					audioInput.getCaptureLatencyMs(), audioInput.getFreshBlockRateHz(), analysisUpdateRateHz, shaderMetrics.rms);
			}
		}

		// Shader hot reload is on by default for live authoring; the UI checkbox can disable the filesystem checks.
		if (uiHotReloadShaders) {
			if (!useProcessing) {
				shaderProgram.updateIfChanged(fragmentPath.c_str());
				for (auto& pass : companionPasses) {
					if (pass.active) pass.program.updateIfChanged(pass.path.c_str());
				}
			}
		}

		// (moved) Send ZMQ metrics after smoothing is computed

		// Smoothed uniforms (compute before setting so lambda can use them)
		static ShaderAudioMetrics smoothed{};
		smoothed.rms = smoothed.rms * smoothing + shaderMetrics.rms * (1.0f - smoothing);
		smoothed.bandLow = smoothed.bandLow * smoothing + shaderMetrics.bandLow * (1.0f - smoothing);
		smoothed.bandMid = smoothed.bandMid * smoothing + shaderMetrics.bandMid * (1.0f - smoothing);
		smoothed.bandHigh = smoothed.bandHigh * smoothing + shaderMetrics.bandHigh * (1.0f - smoothing);
		smoothed.onset = onsetDisplay;
		smoothed.flux = smoothed.flux * smoothing + shaderMetrics.flux * (1.0f - smoothing);
		smoothed.percE = smoothed.percE * smoothing + shaderMetrics.percE * (1.0f - smoothing);
		smoothed.harmE = smoothed.harmE * smoothing + shaderMetrics.harmE * (1.0f - smoothing);

		// Send live metrics to external engines after smoothing exists.
		if (zmqPipe || processingPipe) {
			auto appendFloatArray = [](std::ostringstream& out, const char* name, const std::vector<float>& values, const float* scale, float gain) {
				out << ",\"" << name << "\":[";
				const size_t count = std::min<size_t>(16, values.size());
				for (size_t i = 0; i < count; ++i) {
					if (i > 0) out << ",";
					const float s = scale ? scale[i] : 1.0f;
					out << std::setprecision(4) << clamp01(values[i] * s * gain);
				}
				out << "]";
			};
			std::ostringstream json;
			json << std::fixed << std::setprecision(4)
				<< "{\"rms\":" << smoothed.rms
				<< ",\"bandLow\":" << smoothed.bandLow
				<< ",\"bandMid\":" << smoothed.bandMid
				<< ",\"bandHigh\":" << smoothed.bandHigh
				<< ",\"onset\":" << smoothed.onset
				<< ",\"flux\":" << smoothed.flux
				<< ",\"bpm\":" << std::setprecision(2) << shaderAf.bpm << std::setprecision(4)
				<< ",\"beatEnv\":" << shaderAf.beatEnvelope
				<< ",\"beat\":" << (shaderAf.beatTriggered ? 1.0f : 0.0f)
				<< ",\"percE\":" << smoothed.percE
				<< ",\"harmE\":" << smoothed.harmE
				<< ",\"percRatio\":" << shaderAf.percussiveRatio
				<< ",\"kick\":" << shaderAf.kickImpact
				<< ",\"snare\":" << shaderAf.snareImpact
				<< ",\"hat\":" << shaderAf.hatTick
				<< ",\"beatPulse\":" << shaderAf.beatPulse
				<< ",\"sub\":" << shaderAf.subBody
				<< ",\"bass\":" << shaderAf.bassBody
				<< ",\"body\":" << shaderAf.harmonicBody
				<< ",\"harmonic\":" << shaderAf.harmonicBody
				<< ",\"lead\":" << shaderAf.leadPresence
				<< ",\"air\":" << shaderAf.airPresence
				<< ",\"transient\":" << shaderAf.transientDensity
				<< ",\"novelty\":" << shaderAf.novelty
				<< ",\"brightness\":" << shaderAf.brightness
				<< ",\"percussive\":" << shaderAf.percussiveFocus
				<< ",\"energy\":" << shaderAf.energyLevel
				<< ",\"tension\":" << shaderAf.tension
				<< ",\"release\":" << shaderAf.release
				<< ",\"drop\":" << shaderAf.dropEvent
				<< ",\"section\":" << shaderAf.sectionChange
				<< ",\"roleKickCenterNorm\":" << shaderAf.roleKickCenterNorm
				<< ",\"roleBassCenterNorm\":" << shaderAf.roleBassCenterNorm
				<< ",\"roleLeadCenterNorm\":" << shaderAf.roleLeadCenterNorm
				<< ",\"roleAirCenterNorm\":" << shaderAf.roleAirCenterNorm
				<< ",\"roleKickConfidence\":" << shaderAf.roleKickConfidence
				<< ",\"roleBassConfidence\":" << shaderAf.roleBassConfidence
				<< ",\"roleLeadConfidence\":" << shaderAf.roleLeadConfidence
				<< ",\"roleAirConfidence\":" << shaderAf.roleAirConfidence;
			appendFloatArray(json, "bands", shaderAf.bandEnergyNorm, bandScale, bandsGain);
			appendFloatArray(json, "onsets", shaderAf.bandOnset, onsetScale, onsetsGain);
			json << "}\n";
			const std::string line = json.str();
			auto writePipeLine = [](FILE* pipe, const std::string& text) {
				if (!pipe || text.empty()) return;
				fwrite(text.data(), 1, text.size(), pipe);
				fflush(pipe);
			};
			writePipeLine(zmqPipe, line);
			writePipeLine(processingPipe, line);
		}

		auto setAllUniforms = [&](ShaderProgram& program, int resx, int resy, bool includeUiUniforms){
			program.setUniform("u_time", timeSeconds);
			program.setUniform("u_resolution", (float)resx, (float)resy);

			program.setUniform("u_rms", smoothed.rms);
			program.setUniform("u_bandLow", smoothed.bandLow);
			program.setUniform("u_bandMid", smoothed.bandMid);
			program.setUniform("u_bandHigh", smoothed.bandHigh);
			program.setUniform("u_onset", smoothed.onset);
			// Optional RM spheres controls (harmless if shader doesn't declare them)
			program.setUniform("u_camVel", ui_camVel);
			program.setUniform("u_useCamVel", ui_useCamVel ? 1.0f : 0.0f);
			program.setUniform("u_warpIntensity", ui_warpIntensity);
			program.setUniform("u_stepFactor", ui_stepFactor);
			program.setUniform("u_epsilon", ui_epsilon);
			program.setUniform("u_maxStepsF", (float)ui_maxSteps);
			program.setUniform("u_fogDensity", ui_fogDensity);
			program.setUniform("u_barrelK", ui_barrelK);
			program.setUniform("u_colorGain", ui_colorGain);
			program.setUniform("u_cellScale", ui_cellScale);
			program.setUniform("u_r1Base", ui_r1Base);
			program.setUniform("u_r2Base", ui_r2Base);
			program.setUniform("u_flashGain", ui_flashGain);
			// Advanced analysis arrays and scalars
			if (!shaderAf.bandEnergyNorm.empty()) {
				int n = (int)std::min<size_t>(16, shaderAf.bandEnergyNorm.size());
				float bands[16];
				float onsets[16];
				for (int i = 0; i < n; ++i) {
					bands[i] = std::max(0.0f, std::min(1.0f, shaderAf.bandEnergyNorm[i] * bandScale[i] * bandsGain));
					onsets[i] = std::max(0.0f, std::min(1.0f, shaderAf.bandOnset[i] * onsetScale[i] * onsetsGain));
				}
				program.setUniform1fv("u_bands", bands, n);
				program.setUniform1fv("u_onsets", onsets, n);
			}
				float centroidNorm = 0.0f;
				if (analyzer.getFftSize() > 0) {
					float nyq = (float)analyzer.getSampleRate() * 0.5f;
					centroidNorm = nyq > 1.0f ? std::max(0.0f, std::min(1.0f, shaderAf.spectralCentroidHz / nyq)) : 0.0f;
				}
				program.setUniform("u_centroidNorm", centroidNorm);
				program.setUniform("u_flux", smoothed.flux);
				program.setUniform("u_beat", shaderAf.beatTriggered ? 1.0f : 0.0f);
				program.setUniform("u_beatEnv", shaderAf.beatEnvelope);
				program.setUniform("u_bpm", shaderAf.bpm);
				program.setUniform("u_percE", smoothed.percE);
				program.setUniform("u_harmE", smoothed.harmE);
				program.setUniform("u_percRatio", shaderAf.percussiveRatio);
				program.setUniform("u_kickImpact", shaderAf.kickImpact);
				program.setUniform("u_snareImpact", shaderAf.snareImpact);
				program.setUniform("u_hatTick", shaderAf.hatTick);
				program.setUniform("u_beatPulse", shaderAf.beatPulse);
				program.setUniform("u_subBody", shaderAf.subBody);
				program.setUniform("u_bassBody", shaderAf.bassBody);
				program.setUniform("u_harmonicBody", shaderAf.harmonicBody);
				program.setUniform("u_leadPresence", shaderAf.leadPresence);
				program.setUniform("u_airPresence", shaderAf.airPresence);
				program.setUniform("u_transientDensity", shaderAf.transientDensity);
				program.setUniform("u_novelty", shaderAf.novelty);
				program.setUniform("u_brightness", shaderAf.brightness);
				program.setUniform("u_percussiveFocus", shaderAf.percussiveFocus);
				program.setUniform("u_energyLevel", shaderAf.energyLevel);
				program.setUniform("u_tension", shaderAf.tension);
				program.setUniform("u_release", shaderAf.release);
				program.setUniform("u_dropEvent", shaderAf.dropEvent);
				program.setUniform("u_sectionChange", shaderAf.sectionChange);
				program.setUniform("u_roleKickCenterNorm", shaderAf.roleKickCenterNorm);
				program.setUniform("u_roleBassCenterNorm", shaderAf.roleBassCenterNorm);
				program.setUniform("u_roleLeadCenterNorm", shaderAf.roleLeadCenterNorm);
				program.setUniform("u_roleAirCenterNorm", shaderAf.roleAirCenterNorm);
				program.setUniform("u_roleKickConfidence", shaderAf.roleKickConfidence);
				program.setUniform("u_roleBassConfidence", shaderAf.roleBassConfidence);
				program.setUniform("u_roleLeadConfidence", shaderAf.roleLeadConfidence);
				program.setUniform("u_roleAirConfidence", shaderAf.roleAirConfidence);
			// Push auto UI uniforms if any
			if (includeUiUniforms) {
				for (const auto& uname : uiUniformOrder) {
					auto it = uiUniforms.find(uname);
					if (it == uiUniforms.end()) continue;
					const UIUniform& u = it->second;
					if (u.components == 1) {
						if (u.type == GL_INT || u.type == GL_BOOL) program.setUniformi(uname.c_str(), (int)u.values[0]);
						else program.setUniform(uname.c_str(), u.values[0]);
					} else if (u.components == 2) {
						program.setUniform(uname.c_str(), u.values[0], u.values[1]);
					} else if (u.components == 3) {
						program.setUniform(uname.c_str(), u.values[0], u.values[1], u.values[2]);
					} else if (u.components == 4) {
						program.setUniform(uname.c_str(), u.values[0], u.values[1], u.values[2], u.values[3]);
					}
				}
			}
		};

		auto bindShaderChannels = [&](ShaderProgram& program, const std::array<GLuint, 5>& textures) {
			for (int i = 0; i < 5; ++i) {
				char name[16];
				std::snprintf(name, sizeof(name), "iChannel%d", i);
				program.setUniformi(name, i);
				glActiveTexture(GL_TEXTURE0 + i);
				glBindTexture(GL_TEXTURE_2D, textures[i]);
			}
			program.setUniform("iResolution", (float)previewSize, (float)previewSize);
		};

		// Render preview to offscreen square target
		if (firstFrameTrace) logStartupStage("first frame preview render begin");
		glBindFramebuffer(GL_FRAMEBUFFER, previewFbo);
		glViewport(0, 0, previewSize, previewSize);
		glClearColor(0.03f, 0.03f, 0.05f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		if (!useFFglitch && !useProcessing) {
			bool hasCompanion = false;
			for (const auto& pass : companionPasses) hasCompanion = hasCompanion || pass.active;
			if (hasCompanion) {
				if (uiUniforms.empty()) rebuildUniformUI(*getUiControlProgram());
				std::array<GLuint, 5> latestTextures = {0, 0, 0, 0, 0};
				for (size_t i = 0; i < companionPasses.size(); ++i) {
					if (companionPasses[i].active) latestTextures[i] = companionPasses[i].tex[companionPasses[i].readIndex];
				}
				for (size_t i = 0; i < companionPasses.size(); ++i) {
					auto& pass = companionPasses[i];
					if (!pass.active) continue;
					for (int iter = 0; iter < pass.iterations; ++iter) {
						glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pass.tex[pass.writeIndex], 0);
						glClear(GL_COLOR_BUFFER_BIT);
						pass.program.use();
						setAllUniforms(pass.program, previewSize, previewSize, pass.program.getProgramId() == uiTargetProgramId);
						bindShaderChannels(pass.program, latestTextures);
						pass.program.setUniform("u_passIteration", (float)iter);
						glDrawArrays(GL_TRIANGLES, 0, 3);
						std::swap(pass.readIndex, pass.writeIndex);
						latestTextures[i] = pass.tex[pass.readIndex];
					}
				}
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, previewTex, 0);
				shaderProgram.use();
				setAllUniforms(shaderProgram, previewSize, previewSize, shaderProgram.getProgramId() == uiTargetProgramId);
				bindShaderChannels(shaderProgram, latestTextures);
				glDrawArrays(GL_TRIANGLES, 0, 3);
				previewDisplayTex = previewTex;
				for (int i = 0; i < 5; ++i) {
					glActiveTexture(GL_TEXTURE0 + i);
					glBindTexture(GL_TEXTURE_2D, 0);
				}
				glActiveTexture(GL_TEXTURE0);
			} else {
				if (uiUniforms.empty()) rebuildUniformUI(shaderProgram);
				GLuint renderTarget = uiShaderFeedback ? feedbackTex[fbWrite] : previewTex;
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderTarget, 0);
				previewDisplayTex = uiShaderFeedback ? feedbackTex[fbRead] : previewTex;
				shaderProgram.use();
				setAllUniforms(shaderProgram, previewSize, previewSize, shaderProgram.getProgramId() == uiTargetProgramId);
				if (uiShaderFeedback) {
					std::array<GLuint, 5> feedbackTextures = { feedbackTex[fbRead], 0, 0, 0, 0 };
					bindShaderChannels(shaderProgram, feedbackTextures);
				}
				glDrawArrays(GL_TRIANGLES, 0, 3);
				if (uiShaderFeedback) {
					std::swap(fbRead, fbWrite);
					previewDisplayTex = feedbackTex[fbRead];
					for (int i = 0; i < 5; ++i) {
						glActiveTexture(GL_TEXTURE0 + i);
						glBindTexture(GL_TEXTURE_2D, 0);
					}
					glActiveTexture(GL_TEXTURE0);
				}
			}
		} else {
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, previewTex, 0);
			previewDisplayTex = previewTex;
			if (blitProgram) {
				glUseProgram(blitProgram);
				if (blitTexLoc >= 0) glUniform1i(blitTexLoc, 0);
			}
		}
		if (useFFglitch) {
			ffgPlayer.update(timeSeconds);
			GLuint tex = ffgPlayer.getTextureId();
			if (tex) {
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, tex);
				glDrawArrays(GL_TRIANGLES, 0, 3);
				glBindTexture(GL_TEXTURE_2D, 0);
			}
			// if no texture yet, skip draw to avoid sampling an unbound texture
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (firstFrameTrace) logStartupStage("first frame preview render ok");

		// Clear main window framebuffer for UI
		glViewport(0, 0, display_w, display_h);
		glClearColor(0.03f, 0.03f, 0.05f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		// Render to output fullscreen window if present
		if (outputWindow) {
			if (glfwWindowShouldClose(outputWindow)) {
				glfwMakeContextCurrent(outputWindow);
				if (outputVao) { glDeleteVertexArrays(1, &outputVao); outputVao = 0; }
				glfwDestroyWindow(outputWindow);
				outputWindow = nullptr;
				if (useProcessing) processingOutputTarget = 0;
			} else {
				glfwMakeContextCurrent(outputWindow);
				int ow, oh;
				glfwGetFramebufferSize(outputWindow, &ow, &oh);
				glViewport(0, 0, ow, oh);
				glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
				glClear(GL_COLOR_BUFFER_BIT);
				if (useProcessing && processingOutputTarget == 1) {
					restartProcessingEngineForCapture(std::max(1, ow), std::max(1, oh), 60);
	#if defined(__linux__)
					attachProcessingViewportBrowser(glfwGetX11Window(outputWindow), 0, 0, ow, oh, "fullscreen output");
	#else
					attachProcessingViewportBrowser(0, 0, 0, ow, oh, "fullscreen output");
	#endif
					if (processingFrameReady && processingFrameTex) {
						if (outputVao == 0) { glGenVertexArrays(1, &outputVao); }
						glBindVertexArray(outputVao);
						if (blitProgram) {
							glUseProgram(blitProgram);
							if (blitTexLoc >= 0) glUniform1i(blitTexLoc, 0);
						}
						glActiveTexture(GL_TEXTURE0);
						glBindTexture(GL_TEXTURE_2D, processingFrameTex);
						glDrawArrays(GL_TRIANGLES, 0, 3);
						glBindTexture(GL_TEXTURE_2D, 0);
					}
				} else {
					if (outputVao == 0) { glGenVertexArrays(1, &outputVao); }
					glBindVertexArray(outputVao);
					if (blitProgram) {
						glUseProgram(blitProgram);
						if (blitTexLoc >= 0) glUniform1i(blitTexLoc, 0);
					}
					glActiveTexture(GL_TEXTURE0);
					glBindTexture(GL_TEXTURE_2D, previewDisplayTex);
					glDrawArrays(GL_TRIANGLES, 0, 3);
					glBindTexture(GL_TEXTURE_2D, 0);
				}
				glfwSwapBuffers(outputWindow);
				glfwMakeContextCurrent(window);
			}
		}

		// UI
		if (firstFrameTrace) logStartupStage("first frame imgui begin");
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		// Dockspace host window
		{
			ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
			ImGuiViewport* vp = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(vp->Pos);
			ImGui::SetNextWindowSize(vp->Size);
			// Viewport docking used implicitly by docking branch; no SetNextWindowViewport call needed
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			ImGui::Begin("DockSpaceRoot", nullptr, window_flags);
			ImGui::PopStyleVar(2);
			ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0,0), ImGuiDockNodeFlags_PassthruCentralNode);
        static bool dockBuilt = false;
        if (!dockBuilt || requestDockLayoutReset) {
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
            ImGui::DockBuilderSetNodeSize(dockspace_id, vp->Size);
            ImGuiID dock_left = 0, dock_right = 0, dock_bottom = 0, dock_diag = 0, dock_left_top = 0;
            dock_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.34f, nullptr, &dockspace_id);
            dock_right = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.30f, nullptr, &dockspace_id);
            dock_bottom = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.22f, nullptr, &dockspace_id);
            dock_left_top = ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Down, 0.44f, nullptr, &dock_left);
            dock_diag = dock_left;
            ImGui::DockBuilderDockWindow("Source", dock_left_top);
            ImGui::DockBuilderDockWindow("Visual", dock_right);
            ImGui::DockBuilderDockWindow("Diagnostics", dock_diag);
            ImGui::DockBuilderDockWindow("Expert", dock_diag);
            ImGui::DockBuilderDockWindow("Output", dock_bottom);
            ImGui::DockBuilderDockWindow("Viewport", dockspace_id);
            ImGui::DockBuilderFinish(dockspace_id);
            dockBuilt = true;
            requestDockLayoutReset = false;
        }
			ImGui::End();
		}

        // Visual UI
		{
			ImGuiWindowFlags sflags = ImGuiWindowFlags_NoCollapse;
			if (ImGui::Begin("Visual", &showVisualWindow, sflags)) {
			pushContentWrapPos();
			ImGui::PushItemWidth(-FLT_MIN);
			int mode = useProcessing ? 1 : (useFFglitch ? 2 : 0);
			if (ImGui::RadioButton("Shader##visual_mode", mode == 0)) {
				mode = 0;
				useProcessing = false;
				useFFglitch = false;
				processingOutputTarget = 0;
				uiMetadataPath = fragmentPath;
				invalidateUniformUI();
			}
			ImGui::SameLine();
			if (ImGui::RadioButton("Processing##visual_mode", mode == 1)) {
				mode = 1;
				useProcessing = true;
				useFFglitch = false;
				processingOutputTarget = 0;
				uiMetadataPath = fragmentPath;
				startProcessingEngine();
				invalidateUniformUI();
			}
#if VISUALIZA_HAS_FFGLITCH
			ImGui::SameLine();
			if (ImGui::RadioButton("FFglitch##visual_mode", mode == 2)) {
				mode = 2;
				useProcessing = false;
				useFFglitch = true;
				processingOutputTarget = 0;
				invalidateUniformUI();
			}
#else
			if (useFFglitch) {
				useFFglitch = false;
				mode = useProcessing ? 1 : 0;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("FFglitch disabled");
#endif
			ImGui::Separator();
            if (ImGui::CollapsingHeader("Active Visual", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::string cur = useProcessing ? processingSketchPath : fragmentPath;
                size_t slash = cur.find_last_of("/\\");
                if (slash != std::string::npos) cur = cur.substr(slash + 1);
                ImGui::TextWrapped("Current: %s", cur.c_str());
            }
			if (mode == 0) {
                ImGui::TextUnformatted("Shader");
                if (ImGui::BeginCombo("##visual_shader_select", "Select")) {
					for (size_t i = 0; i < shaderFiles.size(); ++i) {
						std::string name = shaderFiles[i]; size_t s = name.find_last_of("/\\"); if (s != std::string::npos) name = name.substr(s + 1);
						bool sel = (shaderFiles[i] == fragmentPath);
                        if (ImGui::Selectable(name.c_str(), sel)) { fragmentPath = shaderFiles[i]; uiMetadataPath = fragmentPath; shaderProgram.buildFromFiles(fragmentPath.c_str()); invalidateUniformUI(); refreshCompanionPasses(); }
						if (sel) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
                if (ImGui::Button("Reload Visual")) { uiMetadataPath = fragmentPath; shaderProgram.forceReload(fragmentPath.c_str()); invalidateUniformUI(); refreshCompanionPasses(); }
                ImGui::SameLine();
                if (ImGui::Button("Refresh Shader List")) {
                    shaderFiles = collectShaders({"../assets/shaders", bundleAssets(), "assets/shaders"});
                    shadersDir = std::filesystem::exists("../assets/shaders") ? "../assets/shaders" : bundleAssets();
                    refreshCompanionPasses();
                }
                ImGui::Separator();
                if (ImGui::CollapsingHeader("Presets", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (currentLookPresetName.empty()) currentLookPresetName = std::filesystem::path(fragmentPath).filename().string();
                    if (ImGui::BeginTable("look_presets", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                        ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthStretch, 1.3f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::InputText("##look_preset_name", lookPresetName, IM_ARRAYSIZE(lookPresetName));
                        ImGui::TableSetColumnIndex(1);
                        std::string currentLookPreset = currentLookPresetName.empty()
                            ? (lookPresetFiles.empty() ? std::string("No look presets") : lookPresetFiles.front())
                            : currentLookPresetName;
                        if (ImGui::BeginCombo("##look_preset_load", currentLookPreset.c_str())) {
                            for (const auto& presetFile : lookPresetFiles) {
                                if (ImGui::Selectable(presetFile.c_str(), false)) {
                                    LookPreset preset{};
                                    if (loadLookPreset(lookPresetDir / presetFile, preset)) {
                                        applyLookPresetState(preset, presetFile);
                                        presetStatus = "Loaded look " + presetFile;
                                    } else {
                                        presetStatus = "Failed to load look " + presetFile;
                                    }
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::TableSetColumnIndex(2);
                        if (ImGui::Button("Save look")) {
                            LookPreset preset{};
                            preset.fragmentPath = fragmentPath;
                            preset.processingSketchPath = processingSketchPath;
                            preset.useProcessing = useProcessing;
                            preset.useFFglitch = useFFglitch;
                            preset.shaderFeedback = uiShaderFeedback;
                            preset.hotReloadShaders = uiHotReloadShaders;
                            preset.previewSize = previewSize;
                            preset.audioTuning = audioTuning;
                            std::string stem = std::strlen(lookPresetName) > 0 ? lookPresetName : "default_look";
                            if (saveLookPreset(lookPresetDir / (stem + ".cfg"), preset)) {
                                currentLookPresetName = stem + ".cfg";
                                presetStatus = "Saved look " + stem + ".cfg";
                                refreshPresetFiles(lookPresetDir, lookPresetFiles);
                            } else {
                                presetStatus = "Failed to save look preset";
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Refresh looks")) refreshPresetFiles(lookPresetDir, lookPresetFiles);
                        ImGui::EndTable();
                    }
                    if (!presetStatus.empty()) ImGui::TextDisabled("%s", presetStatus.c_str());

                    if (ImGui::BeginTable("workspace_presets_visual", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("Workspace", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                        ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthStretch, 1.3f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::InputText("##workspace_preset_name_visual", workspacePresetName, IM_ARRAYSIZE(workspacePresetName));
                        ImGui::TableSetColumnIndex(1);
                        std::string currentWorkspacePreset = currentWorkspacePresetName.empty()
                            ? (workspacePresetFiles.empty() ? std::string("Built-in Default") : workspacePresetFiles.front())
                            : currentWorkspacePresetName;
                        if (ImGui::BeginCombo("##workspace_preset_load_visual", currentWorkspacePreset.c_str())) {
                            if (ImGui::Selectable("Built-in Default", currentWorkspacePresetName == "Built-in Default")) {
                                showSourceWindow = true;
                                showVisualWindow = true;
                                showDiagnosticsWindow = true;
                                showExpertWindow = true;
                                showOutputWindow = true;
                                showViewportWindow = true;
                                requestDockLayoutReset = true;
                                currentWorkspacePresetName = "Built-in Default";
                                presetStatus = "Using built-in default workspace";
                            }
                            for (const auto& presetFile : workspacePresetFiles) {
                                if (ImGui::Selectable(presetFile.c_str(), false)) {
                                    WorkspacePreset preset{};
                                    if (loadWorkspacePreset(workspacePresetDir / presetFile, preset)) {
                                        applyWorkspacePresetState(preset, presetFile);
                                        presetStatus = "Loaded workspace " + presetFile;
                                    } else {
                                        presetStatus = "Failed to load workspace " + presetFile;
                                    }
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::TableSetColumnIndex(2);
                        if (ImGui::Button("Save workspace##visual")) {
                            WorkspacePreset preset{};
                            preset.look.fragmentPath = fragmentPath;
                            preset.look.processingSketchPath = processingSketchPath;
                            preset.look.useProcessing = useProcessing;
                            preset.look.useFFglitch = useFFglitch;
                            preset.look.shaderFeedback = uiShaderFeedback;
                            preset.look.hotReloadShaders = uiHotReloadShaders;
                            preset.look.previewSize = previewSize;
                            preset.look.audioTuning = audioTuning;
                            preset.smoothing = smoothing;
                            preset.bandsGain = bandsGain;
                            preset.onsetsGain = onsetsGain;
                            preset.selectedMonitor = selectedMonitor;
                            preset.showSource = showSourceWindow;
                            preset.showVisual = showVisualWindow;
                            preset.showDiagnostics = showDiagnosticsWindow;
                            preset.showExpert = showExpertWindow;
                            preset.showOutput = showOutputWindow;
                            preset.showViewport = showViewportWindow;
                            std::string stem = std::strlen(workspacePresetName) > 0 ? workspacePresetName : "default_workspace";
                            if (saveWorkspacePreset(workspacePresetDir / (stem + ".cfg"), preset, io.IniFilename)) {
                                currentWorkspacePresetName = stem + ".cfg";
                                presetStatus = "Saved workspace " + stem + ".cfg";
                                refreshPresetFiles(workspacePresetDir, workspacePresetFiles);
                            } else {
                                presetStatus = "Failed to save workspace";
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Refresh workspaces##visual")) refreshPresetFiles(workspacePresetDir, workspacePresetFiles);
                        ImGui::EndTable();
                    }

                    if (ImGui::BeginTable("startup_workspace_prefs", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("Startup", ImGuiTableColumnFlags_WidthStretch, 0.95f);
                        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 1.35f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted("Restore last session");
                        ImGui::TableSetColumnIndex(1);
                        if (ImGui::Checkbox("##restore_last_session_startup", &uiPreferences.restoreLastSessionOnStartup)) {
                            saveUiPreferences(uiPreferencesPath, uiPreferences);
                        }
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted("Startup workspace");
                        ImGui::TableSetColumnIndex(1);
                        std::string startupWorkspaceLabel = uiPreferences.startupWorkspace.empty() ? "Built-in Default" : uiPreferences.startupWorkspace;
                        if (ImGui::BeginCombo("##startup_workspace_select", startupWorkspaceLabel.c_str())) {
                            if (ImGui::Selectable("Built-in Default", uiPreferences.startupWorkspace.empty())) {
                                uiPreferences.startupWorkspace.clear();
                                saveUiPreferences(uiPreferencesPath, uiPreferences);
                            }
                            for (const auto& presetFile : workspacePresetFiles) {
                                bool selected = (uiPreferences.startupWorkspace == presetFile);
                                if (ImGui::Selectable(presetFile.c_str(), selected)) {
                                    uiPreferences.startupWorkspace = presetFile;
                                    saveUiPreferences(uiPreferencesPath, uiPreferences);
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::EndTable();
                    }
                    ImGui::TextWrapped("Startup uses the built-in performance workspace unless you enable last-session restore or choose a saved workspace.");
                }
                ImGui::Separator();
                if (ImGui::CollapsingHeader("Audio Tuning", ImGuiTreeNodeFlags_DefaultOpen)) {
                    static std::unordered_map<std::string, float> tuningMeterPeaks;
                    const float autoStyleStrength = audioTuning.autoStyleEnabled ? clamp01(audioTuning.autoStyleBlend * styleWeights.activity) : 0.0f;
                    auto pulseLiveRaw = clamp01(std::max({af.kickImpact, af.snareImpact, af.beatPulse, af.dropEvent}));
                    auto pulseLiveTuned = clamp01(std::max({shaderAf.kickImpact, shaderAf.snareImpact, shaderAf.beatPulse, shaderAf.dropEvent}));
                    auto bodyLiveRaw = clamp01(std::max({af.subBody, af.bassBody, af.harmonicBody, af.leadPresence, af.airPresence}));
                    auto bodyLiveTuned = clamp01(std::max({shaderAf.subBody, shaderAf.bassBody, shaderAf.harmonicBody, shaderAf.leadPresence, shaderAf.airPresence}));
                    auto detailLiveRaw = clamp01(std::max({af.hatTick, af.transientDensity, af.novelty, af.brightness, af.percussiveFocus}));
                    auto detailLiveTuned = clamp01(std::max({shaderAf.hatTick, shaderAf.transientDensity, shaderAf.novelty, shaderAf.brightness, shaderAf.percussiveFocus}));
                    auto macroLiveRaw = clamp01(std::max({af.energyLevel, af.tension, af.release, af.sectionChange}));
                    auto macroLiveTuned = clamp01(std::max({shaderAf.energyLevel, shaderAf.tension, shaderAf.release, shaderAf.sectionChange}));
                    auto drawLiveMeter = [](const char* id, float value) {
                        float& peak = tuningMeterPeaks[id];
                        peak = std::max(value, peak * 0.965f);
                        peak = std::max(peak, 0.08f);
                        float barValue = clamp01(value / peak);
                        char overlay[16];
                        std::snprintf(overlay, sizeof(overlay), "%.2f", value);
                        ImGui::ProgressBar(barValue, ImVec2(-FLT_MIN, 0.0f), overlay);
                    };
                    auto drawTuningRow = [&](const char* label, const char* id, float* value, float minv, float maxv, float rawLive, float tunedLive) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(label);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        ImGui::SliderFloat(id, value, minv, maxv);
                        ImGui::TableSetColumnIndex(2);
                        drawLiveMeter((std::string(id) + "_raw").c_str(), rawLive);
                        ImGui::TableSetColumnIndex(3);
                        drawLiveMeter((std::string(id) + "_tuned").c_str(), tunedLive);
                    };
                    if (ImGui::BeginTable("audio_tuning_presets", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                        ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthStretch, 1.3f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::InputText("##audio_tuning_preset", audioTuningPresetName, IM_ARRAYSIZE(audioTuningPresetName));
                        ImGui::TableSetColumnIndex(1);
                        std::string currentPreset = audioTuningPresetFiles.empty() ? std::string("No presets") : audioTuningPresetFiles.front();
                        if (ImGui::BeginCombo("##audio_tuning_load", currentPreset.c_str())) {
                            for (const auto& presetFile : audioTuningPresetFiles) {
                                bool selected = false;
                                if (ImGui::Selectable(presetFile.c_str(), selected)) {
                                    if (loadAudioTuningProfile(audioTuningDir / presetFile, audioTuning)) {
                                        styleAutoState = StyleAutoTuningState{};
                                        styleWeights = StyleProfileWeights{};
                                        audioTuningStatus = "Loaded " + presetFile;
                                        std::snprintf(audioTuningPresetName, sizeof(audioTuningPresetName), "%s", std::filesystem::path(presetFile).stem().string().c_str());
                                    } else {
                                        audioTuningStatus = "Failed to load " + presetFile;
                                    }
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::TableSetColumnIndex(2);
                        if (ImGui::Button("Save preset")) {
                            std::string stem = std::strlen(audioTuningPresetName) > 0 ? audioTuningPresetName : "live_default";
                            std::filesystem::path outPath = audioTuningDir / (stem + ".cfg");
                            std::error_code ec;
                            std::filesystem::create_directories(audioTuningDir, ec);
                            if (!ec && saveAudioTuningProfile(outPath, audioTuning)) {
                                audioTuningStatus = "Saved " + outPath.filename().string();
                                refreshAudioTuningPresets();
                            } else {
                                audioTuningStatus = "Failed to save preset";
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Reset tuning")) {
                            audioTuning = makeDefaultAudioTuningProfile();
                            styleAutoState = StyleAutoTuningState{};
                            styleWeights = StyleProfileWeights{};
                            audioTuningStatus = "Reset audio tuning to defaults";
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Refresh list")) refreshAudioTuningPresets();
                        ImGui::EndTable();
                    }
                    if (!audioTuningStatus.empty()) ImGui::TextWrapped("%s", audioTuningStatus.c_str());
                    ImGui::Separator();
                    if (ImGui::BeginTable("audio_style_auto", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enable auto style tuning");
                        ImGui::TableSetColumnIndex(1); ImGui::Checkbox("##auto_style_enabled", &audioTuning.autoStyleEnabled);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Style blend");
                        ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::SliderFloat("##auto_style_blend", &audioTuning.autoStyleBlend, 0.0f, 1.0f, "%.2f");
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Style response");
                        ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::SliderFloat("##auto_style_response", &audioTuning.autoStyleResponseSec, 0.15f, 6.0f, "%.2fs");
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Dominant style");
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%s (activity %.2f, effect %.2f)", dominantStyleLabel(styleWeights), styleWeights.activity, autoStyleStrength);
                        ImGui::EndTable();
                    }
                    ImGui::TextUnformatted("Style Weights");
                    ImGui::ProgressBar(styleWeights.houseGroove, ImVec2(-FLT_MIN, 0.0f), "House Groove");
                    ImGui::ProgressBar(styleWeights.technoRumble, ImVec2(-FLT_MIN, 0.0f), "Techno Rumble");
                    ImGui::ProgressBar(styleWeights.tranceLift, ImVec2(-FLT_MIN, 0.0f), "Trance Lift");
                    ImGui::ProgressBar(styleWeights.breakdownSparse, ImVec2(-FLT_MIN, 0.0f), "Breakdown Sparse");
                    if (audioTuning.autoStyleEnabled) {
                        ImGui::TextWrapped("Effective tuning is your manual profile plus a weighted style delta derived from the live semantic analysis.");
                    }
                    ImGui::BeginChild("audio_tuning_child", ImVec2(0.0f, 290.0f), true);
                    if (ImGui::BeginTable("audio_tuning_controls", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 1.1f);
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                        ImGui::TableSetupColumn("Raw", ImGuiTableColumnFlags_WidthStretch, 0.8f);
                        ImGui::TableSetupColumn("Tuned", ImGuiTableColumnFlags_WidthStretch, 0.8f);
                        ImGui::TableHeadersRow();
                        drawTuningRow("Pulse Gain", "##pulse_gain", &audioTuning.pulseGain, 0.2f, 3.0f, pulseLiveRaw, pulseLiveTuned);
                        drawTuningRow("Pulse Knee", "##pulse_knee", &audioTuning.pulseKnee, 0.2f, 2.0f, pulseLiveRaw, pulseLiveTuned);
                        drawTuningRow("Body Gain", "##body_gain", &audioTuning.bodyGain, 0.2f, 3.0f, bodyLiveRaw, bodyLiveTuned);
                        drawTuningRow("Body Knee", "##body_knee", &audioTuning.bodyKnee, 0.2f, 2.0f, bodyLiveRaw, bodyLiveTuned);
                        drawTuningRow("Detail Gain", "##detail_gain", &audioTuning.detailGain, 0.2f, 3.0f, detailLiveRaw, detailLiveTuned);
                        drawTuningRow("Detail Knee", "##detail_knee", &audioTuning.detailKnee, 0.2f, 2.0f, detailLiveRaw, detailLiveTuned);
                        drawTuningRow("Macro Gain", "##macro_gain", &audioTuning.macroGain, 0.2f, 3.0f, macroLiveRaw, macroLiveTuned);
                        drawTuningRow("Macro Knee", "##macro_knee", &audioTuning.macroKnee, 0.2f, 2.0f, macroLiveRaw, macroLiveTuned);
                        drawTuningRow("Bass Gain", "##bass_gain", &audioTuning.bassGain, 0.2f, 3.0f, clamp01(std::max(af.subBody, af.bassBody)), clamp01(std::max(shaderAf.subBody, shaderAf.bassBody)));
                        drawTuningRow("Air Gain", "##air_gain", &audioTuning.airGain, 0.2f, 3.0f, clamp01(af.airPresence), clamp01(shaderAf.airPresence));
                        drawTuningRow("Harmonic Gain", "##harm_gain", &audioTuning.harmonicGain, 0.2f, 3.0f, clamp01(af.harmonicBody), clamp01(shaderAf.harmonicBody));
                        drawTuningRow("Lead Gain", "##lead_gain", &audioTuning.leadGain, 0.2f, 3.0f, clamp01(af.leadPresence), clamp01(shaderAf.leadPresence));
                        drawTuningRow("Novelty Gain", "##novelty_gain", &audioTuning.noveltyGain, 0.2f, 3.0f, clamp01(af.novelty), clamp01(shaderAf.novelty));
                        drawTuningRow("Brightness Gain", "##bright_gain", &audioTuning.brightnessGain, 0.2f, 3.0f, clamp01(af.brightness), clamp01(shaderAf.brightness));
                        drawTuningRow("Percussive Gain", "##perc_gain", &audioTuning.percussiveGain, 0.2f, 3.0f, clamp01(af.percussiveFocus), clamp01(shaderAf.percussiveFocus));
                        drawTuningRow("Tension Gain", "##tension_gain", &audioTuning.tensionGain, 0.2f, 3.0f, clamp01(af.tension), clamp01(shaderAf.tension));
                        drawTuningRow("Release Gain", "##release_gain", &audioTuning.releaseGain, 0.2f, 3.0f, clamp01(af.release), clamp01(shaderAf.release));
                        drawTuningRow("Pulse Floor", "##pulse_floor", &audioTuning.pulseFloor, 0.0f, 0.6f, pulseLiveRaw, pulseLiveTuned);
                        drawTuningRow("Body Floor", "##body_floor", &audioTuning.bodyFloor, 0.0f, 0.6f, bodyLiveRaw, bodyLiveTuned);
                        drawTuningRow("Detail Floor", "##detail_floor", &audioTuning.detailFloor, 0.0f, 0.6f, detailLiveRaw, detailLiveTuned);
                        drawTuningRow("Macro Floor", "##macro_floor", &audioTuning.macroFloor, 0.0f, 0.6f, macroLiveRaw, macroLiveTuned);
                        ImGui::EndTable();
                    }
                    ImGui::EndChild();
                }
                ImGui::Separator();
                if (ImGui::CollapsingHeader("Shader Tweaks", ImGuiTreeNodeFlags_DefaultOpen)) {
                    drawShaderUniformEditor(*getUiControlProgram(), "visual_shader_tweaks_child", 180.0f, true);
                }
                ImGui::Separator();
                ImGui::Checkbox("Enable shader feedback (single-file feedback iChannel0)", &uiShaderFeedback);
                ImGui::Checkbox("Hot reload shaders", &uiHotReloadShaders);
                if (ImGui::SliderInt("Preview resolution", &previewSize, 256, 1024)) { createPreviewTargets(); createCompanionTargets(previewSize); }
			}
			else if (mode == 1) {
				ImGui::TextUnformatted("p5.js Processing Engine");
				std::string currentSketch = processingSketchPath.empty() ? std::string("No sketches") : std::filesystem::path(processingSketchPath).filename().string();
				if (ImGui::BeginCombo("##processing_sketch_select", currentSketch.c_str())) {
					for (const auto& sketchFile : processingSketchFiles) {
						std::string name = std::filesystem::path(sketchFile).filename().string();
						bool selected = (sketchFile == processingSketchPath);
						if (ImGui::Selectable(name.c_str(), selected)) {
							processingSketchPath = sketchFile;
							useProcessing = true;
							useFFglitch = false;
							processingStatus = "Selected " + name + ". Restart the engine to reload the embedded viewport.";
						}
						if (selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				if (ImGui::Button(processingPipe ? "Restart Engine" : "Start Engine")) {
					stopProcessingViewportBrowser();
					if (processingPipe) stopProcessingEngine();
					startProcessingEngine();
				}
				ImGui::SameLine();
				if (ImGui::Button("Reload Viewport")) {
					stopProcessingViewportBrowser();
					startProcessingEngine();
				}
				ImGui::SameLine();
				if (ImGui::Button("Stop Engine")) {
					stopProcessingViewportBrowser();
					stopProcessingEngine();
				}
				ImGui::SameLine();
				if (ImGui::Button("Refresh Sketch List")) {
					processingSketchFiles = collectProcessingSketches({
						"../assets/processing/sketches",
						processingBundle() + "/sketches",
						"assets/processing/sketches"
					});
					if (processingSketchPath.empty() && !processingSketchFiles.empty()) {
						processingSketchPath = processingSketchFiles.front();
					}
				}
				ImGui::Separator();
				if (ImGui::CollapsingHeader("Processing Runtime", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::TextWrapped("This mode runs a real p5.js Processing-family sketch in Chromium. It defaults to the docked Viewport and can be routed to the fullscreen Output window.");
					if (ImGui::RadioButton("Viewport##processing_output", processingOutputTarget == 0)) {
						processingOutputTarget = 0;
					}
					ImGui::SameLine();
					if (ImGui::RadioButton("Fullscreen Output##processing_output", processingOutputTarget == 1)) {
						openOutputWindow();
						if (outputWindow) processingOutputTarget = 1;
					}
					if (ImGui::Button("Show In Viewport")) {
						processingOutputTarget = 0;
					}
					ImGui::SameLine();
					if (ImGui::Button("Send To Output Monitor")) {
						openOutputWindow();
						if (outputWindow) processingOutputTarget = 1;
					}
						ImGui::TextWrapped("URL: %s", processingEngineUrl().c_str());
						ImGui::TextWrapped("Root: %s", processingRootPath.c_str());
						ImGui::TextWrapped("Sketches: assets/processing/sketches/*.js");
						ImGui::TextWrapped("Frame file: %s", processingFrameFilePath.c_str());
						ImGui::TextWrapped("Frame status: %s (%dx%d seq %u)",
							processingFrameReady ? "ready" : "waiting",
							processingFrameWidth,
							processingFrameHeight,
							processingFrameSeq);
						ImGui::TextWrapped("Status: %s", processingStatus.empty() ? (processingPipe ? "Running" : "Stopped") : processingStatus.c_str());
						ImGui::TextWrapped("Renderer: %s", processingBrowserStatus.empty() ? "Waiting for Processing viewport" : processingBrowserStatus.c_str());
						ImGui::TextWrapped("Engine log: %s", processingEngineLogPath.c_str());
						ImGui::TextWrapped("Browser log: %s", processingBrowserLogPath.c_str());
						if (ImGui::Button("Open External Fallback")) {
						if (!processingPipe) startProcessingEngine();
						openProcessingEngine();
					}
				}
			}
#if VISUALIZA_HAS_FFGLITCH
			else {
				// FFglitch via FFlive: built-in scripts and input file
				if (ImGui::BeginCombo("Built-in Script", "Select")) {
					for (size_t i = 0; i < scriptFiles.size(); ++i) {
						std::string name = scriptFiles[i]; size_t s = name.find_last_of("/\\"); if (s != std::string::npos) name = name.substr(s + 1);
						bool sel = (std::strcmp(fflScript, scriptFiles[i].c_str()) == 0);
						if (ImGui::Selectable(name.c_str(), sel)) { std::snprintf(fflScript, sizeof(fflScript), "%s", scriptFiles[i].c_str()); }
						if (sel) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				if (ImGui::Button("Open Script…")) { std::string p = openFileDialog("Select ffglitch script", nullptr, {"js","txt"}); if (!p.empty()) std::snprintf(fflScript, sizeof(fflScript), "%s", p.c_str()); }
				ImGui::TextUnformatted("Input file");
				if (ImGui::Button("Browse Input")) { std::string p = openFileDialog("Select input media", nullptr, {"mp4","mov","mkv","avi","webm"}); if (!p.empty()) std::snprintf(fflInput, sizeof(fflInput), "%s", p.c_str()); }
                // Live pipeline: ffgac -> rawvideo pipe -> fflive fullscreen
				if (!fflRunning.load()) {
					if (ImGui::Button("Start Live Mosher")) {
						std::string found = whichCmd(ffgacPath); if (found.empty()) found = whichCmd("ffgac");
						if (found.empty()) {
							appendFflLog("ffgac not found. Attempting Homebrew install (ffglitch)…\n");
							std::string cmd = "/opt/homebrew/bin/brew install ffglitch 2>&1";
							if (fflThread.joinable()) fflThread.join();
							fflThread = std::thread([&, cmd]{ FILE* fp = popen(cmd.c_str(), "r"); if (fp){ char line[512]; while (fgets(line,sizeof(line),fp)) { appendFflLog(std::string(line)); } pclose(fp);} });
						} else {
							if (std::strlen(fflInput) == 0) {
								std::string p = openFileDialog("Select input media", nullptr, {"mp4","mov","mkv","avi","webm"});
								if (!p.empty()) std::snprintf(fflInput, sizeof(fflInput), "%s", p.c_str());
							}
                            if (std::strlen(fflInput) > 0 && std::strlen(fflScript) > 0) {
                                runFflive();
								// external window handles display
							}
						}
					}
				} else {
					ImGui::TextDisabled("Live running…");
				}
			}
#endif
			ImGui::PopItemWidth();
			ImGui::PopTextWrapPos();
            }
			ImGui::End();
		}
        // Source window
        {
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
            if (ImGui::Begin("Source", &showSourceWindow, flags)) {
            pushContentWrapPos();
            if (ImGui::BeginTable("audio_top_controls", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.90f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.55f);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Freeze audio");
                ImGui::TableSetColumnIndex(1);
                ImGui::Checkbox("##freeze_audio", &freezeAudio);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Smoothing");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderFloat("##audio_smoothing", &smoothing, 0.0f, 0.95f, "%.2f");

                ImGui::EndTable();
            }
            ImGui::PushItemWidth(-FLT_MIN);

            // ZMQ metrics toggle
            static bool zmqEnabled = false;
            bool wasEnabled = zmqEnabled;
            if (ImGui::Checkbox("Send metrics to FFglitch (ZMQ)", &zmqEnabled)) {
                if (zmqEnabled && !wasEnabled) {
                    startZmqSender();
                } else if (!zmqEnabled && wasEnabled) {
                    stopZmqSender();
                }
            }

	            if (ImGui::Checkbox("Use audio file", &useFile)) {
	                if (!useFile) {
	                    if (selectedDeviceId != (unsigned int)-1) {
							if (audioInput.startStreamOnDevice(selectedDeviceId, sampleRate, channelCount)) {
								inputStatus = "Live input ready";
							} else {
								selectedDeviceId = audioInput.getCurrentDeviceId() != 0 ? audioInput.getCurrentDeviceId() : (unsigned int)-1;
								inputStatus = audioInput.getLastError();
							}
						}
	                }
	            }
            if (useFile) {
                ImGui::TextUnformatted("File path");
                ImGui::InputText("##audio_file_path", filePathBuf, IM_ARRAYSIZE(filePathBuf));
                if (ImGui::Button("Browse WAV")) {
                    std::string path = openFileDialog("Select WAV audio", nullptr, {"wav"});
                    if (!path.empty()) {
                        std::snprintf(filePathBuf, sizeof(filePathBuf), "%s", path.c_str());
                    }
                }
                ImGui::SameLine();
	                if (ImGui::Button("Load")) {
	                    if (filePlayer.loadWav(filePathBuf)) {
	                        channelCount = filePlayer.getChannels();
	                        sampleRate = filePlayer.getSampleRate();
	                        analyzer = AudioAnalysis(sampleRate, aaCfg);
	                        audioFileStatus = "Loaded " + std::to_string(sampleRate) + " Hz, " + std::to_string(channelCount) + " channel(s)";
	                    } else {
	                        audioFileStatus = "Failed to load WAV";
	                    }
                }
                ImGui::SameLine();
                bool playing = filePlayer.isPlaying();
                if (ImGui::Checkbox("Playing", &playing)) filePlayer.setPlaying(playing);
                if (!audioFileStatus.empty()) ImGui::TextWrapped("%s", audioFileStatus.c_str());
            } else {
	                if (ImGui::Button("Refresh devices")) {
	                    unsigned int prev = selectedDeviceId;
	                    devices = audioInput.listInputDevices();
	                    selectedDeviceId = audioInput.getCurrentDeviceId() != 0 ? audioInput.getCurrentDeviceId() : (unsigned int)-1;
	                    for (const auto& d : devices) { if (d.id == prev) { selectedDeviceId = d.id; break; } }
	                    if (selectedDeviceId == (unsigned int)-1) {
	                        for (const auto& d : devices) { if (d.isDefault) { selectedDeviceId = d.id; break; } }
	                    }
	                }

		// (FFglitch batch UI removed in favor of FFlive)

	                std::string currentLabel;
	                for (const auto& d : devices) if (d.id == selectedDeviceId) { currentLabel = formatInputDeviceLabel(d); break; }
	                if (currentLabel.empty()) currentLabel = devices.empty() ? "No input devices" : "Select input device";
	                if (ImGui::BeginCombo("Input device", currentLabel.c_str())) {
	                    for (size_t i = 0; i < devices.size(); ++i) {
	                        const auto& d = devices[i];
	                        bool sel = (d.id == selectedDeviceId);
	                        std::string label = formatInputDeviceLabel(d);
	                        if (ImGui::Selectable(label.c_str(), sel)) {
								if (audioInput.startStreamOnDevice(d.id, sampleRate, channelCount)) {
									selectedDeviceId = d.id;
									inputStatus = "Live input ready";
								} else {
									selectedDeviceId = audioInput.getCurrentDeviceId() != 0 ? audioInput.getCurrentDeviceId() : (unsigned int)-1;
									inputStatus = audioInput.getLastError();
								}
	                        }
	                        if (sel) ImGui::SetItemDefaultFocus();
	                    }
	                    ImGui::EndCombo();
	                }
					const AudioInput::DeviceInfo* selectedInfo = nullptr;
					for (const auto& d : devices) {
						if (d.id == selectedDeviceId) {
							selectedInfo = &d;
							break;
						}
					}
					if (selectedInfo && selectedInfo->inputChannels == 1) {
						ImGui::TextWrapped("Selected source is mono. For desktop playback capture, prefer a 2-channel monitor or stereo source if available.");
					} else if (selectedInfo && selectedInfo->inputChannels > 0 && selectedInfo->outputChannels > 0) {
						ImGui::TextWrapped("Selected source is a playback monitor. This should follow desktop audio output.");
					}
					if (!audioInput.isStreamRunning()) {
						if (inputStatus.empty()) inputStatus = "Audio input stream is not running";
						ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", inputStatus.c_str());
					} else {
						ImGui::TextDisabled("%s", inputStatus.c_str());
						if (ImGui::BeginTable("audio_stats", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Capture latency");
							ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f ms", audioInput.getCaptureLatencyMs());
							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Source blocks");
							ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f Hz", audioInput.getFreshBlockRateHz());
							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Shader updates");
							ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f Hz", analysisUpdateRateHz);
							ImGui::EndTable();
						}
					}
	            }

	            ImGui::Separator();
                if (ImGui::BeginTable("headline_metrics", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("RMS");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f", shaderMetrics.rms);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("Low / Mid / High");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f / %.3f / %.3f", shaderMetrics.bandLow, shaderMetrics.bandMid, shaderMetrics.bandHigh);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("Onset");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f", onsetDisplay);
                    ImGui::EndTable();
                }

	            if (!latest.empty()) {
					waveform.clear();
	                unsigned int waveformChannels = std::max(1u, channelCount);
	                for (size_t i = 0; i + waveformChannels <= latest.size(); i += waveformChannels) {
	                    float sum = 0.0f;
	                    for (unsigned int c = 0; c < waveformChannels; ++c) sum += latest[i + c];
	                    waveform.push_back(sum / (float)waveformChannels);
                }
            }
            if (!waveform.empty()) {
                std::vector<float> compactWaveform;
                compactWaveform.reserve(160);
                const size_t targetPoints = 160;
                const size_t stride = std::max<size_t>(1, waveform.size() / targetPoints);
                for (size_t i = 0; i < waveform.size(); i += stride) compactWaveform.push_back(waveform[i]);
                drawCompactWaveform("Waveform", compactWaveform, 80.0f);
            }
            ImGui::PopItemWidth();
            ImGui::PopTextWrapPos();
            }
            ImGui::End();
        }

		// Diagnostics window
        {
            ImGuiWindowFlags sflags = ImGuiWindowFlags_NoCollapse;
            if (ImGui::Begin("Diagnostics", &showDiagnosticsWindow, sflags)) {
            pushContentWrapPos();
            ImGui::PushItemWidth(-FLT_MIN);

            if (ImGui::BeginTable("diag_summary", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("Energy / Tension / Novelty");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f / %.3f / %.3f", af.energyLevel, af.tension, af.novelty);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("Kick / Snare / Hat / Beat");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f / %.3f / %.3f / %.3f", af.kickImpact, af.snareImpact, af.hatTick, af.beatPulse);
                ImGui::EndTable();
            }
            ImGui::Separator();
            int n = (int)std::min<size_t>(16, af.bandEnergyNorm.size());
            if (n > 0) {
                float lowShare = 0.0f;
                float midShare = 0.0f;
                float highShare = 0.0f;
                for (int i = 0; i < n; ++i) {
                    float v = std::max(0.0f, af.bandEnergyNorm[i]);
                    if (i < n / 3) lowShare += v;
                    else if (i < (2 * n) / 3) midShare += v;
                    else highShare += v;
                }
                float totalShare = std::max(1e-6f, lowShare + midShare + highShare);
                lowShare /= totalShare;
                midShare /= totalShare;
                highShare /= totalShare;
                ImGui::TextUnformatted("Live Response");
                ImGui::ProgressBar(lowShare, ImVec2(-FLT_MIN, 0.0f), "Bass");
                ImGui::ProgressBar(midShare, ImVec2(-FLT_MIN, 0.0f), "Body");
                ImGui::ProgressBar(highShare, ImVec2(-FLT_MIN, 0.0f), "Air");
                ImGui::TextWrapped("Beat rate: %.1f/min", beatRateDisplay);
            }
            if (ImGui::CollapsingHeader("Analysis Detail", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::BeginChild("analysis_live_child", ImVec2(0.0f, 360.0f), true);
                if (ImGui::Button("Reset semantic ranges")) {
                    dbgBassBody.reset();
                    dbgHarmonicBody.reset();
                    dbgAirPresence.reset();
                    dbgLeadPresence.reset();
                    dbgRelease.reset();
                    dbgBodyComposite.reset();
                }
                if (ImGui::BeginTable("semantic_debug_ranges", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch, 1.3f);
                    ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthStretch, 0.8f);
                    ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthStretch, 0.8f);
                    ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthStretch, 0.8f);
                    ImGui::TableHeadersRow();
                    auto drawRangeRow = [&](const char* label, float current, const SemanticDebugRange& range) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(label);
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f", current);
                        ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f", range.seen ? range.minValue : 0.0f);
                        ImGui::TableSetColumnIndex(3); ImGui::Text("%.3f", range.seen ? range.maxValue : 0.0f);
                    };
                    drawRangeRow("Bass Body", af.bassBody, dbgBassBody);
                    drawRangeRow("Harmonic Body", af.harmonicBody, dbgHarmonicBody);
                    drawRangeRow("Air Presence", af.airPresence, dbgAirPresence);
                    drawRangeRow("Lead Presence", af.leadPresence, dbgLeadPresence);
                    drawRangeRow("Release", af.release, dbgRelease);
                    drawRangeRow("Body Composite", std::max({af.subBody, af.bassBody, af.harmonicBody, af.leadPresence, af.airPresence}), dbgBodyComposite);
                    ImGui::EndTable();
                }
                ImGui::Separator();
                ImGui::Text("Style Profile: %s", dominantStyleLabel(styleWeights));
                ImGui::ProgressBar(styleWeights.houseGroove, ImVec2(-FLT_MIN, 0.0f), "House Groove");
                ImGui::ProgressBar(styleWeights.technoRumble, ImVec2(-FLT_MIN, 0.0f), "Techno Rumble");
                ImGui::ProgressBar(styleWeights.tranceLift, ImVec2(-FLT_MIN, 0.0f), "Trance Lift");
                ImGui::ProgressBar(styleWeights.breakdownSparse, ImVec2(-FLT_MIN, 0.0f), "Breakdown Sparse");
                ImGui::ProgressBar(styleWeights.activity, ImVec2(-FLT_MIN, 0.0f), "Activity");
                ImGui::Separator();
                if (ImGui::BeginTable("analysis_live_metrics", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("Sub / Bass / Body");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f / %.3f / %.3f", af.subBody, af.bassBody, af.harmonicBody);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("Lead / Air / Bright / Perc");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f / %.3f / %.3f / %.3f", af.leadPresence, af.airPresence, af.brightness, af.percussiveFocus);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("Drop / Section / Release");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f / %.3f / %.3f", af.dropEvent, af.sectionChange, af.release);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("Anchor centers");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("K %.2f  B %.2f  L %.2f  A %.2f", af.roleKickCenterNorm, af.roleBassCenterNorm, af.roleLeadCenterNorm, af.roleAirCenterNorm);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("Anchor confidence");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("K %.2f  B %.2f  L %.2f  A %.2f", af.roleKickConfidence, af.roleBassConfidence, af.roleLeadConfidence, af.roleAirConfidence);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("Centroid / Flux / BPM");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f Hz / %.4f / %.1f", af.spectralCentroidHz, af.spectralFlux, af.bpm);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("Perc / Harm / Ratio");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f / %.3f / %.2f", af.percussiveEnergy, af.harmonicEnergy, af.percussiveRatio);
                    ImGui::EndTable();
                }
                if (n > 0) {
                    std::vector<float> energyBars;
                    std::vector<float> onsetBars;
                    energyBars.reserve((size_t)n);
                    onsetBars.reserve((size_t)n);
                    for (int i = 0; i < n; ++i) {
                        energyBars.push_back(std::max(0.0f, std::min(1.0f, af.bandEnergyNorm[i] * bandScale[i] * bandsGain)));
                        onsetBars.push_back(std::max(0.0f, std::min(1.0f, af.bandOnset[i] * onsetScale[i] * onsetsGain)));
                    }
                    drawCompactBandBars("Band Energy", energyBars, IM_COL32(64, 214, 164, 255));
                    drawCompactBandBars("Band Onset", onsetBars, IM_COL32(255, 162, 64, 255));
                }
                ImGui::EndChild();
            }

            ImGui::PopItemWidth();
            ImGui::PopTextWrapPos();
            }
            ImGui::End();
        }

        // Expert window
        {
            ImGuiWindowFlags sflags = ImGuiWindowFlags_NoCollapse;
            if (ImGui::Begin("Expert", &showExpertWindow, sflags)) {
            pushContentWrapPos();
            ImGui::PushItemWidth(-FLT_MIN);
            if (ImGui::CollapsingHeader("Workspace", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("Workspace save/load and startup behavior live in Visual so the main performance flow stays in one place.");
                if (ImGui::CollapsingHeader("Window Visibility", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Checkbox("Source", &showSourceWindow);
                    ImGui::Checkbox("Visual", &showVisualWindow);
                    ImGui::Checkbox("Diagnostics", &showDiagnosticsWindow);
                    ImGui::Checkbox("Expert", &showExpertWindow);
                    ImGui::Checkbox("Output", &showOutputWindow);
                    ImGui::Checkbox("Viewport", &showViewportWindow);
                }
            }
#if VISUALIZA_HAS_FFGLITCH
            if (ImGui::CollapsingHeader("FFglitch", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Use FFglitch instead of shader", &useFFglitch);
                static char ffgInputUI[1024] = "";
                static char ffgScriptUI[1024] = "";
                ImGui::TextUnformatted("FFg input");
                if (ImGui::Button("Browse…##ffg_input")) {
                    std::string p = openFileDialog("Select input media", nullptr, {"mp4","mov","mkv","avi","webm"});
                    if (!p.empty()) std::snprintf(ffgInputUI, sizeof(ffgInputUI), "%s", p.c_str());
                }
                if (std::strlen(ffgInputUI) > 0) ImGui::TextWrapped("%s", ffgInputUI);
                ImGui::TextUnformatted("FFg script (optional)");
                if (ImGui::Button("Browse…##ffg_script")) {
                    std::string p = openFileDialog("Select ffglitch script", nullptr, {"js","txt"});
                    if (!p.empty()) std::snprintf(ffgScriptUI, sizeof(ffgScriptUI), "%s", p.c_str());
                }
                if (std::strlen(ffgScriptUI) > 0) ImGui::TextWrapped("%s", ffgScriptUI);
            }
#else
            ImGui::TextDisabled("FFglitch controls are unavailable in this lightweight build.");
#endif
            if (ImGui::CollapsingHeader("Shader Loading", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Shader path: %s", fragmentPath.c_str());
                if (ImGui::Button("Open External Shader…")) {
                    std::string path = openFileDialog("Select GLSL Fragment Shader", nullptr, {"frag"});
                    if (!path.empty()) {
                        fragmentPath = path;
                        uiMetadataPath = fragmentPath;
                        shaderProgram.buildFromFiles(fragmentPath.c_str());
                        invalidateUniformUI();
                        refreshCompanionPasses();
                    }
                }
                ImGui::InputText("External .frag path", shaderPathBuf, IM_ARRAYSIZE(shaderPathBuf));
                if (ImGui::Button("Load External")) {
                    if (std::strlen(shaderPathBuf) > 0) {
                        std::string newPath(shaderPathBuf);
                        fragmentPath = newPath;
                        uiMetadataPath = fragmentPath;
                        shaderProgram.buildFromFiles(fragmentPath.c_str());
                        invalidateUniformUI();
                        refreshCompanionPasses();
                    }
                }
            }
            if (ImGui::CollapsingHeader("Analysis Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SliderFloat("Bands Gain", &bandsGain, 0.0f, 4.0f);
                ImGui::SliderFloat("Onsets Gain", &onsetsGain, 0.0f, 4.0f);
                ImGui::Checkbox("Per-band overrides", &showPerBandControls);
                if (showPerBandControls) {
                    int bandCount = (int)std::min<size_t>(16, af.bandEnergyNorm.size());
                    for (int i = 0; i < bandCount; ++i) {
                        char lbl1[32]; std::snprintf(lbl1, sizeof(lbl1), "Band %02d", i);
                        ImGui::SliderFloat(lbl1, &bandScale[i], 0.0f, 4.0f);
                        char lbl2[32]; std::snprintf(lbl2, sizeof(lbl2), "Onset %02d", i);
                        ImGui::SliderFloat(lbl2, &onsetScale[i], 0.0f, 4.0f);
                    }
                }
            }
            if (ImGui::CollapsingHeader("Raw Shader Uniforms", ImGuiTreeNodeFlags_DefaultOpen)) {
                drawShaderUniformEditor(*getUiControlProgram(), "shader_uniforms_child", 260.0f, false);
            }
            ImGui::TextDisabled("Startup environment log: build/startup_env.log");
            ImGui::PopItemWidth();
            ImGui::PopTextWrapPos();
            }
            ImGui::End();
        }

        // Output window
        {
            ImGuiWindowFlags oflags = ImGuiWindowFlags_NoCollapse;
            if (ImGui::Begin("Output", &showOutputWindow, oflags)) {
            pushContentWrapPos();
            ImGui::PushItemWidth(-FLT_MIN);
            if (ImGui::BeginTable("output_perf", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("FPS");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f", fpsDisplay);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Frame time");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f ms", frameTimeMs);
                ImGui::EndTable();
            }
            ImGui::Separator();
            std::string curMon = monitorNames.empty() ? std::string("No monitors") : monitorNames[selectedMonitor];
            if (ImGui::BeginCombo("Output monitor", curMon.c_str())) {
                for (int i = 0; i < (int)monitorNames.size(); ++i) {
                    bool sel = (i == selectedMonitor);
                    if (ImGui::Selectable(monitorNames[i].c_str(), sel)) selectedMonitor = i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            const char* outputButtonLabel = outputWindow
				? (useProcessing && processingOutputTarget == 1 ? "Close p5 Fullscreen Output" : "Close Output")
				: (useProcessing ? "Open p5 Fullscreen Output" : "Open Fullscreen Output");
            if (ImGui::Button(outputButtonLabel)) {
                if (outputWindow) {
                    closeOutputWindow();
                } else {
					openOutputWindow();
					if (useProcessing && outputWindow) processingOutputTarget = 1;
                }
            }
            ImGui::PopItemWidth();
            ImGui::PopTextWrapPos();
            }
            ImGui::End();
        }

        // Docked viewport window that fits remaining space and centers a square texture
        {
            ImGuiWindowFlags vflags = ImGuiWindowFlags_NoCollapse;
            if (ImGui::Begin("Viewport", &showViewportWindow, vflags)) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
			if (useProcessing && processingOutputTarget == 0) {
				if (!processingPipe) startProcessingEngine();
				ImVec2 screenPos = ImGui::GetCursorScreenPos();
				ImGuiViewport* mainViewport = ImGui::GetMainViewport();
				const int hostX = (int)std::round(screenPos.x - mainViewport->Pos.x);
				const int hostY = (int)std::round(screenPos.y - mainViewport->Pos.y);
				const int hostW = (int)std::round(std::max(1.0f, avail.x));
				const int hostH = (int)std::round(std::max(1.0f, avail.y));
				restartProcessingEngineForCapture(hostW, hostH, 60);
#if defined(__linux__)
				attachProcessingViewportBrowser(glfwGetX11Window(window), hostX, hostY, hostW, hostH, "app viewport");
#else
				attachProcessingViewportBrowser(0, hostX, hostY, hostW, hostH, "app viewport");
#endif
				if (processingFrameReady && processingFrameTex) {
					ImGui::Image((ImTextureID)(intptr_t)processingFrameTex, avail, ImVec2(0,1), ImVec2(1,0));
				} else {
					ImGui::TextWrapped("%s", processingBrowserStatus.empty() ? "Waiting for p5 frames" : processingBrowserStatus.c_str());
				}
			} else if (useProcessing && processingOutputTarget == 1) {
				ImGui::TextWrapped("p5 Processing is routed to the fullscreen Output window.");
				ImGui::TextWrapped("Close fullscreen output or choose Viewport in the Processing panel to bring it back here.");
			} else {
				hideProcessingViewportBrowser();
				float side = std::floor(std::min(avail.x, avail.y));
				ImVec2 cursor = ImGui::GetCursorPos();
				ImGui::SetCursorPos(ImVec2(cursor.x + (avail.x - side) * 0.5f, cursor.y + (avail.y - side) * 0.5f));
				ImGui::Image((ImTextureID)(intptr_t)previewDisplayTex, ImVec2(side, side), ImVec2(0,1), ImVec2(1,0));
			}
            }
			if (!showViewportWindow || !useProcessing) hideProcessingViewportBrowser();
            ImGui::End();
        }

		if (showDemoWindow) ImGui::ShowDemoWindow(&showDemoWindow);

		ImGui::Render();
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		if (firstFrameTrace) logStartupStage("first frame imgui render ok");

		glfwSwapBuffers(window);
		if (firstFrameTrace) {
			logStartupStage("first frame swap ok");
			firstFrameTrace = false;
		}
		constexpr double targetFrameSeconds = 1.0 / 60.0;
		const auto targetFrameDuration = std::chrono::duration<double>(targetFrameSeconds);
		const auto frameEndTime = std::chrono::steady_clock::now();
		const auto elapsed = frameEndTime - frameStartTime;
		if (elapsed < targetFrameDuration) {
			std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::microseconds>(targetFrameDuration - elapsed));
		}
	}

	{
		WorkspacePreset lastSession{};
		lastSession.look.fragmentPath = fragmentPath;
		lastSession.look.processingSketchPath = processingSketchPath;
		lastSession.look.useProcessing = useProcessing;
		lastSession.look.useFFglitch = useFFglitch;
		lastSession.look.shaderFeedback = uiShaderFeedback;
		lastSession.look.hotReloadShaders = uiHotReloadShaders;
		lastSession.look.previewSize = previewSize;
		lastSession.look.audioTuning = audioTuning;
		lastSession.smoothing = smoothing;
		lastSession.bandsGain = bandsGain;
		lastSession.onsetsGain = onsetsGain;
		lastSession.selectedMonitor = selectedMonitor;
		lastSession.showSource = showSourceWindow;
		lastSession.showVisual = showVisualWindow;
		lastSession.showDiagnostics = showDiagnosticsWindow;
		lastSession.showExpert = showExpertWindow;
		lastSession.showOutput = showOutputWindow;
		lastSession.showViewport = showViewportWindow;
		saveWorkspacePreset(lastSessionWorkspacePath, lastSession, io.IniFilename);
		saveUiPreferences(uiPreferencesPath, uiPreferences);
	}

	stopProcessingViewportBrowser();
	stopProcessingEngine();
	stopZmqSender();
	if (fflThread.joinable()) {
		fflThread.join();
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glDeleteVertexArrays(1, &vao);

	glfwDestroyWindow(window);
	glfwTerminate();
	return EXIT_SUCCESS;
}
