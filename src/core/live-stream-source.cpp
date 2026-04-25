// live-stream-source.cpp
#include "core/live-stream-source.h"
#include "core/plugin-settings.h"
#include "html-assets.h"
#include "util/platform.h"
#include "utils/reconnect-manager.h"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#endif

#include <obs-frontend-api.h>

namespace lss {

#define PROP_WHEP_MODE "whep_mode"

static video_format convert_pixel_format(AVPixelFormat func) {
  switch (func) {
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUVJ420P: // Corrected J420P -> YUVJ420P
    return VIDEO_FORMAT_I420;
  case AV_PIX_FMT_NV12:
    return VIDEO_FORMAT_NV12;
  case AV_PIX_FMT_YUYV422:
    return VIDEO_FORMAT_YUY2;
  case AV_PIX_FMT_UYVY422:
    return VIDEO_FORMAT_UYVY;
  case AV_PIX_FMT_BGRA:
    return VIDEO_FORMAT_BGRA;
  case AV_PIX_FMT_YUV444P:
    return VIDEO_FORMAT_I444;
  default:
    return VIDEO_FORMAT_NONE; // Ensure fallback
  }
}

static audio_format convert_audio_format(AVSampleFormat fmt) {
  switch (fmt) {
  case AV_SAMPLE_FMT_U8:
  case AV_SAMPLE_FMT_U8P:
    return AUDIO_FORMAT_U8BIT;
  case AV_SAMPLE_FMT_S16:
  case AV_SAMPLE_FMT_S16P:
    return AUDIO_FORMAT_16BIT;
  case AV_SAMPLE_FMT_S32:
  case AV_SAMPLE_FMT_S32P:
    return AUDIO_FORMAT_32BIT;
  case AV_SAMPLE_FMT_FLT:
  case AV_SAMPLE_FMT_FLTP:
    return AUDIO_FORMAT_FLOAT;
  default:
    return AUDIO_FORMAT_UNKNOWN;
  }
}

LiveStreamSource::LiveStreamSource(obs_source_t *source, obs_data_t *settings)
    : obs_source_(source) {
  width_.store(0);
  height_.store(0);
  active_.store(false);
  connected_.store(false);

  last_video_ts_ = 0;
  video_pts_ns_ = 0;
  video_base_pts_ns_ = 0;
  video_start_pts_rtp_ = 0;

  // Initialize queues with default capacity
  video_queue_ = std::make_unique<VideoFrameQueue>(FRAME_QUEUE_CAPACITY);
  audio_queue_ = std::make_unique<AudioFrameQueue>(FRAME_QUEUE_CAPACITY);

  init_stats_dir();
  install_stats_html();
  install_overlay_html();
  WsStatsServer::instance().add_ref();
  update(settings);
}

LiveStreamSource::~LiveStreamSource() {
  stop_stream();
  const char *name = obs_source_get_name(obs_source_);
  if (name)
    WsStatsServer::instance().remove_source(name);
  WsStatsServer::instance().release();
}

void LiveStreamSource::update(obs_data_t *settings) {
  bool is_refresh = pending_refresh_.exchange(false);

  std::string new_url = obs_data_get_string(settings, PROP_URL);
  int new_kbps = static_cast<int>(obs_data_get_int(settings, PROP_LOW_BITRATE));
  bool new_catchup = obs_data_get_bool(settings, PROP_AUTO_CATCHUP);
  bool new_hw_decode = obs_data_get_bool(settings, PROP_HW_DECODE);
  auto new_stream_type =
      static_cast<StreamType>(obs_data_get_int(settings, PROP_STREAM_TYPE));
  
  // Buffer configuration
  int new_preset = static_cast<int>(obs_data_get_int(settings, PROP_BUFFER_PRESET));
  int new_custom = static_cast<int>(obs_data_get_int(settings, PROP_BUFFER_CUSTOM));

  const char *lb_src = obs_data_get_string(settings, PROP_LOW_BITRATE_SOURCE);
  const char *dc_src = obs_data_get_string(settings, PROP_DISCONNECT_SOURCE);
  const char *ld_src = obs_data_get_string(settings, PROP_LOADING_SOURCE);

  bool url_changed = (new_url != url_);
  bool hw_changed = (new_hw_decode != hw_decode_);
  bool type_changed = (new_stream_type != stream_type_);

  url_ = new_url;
  low_bitrate_kbps_ = (new_kbps > 0) ? new_kbps : DEFAULT_LOW_BITRATE_KBPS;
  auto_catchup_ = new_catchup;
  hw_decode_ = new_hw_decode;
  bool prev_shimmer = show_shimmer_;
  show_shimmer_ = obs_data_get_bool(settings, PROP_SHOW_SHIMMER);
  
  // If shimmer was just disabled, clear the frame immediately
  if (prev_shimmer && !show_shimmer_ && !first_frame_received_.load()) {
    obs_source_output_video2(obs_source_, nullptr);
  }
  stream_type_ = new_stream_type;
  buffer_preset_ = static_cast<BufferPreset>(new_preset);
  custom_buffer_frames_ = std::clamp(new_custom, BUFFER_SIZE_MIN, BUFFER_SIZE_MAX);
  low_bitrate_src_name_ = lb_src ? lb_src : "";
  disconnect_src_name_ = dc_src ? dc_src : "";
  loading_src_name_ = ld_src ? ld_src : "";

  const char *whep_tok = obs_data_get_string(settings, PROP_WHEP_TOKEN);
  whep_token_ = whep_tok ? whep_tok : "";

  int new_whep_mode =
      static_cast<int>(obs_data_get_int(settings, PROP_WHEP_MODE));
  bool whep_mode_changed = (new_whep_mode != whep_mode_.load());
  whep_mode_.store(new_whep_mode);

  bitrate_mon_.set_threshold_kbps(low_bitrate_kbps_);
  catchup_.set_enabled(auto_catchup_);

  // Calculate new buffer size
  int new_buffer_size = calculate_buffer_size();
  
  // Log buffer configuration
  const char* preset_name = "Unknown";
  switch (buffer_preset_) {
    case BufferPreset::Auto: preset_name = "Auto"; break;
    case BufferPreset::UltraLowLatency: preset_name = "Ultra Low Latency"; break;
    case BufferPreset::LowLatency: preset_name = "Low Latency"; break;
    case BufferPreset::Balanced: preset_name = "Balanced"; break;
    case BufferPreset::Stable: preset_name = "Stable"; break;
    case BufferPreset::MaxStability: preset_name = "Max Stability"; break;
    case BufferPreset::Custom: preset_name = "Custom"; break;
  }
  lss_log_info("Buffer preset: %s → %d frames", preset_name, new_buffer_size);
  
  // Recreate queues if size changed
  if (!video_queue_ || video_queue_->capacity() != new_buffer_size) {
    recreate_frame_queues(new_buffer_size);
  }

  prev_disconnected_.store(!connected_.load());
  prev_low_bitrate_.store(!bitrate_mon_.is_low());

  bool needs_restart = is_refresh || url_changed || hw_changed ||
                       type_changed || whep_mode_changed;

  if (needs_restart && active_.load()) {
    stop_stream();
    if (!url_.empty())
      start_stream();
  }
}

void LiveStreamSource::activate() {
  active_.store(true);
  if (!url_.empty())
    start_stream();
}

void LiveStreamSource::deactivate() {
  active_.store(false);
  stop_stream();
}

//  Frame Output

void LiveStreamSource::output_video_frame(AVFrame *frame) {
  static int log_count = 0;

  if (!frame || !frame->data[0]) {
    if (log_count < 5) {
      lss_log_warn("output_video_frame: null frame or data[0]");
      log_count++;
    }
    return;
  }

  video_format obs_fmt =
      convert_pixel_format(static_cast<AVPixelFormat>(frame->format));

  if (obs_fmt == VIDEO_FORMAT_NONE) {
    if (frame->format == AV_PIX_FMT_YUVJ420P)
      obs_fmt = VIDEO_FORMAT_I420;
    else {
      if (log_count < 5) {
        lss_log_warn("output_video_frame: unsupported format %d",
                     frame->format);
        log_count++;
      }
      return;
    }
  }

  bool full_range = (frame->color_range == AVCOL_RANGE_JPEG) ||
                    (frame->format == AV_PIX_FMT_YUVJ420P);

  obs_source_frame2 obs_frame = {};
  obs_frame.width = frame->width;
  obs_frame.height = frame->height;
  obs_frame.format = obs_fmt;
  obs_frame.range = full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;

  for (int i = 0; i < MAX_AV_PLANES; i++) {
    obs_frame.data[i] = frame->data[i];
    obs_frame.linesize[i] = static_cast<uint32_t>(frame->linesize[i]);
  }

  enum video_colorspace cs =
      (frame->height >= 720) ? VIDEO_CS_709 : VIDEO_CS_601;
  video_format_get_parameters_for_format(
      cs, obs_frame.range, obs_fmt, obs_frame.color_matrix,
      obs_frame.color_range_min, obs_frame.color_range_max);

  int64_t computed_ts = 0;
  int64_t stream_pts_ns = 0;

  if (frame->pts > 1000000000000000000LL) {
    computed_ts = frame->pts;
    stream_pts_ns = frame->pts; // For drift logic compatibility
  } else {
    if (frame->pts != AV_NOPTS_VALUE) {
      AVRational tb = demuxer_.video_time_base();
      if (tb.den > 0)
        stream_pts_ns = av_rescale_q(frame->pts, tb, {1, 1000000000});
      else
        stream_pts_ns = static_cast<int64_t>(os_gettime_ns());
    } else {
      stream_pts_ns = static_cast<int64_t>(os_gettime_ns());
    }

    if (!has_pts_offset_) {
      // Wait for both audio and video before anchoring
      // This prevents initial desync (100-200ms)
      first_video_pts_ns_ = stream_pts_ns;
      
      if (first_audio_pts_ns_ != 0 || demuxer_.audio_stream_index() < 0) {
        // Audio already arrived OR no audio stream - anchor now
        int64_t anchor_pts = (first_audio_pts_ns_ != 0) 
                              ? std::min(first_video_pts_ns_, first_audio_pts_ns_)
                              : first_video_pts_ns_;
        pts_to_obs_offset_ns_ = static_cast<int64_t>(os_gettime_ns()) - anchor_pts;
        has_pts_offset_ = true;
        lss_log_info("PTS anchored: video=%lld ns, audio=%lld ns, anchor=%lld ns",
                     (long long)first_video_pts_ns_, (long long)first_audio_pts_ns_,
                     (long long)anchor_pts);
      }
    }
    computed_ts = stream_pts_ns + pts_to_obs_offset_ns_;
  }
  bool is_hls = (stream_type_ == StreamType::HLS) ||
                (stream_type_ == StreamType::AmazonIVS);

  if (!is_hls) {
    int64_t wall_ns = static_cast<int64_t>(os_gettime_ns());
    int64_t drift_ns = wall_ns - computed_ts;
    if (drift_ns > 500000000LL || drift_ns < -500000000LL) {
      pts_to_obs_offset_ns_ = wall_ns - stream_pts_ns;
      computed_ts = stream_pts_ns + pts_to_obs_offset_ns_;
    }

    int64_t last_pts_ns = last_video_pts_us_.load() * 1000;
    if (has_pts_offset_ && stream_pts_ns < last_pts_ns) {
      int64_t diff = last_pts_ns - stream_pts_ns;
      if (diff > 100000000LL) {
        lss_log_warn("PTS rollback: %lld -> %lld (diff %lld ms). Re-anchoring.",
                     (long long)last_pts_ns, (long long)stream_pts_ns,
                     (long long)(diff / 1000000));
        pts_to_obs_offset_ns_ =
            static_cast<int64_t>(os_gettime_ns()) - stream_pts_ns;
        computed_ts = stream_pts_ns + pts_to_obs_offset_ns_;
      }
    }
  }
  last_video_pts_us_.store(stream_pts_ns / 1000);

  obs_frame.timestamp = static_cast<uint64_t>(computed_ts);

  if (is_hls) {
    int64_t pts_ms = stream_pts_ns / 1000000;
    int64_t wall_ms = now_ms();

    if (catchup_first_wall_ms_ == 0) {
      catchup_first_wall_ms_ = wall_ms;
      catchup_first_pts_ms_ = pts_ms;
    }

    int64_t pts_elapsed = pts_ms - catchup_first_pts_ms_;
    int64_t wall_elapsed = wall_ms - catchup_first_wall_ms_;
    int64_t ahead_ms = pts_elapsed - wall_elapsed;

    if (stream_type_ == StreamType::AmazonIVS) {
      if (ahead_ms < -300) {
        catchup_first_wall_ms_ = wall_ms;
        catchup_first_pts_ms_ = pts_ms;
        wall_elapsed = 0;
        ahead_ms = 0;
      } else if (ahead_ms < -50) {
        catchup_first_wall_ms_ += 2;
        wall_elapsed = now_ms() - catchup_first_wall_ms_;
        ahead_ms = pts_elapsed - wall_elapsed;
      }
    } else if (ahead_ms < -500) {
      catchup_first_wall_ms_ += 200;
      wall_elapsed = now_ms() - catchup_first_wall_ms_;
      ahead_ms = pts_elapsed - wall_elapsed;
    }

    if (ahead_ms > 10000 || ahead_ms < -10000) {
      catchup_first_wall_ms_ = wall_ms;
      catchup_first_pts_ms_ = pts_ms;
      ahead_ms = 0;
    }

    while (ahead_ms > 2 && running_.load()) {
      int sleep = (ahead_ms > 10) ? 5 : 1;
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
      wall_elapsed = now_ms() - catchup_first_wall_ms_;
      ahead_ms = pts_elapsed - wall_elapsed;
    }
  }

  if (log_count < 5) {
    lss_log_debug("output_video_frame: fmt=%d obs_fmt=%d %dx%d range=%d "
                  "pts_ns=%lld obs_ts=%llu",
                  frame->format, (int)obs_fmt, frame->width, frame->height,
                  (int)obs_frame.range, (long long)stream_pts_ns,
                  (unsigned long long)obs_frame.timestamp);
    log_count++;
  }

  obs_source_output_video2(obs_source_, &obs_frame);
  first_frame_received_.store(true);
  ever_received_frame_.store(true);

  if (width_.load() != frame->width || height_.load() != frame->height) {
    width_.store(frame->width);
    height_.store(frame->height);
  }

  int64_t recv_t = last_pkt_recv_us_.load();
  if (recv_t > 0) {
    pipeline_latency_ms_.store((now_us() - recv_t) / 1000);
  }
}

void LiveStreamSource::output_audio_frame(DecodedAudioFrame &af) {
  if (!af.data)
    return;

  obs_source_audio obs_audio = {};

  int bytes_per_sample = sizeof(float);
  int plane_size = af.frames * bytes_per_sample;

  obs_audio.data[0] = af.data; // Left / mono channel
  if (af.channels >= 2) {
    obs_audio.data[1] = af.data + plane_size; // Right channel
  }

  obs_audio.frames = af.frames;
  obs_audio.speakers = (af.channels >= 2) ? SPEAKERS_STEREO : SPEAKERS_MONO;
  obs_audio.samples_per_sec = af.sample_rate;
  obs_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;

  int64_t audio_pts_ns = af.pts_us * 1000; // us -> ns

  if (!has_pts_offset_) {
    // Wait for both audio and video before anchoring
    // This prevents initial desync (100-200ms)
    first_audio_pts_ns_ = audio_pts_ns;
    
    if (first_video_pts_ns_ != 0 || demuxer_.video_stream_index() < 0) {
      // Video already arrived OR no video stream - anchor now
      int64_t anchor_pts = (first_video_pts_ns_ != 0)
                            ? std::min(first_video_pts_ns_, first_audio_pts_ns_)
                            : first_audio_pts_ns_;
      pts_to_obs_offset_ns_ = static_cast<int64_t>(os_gettime_ns()) - anchor_pts;
      has_pts_offset_ = true;
      lss_log_info("PTS anchored: video=%lld ns, audio=%lld ns, anchor=%lld ns",
                   (long long)first_video_pts_ns_, (long long)first_audio_pts_ns_,
                   (long long)anchor_pts);
    } else {
      // Waiting for video - don't output audio yet
      return;
    }
  }

  int64_t audio_computed = audio_pts_ns + pts_to_obs_offset_ns_;
  obs_audio.timestamp = static_cast<uint64_t>(audio_computed);

  // Gentle audio pacing for HLS streams
  // Only pace if audio is VERY far ahead of video (>100ms)
  // This prevents audio rushing ahead while avoiding OBS buffering messages
  bool is_hls = (stream_type_ == StreamType::HLS) ||
                (stream_type_ == StreamType::AmazonIVS);
  if (is_hls && catchup_first_wall_ms_ != 0) {
    int64_t pts_ms = audio_pts_ns / 1000000;
    int64_t pts_elapsed = pts_ms - catchup_first_pts_ms_;
    int64_t wall_elapsed = now_ms() - catchup_first_wall_ms_;
    int64_t ahead_ms = pts_elapsed - wall_elapsed;
    
    // Only pace if significantly ahead (>100ms threshold)
    // Use gentle pacing (sleep half the ahead time, max 10ms)
    if (ahead_ms > 100) {
      int sleep = std::min(10, static_cast<int>(ahead_ms / 2));
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
    }
  }

  obs_source_output_audio(obs_source_, &obs_audio);
}

void LiveStreamSource::output_shimmer_frame() {
  int w = width_.load();
  int h = height_.load();
  if (w <= 0 || h <= 0) {
    w = 1920;
    h = 1080;
  }

  int linesize = w * 4;
  size_t required_size = static_cast<size_t>(linesize) * h;

  if (shimmer_buffer_.size() < required_size) {
    shimmer_buffer_.resize(required_size);
  }

  uint8_t *buf_ptr = shimmer_buffer_.data();

  const double PI = 3.14159265358979;
  int64_t t = now_ms();

  double breath = 0.5 + 0.5 * sin((double)t / 1200.0 * PI);
  double spin_angle = fmod((double)t / 1500.0, 1.0) * 2.0 * PI;
  double bg_pulse = 0.5 + 0.5 * sin((double)t / 2000.0 * PI);

  double cx = w * 0.5;
  double cy = h * 0.5;
  double dim = (double)(w < h ? w : h);
  double ring_r = dim * 0.06;
  double ring_thick = dim * 0.005;

  for (int y = 0; y < h; y++) {
    double dy = y - cy;
    for (int x = 0; x < w; x++) {
      double dx = x - cx;
      double dist = sqrt(dx * dx + dy * dy);
      int idx = y * linesize + x * 4;

      double bg_dist = dist / (dim * 0.7);
      if (bg_dist > 1.0)
        bg_dist = 1.0;
      double bg_v = 0.07 - 0.04 * bg_dist * bg_dist;
      bg_v += 0.008 * bg_pulse; // subtle pulse
      double r = bg_v * 0.85;
      double g = bg_v * 0.88;
      double b = bg_v * 1.0;

      double glow_r = dim * 0.12;
      if (dist < glow_r) {
        double gi = 1.0 - dist / glow_r;
        gi = gi * gi * gi; // cubic falloff
        double glow_str = 0.06 + 0.04 * breath;
        r += glow_str * gi * 0.3;
        g += glow_str * gi * 0.9;
        b += glow_str * gi * 1.0;
      }

      double ring_dist = fabs(dist - ring_r);
      double ring_alpha = ring_thick + ring_thick * 0.3 * breath;
      if (ring_dist < ring_alpha) {
        double ri = 1.0 - ring_dist / ring_alpha;
        ri = ri * ri; // smooth edge
        double ring_bright = 0.15 + 0.1 * breath;
        r += ring_bright * ri * 0.3;
        g += ring_bright * ri * 0.85;
        b += ring_bright * ri * 1.0;
      }

      if (ring_dist < ring_alpha * 2.0) {
        double angle = atan2(dy, dx);
        double arc_diff = angle - spin_angle;
        while (arc_diff > PI)
          arc_diff -= 2.0 * PI;
        while (arc_diff < -PI)
          arc_diff += 2.0 * PI;
        double arc_len = PI * 0.4; // 72 degree arc
        if (fabs(arc_diff) < arc_len) {
          double ai = 1.0 - fabs(arc_diff) / arc_len;
          ai = ai * ai;
          double ri2 = 1.0 - ring_dist / (ring_alpha * 2.0);
          ri2 = ri2 * ri2;
          double spin_bright = 0.35 * ai * ri2;
          r += spin_bright * 0.4;
          g += spin_bright * 0.95;
          b += spin_bright * 1.0;
        }
      }

      if (r > 1.0)
        r = 1.0;
      if (g > 1.0)
        g = 1.0;
      if (b > 1.0)
        b = 1.0;

      buf_ptr[idx + 0] = (uint8_t)(b * 255.0); // B
      buf_ptr[idx + 1] = (uint8_t)(g * 255.0); // G
      buf_ptr[idx + 2] = (uint8_t)(r * 255.0); // R
      buf_ptr[idx + 3] = 255;                  // A
    }
  }

  obs_source_frame2 frame = {};
  frame.data[0] = buf_ptr;
  frame.linesize[0] = linesize;
  frame.width = w;
  frame.height = h;
  frame.format = VIDEO_FORMAT_BGRA;
  frame.timestamp = os_gettime_ns();

  obs_source_output_video2(obs_source_, &frame);
}

void LiveStreamSource::worker_thread_func() {
  lss_log_debug("Worker thread started");
  AVPacket *pkt = nullptr;
  try {

    if (stream_type_ == StreamType::WHEP) {
      whep_worker();
      return;
    }

    pkt = av_packet_alloc();
    lss_log_debug("Worker: Entering main loop");

    while (running_.load()) {
      static int64_t last_heartbeat = 0;
      if (now_ms() - last_heartbeat > 2000) {
        lss_log_debug("Worker: Loop heartbeat (connected=%d)",
                      connected_.load());
        last_heartbeat = now_ms();
      }

      if (!connected_.load()) {
        if (now_ms() % 1000 == 0)
          lss_log_debug("Worker: Not connected, waiting...");

        if (connection_in_progress_.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }

        if (!reconnect_mgr_.can_retry()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(33));
          continue;
        }

        if (!running_.load())
          break;

        if (connect_thread_.joinable())
          connect_thread_.join();

        connection_in_progress_.store(true);
        lss_log_debug("Attempting connection (Async)... Protocol: %s",
                      (stream_type_ == StreamType::Standard) ? "Standard"
                                                             : "HLS/IVS");

        connect_thread_ = std::thread([this]() {
          if (!running_.load()) {
            connection_in_progress_.store(false);
            return;
          }

          if (try_connect()) {
            reconnect_mgr_.reset();
            catching_up_ = catchup_.is_enabled();
            catchup_first_wall_ms_ = 0;
            catchup_first_pts_ms_ = 0;
            // Hide disconnect source on successful reconnect
            prev_disconnected_.store(true); // Force re-evaluation
            update_source_toggles();
          } else {
            reconnect_mgr_.mark_failed();
          }
          connection_in_progress_.store(false);
        });

        continue;
      }

      int ret = demuxer_.read_packet(pkt);

      if (ret < 0 && ret != AVERROR(EAGAIN)) {
        static int64_t last_err_log = 0;
        if (now_ms() - last_err_log > 1000) {
          lss_log_debug("Worker: read_packet returned %d", ret);
          last_err_log = now_ms();
        }
      }

      if (ret < 0 && ret != AVERROR(EAGAIN)) {
        bool is_hls = (stream_type_ == StreamType::HLS) ||
                      (stream_type_ == StreamType::AmazonIVS);

        if (is_hls && ret == AVERROR_EOF) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }

        lss_log_warn("Stream EOF or IO error (ret=%d)", ret);
        bitrate_mon_.reset();
        connected_.store(false);

        current_fps_.store(0.0);
        pipeline_latency_ms_.store(0);
        stream_delay_ms_.store(0);
        has_delay_ref_ = false;
        first_frame_received_.store(false);

        has_pts_offset_ = false;
        pts_to_obs_offset_ns_ = 0;
        first_video_pts_ns_ = 0;
        first_audio_pts_ns_ = 0;
        
        // Clear last frame and output shimmer immediately
        obs_source_output_video2(obs_source_, nullptr);
        output_shimmer_frame();  // Show shimmer immediately
        
        write_stats_json();
        update_source_toggles();

        av_packet_unref(pkt);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }

      if (ret == AVERROR(EAGAIN)) {
        if (!first_frame_received_.load()) {
          // (Removed: Shimmer now in dedicated thread)
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      if (pkt->stream_index != demuxer_.video_stream_index() &&
          pkt->stream_index != demuxer_.audio_stream_index()) {
        av_packet_unref(pkt);
        continue;
      }

      bitrate_mon_.record_bytes(pkt->size);

      if (catching_up_ && pkt->stream_index == demuxer_.video_stream_index()) {
        if (pkt->dts != AV_NOPTS_VALUE) {
          AVRational tb = demuxer_.video_time_base();
          int64_t pts_ms = av_rescale_q(pkt->dts, tb, {1, 1000});
          int64_t wall_ms = now_ms();

          if (catchup_first_wall_ms_ == 0) {
            catchup_first_wall_ms_ = wall_ms;
            catchup_first_pts_ms_ = pts_ms;
          } else {
            int64_t wall_elapsed = wall_ms - catchup_first_wall_ms_;
            int64_t pts_elapsed = pts_ms - catchup_first_pts_ms_;

            bool is_ivs = (stream_type_ == StreamType::AmazonIVS);
            bool still_bursting =
                (is_ivs && (wall_ms - last_pkt_recv_ms_) < 30);

            bool force_start = (is_ivs && wall_elapsed > 2500);
            bool edge_jump = (is_ivs && pts_elapsed < 300);

            double threshold = is_ivs ? 0.99 : 0.8;
            if (force_start ||
                (!still_bursting && !edge_jump && pts_elapsed > 100 &&
                 wall_elapsed >= pts_elapsed * threshold)) {

              if (pkt->flags & AV_PKT_FLAG_KEY) {
                catching_up_ = false;

                catchup_first_wall_ms_ = now_ms();
                catchup_first_pts_ms_ = pts_ms;

                lss_log_debug("Catchup: Anchor Locked! PTS=%lld ms",
                              (long long)pts_ms);
                has_delay_ref_ = false;
                has_pts_offset_ = false;
                pts_to_obs_offset_ns_ = 0;
                audio_dec_.reset_state();
              }
            } else {
              av_packet_unref(pkt);
              continue;
            }
          }
        }
      } else if (catching_up_ &&
                 pkt->stream_index == demuxer_.audio_stream_index()) {
        av_packet_unref(pkt);
        continue;
      }

      if (pkt->stream_index == demuxer_.video_stream_index()) {
        last_pkt_recv_us_.store(now_us());
        total_bytes_video_.fetch_add(pkt->size);

        if (pkt->dts != AV_NOPTS_VALUE) {
          AVRational tb = demuxer_.video_time_base();
          int64_t pts_ms = av_rescale_q(pkt->dts, tb, {1, 1000});
          int64_t wall_ms = now_ms();
          if (!has_delay_ref_) {
            delay_ref_wall_ms_ = wall_ms;
            delay_ref_pts_ms_ = pts_ms;
            has_delay_ref_ = true;
          } else {
            int64_t elapsed_wall = wall_ms - delay_ref_wall_ms_;
            int64_t elapsed_pts = pts_ms - delay_ref_pts_ms_;
            int64_t delay = elapsed_wall - elapsed_pts;
            
            // Delay includes: network latency + decode latency + buffer latency
            // This is the total end-to-end delay from stream to output
            if (delay < 0) {
              delay_ref_wall_ms_ = wall_ms;
              delay_ref_pts_ms_ = pts_ms;
              stream_delay_ms_.store(0);
            } else {
              stream_delay_ms_.store(delay);
            }
          }
        }

        int res = video_dec_.decode(
            pkt, [this](AVFrame *f) { output_video_frame(f); });

        if (res == 0)
          total_frames_decoded_.fetch_add(1);
        else
          total_frames_dropped_decode_.fetch_add(1);

      } else if (pkt->stream_index == demuxer_.audio_stream_index()) {
        total_bytes_audio_.fetch_add(pkt->size);
        audio_dec_.decode(pkt, *audio_queue_);
        DecodedAudioFrame af;
        while (audio_queue_->pop(af)) {
          output_audio_frame(af);
          af.free_buffers();
        }
      }

      last_pkt_recv_ms_ = now_ms();
      av_packet_unref(pkt);

      // Periodic buffer monitoring and adjustment
      monitor_and_adjust_buffer();

      // dedicated thread)

      int64_t now = now_ms();
      if (now - stats_last_write_ms_ >= STATS_WRITE_INTERVAL_MS) {
        int64_t decoded_now = total_frames_decoded_.load();
        int64_t elapsed = now - last_fps_calc_ms_;
        if (last_fps_calc_ms_ > 0 && elapsed > 0) {
          double fps = (decoded_now - last_frames_count_) * 1000.0 / elapsed;
          current_fps_.store(fps);
        }
        last_fps_calc_ms_ = now;
        last_frames_count_ = decoded_now;

        stats_last_write_ms_ = now;
        write_stats_json();
      }
    }
  } catch (const std::exception &e) {
    lss_log_error("Worker thread EXCEPTION: %s", e.what());
  } catch (...) {
    lss_log_error("Worker thread UNKNOWN EXCEPTION");
  }

  if (pkt)
    av_packet_free(&pkt);

  lss_log_debug("Worker thread stopped");
}

void LiveStreamSource::shimmer_thread_func() {
  lss_log_debug("Shimmer thread started");
  while (running_.load()) {
    if (show_shimmer_ && !first_frame_received_.load()) {
      output_shimmer_frame();
    }

    // even if the worker thread is blocked waiting for sluggish network I/O.
    update_source_toggles();

    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  lss_log_debug("Shimmer thread stopped");
}

void LiveStreamSource::start_stream() {
  if (running_.load())
    stop_stream();

  running_.store(true);
  reconnect_mgr_.reset();
  demuxer_.reset_abort();
  stats_last_write_ms_ = now_ms();
  stream_start_time_ = std::chrono::steady_clock::now();
  prev_disconnected_.store(false);
  prev_low_bitrate_.store(false);
  last_low_bitrate_time_ms_.store(0);

  if (stream_type_ == StreamType::AmazonIVS ||
      stream_type_ == StreamType::HLS) {
    catchup_.set_enabled(false);
    lss_log_info("HLS/IVS detected: Disabling low-latency catchup logic");
  } else {
    catchup_.set_enabled(auto_catchup_);
  }

  if (!low_bitrate_src_name_.empty())
    toggle_source_visibility(low_bitrate_src_name_, false);
  if (!disconnect_src_name_.empty())
    toggle_source_visibility(disconnect_src_name_, false);
  if (!loading_src_name_.empty())
    toggle_source_visibility(loading_src_name_, false);
  prev_loading_.store(false);

  if (!loading_src_name_.empty()) {
    toggle_source_visibility(loading_src_name_, true);
    prev_loading_.store(true);
  }

  write_stats_json();

  connected_.store(false);
  worker_thread_ = std::thread(&LiveStreamSource::worker_thread_func, this);
  shimmer_thread_ = std::thread(&LiveStreamSource::shimmer_thread_func, this);
}

void LiveStreamSource::stop_stream() {
  lss_log_debug("stop_stream: begin");
  running_.store(false);

  lss_log_debug("stop_stream: requesting abort");
  demuxer_.request_abort();

  lss_log_debug("stop_stream: joining worker thread");
  if (worker_thread_.joinable())
    worker_thread_.join();

  lss_log_debug("stop_stream: joining connect thread");
  if (connect_thread_.joinable())
    connect_thread_.join();

  lss_log_debug("stop_stream: joining shimmer thread");
  if (shimmer_thread_.joinable())
    shimmer_thread_.join();

  lss_log_debug("stop_stream: closing demuxer");
  demuxer_.close();

  lss_log_debug("stop_stream: flushing video decoder");
  video_dec_.flush([](AVFrame *) {}); // Flush to release HW surfaces

  lss_log_debug("stop_stream: closing video decoder");
  video_dec_.close();

  lss_log_debug("stop_stream: closing audio decoder");
  audio_dec_.close();
  bitrate_mon_.reset();
  connected_.store(false);
  first_frame_received_.store(false);
  ever_received_frame_.store(false);

  obs_source_output_video2(obs_source_, nullptr);

  width_.store(0);
  height_.store(0);

  has_pts_offset_ = false;
  pts_to_obs_offset_ns_ = 0;
  first_video_pts_ns_ = 0;
  first_audio_pts_ns_ = 0;
  last_video_pts_us_.store(0);
  last_audio_pts_us_.store(0);
  catching_up_ = false;
  catchup_first_wall_ms_ = 0;
  catchup_first_pts_ms_ = 0;
  pipeline_latency_ms_.store(0);
  has_delay_ref_ = false;
  stream_delay_ms_.store(0);

  // Flush queues to discard stale frames
  if (video_queue_) video_queue_->flush();
  if (audio_queue_) audio_queue_->flush();

  if (!loading_src_name_.empty())
    toggle_source_visibility(loading_src_name_, false);
  prev_loading_.store(false);
  lss_log_debug("stop_stream: done");
}

bool LiveStreamSource::try_connect() {
  connected_.store(false);
  demuxer_.close();
  video_dec_.close();
  audio_dec_.close();

  // Complete state reset for clean reconnection
  has_pts_offset_ = false;
  pts_to_obs_offset_ns_ = 0;
  first_video_pts_ns_ = 0;
  first_audio_pts_ns_ = 0;
  catching_up_ = false;
  catchup_first_wall_ms_ = 0;
  catchup_first_pts_ms_ = 0;
  has_delay_ref_ = false;
  last_video_pts_us_.store(0);
  last_audio_pts_us_.store(0);
  total_bytes_audio_.store(0);
  total_bytes_video_.store(0);
  pipeline_latency_ms_.store(0);
  stream_delay_ms_.store(0);
  
  // Flush queues
  if (video_queue_) {
    video_queue_->flush();
  }
  if (audio_queue_) {
    audio_queue_->flush();
  }
  
  // Reset auto buffer flag for re-detection
  if (buffer_preset_ == BufferPreset::Auto) {
    auto_buffer_adjusted_ = false;
  }

  // before attempting to re-initialize.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  if (demuxer_.open(url_, stream_type_) < 0)
    return false;

  if (demuxer_.video_stream_index() >= 0) {
    video_dec_.init(demuxer_.video_codecpar(), hw_decode_);
    video_dec_.set_stream_time_base(demuxer_.video_time_base());
    video_codec_name_ = avcodec_get_name(demuxer_.video_codecpar()->codec_id);
  } else {
    video_codec_name_ = "none";
  }
  if (demuxer_.audio_stream_index() >= 0) {
    audio_dec_.init(demuxer_.audio_codecpar());
    audio_dec_.set_stream_time_base(demuxer_.audio_time_base());
    audio_codec_name_ = avcodec_get_name(demuxer_.audio_codecpar()->codec_id);
  } else {
    audio_codec_name_ = "none";
  }

  connected_.store(true);
  last_connect_ms_.store(now_ms());
  reconnect_count_.fetch_add(1);

  if (stream_type_ == StreamType::AmazonIVS) {
    catching_up_ = false;
    catchup_first_wall_ms_ = 0;
    catchup_first_pts_ms_ = 0;
    last_pkt_recv_ms_ = now_ms();
    lss_log_debug("IVS Mode: Custom LL-HLS  - no catchup (live edge data)");
  }

  return true;
}

//  Source Toggles

struct SourceTaskData {
  std::string name;
  bool visible;
};

struct FindItemData {
  const char *name;
  bool visible;
};

static bool set_visibility_in_scene(void *param, obs_source_t *scene_source) {
  auto *d = static_cast<FindItemData *>(param);

  obs_scene_t *scene = obs_scene_from_source(scene_source);
  if (!scene)
    return true;

  obs_sceneitem_t *item = obs_scene_find_source(scene, d->name);
  if (item) {
    obs_sceneitem_set_visible(item, d->visible);
  }

  return true; // continue to set in all scenes
}

static void source_visibility_task(void *param) {
  auto *d = static_cast<SourceTaskData *>(param);
  if (!d)
    return;

  FindItemData find{d->name.c_str(), d->visible};
  obs_enum_scenes(set_visibility_in_scene, &find);

  delete d;
}

void LiveStreamSource::video_tick(float seconds) {
  (void)seconds;
  // causes D3D11 crashes because avcodec_free_context releases
}

void LiveStreamSource::update_source_toggles() {
  bool connected = connected_.load();
  bool is_currently_low = bitrate_mon_.is_low();
  bool was_showing = prev_low_bitrate_.load();
  int64_t now = now_ms();
  bool low_now;

  if (!connected) {
    low_now = false;
  } else if ((now - last_connect_ms_.load()) < 5000) {
    low_now = false;
  } else if (!was_showing) {
    low_now = is_currently_low;
  } else {
    int64_t shown_since = last_low_bitrate_time_ms_.load();
    if ((now - shown_since) < 2000) {
      low_now = true;
    } else {
      low_now = is_currently_low;
    }
  }

  if (low_now != was_showing) {
    if (low_now) {
      last_low_bitrate_time_ms_.store(now);
    }
    if (!low_bitrate_src_name_.empty()) {
      toggle_source_visibility(low_bitrate_src_name_, low_now);
      lss_log_debug("[TOGGLE] Low-bitrate '%s' -> %s",
                    low_bitrate_src_name_.c_str(),
                    low_now ? "VISIBLE" : "HIDDEN");
    }
    prev_low_bitrate_.store(low_now);
  }

  bool ever_frame = ever_received_frame_.load();
  int attempts = reconnect_mgr_.get_attempts();
  bool first_frame = first_frame_received_.load();

  bool dis_now = false;
  bool loading_now = false;

  // 1. We are connected but buffering (no frame yet)
  bool buffering = connected && !first_frame;
  
  // 2. Initial grace period: only at the very beginning (no frames ever received)
  bool initial_connecting = !ever_frame && (attempts <= 2);

  lss_log_debug("[TOGGLE] State: connected=%d first_frame=%d ever_frame=%d attempts=%d buffering=%d initial=%d",
                connected, first_frame, ever_frame, attempts, buffering, initial_connecting);

  if (buffering) {
    // Connected but waiting for first frame - show loading
    loading_now = true;
  } else if (initial_connecting) {
    // Initial connection attempts - show loading
    loading_now = true;
  } else if (!connected) {
    // Disconnected after receiving frames - show disconnect
    dis_now = true;
  }

  lss_log_debug("[TOGGLE] Decision: loading_now=%d dis_now=%d", loading_now, dis_now);

  bool dis_showing = prev_disconnected_.load();
  if (dis_now != dis_showing) {
    lss_log_info("[TOGGLE] Disconnect state changed: connected=%d dis_now=%d "
                  "dis_showing=%d ever_frame=%d attempts=%d src='%s'",
                  connected, dis_now, dis_showing, ever_frame, attempts,
                  disconnect_src_name_.c_str());

    if (dis_now)
      obs_source_output_video2(obs_source_, nullptr);

    if (!disconnect_src_name_.empty()) {
      toggle_source_visibility(disconnect_src_name_, dis_now);
      lss_log_debug("[TOGGLE] Disconnect source '%s' -> %s",
                    disconnect_src_name_.c_str(),
                    dis_now ? "VISIBLE" : "HIDDEN");
    }
    prev_disconnected_.store(dis_now);
  }

  bool loading_was = prev_loading_.load();
  if (loading_now != loading_was) {
    lss_log_debug(
        "[TOGGLE] Loading state changed: loading_now=%d loading_was=%d "
        "connected=%d first_frame=%d ever_frame=%d src='%s'",
        loading_now, loading_was, connected, first_frame_received_.load(),
        ever_received_frame_.load(), loading_src_name_.c_str());
    if (!loading_src_name_.empty()) {
      toggle_source_visibility(loading_src_name_, loading_now);
      lss_log_debug("[TOGGLE] Loading source '%s' -> %s",
                    loading_src_name_.c_str(),
                    loading_now ? "VISIBLE" : "HIDDEN");
    }
    prev_loading_.store(loading_now);
  }
}

void LiveStreamSource::toggle_source_visibility(const std::string &name,
                                                bool visible) {
  if (name.empty())
    return;
  auto *data = new SourceTaskData{name, visible};
  obs_queue_task(OBS_TASK_UI, source_visibility_task, data, false);
}

//  Statistics

void LiveStreamSource::init_stats_dir() {
  char path[512];
  int ret = os_get_config_path(path, sizeof(path),
                               "obs-studio/plugin_config/live-stream-source");
  if (ret > 0) {
    stats_dir_ = path;
    os_mkdir(stats_dir_.c_str());
    stats_html_path_ = stats_dir_ + "/dashboard.html";
    overlay_html_path_ = stats_dir_ + "/overlay.html";
  }
}

void LiveStreamSource::write_stats_json() {
  auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - stream_start_time_)
                      .count();

  std::ostringstream fps_str;
  fps_str << std::fixed << std::setprecision(2) << current_fps_.load();

  const char *src_name = obs_source_get_name(obs_source_);
  std::string source_name = src_name ? src_name : "Unknown";

  std::ostringstream ss;
  ss << "{";
  ss << "\"connected\":" << (connected_.load() ? "true" : "false") << ",";
  ss << "\"width\":" << width_.load() << ",";
  ss << "\"height\":" << height_.load() << ",";
  ss << "\"kbps\":" << static_cast<int>(bitrate_mon_.current_kbps()) << ",";
  ss << "\"fps\":" << fps_str.str() << ",";
  ss << "\"latency_ms\":" << pipeline_latency_ms_.load() << ",";
  ss << "\"total_decoded\":" << total_frames_decoded_.load() << ",";
  ss << "\"dropped_frames\":" << total_frames_dropped_.load() << ",";
  ss << "\"dropped_decode\":" << total_frames_dropped_decode_.load() << ",";
  ss << "\"dropped_latency\":" << total_frames_dropped_latency_.load() << ",";
  ss << "\"total_bytes_video\":" << total_bytes_video_.load() << ",";
  ss << "\"total_bytes_audio\":" << total_bytes_audio_.load() << ",";
  ss << "\"reconnects\":" << reconnect_count_.load() << ",";
  ss << "\"catchup_active\":" << (catchup_.is_catching_up() ? "true" : "false")
     << ",";
  ss << "\"catchup_drift_ms\":" << catchup_.drift_ms() << ",";
  ss << "\"hw_accel\":" << (video_dec_.is_hw_active() ? "true" : "false")
     << ",";
  ss << "\"video_codec\":\"" << video_codec_name_ << "\",";
  ss << "\"audio_codec\":\"" << audio_codec_name_ << "\",";
  ss << "\"stream_delay_ms\":" << stream_delay_ms_.load() << ",";
  ss << "\"uptime_s\":" << uptime_s;
  ss << "}";

  std::string json = ss.str();

  WsStatsServer::instance().update_source(source_name, json);
}

void LiveStreamSource::install_stats_html() {
  if (stats_html_path_.empty())
    return;
  std::ofstream out(stats_html_path_);
  out << DASHBOARD_HTML;
}

void LiveStreamSource::install_overlay_html() {
  if (overlay_html_path_.empty())
    return;
  std::ofstream out(overlay_html_path_);
  out << OVERLAY_HTML;
}

bool LiveStreamSource::on_open_stats_clicked(obs_properties_t *,
                                             obs_property_t *, void *data) {
  auto *self = static_cast<LiveStreamSource *>(data);
  if (!self || self->stats_html_path_.empty())
    return false;

  std::string path = self->stats_html_path_;
  std::replace(path.begin(), path.end(), '\\', '/');

  std::string url = "file:///" + path;
  url += "?ip=" + get_plugin_bind_ip();
  url += "&port=" + std::to_string(get_plugin_port());

#ifdef _WIN32
  ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif __APPLE__
  std::thread([url]() {
    // operator
    std::string cmd = "open '" + url + "'";
    system(cmd.c_str());
  }).detach();
#else
  std::thread([url]() {
    // operator
    std::string cmd = "xdg-open '" + url + "'";
    system(cmd.c_str());
  }).detach();
#endif
  return true;
}

bool LiveStreamSource::on_show_window_stats_clicked(obs_properties_t *,
                                                    obs_property_t *, void *) {
  PluginDialog::show_instance();
  return true;
}

struct AddOverlayTaskData {
  std::string overlay_url;
  std::string source_name;
  bool is_vertical;
};

static void add_overlay_browser_source_task(void *param) {
  auto *d = static_cast<AddOverlayTaskData *>(param);
  if (!d)
    return;

  obs_source_t *scene_source = obs_frontend_get_current_scene();
  if (!scene_source) {
    delete d;
    return;
  }

  obs_scene_t *scene = obs_scene_from_source(scene_source);
  if (!scene) {
    obs_source_release(scene_source);
    delete d;
    return;
  }

  std::string overlay_name = d->source_name + " - Stats Overlay";

  obs_sceneitem_t *existing =
      obs_scene_find_source(scene, overlay_name.c_str());
  if (existing) {
    lss_log_info("Stats overlay '%s' already exists in scene",
                 overlay_name.c_str());
    obs_source_release(scene_source);
    delete d;
    return;
  }

  obs_data_t *browser_settings = obs_data_create();
  obs_data_set_string(browser_settings, "url", d->overlay_url.c_str());

  if (d->is_vertical) {
    obs_data_set_int(browser_settings, "width", 200);
    obs_data_set_int(browser_settings, "height", 600);
  } else {
    obs_data_set_int(browser_settings, "width", 1200);
    obs_data_set_int(browser_settings, "height", 120);
  }
  obs_data_set_bool(browser_settings, "shutdown", true);
  obs_data_set_int(browser_settings, "fps", 30);

  obs_source_t *browser_source = obs_source_create(
      "browser_source", overlay_name.c_str(), browser_settings, nullptr);
  obs_data_release(browser_settings);

  if (!browser_source) {
    lss_log_error("Failed to create browser source for overlay");
    obs_source_release(scene_source);
    delete d;
    return;
  }

  obs_sceneitem_t *item = obs_scene_add(scene, browser_source);
  if (item) {
    struct vec2 scale;
    scale.x = 0.5f;
    scale.y = 0.5f;
    obs_sceneitem_set_scale(item, &scale);
    lss_log_info("Stats overlay '%s' added to scene (1200x120 @ 0.5x scale)",
                 overlay_name.c_str());
  }

  obs_source_release(browser_source);
  obs_source_release(scene_source);
  delete d;
}

bool LiveStreamSource::on_add_overlay_horz_clicked(obs_properties_t *,
                                                   obs_property_t *,
                                                   void *data) {
  auto *self = static_cast<LiveStreamSource *>(data);
  if (!self || self->overlay_html_path_.empty())
    return false;

  std::string path = self->overlay_html_path_;
  std::replace(path.begin(), path.end(), '\\', '/');

  std::string url = "file:///" + path;
  url += "?ip=" + get_plugin_bind_ip();
  url += "&port=" + std::to_string(get_plugin_port());
  url += "&shadow=1&bg=rgba(0,0,0,0.5)";

  const char *src_name = obs_source_get_name(self->obs_source_);
  std::string source_name = src_name ? src_name : "Live Stream";

  url += "&source=" + source_name;

  auto *task_data = new AddOverlayTaskData{url, source_name, false};
  obs_queue_task(OBS_TASK_UI, add_overlay_browser_source_task, task_data,
                 false);

  return true;
}

bool LiveStreamSource::on_add_overlay_vert_clicked(obs_properties_t *,
                                                   obs_property_t *,
                                                   void *data) {
  auto *self = static_cast<LiveStreamSource *>(data);
  if (!self || self->overlay_html_path_.empty())
    return false;

  std::string path = self->overlay_html_path_;
  std::replace(path.begin(), path.end(), '\\', '/');

  std::string url = "file:///" + path;
  url += "?ip=" + get_plugin_bind_ip();
  url += "&port=" + std::to_string(get_plugin_port());
  url += "&shadow=1&bg=rgba(0,0,0,0.5)";

  const char *src_name = obs_source_get_name(self->obs_source_);
  std::string source_name = src_name ? src_name : "Live Stream";

  url += "&source=" + source_name;

  auto *task_data = new AddOverlayTaskData{url, source_name, true};
  obs_queue_task(OBS_TASK_UI, add_overlay_browser_source_task, task_data,
                 false);

  return true;
}

bool LiveStreamSource::on_refresh_clicked(obs_properties_t *, obs_property_t *,
                                          void *data) {
  auto *self = static_cast<LiveStreamSource *>(data);
  if (!self)
    return false;

  // Don't restart directly from the UI thread  - unsafe!
  self->pending_refresh_.store(true);

  obs_source_update(self->obs_source_, nullptr);

  return true;
}

struct PopulateListData {
  obs_property_t *list;
  obs_source_t *self_source;
};

static bool enum_scene_items_proc(obs_scene_t *, obs_sceneitem_t *item,
                                  void *param) {
  auto *d = static_cast<PopulateListData *>(param);
  obs_source_t *source = obs_sceneitem_get_source(item);

  if (source == d->self_source)
    return true;

  const char *name = obs_source_get_name(source);
  if (name && *name)
    obs_property_list_add_string(d->list, name, name);

  return true;
}

static bool find_scene_proc(void *param, obs_source_t *scene_source) {
  auto *d = static_cast<PopulateListData *>(param);

  obs_scene_t *scene = obs_scene_from_source(scene_source);
  if (!scene)
    return true; // not a scene, continue

  const char *self_name = obs_source_get_name(d->self_source);
  obs_sceneitem_t *item = obs_scene_find_source(scene, self_name);
  if (!item)
    return true; // our source not in this scene, continue

  obs_scene_enum_items(scene, enum_scene_items_proc, d);

  return false; // stop after first matching scene
}

void LiveStreamSource::populate_source_list(obs_property_t *list,
                                            obs_source_t *self_source) {
  obs_property_list_clear(list);
  obs_property_list_add_string(list, "(None)", "");

  if (!self_source)
    return;

  PopulateListData data{list, self_source};
  obs_enum_scenes(find_scene_proc, &data);
}

//  Static Dispatchers

void *LiveStreamSource::create(obs_data_t *settings, obs_source_t *source) {
  return new LiveStreamSource(source, settings);
}
void LiveStreamSource::destroy(void *data) {
  delete static_cast<LiveStreamSource *>(data);
}
void LiveStreamSource::s_update(void *d, obs_data_t *s) {
  static_cast<LiveStreamSource *>(d)->update(s);
}
void LiveStreamSource::s_activate(void *d) {
  static_cast<LiveStreamSource *>(d)->activate();
}
void LiveStreamSource::s_deactivate(void *d) {
  static_cast<LiveStreamSource *>(d)->deactivate();
}
void LiveStreamSource::s_video_tick(void *d, float seconds) {
  static_cast<LiveStreamSource *>(d)->video_tick(seconds);
}

bool LiveStreamSource::s_stream_type_modified(obs_properties_t *props,
                                              obs_property_t *,
                                              obs_data_t *settings) {
  auto type =
      static_cast<StreamType>(obs_data_get_int(settings, PROP_STREAM_TYPE));
  bool is_whep = (type == StreamType::WHEP);

  obs_property_t *p_token = obs_properties_get(props, PROP_WHEP_TOKEN);
  obs_property_t *p_mode = obs_properties_get(props, PROP_WHEP_MODE);

  if (p_token)
    obs_property_set_visible(p_token, is_whep);
  if (p_mode)
    obs_property_set_visible(p_mode, is_whep);

  return true;
}

bool LiveStreamSource::s_buffer_preset_modified(obs_properties_t *props,
                                                obs_property_t *,
                                                obs_data_t *settings) {
  int preset = static_cast<int>(obs_data_get_int(settings, PROP_BUFFER_PRESET));
  bool is_custom = (preset == static_cast<int>(BufferPreset::Custom));
  
  obs_property_t *custom_prop = obs_properties_get(props, PROP_BUFFER_CUSTOM);
  if (custom_prop) {
    obs_property_set_visible(custom_prop, is_custom);
  }
  
  return true;
}

const char *LiveStreamSource::s_get_name(void *) {
  return obs_module_text("LiveStreamSource");
}

obs_properties_t *LiveStreamSource::get_properties(void *data) {
  LiveStreamSource *self = static_cast<LiveStreamSource *>(data);
  obs_properties_t *props = obs_properties_create();

  obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

  obs_properties_add_text(props, PROP_URL, obs_module_text("StreamURL"),
                          OBS_TEXT_DEFAULT);

  obs_property_t *st = obs_properties_add_list(
      props, PROP_STREAM_TYPE, obs_module_text("StreamType"),
      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(st, obs_module_text("TypeRTMP"),
                            static_cast<int>(StreamType::Standard));
  obs_property_list_add_int(st, obs_module_text("TypeIVS"),
                            static_cast<int>(StreamType::AmazonIVS));
  obs_property_list_add_int(st, obs_module_text("TypeHLS"),
                            static_cast<int>(StreamType::HLS));
  obs_property_list_add_int(st, obs_module_text("TypeWHEP"),
                            static_cast<int>(StreamType::WHEP));

  obs_property_set_modified_callback(st, s_stream_type_modified);

  obs_properties_add_text(props, PROP_WHEP_TOKEN, obs_module_text("WHEPToken"),
                          OBS_TEXT_PASSWORD);

  obs_property_t *wm = obs_properties_add_list(
      props, PROP_WHEP_MODE, obs_module_text("WHEPMode"), OBS_COMBO_TYPE_LIST,
      OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(wm, obs_module_text("WHEPModeAuto"),
                            static_cast<int>(WhepClient::WhepMode::Auto));
  obs_property_list_add_int(wm, obs_module_text("WHEPModeVideoAudio"),
                            static_cast<int>(WhepClient::WhepMode::VideoAudio));
  obs_property_list_add_int(wm, obs_module_text("WHEPModeVideoOnly"),
                            static_cast<int>(WhepClient::WhepMode::VideoOnly));
  obs_property_list_add_int(wm, obs_module_text("WHEPModeAudioOnly"),
                            static_cast<int>(WhepClient::WhepMode::AudioOnly));

  obs_properties_add_button(props, PROP_REFRESH, obs_module_text("RefreshBtn"),
                            on_refresh_clicked);
  obs_properties_add_bool(props, PROP_HW_DECODE, obs_module_text("HwDecode"));
  obs_properties_add_bool(props, PROP_SHOW_SHIMMER, obs_module_text("ShowShimmer"));
  obs_properties_add_int(props, PROP_LOW_BITRATE,
                         obs_module_text("LowBitrateThreshold"), 10, 10000, 10);

  // Buffer preset dropdown
  obs_property_t *buffer_preset = obs_properties_add_list(
      props, PROP_BUFFER_PRESET, obs_module_text("BufferPreset"),
      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(buffer_preset, obs_module_text("BufferAuto"),
                            static_cast<int>(BufferPreset::Auto));
  obs_property_list_add_int(buffer_preset, obs_module_text("BufferUltraLowLatency"),
                            static_cast<int>(BufferPreset::UltraLowLatency));
  obs_property_list_add_int(buffer_preset, obs_module_text("BufferLowLatency"),
                            static_cast<int>(BufferPreset::LowLatency));
  obs_property_list_add_int(buffer_preset, obs_module_text("BufferBalanced"),
                            static_cast<int>(BufferPreset::Balanced));
  obs_property_list_add_int(buffer_preset, obs_module_text("BufferStable"),
                            static_cast<int>(BufferPreset::Stable));
  obs_property_list_add_int(buffer_preset, obs_module_text("BufferMaxStability"),
                            static_cast<int>(BufferPreset::MaxStability));
  obs_property_list_add_int(buffer_preset, obs_module_text("BufferCustom"),
                            static_cast<int>(BufferPreset::Custom));
  
  obs_property_set_modified_callback(buffer_preset, s_buffer_preset_modified);
  
  // Custom buffer size (only visible when Custom selected)
  obs_properties_add_int(props, PROP_BUFFER_CUSTOM,
                         obs_module_text("CustomBufferFrames"),
                         BUFFER_SIZE_MIN, BUFFER_SIZE_MAX, 5);

  obs_property_t *lb = obs_properties_add_list(
      props, PROP_LOW_BITRATE_SOURCE, obs_module_text("LowBitrateSource"),
      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_t *dc = obs_properties_add_list(
      props, PROP_DISCONNECT_SOURCE, obs_module_text("DisconnectSource"),
      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_t *ld = obs_properties_add_list(
      props, PROP_LOADING_SOURCE, obs_module_text("LoadingSource"),
      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

  if (self) {
    populate_source_list(lb, self->obs_source_);
    populate_source_list(dc, self->obs_source_);
    populate_source_list(ld, self->obs_source_);
  }

  obs_properties_add_button(props, PROP_OPEN_STATS,
                            obs_module_text("OpenStatsDash"),
                            on_open_stats_clicked);

  obs_properties_add_button(props, "show_window_stats",
                            obs_module_text("ShowStatsWin"),
                            on_show_window_stats_clicked);

  obs_properties_add_button(props, PROP_ADD_OVERLAY_HORZ,
                            obs_module_text("AddStatsOverlayHorz"),
                            on_add_overlay_horz_clicked);

  obs_properties_add_button(props, PROP_ADD_OVERLAY_VERT,
                            obs_module_text("AddStatsOverlayVert"),
                            on_add_overlay_vert_clicked);

  return props;
}

void LiveStreamSource::get_defaults(obs_data_t *settings) {
  obs_data_set_default_string(settings, PROP_URL, "rtmp://localhost/live/test");
  obs_data_set_default_int(settings, PROP_STREAM_TYPE,
                           static_cast<int>(StreamType::Standard));
  obs_data_set_default_int(settings, PROP_LOW_BITRATE, DEFAULT_LOW_BITRATE_KBPS);
  obs_data_set_default_bool(settings, PROP_AUTO_CATCHUP, true);
  obs_data_set_default_bool(settings, PROP_HW_DECODE, true);
  obs_data_set_default_bool(settings, PROP_SHOW_SHIMMER, true);
  obs_data_set_default_int(settings, PROP_WHEP_MODE,
                           static_cast<int>(WhepClient::WhepMode::Auto));
  obs_data_set_default_int(settings, PROP_BUFFER_PRESET,
                           static_cast<int>(BufferPreset::Auto));
  obs_data_set_default_int(settings, PROP_BUFFER_CUSTOM, 60);
}

//
// WHEP WebRTC Worker Thread
//

void LiveStreamSource::whep_worker() {
  lss_log_debug("[WHEP] Worker thread started  - URL: %s", url_.c_str());

  WhepClient client;

  const AVCodec *h264_decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!h264_decoder) {
    lss_log_error("[WHEP] H.264 decoder not found");
    return;
  }
  AVCodecContext *h264_ctx = avcodec_alloc_context3(h264_decoder);
  h264_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
  h264_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
  h264_ctx->thread_count = 1; // back to 1 for stability (no green artifacts)
  h264_ctx->thread_type = 0;  // disable slice threading
  h264_ctx->delay = 0;
  av_opt_set(h264_ctx->priv_data, "tune", "zerolatency", 0);
  if (avcodec_open2(h264_ctx, h264_decoder, nullptr) < 0) {
    lss_log_error("[WHEP] Failed to open H.264 decoder");
    avcodec_free_context(&h264_ctx);
    return;
  }
  lss_log_debug("[WHEP] H.264 decoder initialized");

  const AVCodec *opus_codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
  AVCodecContext *opus_ctx = nullptr;
  if (opus_codec) {
    opus_ctx = avcodec_alloc_context3(opus_codec);
    opus_ctx->sample_rate = 48000;
    opus_ctx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    if (avcodec_open2(opus_ctx, opus_codec, nullptr) < 0) {
      lss_log_warn("[WHEP] Failed to open Opus decoder");
      avcodec_free_context(&opus_ctx);
      opus_ctx = nullptr;
    } else {
      lss_log_debug("[WHEP] Opus decoder initialized (48kHz stereo)");
    }
  }

  AVFrame *dec_frame = av_frame_alloc();
  AVPacket *dec_pkt = av_packet_alloc();

  client.set_video_callback([this, h264_ctx, dec_frame,
                             dec_pkt](const uint8_t *data, size_t len,
                                      uint64_t timestamp) {
    if (!running_.load())
      return;

    dec_pkt->data = const_cast<uint8_t *>(data);
    dec_pkt->size = static_cast<int>(len);

    total_bytes_video_.fetch_add(len);

    // wall-clock.
    if (video_base_pts_ns_ == 0) {
      video_base_pts_ns_ = (uint64_t)os_gettime_ns();
      video_start_pts_rtp_ = (uint32_t)timestamp;
    }

    uint32_t delta_rtp = (uint32_t)timestamp - video_start_pts_rtp_;
    uint64_t current_pts =
        video_base_pts_ns_ + ((uint64_t)delta_rtp * 100000ULL / 9ULL);

    dec_pkt->pts = (int64_t)current_pts;
    dec_pkt->dts = dec_pkt->pts;

    int ret = avcodec_send_packet(h264_ctx, dec_pkt);
    if (ret < 0)
      return;

    while ((ret = avcodec_receive_frame(h264_ctx, dec_frame)) == 0) {
      if (!dec_frame->data[0] || dec_frame->width <= 0 ||
          dec_frame->height <= 0)
        continue;

      // Update source dimensions
      width_.store(dec_frame->width);
      height_.store(dec_frame->height);

      // Direct output for WHEP  - bypass all HLS pacing logic
      video_format obs_fmt =
          convert_pixel_format(static_cast<AVPixelFormat>(dec_frame->format));
      if (obs_fmt == VIDEO_FORMAT_NONE) {
        if (dec_frame->format == AV_PIX_FMT_YUVJ420P)
          obs_fmt = VIDEO_FORMAT_I420;
        else
          continue;
      }

      bool full_range = (dec_frame->color_range == AVCOL_RANGE_JPEG) ||
                        (dec_frame->format == AV_PIX_FMT_YUVJ420P);

      obs_source_frame2 obs_frame = {};
      obs_frame.width = dec_frame->width;
      obs_frame.height = dec_frame->height;
      obs_frame.format = obs_fmt;
      obs_frame.range = full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;

      for (int i = 0; i < MAX_AV_PLANES; i++) {
        obs_frame.data[i] = dec_frame->data[i];
        obs_frame.linesize[i] = static_cast<uint32_t>(dec_frame->linesize[i]);
      }

      enum video_colorspace cs =
          (dec_frame->height >= 720) ? VIDEO_CS_709 : VIDEO_CS_601;
      video_format_get_parameters_for_format(
          cs, obs_frame.range, obs_fmt, obs_frame.color_matrix,
          obs_frame.color_range_min, obs_frame.color_range_max);

      // Use RTP-anchored PTS directly  - no drift correction, no pacing sleep
      obs_frame.timestamp = current_pts;

      obs_source_output_video2(obs_source_, &obs_frame);
      total_frames_decoded_.fetch_add(1);
      whep_last_media_ms_.store(now_ms());
      whep_last_video_packet_ms_.store(now_ms());

      // Track bitrate for low-bitrate detection
      bitrate_mon_.record_bytes(static_cast<int>(len));

      // Reconnection: if connected_ was set to false (e.g. video timeout),
      // restore it now that frames are flowing again
      if (!connected_.load()) {
        connected_.store(true);
        last_connect_ms_.store(now_ms());
        lss_log_debug("[WHEP] Video resumed  - marking as connected again");
      }

      if (!first_frame_received_.load()) {
        first_frame_received_.store(true);
        ever_received_frame_.store(true);
        stream_start_time_ = SteadyClock::now();
        update_source_toggles();
        lss_log_debug("[WHEP] First video frame decoded (%dx%d)",
                      dec_frame->width, dec_frame->height);
      }
    }
  });

  // Audio callback: Opus frame -> decode -> output
  if (opus_ctx) {
    client.set_audio_callback([this, opus_ctx, dec_frame,
                               dec_pkt](const uint8_t *data, size_t len,
                                        uint64_t timestamp) {
      if (!running_.load())
        return;

      AVPacket *audio_pkt = av_packet_alloc();
      audio_pkt->data = const_cast<uint8_t *>(data);
      audio_pkt->size = static_cast<int>(len);
      audio_pkt->pts = static_cast<int64_t>(timestamp);

      total_bytes_audio_.fetch_add(len);

      AVFrame *audio_frame = av_frame_alloc();
      int ret = avcodec_send_packet(opus_ctx, audio_pkt);
      if (ret >= 0) {
        while (avcodec_receive_frame(opus_ctx, audio_frame) == 0) {
          // Convert to OBS audio
          struct obs_source_audio obs_audio = {};
          obs_audio.samples_per_sec = audio_frame->sample_rate;
          obs_audio.frames = audio_frame->nb_samples;

          // Audio Sync
          // Use sample-count based timing for absolute smoothness
          if (audio_base_pts_ns_ == 0) {
            audio_start_pts_rtp_ = (uint32_t)timestamp;
            if (video_base_pts_ns_ == 0) {
              video_base_pts_ns_ = (uint64_t)os_gettime_ns();
            }
            audio_base_pts_ns_ = video_base_pts_ns_;
            dec_audio_samples_ = 0;
          }

          obs_audio.timestamp =
              audio_base_pts_ns_ + (dec_audio_samples_ * 1000000000ULL / 48000);
          dec_audio_samples_ += audio_frame->nb_samples;

          // Map sample format accurately (Planar vs Interleaved)
          if (audio_frame->format == AV_SAMPLE_FMT_FLTP) {
            obs_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
          } else if (audio_frame->format == AV_SAMPLE_FMT_FLT) {
            obs_audio.format = AUDIO_FORMAT_FLOAT;
          } else if (audio_frame->format == AV_SAMPLE_FMT_S16P) {
            obs_audio.format = AUDIO_FORMAT_16BIT_PLANAR;
          } else if (audio_frame->format == AV_SAMPLE_FMT_S16) {
            obs_audio.format = AUDIO_FORMAT_16BIT;
          } else {
            obs_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
          }

          // Map channel layout
          int channels = audio_frame->ch_layout.nb_channels;
          obs_audio.speakers =
              (channels >= 2) ? SPEAKERS_STEREO : SPEAKERS_MONO;

          // Data pointers
          bool planar = (audio_frame->format == AV_SAMPLE_FMT_FLTP ||
                         audio_frame->format == AV_SAMPLE_FMT_S16P);
          if (planar) {
            for (int ch = 0; ch < channels && ch < MAX_AV_PLANES; ch++)
              obs_audio.data[ch] = audio_frame->data[ch];
          } else {
            obs_audio.data[0] = audio_frame->data[0];
          }

          obs_source_output_audio(obs_source_, &obs_audio);
          whep_last_media_ms_.store(now_ms());

          // Audio alone keeps connection alive
          if (!connected_.load()) {
            connected_.store(true);
            last_connect_ms_.store(now_ms());
            lss_log_debug("[WHEP] Audio resumed  - marking as connected");
          }
        }
      }
      av_frame_free(&audio_frame);
      av_packet_free(&audio_pkt);
    });
  }

  // State callback
  client.set_state_callback([this](const std::string &state) {
    if (state == "connected") {
      connected_.store(true);
      last_connect_ms_.store(now_ms());
      reconnect_count_.fetch_add(1);
      video_codec_name_ = "H.264 (WebRTC)";
      audio_codec_name_ = "Opus (WebRTC)";
      update_source_toggles();
    } else if (state == "disconnected" || state == "failed" ||
               state == "closed") {
      connected_.store(false);
      first_frame_received_.store(false);
      current_fps_.store(0.0);
      pipeline_latency_ms_.store(0);
      stream_delay_ms_.store(0);
      bitrate_mon_.reset();

      // Clear the video frame  - makes source transparent
      obs_source_output_video2(obs_source_, nullptr);

      // Reset PTS anchors for next connection
      video_base_pts_ns_ = 0;
      video_start_pts_rtp_ = 0;

      update_source_toggles();
    }
  });

  // Show shimmer while connecting
  if (!first_frame_received_.load())
    output_shimmer_frame();

  // Start WHEP client
  // atomic or guarded. But settings are in update(). Let's store mode in
  // member.
  WhepClient::WhepMode mode =
      static_cast<WhepClient::WhepMode>(whep_mode_.load());

  if (!client.start(url_, whep_token_, mode)) {
    lss_log_error("[WHEP] Failed to start WHEP client");
    av_frame_free(&dec_frame);
    av_packet_free(&dec_pkt);
    avcodec_free_context(&h264_ctx);
    if (opus_ctx)
      avcodec_free_context(&opus_ctx);
    return;
  }

  // Main loop: wait for connection + keep alive
  while (running_.load()) {
    // Show shimmer while waiting for first frame
    if (!first_frame_received_.load()) {
      int64_t now_t = now_ms();
      if (now_t - last_shimmer_ms_ >= 33) {
        output_shimmer_frame();
        last_shimmer_ms_ = now_t;
      }
    }

    // Periodically update source toggles (disconnect, low-bitrate, loading)
    update_source_toggles();

    // Update stats periodically
    int64_t now_t = now_ms();
    if (now_t - stats_last_write_ms_ > STATS_WRITE_INTERVAL_MS) {
      // FPS calculation
      int64_t decoded_now = total_frames_decoded_.load();
      int64_t elapsed = now_t - last_fps_calc_ms_;
      if (last_fps_calc_ms_ > 0 && elapsed > 0) {
        double fps = (decoded_now - last_frames_count_) * 1000.0 / elapsed;
        current_fps_.store(fps);
      }
      last_fps_calc_ms_ = now_t;
      last_frames_count_ = decoded_now;

      // Latency (estimate for WebRTC)
      pipeline_latency_ms_.store(200);

      write_stats_json();
      stats_last_write_ms_ = now_t;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Audio-Only Mode Handling
    // If we are receiving audio (media active) but NO VIDEO (for >200ms),
    // we must generate dummy video frames to keep OBS's video clock running.
    // Otherwise, async video sources won't play audio.
    if (connected_.load()) {
      int64_t now_ms_val = now_ms();
      int64_t last_m = whep_last_media_ms_.load();
      int64_t last_v = whep_last_video_packet_ms_.load();

      // If audio is flowing (seen in last 1s) but video is stalled/missing
      if ((now_ms_val - last_m < 1000) &&
          (last_v == 0 || (now_ms_val - last_v > 200))) {

        static int64_t last_dummy_log = 0;
        if (now_ms_val - last_dummy_log > 10000) {
          lss_log_debug("[WHEP] Audio-Only Logic Active: last_media=%lld "
                        "last_video=%lld now=%lld",
                        (long long)last_m, (long long)last_v,
                        (long long)now_ms_val);
          last_dummy_log = now_ms_val;
        }

        static const uint8_t black_y[4] = {0, 0, 0, 0};
        static const uint8_t black_uv[1] = {128};
        obs_source_frame2 black = {};
        black.width = 2;
        black.height = 2;
        black.format = VIDEO_FORMAT_I420;
        black.range = VIDEO_RANGE_PARTIAL;
        black.data[0] = const_cast<uint8_t *>(black_y);
        black.linesize[0] = 2;
        black.data[1] = const_cast<uint8_t *>(black_uv);
        black.linesize[1] = 1;
        black.data[2] = const_cast<uint8_t *>(black_uv);
        black.linesize[2] = 1;
        black.timestamp = (uint64_t)os_gettime_ns();
        obs_source_output_video2(obs_source_, &black);

        if (!first_frame_received_.load()) {
          first_frame_received_.store(true);
          ever_received_frame_.store(true);
          update_source_toggles();
          lss_log_debug(
              "[WHEP] Audio-only detected: generating dummy video frames");
        }
      }
    }

    // Check if connection lost (ICE/DTLS level)
    if (!client.is_running() && !client.is_connected()) {
      lss_log_warn("[WHEP] Connection lost (ICE/DTLS), stopping");
      connected_.store(false);
      first_frame_received_.store(false);
      current_fps_.store(0.0);
      pipeline_latency_ms_.store(0);
      obs_source_output_video2(obs_source_, nullptr);
      update_source_toggles();
      break;
    }

    // Check if video stream stalled (no frames for 3 seconds)
    int64_t last_v = whep_last_media_ms_.load();
    if (last_v > 0 && first_frame_received_.load()) {
      int64_t silence_ms = now_ms() - last_v;
      if (silence_ms > 3000) {
        lss_log_warn(
            "[WHEP] No video frames for %lld ms  - treating as disconnect",
            (long long)silence_ms);
        connected_.store(false);
        first_frame_received_.store(false);
        current_fps_.store(0.0);
        pipeline_latency_ms_.store(0);
        bitrate_mon_.reset();
        obs_source_output_video2(obs_source_, nullptr);
        update_source_toggles();

        // Reset PTS anchors so re-connection starts fresh
        video_base_pts_ns_ = 0;
        video_start_pts_rtp_ = 0;
        whep_last_media_ms_.store(0);

        // Don't break  - stay in loop so reconnection via PeerConnection
        // state change can bring us back
      }
    }
  }

  // Cleanup
  client.stop();
  av_frame_free(&dec_frame);
  av_packet_free(&dec_pkt);
  avcodec_free_context(&h264_ctx);
  if (opus_ctx)
    avcodec_free_context(&opus_ctx);

  connected_.store(false);
  first_frame_received_.store(false);
  current_fps_.store(0.0);
  pipeline_latency_ms_.store(0);
  bitrate_mon_.reset();
  obs_source_output_video2(obs_source_, nullptr);
  update_source_toggles();
  lss_log_debug("[WHEP] Worker thread finished");
}
struct obs_source_info live_stream_source_info = {};

void register_live_stream_source() {
  live_stream_source_info.id = "live_stream_source";
  live_stream_source_info.type = OBS_SOURCE_TYPE_INPUT;
  live_stream_source_info.icon_type = OBS_ICON_TYPE_MEDIA;
  live_stream_source_info.output_flags =
      OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
  live_stream_source_info.get_name = LiveStreamSource::s_get_name;
  live_stream_source_info.create = LiveStreamSource::create;
  live_stream_source_info.destroy = LiveStreamSource::destroy;
  live_stream_source_info.update = LiveStreamSource::s_update;
  live_stream_source_info.activate = LiveStreamSource::s_activate;
  live_stream_source_info.deactivate = LiveStreamSource::s_deactivate;
  live_stream_source_info.video_tick = LiveStreamSource::s_video_tick;
  live_stream_source_info.get_properties = LiveStreamSource::get_properties;
  live_stream_source_info.get_defaults = LiveStreamSource::get_defaults;

  obs_register_source(&live_stream_source_info);
}

//
// Buffer Calculation Methods
//

int LiveStreamSource::calculate_buffer_size() const {
  if (buffer_preset_ == BufferPreset::Custom) {
    return custom_buffer_frames_;
  } else if (buffer_preset_ == BufferPreset::Auto) {
    return calculate_smart_auto_buffer();
  } else {
    return calculate_preset_buffer(buffer_preset_);
  }
}

int LiveStreamSource::calculate_frames_for_duration(double fps, int target_ms) const {
  if (fps < 5.0) fps = 30.0;  // Fallback
  double frames = (fps * target_ms) / 1000.0;
  return static_cast<int>(std::ceil(frames));
}

int LiveStreamSource::get_preset_target_ms(BufferPreset preset) const {
  switch (preset) {
    case BufferPreset::UltraLowLatency:
      return BUFFER_ULTRA_LOW_LATENCY_MS;
    case BufferPreset::LowLatency:
      return BUFFER_LOW_LATENCY_MS;
    case BufferPreset::Balanced:
      return BUFFER_BALANCED_MS;
    case BufferPreset::Stable:
      return BUFFER_STABLE_MS;
    case BufferPreset::MaxStability:
      return BUFFER_MAX_STABILITY_MS;
    default:
      return BUFFER_STABLE_MS;
  }
}

double LiveStreamSource::get_bitrate_multiplier() const {
  double kbps = bitrate_mon_.current_kbps();
  
  // If bitrate not yet known, assume high bitrate (safe default)
  if (kbps < 100.0) {
    return 1.3;  // Assume high bitrate for initial buffer
  }
  
  if (kbps >= BITRATE_ULTRA_HIGH) {
    return 1.5;
  } else if (kbps >= BITRATE_HIGH) {
    return 1.3;
  } else if (kbps >= BITRATE_MEDIUM_HIGH) {
    return 1.1;
  } else if (kbps >= BITRATE_MEDIUM) {
    return 1.0;
  } else if (kbps >= BITRATE_LOW) {
    return 0.9;
  } else {
    return 0.8;
  }
}

double LiveStreamSource::get_resolution_multiplier() const {
  int w = width_.load();
  int h = height_.load();
  int pixels = w * h;
  
  // If resolution not yet known, assume 1080p (safe default)
  if (pixels == 0) {
    return 1.2;  // Assume 1080p for initial buffer
  }
  
  if (pixels >= 3840 * 2160) {
    return 1.5;  // 4K
  } else if (pixels >= 2560 * 1440) {
    return 1.3;  // 1440p
  } else if (pixels >= 1920 * 1080) {
    return 1.2;  // 1080p
  } else if (pixels >= 1280 * 720) {
    return 1.0;  // 720p (baseline)
  } else if (pixels >= 854 * 480) {
    return 0.8;  // 480p
  } else {
    return 0.7;  // <480p
  }
}

double LiveStreamSource::get_stability_multiplier() const {
  int64_t recent_drops = total_frames_dropped_.load() - last_drop_count_;
  int64_t recent_decoded = total_frames_decoded_.load() - last_decoded_count_;
  
  if (recent_decoded < 100) {
    return 1.0;  // Not enough data yet
  }
  
  double drop_rate = static_cast<double>(recent_drops) / recent_decoded;
  
  if (drop_rate > 0.05) {
    return 1.5;  // >5% drops: very bad network
  } else if (drop_rate > 0.02) {
    return 1.3;  // 2-5% drops: bad network
  } else if (drop_rate > 0.01) {
    return 1.1;  // 1-2% drops: mediocre network
  } else {
    return 1.0;  // <1% drops: good network
  }
}

int LiveStreamSource::calculate_preset_buffer(BufferPreset preset) const {
  // Get target duration
  int target_ms = get_preset_target_ms(preset);
  
  // Get current FPS
  double fps = current_fps_.load();
  if (fps < 5.0) fps = 30.0;  // Fallback
  
  // Calculate base frames for target duration
  int base_frames = calculate_frames_for_duration(fps, target_ms);
  
  // Apply smart multipliers
  double bitrate_mult = get_bitrate_multiplier();
  double resolution_mult = get_resolution_multiplier();
  double stability_mult = get_stability_multiplier();
  
  // Calculate final buffer
  double adjusted = base_frames * bitrate_mult * resolution_mult * stability_mult;
  int final_frames = static_cast<int>(std::round(adjusted));
  
  // Clamp to limits
  return std::clamp(final_frames, BUFFER_SIZE_MIN, BUFFER_SIZE_MAX);
}

int LiveStreamSource::calculate_smart_auto_buffer() const {
  double fps = current_fps_.load();
  
  // Use higher fallback for initial buffer calculation
  // This ensures sufficient buffer for high FPS streams (60fps) before FPS is detected
  if (fps < 5.0) {
    // Check resolution to guess FPS
    int w = width_.load();
    int h = height_.load();
    
    // If resolution not yet known, assume HD 60fps (safe default for modern streams)
    if (w == 0 || h == 0) {
      fps = 60.0;  // Safe default for initial buffer
    }
    // HD/FHD streams are likely 60fps, SD streams likely 30fps
    else if (w >= 1280 && h >= 720) {
      fps = 60.0;  // Assume 60fps for HD+ streams
    } else {
      fps = 30.0;  // Assume 30fps for SD streams
    }
  }
  
  // Round FPS to nearest tier for stable buffer sizing
  // This prevents constant buffer resizing as FPS fluctuates
  static const int tiers[] = {20, 25, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
  static const int tier_count = sizeof(tiers) / sizeof(tiers[0]);
  
  int rounded_fps = tiers[0];  // Default to lowest tier
  double min_diff = std::abs(fps - tiers[0]);
  
  // Find nearest tier
  for (int i = 1; i < tier_count; i++) {
    double diff = std::abs(fps - tiers[i]);
    if (diff < min_diff) {
      min_diff = diff;
      rounded_fps = tiers[i];
    }
  }
  
  // Apply multipliers
  double bitrate_mult = get_bitrate_multiplier();
  double resolution_mult = get_resolution_multiplier();
  double stability_mult = get_stability_multiplier();
  
  double adjusted = rounded_fps * bitrate_mult * resolution_mult * stability_mult;
  int final_frames = static_cast<int>(std::round(adjusted));
  int clamped = std::clamp(final_frames, BUFFER_SIZE_MIN, BUFFER_SIZE_MAX);
  
  // Log calculation details
  lss_log_info("Auto buffer calculation: FPS=%.1f→%d, bitrate_mult=%.2f, res_mult=%.2f, stab_mult=%.2f → %d frames (clamped: %d)",
               fps, rounded_fps, bitrate_mult, resolution_mult, stability_mult, final_frames, clamped);
  
  return clamped;
}

void LiveStreamSource::recreate_frame_queues(int new_capacity) {
  // CRITICAL: This function CANNOT be called from worker thread
  // It would cause deadlock (thread trying to join itself)
  // This should only be called from settings update or initialization
  
  bool was_running = running_.load();
  
  if (was_running) {
    // Pause temporarily
    running_.store(false);
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }
  }
  
  // Flush old queues
  if (video_queue_) {
    video_queue_->flush();
  }
  if (audio_queue_) {
    audio_queue_->flush();
  }
  
  // Create video queue
  video_queue_ = std::make_unique<VideoFrameQueue>(new_capacity);
  
  // Create audio queue with SAME DURATION but different frame count
  // Audio frame rate is different from video frame rate!
  // Video: 60 fps, Audio: ~43 fps (44100 Hz / 1024 samples per frame)
  double fps = current_fps_.load();
  if (fps < 5.0) fps = 60.0;  // Fallback
  
  double target_duration_sec = new_capacity / fps;
  
  // Audio frame rate approximation
  // Most streams: 44100 Hz or 48000 Hz, 1024 samples/frame
  // 44100 / 1024 = ~43 fps
  // 48000 / 1024 = ~47 fps
  int audio_fps = 45;  // Conservative middle ground
  int audio_capacity = static_cast<int>(audio_fps * target_duration_sec);
  audio_capacity = std::clamp(audio_capacity, BUFFER_SIZE_MIN, BUFFER_SIZE_MAX);
  
  audio_queue_ = std::make_unique<AudioFrameQueue>(audio_capacity);
  
  lss_log_info("Frame buffer resized: video=%d frames, audio=%d frames (%.2f sec duration)",
               new_capacity, audio_capacity, target_duration_sec);
  
  // Restart if was running
  if (was_running) {
    running_.store(true);
    worker_thread_ = std::thread(&LiveStreamSource::worker_thread_func, this);
  }
}

void LiveStreamSource::monitor_and_adjust_buffer() {
  int64_t now = now_ms();
  
  // Check every 10 seconds
  if (now - last_buffer_check_ms_ < BUFFER_MONITOR_INTERVAL_MS) {
    return;
  }
  
  last_buffer_check_ms_ = now;
  
  // Update drop counters for stability calculation
  last_drop_count_ = total_frames_dropped_.load();
  last_decoded_count_ = total_frames_decoded_.load();
  
  // Only adjust in Auto mode
  if (buffer_preset_ != BufferPreset::Auto) {
    return;
  }
  
  // Calculate new optimal buffer
  int new_optimal = calculate_buffer_size();
  int current = video_queue_ ? video_queue_->capacity() : FRAME_QUEUE_CAPACITY;
  
  // Only adjust if difference is significant (>20%)
  double diff_percent = std::abs(new_optimal - current) / static_cast<double>(current);
  
  if (diff_percent > BUFFER_ADJUST_THRESHOLD) {
    lss_log_info("Auto buffer: Would adjust %d → %d frames (%.0f%% change) - dynamic resize disabled to prevent deadlock",
                 current, new_optimal, diff_percent * 100);
    // NOTE: Dynamic buffer resizing from worker thread causes deadlock
    // recreate_frame_queues() tries to join the worker thread from within itself
    // Solution: Buffer size is set at initialization and can only be changed via settings
    // This is actually safer - no mid-stream disruptions
    auto_buffer_adjusted_ = true;
  }
}

} // namespace lss
