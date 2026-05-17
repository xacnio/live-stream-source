// live-stream-source.h
#pragma once

#include "core/common.h"
#include "core/catchup-orchestrator.h"
#include "media/audio-decoder.h"
#include "media/frame-queue.h"
#include "utils/bitrate-monitor.h"
#include "utils/buffer-manager.h"
#include "utils/tempo-ramper.h"

#include "media/stream-demuxer.h"
#include "media/video-decoder.h"
#include "network/preview-encoder.h"
#include "network/ws-stats-server.h"
#include "protocols/whep/whep-client.h"
#include "utils/reconnect-manager.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace lss {

class LiveStreamSource {
public:
  LiveStreamSource(obs_source_t *source, obs_data_t *settings);
  ~LiveStreamSource();

  LiveStreamSource(const LiveStreamSource &) = delete;
  LiveStreamSource &operator=(const LiveStreamSource &) = delete;

  // OBS source callbacks
  void update(obs_data_t *settings);
  void activate();
  void deactivate();

  // Static dispatchers
  static void *create(obs_data_t *settings, obs_source_t *source);
  static void destroy(void *data);
  static void get_defaults(obs_data_t *settings);
  static obs_properties_t *get_properties(void *data);
  static void s_update(void *data, obs_data_t *settings);
  static void s_activate(void *data);
  static void s_deactivate(void *data);
  static void s_video_tick(void *data, float seconds);
  static const char *s_get_name(void *);

private:
  // Stream lifecycle
  void start_stream();
  void stop_stream();
  bool try_connect();

  // Worker thread
  void worker_thread_func();
  void whep_worker();
  void shimmer_thread_func();

  // Frame output
  void video_tick(float seconds);
  void output_video_frame(AVFrame *frame);
  void output_audio_frame(DecodedAudioFrame &af);
  void output_shimmer_frame();
  void apply_transition_overlay(obs_source_frame2 &obs_frame,
                                video_format obs_fmt, AVFrame *frame,
                                double alpha);

  // Soft-freeze recovery: skip-via-reanchor (legacy fallback for >4s gaps)
  // gap_ms: source of trigger (0 = manual). set_cooldown: when true,
  // sets the 15s cooldown that silently drops packets — wanted for
  // automatic gap-detected skips (prevents teleport loops on a flaky
  // network) but NOT for the manual "Skip to Live Edge" button (the
  // user wants instant resume).
  void handle_freeze_skip_recovery(int64_t gap_ms, bool set_cooldown = true);

  // Source visibility toggling
  void toggle_source_visibility(const std::string &name, bool visible);
  void update_source_toggles();

  // Statistics
  void init_stats_dir();
  void write_stats_json();
  void install_stats_html();
  void install_overlay_html();
  static bool on_open_stats_clicked(obs_properties_t *props,
                                    obs_property_t *prop, void *data);
  static bool on_add_overlay_horz_clicked(obs_properties_t *props,
                                          obs_property_t *prop, void *data);
  static bool on_add_overlay_vert_clicked(obs_properties_t *props,
                                          obs_property_t *prop, void *data);
  static bool on_show_window_stats_clicked(obs_properties_t *props,
                                           obs_property_t *prop, void *data);
  static bool on_refresh_clicked(obs_properties_t *props, obs_property_t *prop,
                                 void *data);
  static bool on_skip_to_live_clicked(obs_properties_t *props,
                                      obs_property_t *prop, void *data);

  void handle_remote_command(const std::string &cmd);

  static bool s_stream_type_modified(obs_properties_t *props,
                                     obs_property_t *prop,
                                     obs_data_t *settings);
  static void populate_source_list(obs_property_t *list,
                                   obs_source_t *self_source);

  obs_source_t *obs_source_ = nullptr;

  // Configuration
  std::string url_;
  int low_bitrate_kbps_ = DEFAULT_LOW_BITRATE_KBPS;
  bool auto_catchup_ = true;
  bool hw_decode_ = false;
  bool show_shimmer_ = true;
  StreamType stream_type_ = StreamType::Standard;
  std::string low_bitrate_src_name_;
  std::string disconnect_src_name_;
  std::string loading_src_name_;
  std::string whep_token_;

  // Components
  StreamDemuxer demuxer_;
  VideoDecoder video_dec_;
  AudioDecoder audio_dec_;
  std::unique_ptr<VideoFrameQueue> video_queue_;
  std::unique_ptr<AudioFrameQueue> audio_queue_;
  BitrateMonitor bitrate_mon_;
  ReconnectManager reconnect_mgr_;

