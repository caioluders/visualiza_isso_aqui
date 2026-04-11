extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/motion_vector.h>
}

#include <chrono>
#include <cmath>

#include "FFglitchPlayer.h"

FFglitchPlayer::FFglitchPlayer()
	: running(false), width(640), height(360), textureId(0) {}

FFglitchPlayer::~FFglitchPlayer() {
	stop();
	if (textureId) { glDeleteTextures(1, &textureId); textureId = 0; }
}

void FFglitchPlayer::setInput(const std::string& inputPath) { input = inputPath; }
void FFglitchPlayer::setScript(const std::string& scriptPath) { script = scriptPath; }

bool FFglitchPlayer::start() {
	if (running.load()) return true;
	if (worker.joinable()) worker.join();
	running = true;
	worker = std::thread(&FFglitchPlayer::decodingLoop, this);
	return true;
}

void FFglitchPlayer::stop() {
	running = false;
	if (worker.joinable()) worker.join();
}

void FFglitchPlayer::ensureTexture() {
	if (!textureId) {
		glGenTextures(1, &textureId);
		glBindTexture(GL_TEXTURE_2D, textureId);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
}

void FFglitchPlayer::update(float /*timeSeconds*/) {
	if (!running.load()) return;
	ensureTexture();
	std::vector<unsigned char> local;
	std::vector<SimpleMV> localMvs;
	std::vector<unsigned char> localPrevFront;
	uint64_t localFrameSerial = 0;
	bool resetAccumulator = false;
	{
		std::lock_guard<std::mutex> lk(bufferMutex);
		if (frameSerial == uploadedFrameSerial) return;
		local = frontBuffer;
		localMvs = frontMvs;
		localPrevFront = prevFrontBuffer;
		localFrameSerial = frameSerial;
		resetAccumulator = resetGlitchAccumulator;
		resetGlitchAccumulator = false;
	}
	if (local.empty()) return;
	if (resetAccumulator) prevGlitchedBuffer.clear();
	// Apply CPU-side motion-based sink effect if enabled
	if (glitchIntensity > 0.001f && !localMvs.empty()) {
		const std::vector<uint8_t>& srcAccum = !prevGlitchedBuffer.empty() ? prevGlitchedBuffer : localPrevFront;
		if (!srcAccum.empty()) {
			applyMotionSink(local, srcAccum, width, height, localMvs);
			prevGlitchedBuffer = local; // accumulate trails
		}
	}
	glBindTexture(GL_TEXTURE_2D, textureId);
	if (textureWidth != width || textureHeight != height) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, local.data());
		textureWidth = width;
		textureHeight = height;
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, local.data());
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	uploadedFrameSerial = localFrameSerial;
}

