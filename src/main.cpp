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
#include <array>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

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
};

static AudioTuningProfile makeDefaultAudioTuningProfile() {
	return AudioTuningProfile{};
}

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
#endif
}

static void logStartupEnvironment() {
	const std::array<const char*, 11> keys = {
		"DISPLAY",
		"WAYLAND_DISPLAY",
		"XDG_SESSION_TYPE",
		"XDG_CURRENT_DESKTOP",
		"DESKTOP_SESSION",
		"PULSE_SERVER",
		"PIPEWIRE_REMOTE",
		"SDL_VIDEODRIVER",
		"LIBGL_ALWAYS_SOFTWARE",
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
	normalizeGraphicsEnvironment();
	logStartupEnvironment();
	glfwSetErrorCallback(glfwErrorCallback);
	if (!glfwInit()) {
		std::fprintf(stderr, "Failed to initialize GLFW\n");
		return EXIT_FAILURE;
	}

	// macOS prefers 3.2 core
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	#ifdef __APPLE__
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	#endif

	GLFWwindow* window = glfwCreateWindow(1280, 720, "visualiza_isso_aqui", nullptr, nullptr);
	if (!window) {
		std::fprintf(stderr, "Failed to create window\n");
		glfwTerminate();
		return EXIT_FAILURE;
	}
	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);

	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
		std::fprintf(stderr, "Failed to load OpenGL via GLAD\n");
		return EXIT_FAILURE;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	(void)io;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	ImGui::StyleColorsDark();

	const char* glsl_version = "#version 150"; // GL 3.2 core
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init(glsl_version);

	// Audio
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

    // Advanced analysis
    AudioAnalysis::Config aaCfg;
    aaCfg.fftSize = 512;
    aaCfg.numBands = 16;
    AudioAnalysis analyzer(sampleRate, aaCfg);
    AudioAnalysis::AnalysisFrame af{};
    AudioAnalysis::AnalysisFrame shaderAf{};
    const unsigned int analysisBlockFrames = (unsigned int)aaCfg.fftSize;
	ShaderAudioMetrics shaderMetrics{};
    AudioTuningProfile audioTuning = makeDefaultAudioTuningProfile();
    std::filesystem::path audioTuningDir = "presets/audio_tuning";
    std::vector<std::string> audioTuningPresetFiles;
    char audioTuningPresetName[128] = "live_default";
    std::string audioTuningStatus;
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
    refreshAudioTuningPresets();

    // Analysis UI controllers (scalers)
    float bandsGain = 1.0f;
    float onsetsGain = 1.0f;
    bool showPerBandControls = false;
    float bandScale[16];
    float onsetScale[16];
    for (int i = 0; i < 16; ++i) { bandScale[i] = 1.0f; onsetScale[i] = 1.0f; }

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
    auto invalidateUniformUI = [&]() {
        lastUniformSignature.clear();
        uiUniforms.clear();
        uiUniformOrder.clear();
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
            uiUniforms[u.name] = ui;
            uiUniformOrder.push_back(u.name);
        }
    };

    // Graphics and shader
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
    // External shader path input (user-provided absolute or relative file)
    static char shaderPathBuf[1024] = "";
	std::string fragmentPath = shaderFiles.empty() ? std::string("assets/shaders/visual.frag") : shaderFiles[0];
	ShaderProgram shaderProgram;
	FFglitchPlayer ffgPlayer;
	bool useFFglitch = false;
	bool uiShaderFeedback = false; // single-file shader feedback toggle
	bool uiHotReloadShaders = false;
	if (!shaderProgram.buildFromFiles(fragmentPath.c_str())) {
		std::fprintf(stderr, "Initial shader compile failed. Fix the shader file and it will hot-reload.\n");
	}

	// Fullscreen triangle (no attributes needed; vertex shader generates positions)
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

	// Create blit program for FFglitch texture
	GLuint blitProgram = createProgram(kBlitVS, kBlitFS);
	GLint blitTexLoc = blitProgram ? glGetUniformLocation(blitProgram, "u_tex") : -1;

	auto lastTime = std::chrono::high_resolution_clock::now();
	float timeSeconds = 0.0f;
	float onsetDisplay = 0.0f;
	float beatRateDisplay = 0.0f;
	float analysisUpdateRateHz = 0.0f;
	auto lastAnalysisUpdateTime = std::chrono::steady_clock::now();
	auto lastLiveStatsLogTime = std::chrono::steady_clock::now();
	bool showDemoWindow = false;
	bool freezeAudio = false;
	float smoothing = 0.35f;
	unsigned int selectedDeviceId = (unsigned int)-1;
	std::vector<AudioInput::DeviceInfo> devices = audioInput.listInputDevices();
	if (audioInput.getCurrentDeviceId() != 0) selectedDeviceId = audioInput.getCurrentDeviceId();
	if (selectedDeviceId == (unsigned int)-1) {
		for (const auto& d : devices) { if (d.isDefault) { selectedDeviceId = d.id; break; } }
	}
	std::vector<float> waveform; waveform.reserve(2048);

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
	// Output window / monitor selection
	GLFWwindow* outputWindow = nullptr;
	GLuint outputVao = 0; // VAO for the output context (VAOs are NOT shared between contexts)
	std::vector<GLFWmonitor*> monitorList;
	std::vector<std::string> monitorNames;
	int selectedMonitor = 0;
	{
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
	}

	while (!glfwWindowShouldClose(window)) {
		glfwPollEvents();

		int display_w, display_h;
		glfwGetFramebufferSize(window, &display_w, &display_h);
		glViewport(0, 0, display_w, display_h);

		// Timing
		auto now = std::chrono::high_resolution_clock::now();
		std::chrono::duration<float> delta = now - lastTime;
		lastTime = now;
		timeSeconds += delta.count();

	        // Audio -> Features
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
                shaderAf = applyAudioTuning(af, audioTuning);
				shaderMetrics = deriveShaderAudioMetrics(shaderAf);
				const auto analysisNow = std::chrono::steady_clock::now();
				const double updateDtSec = std::chrono::duration<double>(analysisNow - lastAnalysisUpdateTime).count();
				lastAnalysisUpdateTime = analysisNow;
				if (updateDtSec > 1e-6) {
					analysisUpdateRateHz = float(1.0 / updateDtSec);
				}
			}
		}
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

		// Hot reload checks hit the filesystem; keep them opt-in.
		if (uiHotReloadShaders) shaderProgram.updateIfChanged(fragmentPath.c_str());

		// (moved) Send ZMQ metrics after smoothing is computed

		// Render preview to offscreen square target
		glBindFramebuffer(GL_FRAMEBUFFER, previewFbo);
		GLuint renderTarget = (!useFFglitch && uiShaderFeedback) ? feedbackTex[fbWrite] : previewTex;
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderTarget, 0);
		previewDisplayTex = (!useFFglitch && uiShaderFeedback) ? feedbackTex[fbRead] : previewTex;
		glViewport(0, 0, previewSize, previewSize);
		glClearColor(0.03f, 0.03f, 0.05f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		if (!useFFglitch) {
			// Shader mode: support optional feedback (iChannel0 is previous frame)
			// Toggle is in Render panel; variable declared static here for render use
			shaderProgram.use();
			if (uiShaderFeedback) {
				// Bind previous as iChannel0 on texture unit 1
				shaderProgram.setUniformi("iChannel0", 1);
				shaderProgram.setUniform("iResolution", (float)previewSize, (float)previewSize);
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, feedbackTex[fbRead]);
			}
		} else {
			if (blitProgram) {
				glUseProgram(blitProgram);
				if (blitTexLoc >= 0) glUniform1i(blitTexLoc, 0);
			}
		}
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

		// Send ZMQ metrics if running (after smoothing exists)
			if (zmqPipe) {
				char json[512];
				int n = std::snprintf(json, sizeof(json),
					"{\"rms\":%.4f,\"bandLow\":%.4f,\"bandMid\":%.4f,\"bandHigh\":%.4f,\"onset\":%.4f,\"bpm\":%.2f,\"beatEnv\":%.3f,\"percE\":%.3f,\"harmE\":%.3f,\"percRatio\":%.3f,\"kick\":%.3f,\"bass\":%.3f,\"lead\":%.3f,\"air\":%.3f,\"tension\":%.3f,\"drop\":%.3f}\n",
					smoothed.rms, smoothed.bandLow, smoothed.bandMid, smoothed.bandHigh, smoothed.onset,
					shaderAf.bpm, shaderAf.beatEnvelope, smoothed.percE, smoothed.harmE, shaderAf.percussiveRatio,
					shaderAf.kickImpact, shaderAf.bassBody, shaderAf.leadPresence, shaderAf.airPresence, shaderAf.tension, shaderAf.dropEvent);
				if (n > 0) { fwrite(json, 1, (size_t)n, zmqPipe); fflush(zmqPipe); }
			}

		auto setAllUniforms = [&](int resx, int resy){
			shaderProgram.setUniform("u_time", timeSeconds);
			shaderProgram.setUniform("u_resolution", (float)resx, (float)resy);

			shaderProgram.setUniform("u_rms", smoothed.rms);
			shaderProgram.setUniform("u_bandLow", smoothed.bandLow);
			shaderProgram.setUniform("u_bandMid", smoothed.bandMid);
			shaderProgram.setUniform("u_bandHigh", smoothed.bandHigh);
			shaderProgram.setUniform("u_onset", smoothed.onset);
			// Optional RM spheres controls (harmless if shader doesn't declare them)
			shaderProgram.setUniform("u_camVel", ui_camVel);
			shaderProgram.setUniform("u_useCamVel", ui_useCamVel ? 1.0f : 0.0f);
			shaderProgram.setUniform("u_warpIntensity", ui_warpIntensity);
			shaderProgram.setUniform("u_stepFactor", ui_stepFactor);
			shaderProgram.setUniform("u_epsilon", ui_epsilon);
			shaderProgram.setUniform("u_maxStepsF", (float)ui_maxSteps);
			shaderProgram.setUniform("u_fogDensity", ui_fogDensity);
			shaderProgram.setUniform("u_barrelK", ui_barrelK);
			shaderProgram.setUniform("u_colorGain", ui_colorGain);
			shaderProgram.setUniform("u_cellScale", ui_cellScale);
			shaderProgram.setUniform("u_r1Base", ui_r1Base);
			shaderProgram.setUniform("u_r2Base", ui_r2Base);
			shaderProgram.setUniform("u_flashGain", ui_flashGain);
			// Advanced analysis arrays and scalars
			if (!shaderAf.bandEnergyNorm.empty()) {
				int n = (int)std::min<size_t>(16, shaderAf.bandEnergyNorm.size());
				float bands[16];
				float onsets[16];
				for (int i = 0; i < n; ++i) {
					bands[i] = std::max(0.0f, std::min(1.0f, shaderAf.bandEnergyNorm[i] * bandScale[i] * bandsGain));
					onsets[i] = std::max(0.0f, std::min(1.0f, shaderAf.bandOnset[i] * onsetScale[i] * onsetsGain));
				}
				shaderProgram.setUniform1fv("u_bands", bands, n);
				shaderProgram.setUniform1fv("u_onsets", onsets, n);
			}
				float centroidNorm = 0.0f;
				if (analyzer.getFftSize() > 0) {
					float nyq = (float)analyzer.getSampleRate() * 0.5f;
					centroidNorm = nyq > 1.0f ? std::max(0.0f, std::min(1.0f, shaderAf.spectralCentroidHz / nyq)) : 0.0f;
				}
				shaderProgram.setUniform("u_centroidNorm", centroidNorm);
				shaderProgram.setUniform("u_flux", smoothed.flux);
				shaderProgram.setUniform("u_beat", shaderAf.beatTriggered ? 1.0f : 0.0f);
				shaderProgram.setUniform("u_beatEnv", shaderAf.beatEnvelope);
				shaderProgram.setUniform("u_bpm", shaderAf.bpm);
				shaderProgram.setUniform("u_percE", smoothed.percE);
				shaderProgram.setUniform("u_harmE", smoothed.harmE);
				shaderProgram.setUniform("u_percRatio", shaderAf.percussiveRatio);
				shaderProgram.setUniform("u_kickImpact", shaderAf.kickImpact);
				shaderProgram.setUniform("u_snareImpact", shaderAf.snareImpact);
				shaderProgram.setUniform("u_hatTick", shaderAf.hatTick);
				shaderProgram.setUniform("u_beatPulse", shaderAf.beatPulse);
				shaderProgram.setUniform("u_subBody", shaderAf.subBody);
				shaderProgram.setUniform("u_bassBody", shaderAf.bassBody);
				shaderProgram.setUniform("u_harmonicBody", shaderAf.harmonicBody);
				shaderProgram.setUniform("u_leadPresence", shaderAf.leadPresence);
				shaderProgram.setUniform("u_airPresence", shaderAf.airPresence);
				shaderProgram.setUniform("u_transientDensity", shaderAf.transientDensity);
				shaderProgram.setUniform("u_novelty", shaderAf.novelty);
				shaderProgram.setUniform("u_brightness", shaderAf.brightness);
				shaderProgram.setUniform("u_percussiveFocus", shaderAf.percussiveFocus);
				shaderProgram.setUniform("u_energyLevel", shaderAf.energyLevel);
				shaderProgram.setUniform("u_tension", shaderAf.tension);
				shaderProgram.setUniform("u_release", shaderAf.release);
				shaderProgram.setUniform("u_dropEvent", shaderAf.dropEvent);
				shaderProgram.setUniform("u_sectionChange", shaderAf.sectionChange);
				shaderProgram.setUniform("u_roleKickCenterNorm", shaderAf.roleKickCenterNorm);
				shaderProgram.setUniform("u_roleBassCenterNorm", shaderAf.roleBassCenterNorm);
				shaderProgram.setUniform("u_roleLeadCenterNorm", shaderAf.roleLeadCenterNorm);
				shaderProgram.setUniform("u_roleAirCenterNorm", shaderAf.roleAirCenterNorm);
				shaderProgram.setUniform("u_roleKickConfidence", shaderAf.roleKickConfidence);
				shaderProgram.setUniform("u_roleBassConfidence", shaderAf.roleBassConfidence);
				shaderProgram.setUniform("u_roleLeadConfidence", shaderAf.roleLeadConfidence);
				shaderProgram.setUniform("u_roleAirConfidence", shaderAf.roleAirConfidence);
			// Push auto UI uniforms if any
			for (const auto& uname : uiUniformOrder) {
				auto it = uiUniforms.find(uname);
				if (it == uiUniforms.end()) continue;
				const UIUniform& u = it->second;
				if (u.components == 1) {
					if (u.type == GL_INT || u.type == GL_BOOL) shaderProgram.setUniformi(uname.c_str(), (int)u.values[0]);
					else shaderProgram.setUniform(uname.c_str(), u.values[0]);
				} else if (u.components == 2) {
					shaderProgram.setUniform(uname.c_str(), u.values[0], u.values[1]);
				} else if (u.components == 3) {
					shaderProgram.setUniform(uname.c_str(), u.values[0], u.values[1], u.values[2]);
				} else if (u.components == 4) {
					shaderProgram.setUniform(uname.c_str(), u.values[0], u.values[1], u.values[2], u.values[3]);
				}
			}
		};

		setAllUniforms(previewSize, previewSize);
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
		} else {
			glDrawArrays(GL_TRIANGLES, 0, 3);
			// If feedback, swap and update previewTex alias
			// Note: when not feedback, previewFbo targets previewTex already
			// For feedback, we attached feedbackTex[fbWrite]
			// swap: new rendered becomes fbRead
			{
				if (uiShaderFeedback) {
					std::swap(fbRead, fbWrite);
					previewDisplayTex = feedbackTex[fbRead];
					// detach textures from units
					glActiveTexture(GL_TEXTURE1);
					glBindTexture(GL_TEXTURE_2D, 0);
					glActiveTexture(GL_TEXTURE0);
				}
			}
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

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
			} else {
				glfwMakeContextCurrent(outputWindow);
				int ow, oh;
				glfwGetFramebufferSize(outputWindow, &ow, &oh);
				glViewport(0, 0, ow, oh);
				glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
				glClear(GL_COLOR_BUFFER_BIT);
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
				glfwSwapBuffers(outputWindow);
				glfwMakeContextCurrent(window);
			}
		}

		// UI
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
        if (!dockBuilt) {
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
            ImGui::DockBuilderSetNodeSize(dockspace_id, vp->Size);
            ImGuiID dock_left = 0, dock_right = 0, dock_bottom = 0;
            dock_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.34f, nullptr, &dockspace_id);
            dock_right = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.30f, nullptr, &dockspace_id);
            dock_bottom = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.30f, nullptr, &dockspace_id);
            ImGui::DockBuilderDockWindow("Audio", dock_left);
            ImGui::DockBuilderDockWindow("Render", dock_right);
            ImGui::DockBuilderDockWindow("Output", dock_bottom);
            ImGui::DockBuilderDockWindow("Viewport", dockspace_id);
            ImGui::DockBuilderFinish(dockspace_id);
            dockBuilt = true;
        }
			ImGui::End();
		}

		// Minimal Render UI (Shader or FFglitch via FFlive)
		{
			ImGuiWindowFlags sflags = ImGuiWindowFlags_NoCollapse;
			ImGui::Begin("Render", nullptr, sflags);
			pushContentWrapPos();
			ImGui::PushItemWidth(-FLT_MIN);
			int mode = useFFglitch ? 1 : 0;
			if (ImGui::RadioButton("Shader", mode == 0)) { mode = 0; useFFglitch = false; }
#if VISUALIZA_HAS_FFGLITCH
			ImGui::SameLine();
			if (ImGui::RadioButton("FFglitch", mode == 1)) { mode = 1; useFFglitch = true; }
#else
			useFFglitch = false;
			mode = 0;
			ImGui::SameLine();
			ImGui::TextDisabled("FFglitch disabled");
#endif
			ImGui::Separator();
			if (mode == 0) {
				// Built-in shaders
                if (ImGui::BeginCombo("Built-in Shader", "Select")) {
					for (size_t i = 0; i < shaderFiles.size(); ++i) {
						std::string name = shaderFiles[i]; size_t s = name.find_last_of("/\\"); if (s != std::string::npos) name = name.substr(s + 1);
						bool sel = (shaderFiles[i] == fragmentPath);
                        if (ImGui::Selectable(name.c_str(), sel)) { fragmentPath = shaderFiles[i]; shaderProgram.buildFromFiles(fragmentPath.c_str()); invalidateUniformUI(); }
						if (sel) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
                if (ImGui::Button("Open Shader…")) { std::string path = openFileDialog("Select GLSL Fragment Shader", nullptr, {"frag"}); if (!path.empty()) { fragmentPath = path; shaderProgram.buildFromFiles(fragmentPath.c_str()); } }
                ImGui::Separator();
                ImGui::Checkbox("Enable shader feedback (single-file feedback iChannel0)", &uiShaderFeedback);
                ImGui::Checkbox("Hot reload shaders", &uiHotReloadShaders);
                if (ImGui::SliderInt("Preview resolution", &previewSize, 256, 1024)) createPreviewTargets();
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
			ImGui::End();
		}
        // Audio window
        {
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
            ImGui::Begin("Audio", nullptr, flags);
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
						ImGui::TextDisabled("Selected source is mono. For desktop playback capture, prefer a 2-channel monitor/stereo source if available.");
					} else if (selectedInfo && selectedInfo->inputChannels > 0 && selectedInfo->outputChannels > 0) {
						ImGui::TextDisabled("Selected source is a playback monitor. This should follow desktop audio output.");
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
            // Move Analysis Live here to avoid cluttering Shader tab
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Analysis Live", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::BeginChild("analysis_live_child", ImVec2(0.0f, 310.0f), true);
                if (ImGui::BeginTable("analysis_live_metrics", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("Energy / Tension / Novelty");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f / %.3f / %.3f", af.energyLevel, af.tension, af.novelty);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("Kick / Snare / Hat / Beat");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f / %.3f / %.3f / %.3f", af.kickImpact, af.snareImpact, af.hatTick, af.beatPulse);
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

                    ImGui::TextUnformatted("EDM Response");
                    ImGui::ProgressBar(lowShare, ImVec2(-FLT_MIN, 0.0f), "Bass");
                    ImGui::ProgressBar(midShare, ImVec2(-FLT_MIN, 0.0f), "Body");
                    ImGui::ProgressBar(highShare, ImVec2(-FLT_MIN, 0.0f), "Air");
                    ImGui::Text("Beat rate: %.1f/min", beatRateDisplay);
                    ImGui::SameLine();
                    ImGui::Text("Centroid: %.0f%%", std::max(0.0f, std::min(1.0f, af.spectralCentroidHz / std::max(1.0f, (float)sampleRate * 0.5f))) * 100.0f);
                    if (highShare < 0.08f) ImGui::TextDisabled("Air response is low; high-band nuance may be subtle.");
                    if (midShare < 0.16f) ImGui::TextDisabled("Body response is low; visuals may lean bass-heavy.");
                    ImGui::Separator();
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
            ImGui::End();
        }

		// Shader window (reworked)
        {
            ImGuiWindowFlags sflags = ImGuiWindowFlags_NoCollapse;
            ImGui::Begin("Shader", nullptr, sflags);
            pushContentWrapPos();
            ImGui::PushItemWidth(-FLT_MIN);

#if VISUALIZA_HAS_FFGLITCH
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
			if (useFFglitch && !ffgPlayer.isRunning()) {
				if (std::strlen(ffgInputUI) > 0) {
					ffgPlayer.setInput(ffgInputUI);
					if (std::strlen(ffgScriptUI) > 0) ffgPlayer.setScript(ffgScriptUI);
					ffgPlayer.start();
				} else {
					ImGui::TextDisabled("Set an input file to start FFglitch.");
				}
			}

			if (useFFglitch) {
				ImGui::Separator();
				static float ui_glitch = 0.0f; // 0..1
				static int ui_block = 16;
				static int ui_seed = 12345;
				static bool ui_loop = true;
				ImGui::TextUnformatted("Glitch Controls");
				ImGui::SliderFloat("Intensity", &ui_glitch, 0.0f, 1.0f);
				ImGui::SliderInt("Block Size", &ui_block, 4, 128);
				ImGui::InputInt("Seed", &ui_seed);
				ImGui::Checkbox("Loop", &ui_loop);
				ffgPlayer.setGlitchIntensity(ui_glitch);
				ffgPlayer.setGlitchBlockSize(ui_block);
				ffgPlayer.setGlitchSeed((uint32_t)ui_seed);
				ffgPlayer.setLoop(ui_loop);
			}
#else
			useFFglitch = false;
			ImGui::TextDisabled("FFglitch controls are unavailable in this lightweight build.");
#endif

            if (ImGui::CollapsingHeader("Active Shader", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::string cur = fragmentPath;
                size_t slash = cur.find_last_of("/\\");
                if (slash != std::string::npos) cur = cur.substr(slash + 1);
                ImGui::TextWrapped("Current: %s", cur.c_str());
                if (ImGui::Button("Reload")) shaderProgram.forceReload(fragmentPath.c_str());
            }

            if (ImGui::CollapsingHeader("Built-in Shaders", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginCombo("Select", "Built-ins")) {
                    for (size_t i = 0; i < shaderFiles.size(); ++i) {
                        std::string name = shaderFiles[i];
                        size_t s = name.find_last_of("/\\"); if (s != std::string::npos) name = name.substr(s + 1);
                        bool sel = (shaderFiles[i] == fragmentPath);
                        if (ImGui::Selectable(name.c_str(), sel)) {
                            fragmentPath = shaderFiles[i];
                            shaderProgram.buildFromFiles(fragmentPath.c_str());
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Refresh")) {
                    shaderFiles = collectShaders({"../assets/shaders", bundleAssets(), "assets/shaders"});
                    shadersDir = std::filesystem::exists("../assets/shaders") ? "../assets/shaders" : bundleAssets();
                }
            }

            if (ImGui::CollapsingHeader("External Shader", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("Open .frag shader");
                if (ImGui::Button("Browse…")) {
                    std::string path = openFileDialog("Select GLSL Fragment Shader", nullptr, {"frag"});
                    if (!path.empty()) {
                        fragmentPath = path;
                        shaderProgram.buildFromFiles(fragmentPath.c_str());
                        invalidateUniformUI();
                    }
                }
            }
            std::string cur = fragmentPath;
            size_t slash = cur.find_last_of("/\\");
            if (slash != std::string::npos) cur = cur.substr(slash + 1);
            if (ImGui::BeginCombo("Shader", cur.c_str())) {
                for (size_t i = 0; i < shaderFiles.size(); ++i) {
                    std::string name = shaderFiles[i];
                    size_t s = name.find_last_of("/\\"); if (s != std::string::npos) name = name.substr(s + 1);
                    bool sel = (shaderFiles[i] == fragmentPath);
                    if (ImGui::Selectable(name.c_str(), sel)) {
                        fragmentPath = shaderFiles[i];
                        shaderProgram.buildFromFiles(fragmentPath.c_str());
                        invalidateUniformUI();
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Refresh shaders")) {
                shaderFiles = collectShaders({"../assets/shaders", "assets/shaders"});
                shadersDir = std::filesystem::exists("../assets/shaders") ? "../assets/shaders" : "assets/shaders";
            }
            if (ImGui::Button("Reload Shader")) { shaderProgram.forceReload(fragmentPath.c_str()); invalidateUniformUI(); }
            ImGui::Text("Shader path: %s", fragmentPath.c_str());

            // External shader loader (hot-reload works after load)
            ImGui::Separator();
            ImGui::InputText("External .frag path", shaderPathBuf, IM_ARRAYSIZE(shaderPathBuf));
            if (ImGui::Button("Load External")) {
                if (std::strlen(shaderPathBuf) > 0) {
                    std::string newPath(shaderPathBuf);
                    fragmentPath = newPath;
                    shaderProgram.buildFromFiles(fragmentPath.c_str());
                    invalidateUniformUI();
                }
            }

            ImGui::Separator();
            if (ImGui::CollapsingHeader("Audio Tuning", ImGuiTreeNodeFlags_DefaultOpen)) {
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
                        audioTuningStatus = "Reset audio tuning to defaults";
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Refresh list")) refreshAudioTuningPresets();
                    ImGui::EndTable();
                }
                if (!audioTuningStatus.empty()) ImGui::TextDisabled("%s", audioTuningStatus.c_str());
                ImGui::BeginChild("audio_tuning_child", ImVec2(0.0f, 290.0f), true);
                if (ImGui::BeginTable("audio_tuning_controls", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::SliderFloat("Pulse Gain", &audioTuning.pulseGain, 0.2f, 3.0f); ImGui::TableSetColumnIndex(1); ImGui::SliderFloat("Pulse Knee", &audioTuning.pulseKnee, 0.2f, 2.0f);
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::SliderFloat("Body Gain", &audioTuning.bodyGain, 0.2f, 3.0f); ImGui::TableSetColumnIndex(1); ImGui::SliderFloat("Body Knee", &audioTuning.bodyKnee, 0.2f, 2.0f);
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::SliderFloat("Detail Gain", &audioTuning.detailGain, 0.2f, 3.0f); ImGui::TableSetColumnIndex(1); ImGui::SliderFloat("Detail Knee", &audioTuning.detailKnee, 0.2f, 2.0f);
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::SliderFloat("Macro Gain", &audioTuning.macroGain, 0.2f, 3.0f); ImGui::TableSetColumnIndex(1); ImGui::SliderFloat("Macro Knee", &audioTuning.macroKnee, 0.2f, 2.0f);
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::SliderFloat("Bass Gain", &audioTuning.bassGain, 0.2f, 3.0f); ImGui::TableSetColumnIndex(1); ImGui::SliderFloat("Air Gain", &audioTuning.airGain, 0.2f, 3.0f);
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::SliderFloat("Harmonic Gain", &audioTuning.harmonicGain, 0.2f, 3.0f); ImGui::TableSetColumnIndex(1); ImGui::SliderFloat("Lead Gain", &audioTuning.leadGain, 0.2f, 3.0f);
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::SliderFloat("Novelty Gain", &audioTuning.noveltyGain, 0.2f, 3.0f); ImGui::TableSetColumnIndex(1); ImGui::SliderFloat("Brightness Gain", &audioTuning.brightnessGain, 0.2f, 3.0f);
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::SliderFloat("Percussive Gain", &audioTuning.percussiveGain, 0.2f, 3.0f); ImGui::TableSetColumnIndex(1); ImGui::SliderFloat("Tension Gain", &audioTuning.tensionGain, 0.2f, 3.0f);
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::SliderFloat("Release Gain", &audioTuning.releaseGain, 0.2f, 3.0f); ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("Shader-facing semantic shaping");
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::SliderFloat("Pulse Floor", &audioTuning.pulseFloor, 0.0f, 0.6f); ImGui::TableSetColumnIndex(1); ImGui::SliderFloat("Body Floor", &audioTuning.bodyFloor, 0.0f, 0.6f);
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::SliderFloat("Detail Floor", &audioTuning.detailFloor, 0.0f, 0.6f); ImGui::TableSetColumnIndex(1); ImGui::SliderFloat("Macro Floor", &audioTuning.macroFloor, 0.0f, 0.6f);
                    ImGui::EndTable();
                }
                ImGui::EndChild();
            }

            ImGui::Separator();
            if (ImGui::CollapsingHeader("Analysis Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SliderFloat("Bands Gain", &bandsGain, 0.0f, 4.0f);
                ImGui::SliderFloat("Onsets Gain", &onsetsGain, 0.0f, 4.0f);
                ImGui::Checkbox("Per-band overrides", &showPerBandControls);
                if (showPerBandControls) {
                    int n = (int)std::min<size_t>(16, af.bandEnergyNorm.size());
                    for (int i = 0; i < n; ++i) {
                        char lbl1[32]; std::snprintf(lbl1, sizeof(lbl1), "Band %02d", i);
                        ImGui::SliderFloat(lbl1, &bandScale[i], 0.0f, 4.0f);
                        char lbl2[32]; std::snprintf(lbl2, sizeof(lbl2), "Onset %02d", i);
                        ImGui::SliderFloat(lbl2, &onsetScale[i], 0.0f, 4.0f);
                    }
                }
            }

            if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen)) {

	                // Auto UI for current shader uniforms
	                if (uiUniforms.empty()) rebuildUniformUI(shaderProgram);
	                if (!uiUniformOrder.empty()) {
                    ImGui::Separator();
                    ImGui::TextUnformatted("Shader Uniforms (auto)");
                    ImGui::BeginChild("shader_uniforms_child", ImVec2(0.0f, 260.0f), true);
                    for (const auto& uname : uiUniformOrder) {
                        auto it = uiUniforms.find(uname);
                        if (it == uiUniforms.end()) continue;
                        UIUniform& u = it->second;
                        if (u.components == 1) {
                            if (u.type == GL_INT || u.type == GL_BOOL) {
                                int v = (int)u.values[0];
                                if (ImGui::InputInt(uname.c_str(), &v)) { u.values[0] = (float)v; shaderProgram.setUniformi(uname.c_str(), v); }
                            } else {
                                if (ImGui::SliderFloat(uname.c_str(), &u.values[0], -10.0f, 10.0f)) shaderProgram.setUniform(uname.c_str(), u.values[0]);
                            }
                        } else if (u.components == 2) {
                            float v2[2] = {u.values[0], u.values[1]};
                            if (ImGui::DragFloat2(uname.c_str(), v2, 0.01f)) { u.values[0]=v2[0]; u.values[1]=v2[1]; shaderProgram.setUniform(uname.c_str(), v2[0], v2[1]); }
                        } else if (u.components == 3) {
                            float v3[3] = {u.values[0], u.values[1], u.values[2]};
                            if (ImGui::DragFloat3(uname.c_str(), v3, 0.01f)) { u.values[0]=v3[0]; u.values[1]=v3[1]; u.values[2]=v3[2]; shaderProgram.setUniform(uname.c_str(), v3[0], v3[1], v3[2]); }
                        } else if (u.components == 4) {
                            float v4[4] = {u.values[0], u.values[1], u.values[2], u.values[3]};
                            if (ImGui::DragFloat4(uname.c_str(), v4, 0.01f)) { u.values[0]=v4[0]; u.values[1]=v4[1]; u.values[2]=v4[2]; u.values[3]=v4[3]; shaderProgram.setUniform(uname.c_str(), v4[0], v4[1], v4[2], v4[3]); }
                        }
                    }
                    ImGui::EndChild();
                } else {
                    ImGui::Separator();
                    ImGui::TextDisabled("No adjustable uniforms detected in current shader.");
                }
            }

            ImGui::PopItemWidth();
            ImGui::PopTextWrapPos();
            ImGui::End();
        }

        // Output window
        {
            ImGuiWindowFlags oflags = ImGuiWindowFlags_NoCollapse;
            ImGui::Begin("Output", nullptr, oflags);
            pushContentWrapPos();
            ImGui::PushItemWidth(-FLT_MIN);
            std::string curMon = monitorNames.empty() ? std::string("No monitors") : monitorNames[selectedMonitor];
            if (ImGui::BeginCombo("Output monitor", curMon.c_str())) {
                for (int i = 0; i < (int)monitorNames.size(); ++i) {
                    bool sel = (i == selectedMonitor);
                    if (ImGui::Selectable(monitorNames[i].c_str(), sel)) selectedMonitor = i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button(outputWindow ? "Close Output" : "Open Fullscreen Output")) {
                if (outputWindow) {
                    glfwMakeContextCurrent(outputWindow);
                    if (outputVao) { glDeleteVertexArrays(1, &outputVao); outputVao = 0; }
                    glfwDestroyWindow(outputWindow);
                    outputWindow = nullptr;
                } else if (!monitorList.empty()) {
                    GLFWmonitor* mon = monitorList[std::max(0, std::min(selectedMonitor, (int)monitorList.size()-1))];
                    const GLFWvidmode* vm = glfwGetVideoMode(mon);
                    if (vm) {
                        // Create borderless windowed fullscreen on the chosen monitor to avoid auto-minimize
                        int mx = 0, my = 0;
                        glfwGetMonitorPos(mon, &mx, &my);
                        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
                        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
                        glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
                        glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
                        outputWindow = glfwCreateWindow(vm->width, vm->height, "visual_output", nullptr, window);
                        if (outputWindow) {
                            glfwMakeContextCurrent(outputWindow);
                            glfwSwapInterval(1);
                            if (outputVao) { glDeleteVertexArrays(1, &outputVao); outputVao = 0; }
                            glGenVertexArrays(1, &outputVao);
                            glBindVertexArray(outputVao);
                            glfwSetWindowPos(outputWindow, mx, my);
                            glfwMakeContextCurrent(window);
                        }
                    }
                }
            }
            ImGui::PopItemWidth();
            ImGui::PopTextWrapPos();
            ImGui::End();
        }

        // Docked viewport window that fits remaining space and centers a square texture
        {
            ImGuiWindowFlags vflags = ImGuiWindowFlags_NoCollapse;
            ImGui::Begin("Viewport", nullptr, vflags);
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float side = std::floor(std::min(avail.x, avail.y));
            ImVec2 cursor = ImGui::GetCursorPos();
            ImGui::SetCursorPos(ImVec2(cursor.x + (avail.x - side) * 0.5f, cursor.y + (avail.y - side) * 0.5f));
            ImGui::Image((ImTextureID)(intptr_t)previewDisplayTex, ImVec2(side, side), ImVec2(0,1), ImVec2(1,0));
            ImGui::End();
        }

		if (showDemoWindow) ImGui::ShowDemoWindow(&showDemoWindow);

		ImGui::Render();
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		glfwSwapBuffers(window);
	}

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