  // Professional catch-up system components
  BufferManager buffer_manager_;
  TempoRamper tempo_ramper_;
  std::unique_ptr<CatchupOrchestrator> orchestrator_;

  // Worker thread
  std::thread worker_thread_;
  std::thread connect_thread_;
  std::thread shimmer_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connection_in_progress_{false};


  std::atomic<bool> prev_low_bitrate_{false};
  std::atomic<bool> prev_disconnected_{true};
  std::atomic<bool> prev_loading_{false};
  std::atomic<int64_t> last_low_bitrate_time_ms_{0};
  std::atomic<int64_t> last_connect_ms_{0};

  // Statistics
  std::atomic<int64_t> total_frames_decoded_{0};
  std::atomic<int64_t> total_frames_dropped_{0};
  int64_t stats_last_write_ms_ = 0;
  TimePoint stream_start_time_;
  std::string stats_dir_;
  std::string stats_json_path_;
  std::string stats_html_path_;
  std::string overlay_html_path_;

  std::string video_codec_name_;
  std::string audio_codec_name_;

  std::atomic<int> width_{0};
  std::atomic<int> height_{0};
  // Cached OBS color-matrix params — recomputed only when frame
  // dimensions / format / range change. Saves a per-frame OBS API call
  // (video_format_get_parameters_for_format) at 60fps.
  int cached_color_height_ = 0;
  int cached_color_format_ = 0;
  int cached_color_range_ = 0;
  float cached_color_matrix_[16] = {0};
  float cached_color_range_min_[3] = {0};
  float cached_color_range_max_[3] = {0};
  bool cached_color_valid_ = false;
  std::atomic<bool> active_{false};
  std::atomic<bool> pending_refresh_{false};
  std::atomic<bool> pending_skip_to_live_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> first_frame_received_{false};
  std::atomic<bool> ever_received_frame_{false};
  std::atomic<int> whep_mode_{0}; // Stored as int, cast to WhepClient::WhepMode
  std::atomic<int> reconnect_count_{0};
  std::atomic<double> current_fps_{0.0};
  int64_t last_fps_calc_ms_ = 0;
  int64_t last_shimmer_ms_ = 0;
  int64_t last_frames_count_ = 0;
  int64_t last_pkt_recv_ms_ = 0;

  // Tracks the time we start waiting for the network buffer (EAGAIN loop).
  // Used to accurately measure genuine network freezes without including
  // processing or pacing sleep time.
  int64_t empty_buffer_start_ms_ = 0;

  // RTMP extreme skip: used ONLY for gaps > 30 seconds to drop the burst
  bool rtmp_skip_to_live_ = false;
  int64_t skip_start_ms_ = 0;
  double skip_rate_ema_ = 5.0;
  int64_t skip_prev_pts_ms_ = 0;
  int64_t skip_prev_wall_ms_ = 0;

  int64_t pts_to_obs_offset_ns_ = 0;
  bool has_pts_offset_ = false;
  int64_t first_video_pts_ns_ = 0;
  int64_t first_audio_pts_ns_ = 0;

  bool catching_up_ = false;
  int64_t catchup_first_wall_ms_ = 0;
  int64_t catchup_first_pts_ms_ = 0;

  // State used by the tempo-compression path in output_video_frame and the
  // freeze-recovery cooldown that suppresses catchup re-entry briefly after
  // a skip-resync. Other catchup tracking lives in CatchupOrchestrator.
  int64_t catchup_prev_video_pts_ns_ = 0;
  int64_t catchup_video_output_clock_ns_ = 0;
  int64_t catchup_cooldown_until_ms_ = 0;

  std::atomic<int64_t> total_frames_dropped_decode_{0};
  std::atomic<int64_t> total_frames_dropped_latency_{0};
  std::atomic<int64_t> last_video_pts_us_{0};
  std::atomic<int64_t> last_audio_pts_us_{0};
  std::atomic<int64_t> total_bytes_audio_{0};
  std::atomic<int64_t> total_bytes_video_{0};
  std::atomic<int64_t> last_pkt_recv_us_{0};
  std::atomic<int64_t> pipeline_latency_ms_{0};

  int64_t delay_ref_wall_ms_ = 0;
  int64_t delay_ref_pts_ms_ = 0;
  bool has_delay_ref_ = false;
  std::atomic<int64_t> stream_delay_ms_{0};

  std::vector<uint8_t> shimmer_buffer_; // Reusable buffer for animation

