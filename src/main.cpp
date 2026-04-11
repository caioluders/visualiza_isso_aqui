#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <atomic>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "ShaderProgram.h"
#include "AudioInput.h"
#include "FeatureExtractor.h"
#include "AudioFilePlayer.h"
#include "AudioAnalysis.h"
#include "FileDialog.h"
#include "FFglitchPlayer.h"

static void glfwErrorCallback(int error, const char* description) {
	std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

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
	if (!audioInput.startStream(sampleRate, channelCount)) {
		std::fprintf(stderr, "Failed to start audio input\n");
	}

    FeatureExtractor featureExtractor(sampleRate);
    FeatureExtractor::Features features{};

    // Advanced analysis
    AudioAnalysis::Config aaCfg;
    aaCfg.fftSize = 1024;
    aaCfg.numBands = 16;
    AudioAnalysis analyzer(sampleRate, aaCfg);
    AudioAnalysis::AnalysisFrame af{};

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

    auto isIgnoredUniform = [](const std::string& n) {
        static const char* ignored[] = {
            "u_time","u_resolution",
            "u_rms","u_bandLow","u_bandMid","u_bandHigh","u_onset",
            "u_bands","u_onsets","u_centroidNorm","u_flux","u_beat","u_beatEnv","u_bpm","u_percE","u_harmE","u_percRatio",
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
	if (!shaderProgram.buildFromFiles(fragmentPath.c_str())) {
		std::fprintf(stderr, "Initial shader compile failed. Fix the shader file and it will hot-reload.\n");
	}

	// Fullscreen triangle (no attributes needed; vertex shader generates positions)
	GLuint vao = 0;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	// Offscreen preview framebuffer (square) for centered viewport
	GLuint previewFbo = 0, previewTex = 0;
	int previewSize = 720;
    // Shader feedback ping-pong resources
    GLuint feedbackTex[2] = {0,0};
    int fbRead = 0, fbWrite = 1;
	auto createPreviewTargets = [&]() {
		if (previewTex) glDeleteTextures(1, &previewTex);
		if (previewFbo) glDeleteFramebuffers(1, &previewFbo);
        if (feedbackTex[0]) { glDeleteTextures(1, &feedbackTex[0]); feedbackTex[0] = 0; }
        if (feedbackTex[1]) { glDeleteTextures(1, &feedbackTex[1]); feedbackTex[1] = 0; }
		glGenTextures(1, &previewTex);
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
	bool showDemoWindow = false;
	bool freezeAudio = false;
	float smoothing = 0.6f;
	unsigned int selectedDeviceId = (unsigned int)-1;
	std::vector<AudioInput::DeviceInfo> devices = audioInput.listInputDevices();
	for (const auto& d : devices) { if (d.isDefault) { selectedDeviceId = d.id; break; } }
	std::vector<float> waveform; waveform.reserve(2048);

	// FFglitch integration (external tool invocation)
	static char ffeditPath[256] = "ffedit";
    static char ffgInput[1024] = "";
    static char ffgScript[1024] = "";
    static char ffgOutput[1024] = "udp://127.0.0.1:12345?pkt_size=1316";
	static char ffgArgs[1024] = "-i \"{in}\" -o \"{out}\" -s \"{script}\"";
	std::string ffgLog;
	std::mutex ffgLogMutex;
	std::atomic<bool> ffgRunning(false);
	std::thread ffgThread;
	auto appendFfgLog = [&](const std::string& s){ std::lock_guard<std::mutex> lk(ffgLogMutex); ffgLog += s; };
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
	static char fflOutput[1024] = "udp://127.0.0.1:12345?pkt_size=1316";
	static char fflArgs[1024] = "-i \"{in}\" -s \"{script}\"";
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
	auto buildFfgCommand = [&](){
		std::string cmd = std::string(ffeditPath);
		cmd += " ";
		std::string args = ffgArgs;
		auto replace_all = [](std::string& s, const std::string& a, const std::string& b){ size_t p=0; while((p=s.find(a,p))!=std::string::npos){ s.replace(p,a.size(),b); p += b.size(); } };
		replace_all(args, "{in}", ffgInput);
		replace_all(args, "{out}", ffgOutput);
		replace_all(args, "{script}", ffgScript);
		cmd += args;
		cmd += " 2>&1"; // capture stderr
		return cmd;
	};
	auto runFfg = [&](){
		if (ffgRunning.load()) return;
		ffgRunning = true;
		std::string cmd = buildFfgCommand();
		appendFfgLog(std::string("\n$ ")+cmd+"\n");
		printf("Running ffedit: %s\n", cmd.c_str());
		ffgThread = std::thread([&, cmd]{
			FILE* fp = popen(cmd.c_str(), "r");
			if (!fp) {
				appendFfgLog("Failed to start ffedit.\n");
				ffgRunning = false;
				return;
			}
			char line[512];
			while (fgets(line, sizeof(line), fp)) {
				appendFfgLog(std::string(line));
			}
			pclose(fp);
			ffgRunning = false;
		});
	};

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
		if (!freezeAudio) {
			if (useFile && filePlayer.isPlaying()) {
				filePlayer.getNext(1024, latest);
				if (filePlayer.getSampleRate() != sampleRate) {
					sampleRate = filePlayer.getSampleRate();
					featureExtractor = FeatureExtractor(sampleRate);
                    analyzer = AudioAnalysis(sampleRate, aaCfg);
				}
			} else {
				audioInput.readLatest(1024, latest);
			}
			if (!latest.empty()) {
				features = featureExtractor.compute(latest);
                af = analyzer.processInterleavedStereo(latest);
			}
		}
		// Simple decay for visual onset indicator
		onsetDisplay = std::max(features.onset, onsetDisplay * std::pow(0.5f, delta.count() * 10.0f));

		// Hot reload shader if fragment changed
		shaderProgram.updateIfChanged(fragmentPath.c_str());

		// (moved) Send ZMQ metrics after smoothing is computed

		// Render preview to offscreen square target
		glBindFramebuffer(GL_FRAMEBUFFER, previewFbo);
		glViewport(0, 0, previewSize, previewSize);
		glClearColor(0.03f, 0.03f, 0.05f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		if (!useFFglitch) {
			// Shader mode: support optional feedback (iChannel0 is previous frame)
			// Toggle is in Render panel; variable declared static here for render use
			shaderProgram.use();
			if (uiShaderFeedback) {
				// Attach write target
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, feedbackTex[fbWrite], 0);
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
		static FeatureExtractor::Features smoothed{};
		smoothed.rms = smoothed.rms * smoothing + features.rms * (1.0f - smoothing);
		smoothed.bandLow = smoothed.bandLow * smoothing + features.bandLow * (1.0f - smoothing);
		smoothed.bandMid = smoothed.bandMid * smoothing + features.bandMid * (1.0f - smoothing);
		smoothed.bandHigh = smoothed.bandHigh * smoothing + features.bandHigh * (1.0f - smoothing);
		smoothed.onset = onsetDisplay;

		// Send ZMQ metrics if running (after smoothing exists)
		if (zmqPipe) {
			char json[512];
			int n = std::snprintf(json, sizeof(json),
				"{\"rms\":%.4f,\"bandLow\":%.4f,\"bandMid\":%.4f,\"bandHigh\":%.4f,\"onset\":%.4f,\"bpm\":%.2f,\"beatEnv\":%.3f,\"percE\":%.3f,\"harmE\":%.3f,\"percRatio\":%.3f}\n",
				smoothed.rms, smoothed.bandLow, smoothed.bandMid, smoothed.bandHigh, smoothed.onset,
				af.bpm, af.beatEnvelope, af.percussiveEnergy, af.harmonicEnergy, af.percussiveRatio);
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
			if (!af.bandEnergyNorm.empty()) {
				int n = (int)std::min<size_t>(16, af.bandEnergyNorm.size());
				float bands[16];
				float onsets[16];
				for (int i = 0; i < n; ++i) {
					bands[i] = std::max(0.0f, std::min(1.0f, af.bandEnergyNorm[i] * bandScale[i] * bandsGain));
					onsets[i] = std::max(0.0f, std::min(1.0f, af.bandOnset[i] * onsetScale[i] * onsetsGain));
				}
				shaderProgram.setUniform1fv("u_bands", bands, n);
				shaderProgram.setUniform1fv("u_onsets", onsets, n);
			}
			float centroidNorm = 0.0f;
			if (analyzer.getFftSize() > 0) {
				float nyq = (float)analyzer.getSampleRate() * 0.5f;
				centroidNorm = nyq > 1.0f ? std::max(0.0f, std::min(1.0f, af.spectralCentroidHz / nyq)) : 0.0f;
			}
			shaderProgram.setUniform("u_centroidNorm", centroidNorm);
			shaderProgram.setUniform("u_flux", af.spectralFlux);
			shaderProgram.setUniform("u_beat", af.beatTriggered ? 1.0f : 0.0f);
			shaderProgram.setUniform("u_beatEnv", af.beatEnvelope);
			shaderProgram.setUniform("u_bpm", af.bpm);
			shaderProgram.setUniform("u_percE", af.percussiveEnergy);
			shaderProgram.setUniform("u_harmE", af.harmonicEnergy);
			shaderProgram.setUniform("u_percRatio", af.percussiveRatio);
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
					previewTex = feedbackTex[fbRead];
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
                if (!useFFglitch) {
                    // If feedback is enabled, just blit the preview texture
                    if (uiShaderFeedback) {
						if (blitProgram) {
							glUseProgram(blitProgram);
							if (blitTexLoc >= 0) glUniform1i(blitTexLoc, 0);
						}
						glActiveTexture(GL_TEXTURE0);
						glBindTexture(GL_TEXTURE_2D, previewTex);
						glDrawArrays(GL_TRIANGLES, 0, 3);
						glBindTexture(GL_TEXTURE_2D, 0);
					} else {
						shaderProgram.use();
						setAllUniforms(ow, oh);
						glDrawArrays(GL_TRIANGLES, 0, 3);
					}
				} else {
					if (blitProgram) {
						glUseProgram(blitProgram);
						if (blitTexLoc >= 0) glUniform1i(blitTexLoc, 0);
					}
					ffgPlayer.update(timeSeconds);
					GLuint tex = ffgPlayer.getTextureId();
					if (tex) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); }
					glDrawArrays(GL_TRIANGLES, 0, 3);
					if (tex) { glBindTexture(GL_TEXTURE_2D, 0); }
				}
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
            dock_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.30f, nullptr, &dockspace_id);
            dock_right = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.26f, nullptr, &dockspace_id);
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
			ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
			ImGui::PushItemWidth(-FLT_MIN);
			int mode = useFFglitch ? 1 : 0;
			if (ImGui::RadioButton("Shader", mode == 0)) { mode = 0; useFFglitch = false; }
			ImGui::SameLine();
			if (ImGui::RadioButton("FFglitch", mode == 1)) { mode = 1; useFFglitch = true; }
			ImGui::Separator();
			if (mode == 0) {
				// Built-in shaders
                if (ImGui::BeginCombo("Built-in Shader", "Select")) {
					for (size_t i = 0; i < shaderFiles.size(); ++i) {
						std::string name = shaderFiles[i]; size_t s = name.find_last_of("/\\"); if (s != std::string::npos) name = name.substr(s + 1);
						bool sel = (shaderFiles[i] == fragmentPath);
						if (ImGui::Selectable(name.c_str(), sel)) { fragmentPath = shaderFiles[i]; shaderProgram.buildFromFiles(fragmentPath.c_str()); }
						if (sel) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
                if (ImGui::Button("Open Shader…")) { std::string path = openFileDialog("Select GLSL Fragment Shader", nullptr, {"frag"}); if (!path.empty()) { fragmentPath = path; shaderProgram.buildFromFiles(fragmentPath.c_str()); } }
                ImGui::Separator();
                ImGui::Checkbox("Enable shader feedback (single-file feedback iChannel0)", &uiShaderFeedback);
			} else {
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
			ImGui::PopItemWidth();
			ImGui::PopTextWrapPos();
			ImGui::End();
		}
        // Audio window
        {
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
            ImGui::Begin("Audio", nullptr, flags);
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
            ImGui::PushItemWidth(-FLT_MIN);
            ImGui::Checkbox("Freeze audio", &freezeAudio);
            ImGui::SliderFloat("Smoothing", &smoothing, 0.0f, 0.95f);

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
                    if (selectedDeviceId != (unsigned int)-1) audioInput.startStreamOnDevice(selectedDeviceId, sampleRate, channelCount);
                }
            }
            if (useFile) {
                ImGui::TextUnformatted("File path");
                ImGui::InputText("##audio_file_path", filePathBuf, IM_ARRAYSIZE(filePathBuf));
                ImGui::SameLine();
                if (ImGui::Button("Load")) {
                    if (filePlayer.loadWav(filePathBuf)) {
                        channelCount = filePlayer.getChannels();
                    }
                }
                ImGui::SameLine();
                bool playing = filePlayer.isPlaying();
                if (ImGui::Checkbox("Playing", &playing)) filePlayer.setPlaying(playing);
            } else {
                if (ImGui::Button("Refresh devices")) {
                    unsigned int prev = selectedDeviceId;
                    devices = audioInput.listInputDevices();
                    selectedDeviceId = (unsigned int)-1;
                    for (const auto& d : devices) { if (d.id == prev) { selectedDeviceId = d.id; break; } }
                    if (selectedDeviceId == (unsigned int)-1) {
                        for (const auto& d : devices) { if (d.isDefault) { selectedDeviceId = d.id; break; } }
                    }
                }

		// (FFglitch batch UI removed in favor of FFlive)

                std::string currentLabel;
                for (const auto& d : devices) if (d.id == selectedDeviceId) { currentLabel = d.name + (d.isDefault ? " (default)" : ""); break; }
                if (currentLabel.empty()) currentLabel = devices.empty() ? "No input devices" : devices.front().name;
                if (ImGui::BeginCombo("Input device", currentLabel.c_str())) {
                    for (size_t i = 0; i < devices.size(); ++i) {
                        const auto& d = devices[i];
                        bool sel = (d.id == selectedDeviceId);
                        std::string label = d.name + (d.isDefault ? " (default)" : "");
                        if (ImGui::Selectable(label.c_str(), sel)) {
                            selectedDeviceId = d.id;
                            audioInput.startStreamOnDevice(selectedDeviceId, sampleRate, channelCount);
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::Separator();
            ImGui::Text("RMS: %.3f", features.rms);
            ImGui::Text("Low: %.3f  Mid: %.3f  High: %.3f", features.bandLow, features.bandMid, features.bandHigh);
            ImGui::Text("Onset: %.3f", onsetDisplay);

            waveform.clear();
            if (!latest.empty()) {
                for (size_t i = 0; i + 1 < latest.size(); i += 2) waveform.push_back((latest[i] + latest[i+1]) * 0.5f);
            }
            if (!waveform.empty()) {
                ImGui::PlotLines("Waveform", waveform.data(), (int)waveform.size(), 0, nullptr, -1.0f, 1.0f, ImVec2(0, 80));
            }
            // Move Analysis Live here to avoid cluttering Shader tab
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Analysis Live", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("RMS: %.4f", af.rms);
                ImGui::Text("Spectral Centroid: %.1f Hz", af.spectralCentroidHz);
                ImGui::Text("Spectral Flux: %.4f", af.spectralFlux);
                ImGui::Text("Beat: %s  Envelope: %.2f  BPM: %.1f", af.beatTriggered ? "ON" : "off", af.beatEnvelope, af.bpm);
                ImGui::Text("Percussive: %.3f", af.percussiveEnergy);
				ImGui::Text("Harmonic: %.3f", af.harmonicEnergy);
				ImGui::Text("P-Ratio: %.2f", af.percussiveRatio);

                ImGui::Separator();
                int n = (int)std::min<size_t>(16, af.bandEnergyNorm.size());
                if (n > 0) {
                    ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
                    if (ImGui::BeginTable("bands_table", 3, tf)) {
                        ImGui::TableSetupColumn("Band");
                        ImGui::TableSetupColumn("Energy");
                        ImGui::TableSetupColumn("Onset");
                        ImGui::TableHeadersRow();
                        for (int i = 0; i < n; ++i) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("%02d", i);
                            ImGui::TableSetColumnIndex(1);
                            float e = std::max(0.0f, std::min(1.0f, af.bandEnergyNorm[i] * bandScale[i] * bandsGain));
                            ImGui::ProgressBar(e, ImVec2(-FLT_MIN, 0.0f));
                            ImGui::TableSetColumnIndex(2);
                            float o = std::max(0.0f, std::min(1.0f, af.bandOnset[i] * onsetScale[i] * onsetsGain));
                            ImGui::ProgressBar(o, ImVec2(-FLT_MIN, 0.0f));
                        }
                        ImGui::EndTable();
                    }
                }
            }

            ImGui::PopItemWidth();
            ImGui::PopTextWrapPos();
            ImGui::End();
        }

		// Shader window (reworked)
        {
            ImGuiWindowFlags sflags = ImGuiWindowFlags_NoCollapse;
            ImGui::Begin("Shader", nullptr, sflags);
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
            ImGui::PushItemWidth(-FLT_MIN);

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
                        // force rebuild of uniform UI on shader switch
                        lastUniformSignature.clear();
                        uiUniforms.clear();
                        uiUniformOrder.clear();
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Refresh shaders")) {
                shaderFiles = collectShaders({"../assets/shaders", "assets/shaders"});
                shadersDir = std::filesystem::exists("../assets/shaders") ? "../assets/shaders" : "assets/shaders";
            }
            if (ImGui::Button("Reload Shader")) { shaderProgram.forceReload(fragmentPath.c_str()); lastUniformSignature.clear(); uiUniforms.clear(); uiUniformOrder.clear(); }
            ImGui::Text("Shader path: %s", fragmentPath.c_str());

            // External shader loader (hot-reload works after load)
            ImGui::Separator();
            ImGui::InputText("External .frag path", shaderPathBuf, IM_ARRAYSIZE(shaderPathBuf));
            if (ImGui::Button("Load External")) {
                if (std::strlen(shaderPathBuf) > 0) {
                    std::string newPath(shaderPathBuf);
                    fragmentPath = newPath;
                    shaderProgram.buildFromFiles(fragmentPath.c_str());
                }
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
                rebuildUniformUI(shaderProgram);
                // rebuild on each frame after potential reload/selection
                rebuildUniformUI(shaderProgram);
                if (!uiUniformOrder.empty()) {
                    ImGui::Separator();
                    ImGui::TextUnformatted("Shader Uniforms (auto)");
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
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
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
            ImGui::Image((ImTextureID)(intptr_t)previewTex, ImVec2(side, side), ImVec2(0,1), ImVec2(1,0));
            ImGui::End();
        }

		if (showDemoWindow) ImGui::ShowDemoWindow(&showDemoWindow);

		ImGui::Render();
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		glfwSwapBuffers(window);
	}

	// Join ffglitch thread if still running
	if (ffgThread.joinable()) {
		if (ffgRunning.load()) {
			// cannot safely kill popen; wait for process to end
		}
		ffgThread.join();
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glDeleteVertexArrays(1, &vao);

	glfwDestroyWindow(window);
	glfwTerminate();
	return EXIT_SUCCESS;
}

