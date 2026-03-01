// video-decoder.h - FFmpeg video decoder with optional HW accel
#pragma once

#include "core/common.h"
#include <functional>

namespace lss {

class VideoDecoder {
public:
  VideoDecoder() = default;
  ~VideoDecoder();

  VideoDecoder(const VideoDecoder &) = delete;
  VideoDecoder &operator=(const VideoDecoder &) = delete;

  int init(const AVCodecParameters *par, bool hw_accel = false);
  int decode(AVPacket *pkt, std::function<void(AVFrame *)> callback);
  void flush(std::function<void(AVFrame *)> callback);
  void close();
  void reset_state();
  int width() const { return width_; }
  int height() const { return height_; }
  void set_stream_time_base(AVRational tb) { stream_tb_ = tb; }
  bool is_hw_active() const { return hw_device_ctx_ != nullptr; }

private:
  int init_hw_decoder(const AVCodec *codec, const AVCodecParameters *par);

  AVCodecContext *codec_ctx_ = nullptr;
  SwsContext *sws_ctx_ = nullptr;
  AVFrame *tmp_frame_ = nullptr; // for sws conversion
  AVFrame *hw_frame_ = nullptr;  // for hw->sw transfer
  AVBufferRef *hw_device_ctx_ = nullptr;
  AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;
  int width_ = 0;
  int height_ = 0;
  AVRational stream_tb_ = {0, 1};
  bool hw_accel_requested_ = false;

  int consecutive_errors_ = 0;
  static constexpr int ERROR_THRESHOLD = 30;
};

} // namespace lss
