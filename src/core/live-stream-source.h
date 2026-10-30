// live-stream-source.h
#pragma once

#include "core/common.h"
#include "media/audio-decoder.h"
#include "media/frame-queue.h"
#include "playout/packet-buffer.h"
#include "playout/playout-clock.h"
#include "playout/recovery-policy.h"
#include "playout/sync-registry.h"
#include "utils/bitrate-monitor.h"

#include "media/stream-demuxer.h"
#include "media/video-decoder.h"
#include "core/fallback-layer.h"
#include "media/privacy-blur.h"
#include "network/preview-encoder.h"
#include "network/ws-stats-server.h"
#include "protocols/whep/whep-client.h"
#include "utils/reconnect-manager.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace lss {

// Debounced on/off state for an overlay source. Callers serialize writes on
// toggle_mutex_; atomics only so the state is readable from other threads.
struct OverlayDebouncer {
  std::atomic<bool> raw{false};           // last observed condition
  std::atomic<int64_t> raw_since_ms{0};   // when `raw` last changed
  std::atomic<bool> shown{false};         // debounced output
  std::atomic<int64_t> shown_since_ms{0}; // when `shown` last changed

  void reset(bool state, int64_t now) {
    raw.store(state);
    raw_since_ms.store(now);
    shown.store(state);
    shown_since_ms.store(now);
  }

  // Feeds the raw condition, returns the debounced state. `cond` must hold
  // show_delay_ms to switch on; "on" is held min_visible_ms before going off.
  bool step(bool cond, int64_t now, int64_t show_delay_ms,
            int64_t min_visible_ms) {
    if (cond != raw.load()) {
      raw.store(cond);
      raw_since_ms.store(now);
    }

    const bool cur = shown.load();
    if (cond == cur)
      return cur;

    if (cond) {
      if ((now - raw_since_ms.load()) < show_delay_ms)
        return cur; // blip — too short to bother showing
    } else if ((now - shown_since_ms.load()) < min_visible_ms) {
      return cur; // recovered, but hold the minimum
    }

    shown.store(cond);
    shown_since_ms.store(now);
    return cond;
  }
};

class LiveStreamSource {
public:
  FallbackLayer &fallback_layer() { return fallback_layer_; }

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
  void overlay_thread_func();

  // Frame output
  void video_tick(float seconds);
  void output_video_frame(AVFrame *frame);
  void output_audio_frame(DecodedAudioFrame &af);

  void submit_video_frame(AVFrame *frame, uint64_t timestamp_ns);
  void apply_cut_crossfade(obs_source_frame2 &obs_frame, video_format obs_fmt,
                           AVFrame *frame, double alpha);

  // Playout engine (Standard streams): the worker only reads the socket, the
  // decode thread paces packets against playout_clock_, and the present thread
  // hands frames to OBS on time. See live-stream-playout.cpp.
  // Fixed for the life of a connection: stream_type_ changes as soon as the
  // user edits it, while the decoders stay in use until the refresh happens.
  bool uses_playout() const { return playout_enabled_.load(); }
  void ingest_packet(AVPacket *pkt);
  void playout_reset();
  void playout_decode_thread_func();
  void playout_present_thread_func();
  void playout_restart_state();
  void playout_update_metrics(int64_t now_us);
  bool playout_step_buffering(int64_t now_us);
  bool playout_step_playing(int64_t now_us);
  void playout_run_policy(int64_t now_us);
  void playout_update_sync(int64_t now_us, int64_t latency_us);
  void playout_apply_shift(int64_t now_us, int64_t latency_us, int64_t min_target_us,
                           const SyncConfig &config);
  void playout_land(int64_t now_us, int64_t missing_us, int tolerance_ms);
  void announce_move(int64_t now_us, int64_t by_us);
  void playout_update_listen(int64_t now_us);
  void apply_sync_offset(int64_t delta_us, const SyncConfig &config, int64_t now_us);
  void apply_main_delay_request(int64_t now_us);
  void publish_sync_progress();
  void finish_sync_listen(SyncListenState result);
  void request_sync_listen();
  void apply_fallback_settings(obs_data_t *settings);
  void playout_enter_buffering(int64_t now_us);
  void playout_hold(int64_t now_us);
  void playout_pause(int64_t now_us);
  void playout_cut(int64_t now_us, const CutPoint *cut, int64_t target_us);
  void playout_on_video_frame(AVFrame *frame);
  void playout_feed_video(int64_t now_us);
  void playout_feed_audio();
  void playout_drain_audio();
  void playout_emit_audio(DecodedAudioFrame &af);
  void playout_start_audio_timeline(int64_t wall_us, int sample_rate);
  void playout_write_audio(const float *left, const float *right, uint32_t frames);
  void playout_pad_audio();
  int64_t playout_decode_lead_us() const;
  void open_decoders();
  void request_skip_to_live();
  void update_stream_delay(int64_t dts_ms);
  void update_periodic_stats();

