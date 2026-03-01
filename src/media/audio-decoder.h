// audio-decoder.h - FFmpeg audio decoder + resampler
#pragma once

#include "core/common.h"
#include "media/frame-queue.h"

struct SwrContext;

namespace lss {

class AudioDecoder {
public:
  AudioDecoder() = default;
  ~AudioDecoder();

  AudioDecoder(const AudioDecoder &) = delete;
  AudioDecoder &operator=(const AudioDecoder &) = delete;

  int init(const AVCodecParameters *par);
  void set_stream_time_base(AVRational tb) { stream_tb_ = tb; }
  int decode(AVPacket *pkt, AudioFrameQueue &queue);
  void flush(AudioFrameQueue &queue);
  void close();
  void reset_state();

private:
  void push_resampled(AVFrame *frame, AudioFrameQueue &queue);

  AVCodecContext *codec_ctx_ = nullptr;
  SwrContext *swr_ctx_ = nullptr;
  int out_rate_ = 48000;
  int out_ch_ = 2;
  int consecutive_errors_ = 0;
  static constexpr int ERROR_THRESHOLD = 3;
  AVRational stream_tb_ = {1, 1000}; // stream time base, default to ms
};

} // namespace lss
