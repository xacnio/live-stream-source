// preview-encoder.h
// Lightweight downscale+JPEG video encoder and PCM audio resampler used
// by the dashboard websocket to deliver a low-bitrate live preview to the
// browser. Both paths are no-ops when no client is connected, so the
// per-frame cost is essentially zero in the idle case.

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

struct AVCodecContext;
struct SwsContext;
struct SwrContext;

namespace lss {

class PreviewEncoder {
public:
  PreviewEncoder() = default;
  ~PreviewEncoder();

  PreviewEncoder(const PreviewEncoder &) = delete;
  PreviewEncoder &operator=(const PreviewEncoder &) = delete;

  // Try to encode this decoded video frame as a downscaled JPEG. Returns
  // true if a fresh JPEG was produced (and pushed to `out`), false if
  // throttled or on failure. `out` is *appended to* and starts with the
  // 1-byte type tag (0x01 = video) so the websocket can demux at the
  // receiving end without an extra copy.
  bool encode_video_jpeg(const AVFrame *frame, std::vector<uint8_t> &out);

  // Resample a decoded audio frame to a fixed 16 kHz mono Float32 stream.
  // Appends to `out` prefixed with the 1-byte type tag (0x02 = audio).
  // No throttling — every audio chunk is sent so monitoring stays
  // continuous.
  bool encode_audio_pcm(const uint8_t *const *src_data,
                        int src_samples, int src_rate, int src_channels,
                        int src_sample_fmt,
                        std::vector<uint8_t> &out);

  void close();

  // Tuning knobs (compile-time defaults; can be overridden later).
  static constexpr int PREVIEW_WIDTH  = 854;   // 480p widescreen
  static constexpr int PREVIEW_HEIGHT = 480;
  static constexpr int PREVIEW_FPS    = 10;    // throttle to 10 fps
  static constexpr int PREVIEW_JPEG_QUALITY = 6; // libavcodec qscale (2=best, 31=worst)
  static constexpr int AUDIO_OUT_RATE = 16000; // mono Float32
  static constexpr int AUDIO_OUT_CH   = 1;

private:
  // Video state
  AVCodecContext *jpeg_ctx_ = nullptr;
  SwsContext     *sws_ctx_  = nullptr;
  int sws_in_w_ = 0, sws_in_h_ = 0;
  AVPixelFormat sws_in_fmt_ = AV_PIX_FMT_NONE;
  AVFrame *scaled_frame_ = nullptr;
  int64_t  last_video_ms_ = 0;

  // Audio state
  SwrContext *swr_ctx_ = nullptr;
  int swr_in_rate_ = 0, swr_in_ch_ = 0, swr_in_fmt_ = 0;

  bool ensure_video_ctx(int src_w, int src_h, AVPixelFormat src_fmt);
  bool ensure_audio_ctx(int src_rate, int src_ch, int src_sample_fmt);
};

} // namespace lss