  // Soft-freeze recovery: skip-via-reanchor (legacy fallback for >10s gaps)
  // gap_ms: source of trigger. Sets a cooldown that prevents teleport loops
  // on a flaky network.
  void handle_freeze_skip_recovery(int64_t gap_ms);

  // Source visibility toggling
  void toggle_source_visibility(const std::string &name, bool visible);
  void update_source_toggles();
  bool fallback_covers_picture() const;
  void drop_picture();

  // Statistics
  void init_stats_dir();
  void write_stats_json();
  static bool on_open_stats_clicked(obs_properties_t *props,
                                    obs_property_t *prop, void *data);
  static bool on_add_overlay_horz_clicked(obs_properties_t *props,
                                          obs_property_t *prop, void *data);
  static bool on_add_overlay_vert_clicked(obs_properties_t *props,
                                          obs_property_t *prop, void *data);
  static bool on_fallback_settings_clicked(obs_properties_t *props, obs_property_t *property,
                                           void *data);
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
  std::vector<std::string> scene_source_names() const;

  obs_source_t *obs_source_ = nullptr;

  // Configuration
  std::string url_;
  int low_bitrate_kbps_ = DEFAULT_LOW_BITRATE_KBPS;
  bool auto_catchup_ = true;
  bool hw_decode_ = false;
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

  // Playout engine state. Everything in PlayoutRun belongs to the decode
  // thread; the atomics below it are read by stats and the overlay logic.
  enum class PlayoutPhase { Buffering, Playing };
  struct PlayoutRun {
    PlayoutPhase phase = PlayoutPhase::Buffering;
    bool starting = true;
    int64_t stall_start_us = 0;
    int64_t data_since_us = 0;
    bool reconnect_requested = false;
    int64_t next_metrics_us = 0;
    int64_t next_policy_us = 0;
    int64_t metrics_newest_us = 0;
    int64_t metrics_at_us = 0;
    double ingest_rate = 1.0;

    bool video_primed = true;
    bool need_keyframe = false;
    int64_t key_wait_start_us = 0;
    int64_t preroll_us = INT64_MIN;
    int64_t prime_start_us = 0;
    int64_t last_fed_dts_us = 0;
    int64_t last_frame_media_us = 0;
    int64_t decoder_delay_us = 0;
    int64_t frame_duration_us = 16667;
    int64_t last_audio_wall_us = 0;

    double rate = 1.0;
    double main_delay_us = 0.0;
    bool main_delay_valid = false;
    bool sound_listening = false;
    bool sync_hold = false; // paused on purpose to add delay
    bool force_cut = false; // the next policy round cuts, whatever the backlog
    bool jump_pending = false;
    uint32_t main_move_id = 0;
    bool shift_known = false;
    int shift_applied_ms = 0;
    int shift_seen_ms = 0;
    int64_t shift_seen_us = 0;
    int64_t shift_check_us = 0;

    // Sample-counted audio timeline: rounding can't accumulate into drift.
    uint64_t audio_base_ns = 0;
    uint64_t audio_samples = 0;
    int audio_rate = 0;
    uint32_t audio_fade_left = 0;
    uint32_t audio_fade_total = 0;
  };

  struct PresentFrame {
    AVFrame *frame = nullptr;
    int64_t media_us = 0;
  };

  PlayoutRun run_;
  PacketBuffer packets_;
  PlayoutClock playout_clock_;
  RecoveryPolicy recovery_policy_;
  MediaTimeline media_timeline_;
  // Held around every decoder call. Reconnect paths take it to close or
  // reopen the decoders while the decode thread is known to be idle.
  std::mutex decoder_mutex_;
  std::mutex present_queue_mutex_;
  std::condition_variable present_cv_;
  std::deque<PresentFrame> present_queue_;
  // Held while a frame goes to OBS, so a reset can guarantee none lands after it.
  std::mutex present_output_mutex_;
  std::thread playout_decode_thread_;
  std::thread playout_present_thread_;
  std::atomic<bool> playout_enabled_{false};
  std::atomic<uint64_t> playout_generation_{0};
  std::atomic<bool> playout_stalled_{false};
  std::atomic<bool> playout_has_video_{false};
  std::atomic<bool> playout_has_audio_{false};
  std::atomic<bool> playout_cut_fade_pending_{false};
  std::atomic<int64_t> playout_latency_ms_{0};
  std::atomic<int64_t> playout_target_ms_{0};
  std::atomic<double> playout_rate_{1.0};
  std::atomic<int> target_latency_ms_{DEFAULT_TARGET_LATENCY_MS};
  std::atomic<int> recovery_mode_{static_cast<int>(RecoveryMode::CutToLive)};

  // Delay matching against another source. ArrivalBaseline belongs to the
  // worker; decode reads the published copy.
  ArrivalBaseline arrival_baseline_;
  std::atomic<int64_t> arrival_base_us_{INT64_MAX};
  std::atomic<bool> sync_active_{false};
  std::atomic<int64_t> sync_error_ms_{0};

