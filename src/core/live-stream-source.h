// live-stream-source.h
#pragma once

#include "core/common.h"
#include "media/audio-decoder.h"
#include "media/frame-queue.h"
#include "utils/bitrate-monitor.h"
#include "utils/catchup-controller.h"

#include "media/stream-demuxer.h"
#include "media/video-decoder.h"
#include "network/ws-stats-server.h"
#include "protocols/whep/whep-client.h"
#include "utils/reconnect-manager.h"

#include <atomic>
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
  StreamType stream_type_ = StreamType::Standard;
  std::string low_bitrate_src_name_;
  std::string disconnect_src_name_;
  std::string loading_src_name_;
  std::string whep_token_;

  // Components
  StreamDemuxer demuxer_;
  VideoDecoder video_dec_;
  AudioDecoder audio_dec_;
  AudioFrameQueue audio_queue_{FRAME_QUEUE_CAPACITY};
  BitrateMonitor bitrate_mon_;
  CatchupController catchup_;
  ReconnectManager reconnect_mgr_;

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
  std::atomic<bool> active_{false};
  std::atomic<bool> pending_refresh_{false};
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

  int64_t pts_to_obs_offset_ns_ = 0;
  bool has_pts_offset_ = false;

  bool catching_up_ = false;
  int64_t catchup_first_wall_ms_ = 0;
  int64_t catchup_first_pts_ms_ = 0;
  int64_t ivs_last_keyframe_pts_ms_ = 0; // IVS burst buster tracking

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
