// audio-decoder.h - FFmpeg audio decoder + resampler + SoundTouch time-stretching
#pragma once

#include "core/common.h"
#include "media/frame-queue.h"
#include "media/audio-time-stretcher.h"

#include <atomic>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

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

  // Set playback tempo for time-stretching (1.0 = passthrough).
  // Called from worker; applied lazily inside decode() on the same thread.
  // Clamped to [0.5, 4.0] (SoundTouch's range).
  void set_tempo(double tempo);
  double current_tempo() const { return current_tempo_; }

  // Get reference to the AudioTimeStretcher for orchestrator
  AudioTimeStretcher& get_stretcher() { return stretcher_; }

  // Drain remaining samples from the time-stretcher into the queue.
  // Call before transitioning back to passthrough so SoundTouch's
  // internal buffer doesn't vanish as a gap/pop.
  void drain_stretcher(AudioFrameQueue &queue);

private:
  void push_resampled(AVFrame *frame, AudioFrameQueue &queue);
  bool apply_pending_tempo(AudioFrameQueue &queue);

  AVCodecContext *codec_ctx_ = nullptr;
  SwrContext *swr_ctx_ = nullptr;
  int out_rate_ = 48000;
  int out_ch_ = 2;
  // Last frame pushed to the queue (its pts + sample count). Used by
  // drain_stretcher() to give the flushed tail samples a PTS that lands
  // contiguously after the last real frame instead of using wall-clock,
  // which would be interpreted as stream-PTS downstream and place the
  // flushed audio at a wildly wrong OBS timestamp.
  int64_t last_pushed_pts_us_ = 0;
  uint32_t last_pushed_frames_ = 0;
  int consecutive_errors_ = 0;
  static constexpr int ERROR_THRESHOLD = 3;
  AVRational stream_tb_ = {1, 1000}; // stream time base, default to ms

  // AudioTimeStretcher for professional time-stretching
  AudioTimeStretcher stretcher_;
  std::atomic<double> pending_tempo_{1.0};
  double current_tempo_ = 1.0;
  bool stretcher_initialized_ = false;

  
  // Fallback to atempo filter if SoundTouch fails
  bool use_fallback_atempo_ = false;
  AVFilterGraph *filter_graph_ = nullptr;
  AVFilterContext *filter_src_ = nullptr;
  AVFilterContext *filter_sink_ = nullptr;
  int build_filter_graph(double tempo);
  void teardown_filter_graph();
  void drain_filter(AudioFrameQueue &queue);
};

} // namespace lss