  // "Sync" button: listens to both sources until their sounds line up, then stops.
  std::atomic<int> sync_listen_state_{static_cast<int>(SyncListenState::Idle)};
  std::atomic<int64_t> sync_listen_until_us_{0};
  std::atomic<int> sync_listen_base_delay_ms_{0}; // main delay when the sync began
  std::atomic<int64_t> sync_listen_delta_ms_{0};
  std::atomic<int> sync_heard_main_{0};
  std::atomic<int> sync_heard_own_{0};
  std::atomic<int> sync_listen_left_s_{0};
  std::atomic<int> sync_listen_held_ms_{0};  // main source delayed by this for us
  std::atomic<int> sync_listen_short_ms_{0}; // couldn't catch up by this much
  std::atomic<int64_t> sync_result_until_us_{0};
  SoundTracer sound_tracer_; // decode thread
  int64_t cut_fade_start_ms_ = 0; // present thread: crossfade after a cut

  // Worker thread
  std::thread worker_thread_;
  std::thread connect_thread_;
  std::thread overlay_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connection_in_progress_{false};


  std::atomic<bool> prev_low_bitrate_{false};
  std::atomic<int64_t> last_low_bitrate_time_ms_{0};
  std::atomic<int64_t> last_connect_ms_{0};

  // Overlay visibility state. update_source_toggles() runs from both the
  // worker and the overlay thread, so the whole state machine is serialized.
  std::mutex toggle_mutex_;
  OverlayDebouncer disconnect_overlay_;
  OverlayDebouncer loading_overlay_;
  // Last state pushed to the scene items — only queue a UI task on a change.
  bool prev_disconnected_shown_ = false;
  bool prev_loading_shown_ = false;
  std::atomic<int> disconnect_grace_ms_{DEFAULT_DISCONNECT_GRACE_MS};

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
  uint32_t sync_move_id_ = 0; // decode thread, outlives a reconnect
  std::atomic<bool> disconnect_fallback_{false};
  std::atomic<bool> loading_fallback_{false};
  std::atomic<int> whep_mode_{0}; // Stored as int, cast to WhepClient::WhepMode
  std::atomic<int> reconnect_count_{0};
  std::atomic<double> current_fps_{0.0};
  int64_t last_fps_calc_ms_ = 0;
  int64_t last_frames_count_ = 0;
  int64_t last_pkt_recv_ms_ = 0;

  // Tracks the time we start waiting for the network buffer (EAGAIN loop).
  // Used to accurately measure genuine network freezes without including
  // processing or pacing sleep time.
  int64_t empty_buffer_start_ms_ = 0;

  int64_t pts_to_obs_offset_ns_ = 0;
  bool has_pts_offset_ = false;
  int64_t first_video_pts_ns_ = 0;
  int64_t first_audio_pts_ns_ = 0;

  bool catching_up_ = false;
  int64_t catchup_first_wall_ms_ = 0;
  int64_t catchup_first_pts_ms_ = 0;

  // Suppresses another freeze recovery briefly after a skip-resync.
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
  PrivacyBlur privacy_blur_;
  FallbackLayer fallback_layer_;
  void handle_dashboard_command(const std::string &json_cmd);
  void send_preview_video(const AVFrame *frame);
  void send_preview_audio(const DecodedAudioFrame &af);
  void apply_blur_inplace(obs_source_frame2 &obs_frame, AVFrame *frame);
  void draw_mute_icon(obs_source_frame2 &obs_frame, video_format fmt,
                      AVFrame *frame);

  std::vector<uint8_t> trans_y_buf_;
  std::vector<uint8_t> trans_u_buf_;
  std::vector<uint8_t> trans_v_buf_;
  static constexpr int64_t CUT_FADE_MS = 200;

  // The most recent live frame, for the cut crossfade.
  std::mutex last_frame_mutex_;
  std::vector<uint8_t> last_y_buf_, last_u_buf_, last_v_buf_;
  video_format last_frame_fmt_ = VIDEO_FORMAT_NONE;
  int  last_frame_w_ = 0, last_frame_h_ = 0;
  int  last_frame_ly_ = 0, last_frame_lu_ = 0, last_frame_lv_ = 0;
  bool last_frame_full_range_ = false;
  bool has_last_frame_ = false;

  void cache_last_frame(AVFrame *frame, video_format fmt, bool full_range);

  // Burst-skip (HLS/IVS): packet-level discard for large drops (>2s).
  // Worker reads and discards packets until burst_skip_target_pts_ms_ is
  // reached, then resumes on a keyframe at the live edge.
  bool burst_skip_active_ = false;
  int64_t burst_skip_target_pts_ms_ = 0; // Target PTS (live edge estimate)
  int64_t burst_skip_start_ms_ = 0;      // Wall time when burst_skip started (timeout guard)
  bool burst_skip_wait_key_ = false;     // Target reached, holding for a keyframe

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