void FFglitchPlayer::decodingLoop() {
	// Minimal decoding pipeline with libav*, with basic guards
	// PTS sync state (persistent across frames within a run)
	static bool syncFirstFrame = true;
	static double syncStartPtsSec = 0.0;
	static std::chrono::steady_clock::time_point syncWallStart;
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, input.c_str(), nullptr, nullptr) < 0) { running=false; return; }
	if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); running=false; return; }
	int vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vstream < 0) { avformat_close_input(&fmt); running=false; return; }
	AVStream *st = fmt->streams[vstream];
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec) { avformat_close_input(&fmt); running=false; return; }
	AVCodecContext *dec = avcodec_alloc_context3(codec);
	if (!dec) { avformat_close_input(&fmt); running=false; return; }
	if (avcodec_parameters_to_context(dec, st->codecpar) < 0) { avcodec_free_context(&dec); avformat_close_input(&fmt); running=false; return; }
	// Ask decoder to export motion vectors as side data when available
	dec->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS;
	if (avcodec_open2(dec, codec, nullptr) < 0) { avcodec_free_context(&dec); avformat_close_input(&fmt); running=false; return; }
	width = dec->width > 0 ? dec->width : width;
	height = dec->height > 0 ? dec->height : height;

	SwsContext *sws = sws_getContext(width, height, dec->pix_fmt, width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!sws) { avcodec_free_context(&dec); avformat_close_input(&fmt); running=false; return; }

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frm = av_frame_alloc();
	AVFrame *rgb = av_frame_alloc();
	int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, width, height, 1);
	std::vector<uint8_t> rgbbuf(buf_size);
	av_image_fill_arrays(rgb->data, rgb->linesize, rgbbuf.data(), AV_PIX_FMT_RGBA, width, height, 1);

    while (running.load()) {
        if (av_read_frame(fmt, pkt) < 0) {
            // EOF or error; loop if requested
            if (loopPlayback) {
                if (av_seek_frame(fmt, vstream, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
					avcodec_flush_buffers(dec);
					// reset simple wall-clock sync and accumulation on loop
					syncFirstFrame = true;
					{
						std::lock_guard<std::mutex> lk(bufferMutex);
						prevFrontBuffer.clear();
						resetGlitchAccumulator = true;
					}
					continue;
				}
            }
            break;
        }
		if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }
		if (avcodec_send_packet(dec, pkt) == 0) {
			while (avcodec_receive_frame(dec, frm) == 0) {
				// Drop I-frames to encourage datamosh trails
				if (frm->pict_type == AV_PICTURE_TYPE_I) {
					continue;
				}
				sws_scale(sws, frm->data, frm->linesize, 0, height, rgb->data, rgb->linesize);
				// Extract motion vectors if present
				std::vector<SimpleMV> mvs;
				if (AVFrameSideData* sd = av_frame_get_side_data(frm, AV_FRAME_DATA_MOTION_VECTORS)) {
					const AVMotionVector* mv = reinterpret_cast<const AVMotionVector*>(sd->data);
					int mvcount = sd->size / (int)sizeof(AVMotionVector);
					mvs.reserve((size_t)mvcount);
					for (int i = 0; i < mvcount; ++i) {
						SimpleMV smv;
						smv.srcX = mv[i].src_x;
						smv.srcY = mv[i].src_y;
						smv.dstX = mv[i].dst_x;
						smv.dstY = mv[i].dst_y;
						smv.w = mv[i].w;
						smv.h = mv[i].h;
						smv.source = mv[i].source; // 0 backward, 1 forward
						smv.motionX = mv[i].motion_x;
						smv.motionY = mv[i].motion_y;
						smv.motionScale = mv[i].motion_scale;
						mvs.push_back(smv);
					}
				}
				// Minimal MV debug: print stats every ~30 decoded frames
				{
					static unsigned long long dbgFrame = 0;
					++dbgFrame;
					if ((dbgFrame % 30ull) == 0ull) {
						int total = (int)mvs.size();
						int fwd = 0;
						double sumLen = 0.0;
						for (const auto& v : mvs) {
							if (v.source == 1) ++fwd;
							double dx = (double)v.motionX / (double)(1 << v.motionScale);
							double dy = (double)v.motionY / (double)(1 << v.motionScale);
							sumLen += std::hypot(dx, dy);
						}
						double avg = (total > 0) ? (sumLen / (double)total) : 0.0;
						if (total > 0) {
							const auto& s = mvs[0];
							double sdx = (double)s.motionX / (double)(1 << s.motionScale);
							double sdy = (double)s.motionY / (double)(1 << s.motionScale);
							std::fprintf(stderr,
								"[MV] frame=%llu total=%d forward=%d avgPix=%.2f sample dst=(%d,%d) size=(%d,%d) mv=(%.2f,%.2f) src=%d\n",
								(unsigned long long)dbgFrame, total, fwd, avg,
								s.dstX, s.dstY, s.w, s.h, sdx, sdy, s.source);
						} else {
							std::fprintf(stderr, "[MV] frame=%llu total=0\n", (unsigned long long)dbgFrame);
						}
					}
				}
				// Simple wall-clock sync using PTS to avoid accelerated playback
				double tb = av_q2d(st->time_base);
				int64_t pts = (frm->best_effort_timestamp != AV_NOPTS_VALUE) ? frm->best_effort_timestamp : frm->pts;
				if (pts == AV_NOPTS_VALUE) pts = 0;
				double tSec = pts * tb;
				if (syncFirstFrame) {
					syncFirstFrame = false;
					syncStartPtsSec = tSec;
					syncWallStart = std::chrono::steady_clock::now();
				} else {
					double dt = tSec - syncStartPtsSec;
					auto target = syncWallStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(dt));
					auto now = std::chrono::steady_clock::now();
					if (target > now) {
						std::this_thread::sleep_for(target - now);
					}
				}
				{
					std::lock_guard<std::mutex> lk(bufferMutex);
					backBuffer.resize((size_t)width * height * 4);
					for (int y = 0; y < height; ++y) {
						memcpy(backBuffer.data() + (size_t)y * width * 4, rgbbuf.data() + (size_t)y * rgb->linesize[0], (size_t)width * 4);
					}
					backMvs.swap(mvs);
					// keep previous front before swapping
					prevFrontBuffer = frontBuffer;
					frontBuffer.swap(backBuffer);
					frontMvs.swap(backMvs);
					++frameSerial;
				}
			}
		}
		av_packet_unref(pkt);
	}

	av_frame_free(&rgb);
	av_frame_free(&frm);
	av_packet_free(&pkt);
	sws_freeContext(sws);
	avcodec_free_context(&dec);
	avformat_close_input(&fmt);
	running = false;
}

