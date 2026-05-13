#include "FFglitchPlayer.h"

FFglitchPlayer::FFglitchPlayer()
	: running(false), width(0), height(0), textureId(0) {}

FFglitchPlayer::~FFglitchPlayer() {
	stop();
}

void FFglitchPlayer::setInput(const std::string& inputPath) {
	input = inputPath;
}

void FFglitchPlayer::setScript(const std::string& scriptPath) {
	script = scriptPath;
}

bool FFglitchPlayer::start() {
	running = false;
	return false;
}

void FFglitchPlayer::stop() {
	running = false;
}

void FFglitchPlayer::ensureTexture() {}
void FFglitchPlayer::update(float timeSeconds) { (void)timeSeconds; }
void FFglitchPlayer::decodingLoop() {}

void FFglitchPlayer::applyMotionSink(std::vector<uint8_t>& dstRgba,
		const std::vector<uint8_t>& srcPrevRgba,
		int w,
		int h,
		const std::vector<FFglitchPlayer::SimpleMV>& mvs) {
	(void)dstRgba;
	(void)srcPrevRgba;
	(void)w;
	(void)h;
	(void)mvs;
}
