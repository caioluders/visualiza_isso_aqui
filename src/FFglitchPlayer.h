#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <random>
#include <glad/glad.h>

// Placeholder wrapper for ffglitch-core integration.
// Currently generates a CPU-side frame and uploads to a GL texture.
// Replace the internals with real ffglitch-core decoding/glitching when linked.
class FFglitchPlayer {
public:
	FFglitchPlayer();
	~FFglitchPlayer();

	void setInput(const std::string& inputPath);
	void setScript(const std::string& scriptPath);

	bool start();
	void stop();
	bool isRunning() const { return running; }

	// Call every frame on the GL thread to upload latest frame
	void update(float timeSeconds);

	GLuint getTextureId() const { return textureId; }
	int getWidth() const { return width; }
	int getHeight() const { return height; }

	// Controls
	void setGlitchIntensity(float v) { glitchIntensity = v; }
	void setGlitchBlockSize(int v) { glitchBlockSize = v; }
	void setGlitchSeed(uint32_t s) { glitchSeed = s; }
	void setLoop(bool v) { loopPlayback = v; }
	bool getLoop() const { return loopPlayback; }

private:
	void ensureTexture();
	void decodingLoop();
		struct SimpleMV; // forward declaration for method signature
		void applyMotionSink(std::vector<uint8_t>& dstRgba, const std::vector<uint8_t>& srcPrevRgba, int w, int h, const std::vector<SimpleMV>& mvs);

	// Minimal copy of FFmpeg motion vector struct to avoid exposing FFmpeg in header API
	struct SimpleMV {
		int srcX;
		int srcY;
		int dstX;
		int dstY;
		int w;
		int h;
		int source;      // 0: backward, 1: forward (per FFmpeg)
		int motionX;      // motion in 1/(1<<motionScale) pixel units
		int motionY;      // motion in 1/(1<<motionScale) pixel units
		int motionScale;  // scale exponent
	};

private:
	std::string input;
	std::string script;
	std::atomic<bool> running;
	int width;
	int height;
	GLuint textureId;
	std::vector<unsigned char> frontBuffer;
	std::vector<unsigned char> backBuffer;
	std::vector<unsigned char> prevFrontBuffer;
	std::vector<unsigned char> prevGlitchedBuffer;
	std::vector<SimpleMV> frontMvs;
	std::vector<SimpleMV> backMvs;
	std::mutex bufferMutex;
	std::thread worker;
	float glitchIntensity = 0.0f; // 0..1
	int glitchBlockSize = 16;
	uint32_t glitchSeed = 12345u;
	bool loopPlayback = true;
};