void FFglitchPlayer::applyMotionSink(std::vector<uint8_t>& dstRgba, const std::vector<uint8_t>& srcPrevRgba, int w, int h, const std::vector<FFglitchPlayer::SimpleMV>& mvs) {
    if (mvs.empty()) return;
    if ((int)srcPrevRgba.size() < w * h * 4) return;
    // Datamosh-like: sample from previous glitched frame, negate MVs, bilinear subpixel for smoother trails
    bool hasForward = false;
    for (const auto& mv : mvs) { if (mv.source == 1) { hasForward = true; break; } }
    for (const auto& mv : mvs) {
        if (hasForward && mv.source != 1) continue;
        if (mv.w <= 0 || mv.h <= 0) continue;
        int bx0 = std::max(0, mv.dstX);
        int by0 = std::max(0, mv.dstY);
        int bx1 = std::min(w, mv.dstX + mv.w);
        int by1 = std::min(h, mv.dstY + mv.h);
        if (bx0 >= bx1 || by0 >= by1) continue;
        // Convert fixed-point MV to subpixel shift
        float gain = 1.0f + glitchIntensity * 6.0f; // motion gain
        float dx = (-(float)mv.motionX / (float)(1 << mv.motionScale)) * gain;
        float dy = (-(float)mv.motionY / (float)(1 << mv.motionScale)) * gain;
        if (std::abs(dx) < 1e-6f && std::abs(dy) < 1e-6f) continue;
        int bw = bx1 - bx0;
        int bh = by1 - by0;
        for (int y = by0; y < by1; ++y) {
            for (int x = bx0; x < bx1; ++x) {
                float fsx = (float)x + dx;
                float fsy = (float)y + dy;
                fsx = std::max(0.0f, std::min((float)(w - 1), fsx));
                fsy = std::max(0.0f, std::min((float)(h - 1), fsy));
                int x0 = (int)std::floor(fsx);
                int y0 = (int)std::floor(fsy);
                int x1 = std::min(w - 1, x0 + 1);
                int y1 = std::min(h - 1, y0 + 1);
                float tx = fsx - (float)x0;
                float ty = fsy - (float)y0;
                size_t i00 = ((size_t)y0 * w + x0) * 4;
                size_t i10 = ((size_t)y0 * w + x1) * 4;
                size_t i01 = ((size_t)y1 * w + x0) * 4;
                size_t i11 = ((size_t)y1 * w + x1) * 4;
                size_t di = ((size_t)y * w + x) * 4;
                // Feather near block edges to reduce visible squares
                float fx = (float)(x - bx0) / (float)std::max(1, bw);
                float fy = (float)(y - by0) / (float)std::max(1, bh);
                float edge = std::min(std::min(fx, 1.0f - fx), std::min(fy, 1.0f - fy));
                float alpha = std::min(1.0f, std::max(0.0f, edge * 4.0f)); // 0 at border, ~1 inside
                for (int c = 0; c < 4; ++c) {
                    float v0 = (1.0f - tx) * (float)srcPrevRgba[i00 + c] + tx * (float)srcPrevRgba[i10 + c];
                    float v1 = (1.0f - tx) * (float)srcPrevRgba[i01 + c] + tx * (float)srcPrevRgba[i11 + c];
                    float v = (1.0f - ty) * v0 + ty * v1;
                    float base = (float)dstRgba[di + c];
                    float out = v * alpha + base * (1.0f - alpha);
                    dstRgba[di + c] = (uint8_t)std::max(0, std::min(255, (int)(out + 0.5f)));
                }
            }
        }
    }
}