  // Dashboard live preview & remote controls. The encoder is idle whenever
  // no websocket client is connected; the mute/blur flags are flipped by
  // JSON commands coming in from the dashboard.
  PreviewEncoder preview_encoder_;
  std::vector<uint8_t> preview_video_buf_;
  std::vector<uint8_t> preview_audio_buf_;
  std::atomic<bool> dashboard_muted_{false};
  std::atomic<bool> dashboard_blurred_{false};
  // Per-stream preview gates — flipped by the dashboard's Play / Audio
  // toggles. Default OFF so opening the page doesn't immediately burn
  // bandwidth and CPU encoding a feed nobody is watching yet.
  std::atomic<bool> preview_video_enabled_{false};
  std::atomic<bool> preview_audio_enabled_{false};
  // Blur via 1/8 scale-down + scale-up (cheap swscale, no custom kernel).
  struct SwsContext *blur_sws_down_ = nullptr;
  struct SwsContext *blur_sws_up_   = nullptr;
  int blur_cached_w_   = 0;
  int blur_cached_h_   = 0;
  int blur_cached_fmt_ = -1; // AVPixelFormat
  std::vector<uint8_t> blur_small_buf_; // tiny intermediate I420 frame
  void handle_dashboard_command(const std::string &json_cmd);
  void send_preview_video(const AVFrame *frame);
  void send_preview_audio(const DecodedAudioFrame &af);
  void apply_blur_inplace(obs_source_frame2 &obs_frame, video_format fmt,
                          AVFrame *frame);
  void draw_mute_icon(obs_source_frame2 &obs_frame, video_format fmt,
                      AVFrame *frame);

  // Shimmer → live transition: when the first frame of a stream cycle
  // arrives, overlay decaying TV-static noise on the video for ~700ms so
  // the shimmer dissolves into the picture instead of cutting hard.
  int64_t first_frame_wall_ms_ = 0;            // 0 = not in transition
  std::vector<uint8_t> trans_y_buf_;
  std::vector<uint8_t> trans_u_buf_;
  std::vector<uint8_t> trans_v_buf_;
  static constexpr int64_t TRANSITION_FADE_MS = 700;

  // Live → shimmer fade-out: cache of the most recent live frame so that
  // when the stream drops, the shimmer thread can overlay rising noise on
  // top of it (mirror of the fade-in) instead of hard-cutting to shimmer.
  std::mutex last_frame_mutex_;
  std::vector<uint8_t> last_y_buf_, last_u_buf_, last_v_buf_;
  video_format last_frame_fmt_ = VIDEO_FORMAT_NONE;
  int  last_frame_w_ = 0, last_frame_h_ = 0;
  int  last_frame_ly_ = 0, last_frame_lu_ = 0, last_frame_lv_ = 0;
  bool last_frame_full_range_ = false;
  bool has_last_frame_ = false;
  std::atomic<int64_t> fade_out_start_ms_{0};        // 0 = not active
  static constexpr int64_t FADEOUT_DURATION_MS = 700;

  void cache_last_frame(AVFrame *frame, video_format fmt, bool full_range);
  void apply_noise_overlay_raw(obs_source_frame2 &obs_frame, video_format fmt,
                               int w, int h,
                               const uint8_t *src_y, int ly,
                               const uint8_t *src_u, int lu,
                               const uint8_t *src_v, int lv,
                               double alpha);
  void output_fadeout_frame(double alpha);

  // Burst-skip: packet-level discard for large drops (>2s).
  // Worker reads and discards packets until burst_skip_target_pts_ms_ is reached,
  // then reanchors at the live edge. Gives <1s recovery for 2-12s drops.
  bool burst_skip_active_ = false;
  int64_t burst_skip_target_pts_ms_ = 0; // Target PTS (live edge estimate)
  int64_t burst_skip_start_ms_ = 0;      // Wall time when burst_skip started (timeout guard)

  uint32_t last_video_ts_ = 0;
  uint64_t video_pts_ns_ = 0;
  uint64_t video_base_pts_ns_ = 0;
  uint32_t video_start_pts_rtp_ = 0;
  uint64_t audio_base_pts_ns_ = 0;
  uint32_t audio_start_pts_rtp_ = 0;
  uint64_t dec_audio_samples_ = 0;
  std::atomic<int64_t> whep_last_media_ms_{
      0}; // For timeout-based disconnect detection
  std::atomic<int64_t> whep_last_video_packet_ms_{
      0}; // For detecting audio-only mode
  
};

extern struct obs_source_info live_stream_source_info;
void register_live_stream_source();

} // namespace lss
