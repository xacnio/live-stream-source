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
  case AV_PIX_FMT_YUVJ420P:
    return VIDEO_FORMAT_I420;
  case AV_PIX_FMT_NV12:
    return VIDEO_FORMAT_NV12;
  case AV_PIX_FMT_P010LE:
    return VIDEO_FORMAT_P010;
  case AV_PIX_FMT_YUV420P10LE:
    return VIDEO_FORMAT_I010;
  case AV_PIX_FMT_YUYV422:
    return VIDEO_FORMAT_YUY2;
  case AV_PIX_FMT_UYVY422:
    return VIDEO_FORMAT_UYVY;
  case AV_PIX_FMT_BGRA:
    return VIDEO_FORMAT_BGRA;
  case AV_PIX_FMT_YUV444P:
    return VIDEO_FORMAT_I444;
  default:
    return VIDEO_FORMAT_NONE;
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

  orchestrator_ = std::make_unique<CatchupOrchestrator>(
      buffer_manager_, tempo_ramper_, audio_dec_.get_stretcher());

  init_stats_dir();
  install_stats_html();
  install_overlay_html();
  WsStatsServer::instance().add_ref();
  const char *src_name = obs_source_get_name(obs_source_);
  if (src_name && *src_name) {
    lss_log_info("LiveStreamSource: registering remote command handler for '%s'",
                 src_name);
    WsStatsServer::instance().register_command_handler(
        src_name, [this](const std::string &cmd) { handle_remote_command(cmd); });
    WsStatsServer::instance().register_connect_callback(src_name, [this]() {
      preview_video_enabled_.store(false);
      preview_audio_enabled_.store(false);
    });
  } else {
    lss_log_warn("LiveStreamSource: source has no name at construction, remote commands disabled");
  }
  update(settings);
}

LiveStreamSource::~LiveStreamSource() {
  stop_stream();
  const char *name = obs_source_get_name(obs_source_);
  if (name) {
    WsStatsServer::instance().unregister_command_handler(name);
    WsStatsServer::instance().unregister_connect_callback(name);
    WsStatsServer::instance().remove_source(name);
  }
  WsStatsServer::instance().release();
  if (blur_sws_down_) { sws_freeContext(blur_sws_down_); blur_sws_down_ = nullptr; }
  if (blur_sws_up_)   { sws_freeContext(blur_sws_up_);   blur_sws_up_   = nullptr; }
}

void LiveStreamSource::update(obs_data_t *settings) {
  bool is_refresh = pending_refresh_.exchange(false);

  std::string new_url = obs_data_get_string(settings, PROP_URL);
  int new_kbps = static_cast<int>(obs_data_get_int(settings, PROP_LOW_BITRATE));
  bool new_catchup = obs_data_get_bool(settings, PROP_AUTO_CATCHUP);
  bool new_hw_decode = obs_data_get_bool(settings, PROP_HW_DECODE);
  auto new_stream_type =
      static_cast<StreamType>(obs_data_get_int(settings, PROP_STREAM_TYPE));

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

  int grace = static_cast<int>(obs_data_get_int(settings, PROP_DISCONNECT_GRACE));
  if (grace < MIN_DISCONNECT_GRACE_MS)
    grace = MIN_DISCONNECT_GRACE_MS;
  if (grace > MAX_DISCONNECT_GRACE_MS)
    grace = MAX_DISCONNECT_GRACE_MS;
  disconnect_grace_ms_.store(grace);

  prev_low_bitrate_.store(!bitrate_mon_.is_low());

  bool needs_restart = is_refresh || url_changed || hw_changed ||
                       type_changed || whep_mode_changed;

  if (needs_restart && active_.load()) {
    if (running_.load()) {
      lss_log_info("Update: signaling background refresh to avoid UI block");
      pending_refresh_.store(true);
    } else if (!url_.empty()) {
      start_stream();
    }
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

  // 1. Calculate base stream_pts_ns
  int64_t stream_pts_ns = 0;
  if (frame->pts > 1000000000000000000LL) {
    stream_pts_ns = frame->pts;
  } else if (frame->pts != AV_NOPTS_VALUE) {
    AVRational tb = demuxer_.video_time_base();
    if (tb.den > 0)
      stream_pts_ns = av_rescale_q(frame->pts, tb, {1, 1000000000});
    else
      stream_pts_ns = static_cast<int64_t>(os_gettime_ns());
  } else {
    stream_pts_ns = static_cast<int64_t>(os_gettime_ns());
  }

  // 2. PTS Anchor & Jump Detection (CRITICAL: Must happen before BufferManager)
  int64_t wall_now_ns = static_cast<int64_t>(os_gettime_ns());
  
  if (has_pts_offset_) {
    int64_t computed_ts = stream_pts_ns + pts_to_obs_offset_ns_;
    int64_t abs_drift = std::abs(computed_ts - wall_now_ns);
    if (abs_drift > 5000LL * 1000000LL) {
        lss_log_warn("PTS Jump detected: drift=%lld ms, resetting anchor", 
                     (computed_ts - wall_now_ns) / 1000000);
        has_pts_offset_ = false;
    }
  }

  if (!has_pts_offset_) {
    first_video_pts_ns_ = stream_pts_ns;
    int64_t headroom_ns = 150LL * 1000000LL; // 150ms headroom for stability
    pts_to_obs_offset_ns_ = wall_now_ns + headroom_ns - stream_pts_ns;
    lss_log_info("PTS anchored from video: pts_ns=%lld ns, offset=%lld ns (150ms headroom)",
                 (long long)stream_pts_ns, (long long)pts_to_obs_offset_ns_);
    has_pts_offset_ = true;
    // Reset old catchup clock so it reinitialises at the new anchor point,
    // not from a stale pre-jump position that would corrupt pts_to_obs_offset_ns_.
    catchup_prev_video_pts_ns_ = 0;
  } else if (first_video_pts_ns_ == 0) {
    first_video_pts_ns_ = stream_pts_ns;
    int64_t headroom_ns = 250LL * 1000000LL;
    pts_to_obs_offset_ns_ = wall_now_ns + headroom_ns - stream_pts_ns;
    lss_log_info("PTS re-anchored from video (audio was first): pts_ns=%lld, offset=%lld",
                 (long long)stream_pts_ns, (long long)pts_to_obs_offset_ns_);
  }

  int64_t computed_ts = stream_pts_ns + pts_to_obs_offset_ns_;

  // 3. Update BufferManager & Orchestrator with CORRECTED timestamps
  if (orchestrator_) {
    BufferState state;
    state.video_frames_buffered = video_queue_ ? video_queue_->size() : 0;
    state.audio_frames_buffered = audio_queue_ ? audio_queue_->size() : 0;
    
    int64_t now_ms_val = now_ms();
    int64_t last_pkt = last_pkt_recv_ms_;
    state.network_delay_ms = (last_pkt > 0) ? (now_ms_val - last_pkt) : 0;
    
    int64_t drift_ms = (computed_ts - wall_now_ns) / 1000000;
    if (drift_ms < 0) drift_ms = 0;
    // Subtract the 150ms nominal anchor headroom so drift_from_live measures
    // excess above baseline, not raw output scheduling lead. Without this,
    // the pacing-capped headroom (~700ms) looks like 700ms of stream lag and
    // immediately re-triggers catchup after every cycle.
    constexpr int64_t NOMINAL_HEADROOM_MS = 150LL;
    drift_ms = (drift_ms > NOMINAL_HEADROOM_MS) ? (drift_ms - NOMINAL_HEADROOM_MS) : 0LL;
    state.drift_from_live_ms = drift_ms;
    state.buffer_duration_ms = drift_ms;
    
    buffer_manager_.update(state);
    
    if (buffer_manager_.should_skip_to_live()) {
      lss_log_warn("BufferManager: Hopeless drift (%lld ms), triggering skip-to-live", drift_ms);
      pending_skip_to_live_.store(true);
    }
    
    orchestrator_->update(wall_now_ns);
    double tempo = orchestrator_->get_tempo();
    audio_dec_.set_tempo(tempo);

    static double last_tempo = 1.0;
    if (std::abs(tempo - last_tempo) > 0.01) {
      lss_log_debug("Tempo changed: %.3fx (state=%d)", tempo, (int)orchestrator_->get_state());
      last_tempo = tempo;
    }

    // 1 Hz telemetry heartbeat for observability.
    static int64_t tele_last_ms = 0;
    if (now_ms_val - tele_last_ms >= 1000) {
      tele_last_ms = now_ms_val;
      lss_log_debug("[TELE] headroom=%lld ms | tempo=%.3f | state=%d | qv=%d qa=%d",
                   drift_ms, tempo, (int)orchestrator_->get_state(),
                   state.video_frames_buffered, state.audio_frames_buffered);
    }
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

  // Cache color-matrix parameters across frames — they only depend on
  // height bucket / format / range. Recomputing per frame is a hot-
  // path OBS API call that adds up at 60fps high bitrate.
  int color_key_height = (frame->height >= 720) ? 720 : 0;
  if (!cached_color_valid_ || cached_color_height_ != color_key_height ||
      cached_color_format_ != static_cast<int>(obs_fmt) ||
      cached_color_range_ != static_cast<int>(obs_frame.range)) {
    enum video_colorspace cs =
        (frame->height >= 720) ? VIDEO_CS_709 : VIDEO_CS_601;
    video_format_get_parameters_for_format(
        cs, obs_frame.range, obs_fmt, cached_color_matrix_,
        cached_color_range_min_, cached_color_range_max_);
    cached_color_height_ = color_key_height;
    cached_color_format_ = static_cast<int>(obs_fmt);
    cached_color_range_ = static_cast<int>(obs_frame.range);
    cached_color_valid_ = true;
  }
  memcpy(obs_frame.color_matrix, cached_color_matrix_,
         sizeof(obs_frame.color_matrix));
  memcpy(obs_frame.color_range_min, cached_color_range_min_,
         sizeof(obs_frame.color_range_min));
  memcpy(obs_frame.color_range_max, cached_color_range_max_,
         sizeof(obs_frame.color_range_max));

  // Apply timestamp compression for catch-up
  double tempo = orchestrator_ ? orchestrator_->get_tempo() : 1.0;
  if (tempo > 1.001) {
    if (catchup_prev_video_pts_ns_ == 0) {
      catchup_prev_video_pts_ns_ = stream_pts_ns;
      catchup_video_output_clock_ns_ = computed_ts;
    } else {
      int64_t input_delta = stream_pts_ns - catchup_prev_video_pts_ns_;
      if (input_delta < 0) input_delta = 0;
      int64_t output_delta = static_cast<int64_t>(input_delta / tempo);
      catchup_video_output_clock_ns_ += output_delta;
      catchup_prev_video_pts_ns_ = stream_pts_ns;
      pts_to_obs_offset_ns_ = catchup_video_output_clock_ns_ - stream_pts_ns;
      computed_ts = catchup_video_output_clock_ns_;
    }
  } else {
    catchup_prev_video_pts_ns_ = 0;
  }

  bool is_hls = (stream_type_ == StreamType::HLS) ||
                (stream_type_ == StreamType::AmazonIVS);

  if (!is_hls) {
    // Safety re-anchor backstop: catches any freeze that slipped past the
    // worker-level freeze detection (e.g. exact threshold boundary, or a
    // protocol with a lower freeze threshold like SRT/RTSP).
    // The worker resets has_pts_offset_ for 1–10 s gaps, so this path
    // is a last-resort guard.
    int64_t wall_ns = static_cast<int64_t>(os_gettime_ns());
    int64_t drift_ns = wall_ns - computed_ts;

    constexpr int64_t SAFETY_REANCHOR_NS = 800LL * 1000000LL; // 800ms backstop
    if (drift_ns >= SAFETY_REANCHOR_NS) {
      lss_log_warn("Safety re-anchor: drift=%.1f s (video behind wall)",
                   drift_ns / 1e9);
      // +150ms so we land at the standard headroom baseline, not at 0ms
      // headroom which would immediately re-trigger another safety re-anchor.
      pts_to_obs_offset_ns_ = wall_ns + 150LL * 1000000LL - stream_pts_ns;
      computed_ts = stream_pts_ns + pts_to_obs_offset_ns_;
      // Update the audio stale-frame gate to the new anchor point so that
      // audio frames arriving after this re-anchor are not discarded.
      first_video_pts_ns_ = stream_pts_ns;
      // Reset catch-up tracking so it doesn't overwrite the new anchor
      // with an old, lagging clock state.
      catchup_prev_video_pts_ns_ = 0;
      // Force-exit the orchestrator so its tempo ramp-down doesn't conflict
      // with the freshly established anchor point.
      if (orchestrator_ && orchestrator_->is_active()) {
        orchestrator_->force_exit();
      }
    }

    // PTS rollback: genuine backward jump (encoder reset, server glitch).
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

  // Catchup pacing: cap the output clock lead at 700ms to prevent OBS audio
  // buffer overflow. Extends to COOLDOWN so residual TCP burst content that
  // was queued while the worker slept doesn't dump into headroom all at once
  // and trigger a spurious second catchup cycle.
  if (orchestrator_ && orchestrator_->get_state() != CatchupState::NORMAL) {
    int64_t lead_ns = computed_ts - static_cast<int64_t>(os_gettime_ns());
    if (lead_ns > 700LL * 1000000LL) {
      os_sleep_ms((lead_ns - 700LL * 1000000LL) / 1000000LL);
    }
  }

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

  // Shimmer → live fade-in. Only runs while the user has shimmer enabled —
  // if they've disabled it from the source settings, the picture cuts in
  // cleanly without any noise overlay (matching the no-shimmer fade-out
  // behaviour, which is already gated by show_shimmer_ in shimmer_thread).
  if (show_shimmer_) {
    if (!first_frame_received_.load()) {
      first_frame_wall_ms_ = now_ms();
    }
    if (first_frame_wall_ms_ > 0) {
      const int64_t elapsed = now_ms() - first_frame_wall_ms_;
      if (elapsed < TRANSITION_FADE_MS) {
        double alpha = 1.0 - (double)elapsed / (double)TRANSITION_FADE_MS;
        // Ease-out (quadratic): static fades quickly at first then lingers
        // softly — gives a more cinematic dissolve than linear.
        alpha = alpha * alpha;
        apply_transition_overlay(obs_frame, obs_fmt, frame, alpha);
      } else {
        first_frame_wall_ms_ = 0;  // transition complete
      }
    }
  } else {
    first_frame_wall_ms_ = 0;  // shimmer disabled — never enter fade-in
  }

  // Dashboard remote controls: blur the frame before submitting, then stamp
  // a mute icon in the bottom-right if the audio is muted. Both effects
  // operate on the trans_* buffers via in-place edits of obs_frame.data —
  // the decoder's AVFrame is never touched (cache_last_frame below relies
  // on that to remember the unaltered last live frame for fade-out).
  if (dashboard_blurred_.load()) {
    apply_blur_inplace(obs_frame, obs_fmt, frame);
  }
  if (dashboard_muted_.load()) {
    draw_mute_icon(obs_frame, obs_fmt, frame);
  }

  obs_source_output_video2(obs_source_, &obs_frame);
  // Cache this frame's planes so the shimmer thread can paint a fade-out
  // over it if the stream drops. cache_last_frame uses the *original*
  // decoder planes via `frame`, not the trans buffers — preserving the
  // unaltered last live frame even when a fade-in overlay is still active.
  cache_last_frame(frame, obs_fmt, full_range);
  send_preview_video(frame);
  first_frame_received_.store(true);
  ever_received_frame_.store(true);

  if (width_.load() != frame->width || height_.load() != frame->height) {
    width_.store(frame->width);
    height_.store(frame->height);
  }

  // Output pacing REMOVED: Let OBS handle its own buffering.
  // During network drops, we want frames to accumulate in the buffer
  // so catch-up can play them at high speed when the drop ends.
  // Sleeping here prevents buffer accumulation and causes video to
  // freeze/stutter during drops instead of buffering smoothly.
  //
  // OBS has its own async queue and will handle timing naturally.
  // If we're ahead, OBS will hold frames. If we're behind, OBS will
  // play immediately. This is the correct behavior for live streams.

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
    // Video hasn't anchored yet — wait for it. Anchoring from audio
    // causes A/V desync: audio sets offset during video's decode time
    // (50-100ms), then video re-anchors with a different wall time →
    // OBS buffers audio (up to 960ms) → "restarting source audio".
    // Discarding a few pre-video audio frames (~50-100ms) is
    // imperceptible vs. the persistent audio lagging it prevents.
    return;
  }
  if (first_video_pts_ns_ == 0) {
    // Audio arrived but video hasn't output its first frame yet
    // (still decoding keyframe). Suppress to prevent audio-first
    // anchor mismatch.
    return;
  }

  // Discard audio frames whose stream-PTS is significantly older than the
  // video re-anchor point. After a drop, video re-anchors from the first
  // post-freeze keyframe (first_video_pts_ns_). Any audio packets buffered
  // well before that keyframe carry stale PTS values; playing them places
  // audio behind the new video anchor causing the observed lag.
  //
  // 200 ms tolerance: normal interleaved streams can have audio PTS slightly
  // behind video PTS due to codec framing and decoder pipeline delay.
  constexpr int64_t STALE_AUDIO_TOLERANCE_NS = 200LL * 1000000LL;
  if (audio_pts_ns < first_video_pts_ns_ - STALE_AUDIO_TOLERANCE_NS) {
    return;
  }

  int64_t audio_computed = audio_pts_ns + pts_to_obs_offset_ns_;

  // Audio safety clamp: only for extreme drift that would overflow OBS's
  // audio buffer. Matched to the video safety re-anchor (5s) so both paths
  // correct at the same threshold.
  //
  // If audio is more than 5 s stale, drop the frame rather than rewriting the
  // shared pts_to_obs_offset_ns_.  Writing from here (audio path) while the
  // video path also reads/writes it causes PTS jump cascades that OBS
  // interprets as buffering events (up to 960 ms "Max audio buffering").
  int64_t a_wall_ns = static_cast<int64_t>(os_gettime_ns());
  int64_t a_drift = a_wall_ns - audio_computed;
  constexpr int64_t AUDIO_SAFETY_CLAMP_NS = 5000LL * 1000000LL;
  if (a_drift > AUDIO_SAFETY_CLAMP_NS) {
    lss_log_warn("Audio safety clamp: dropping stale audio frame (drift=%.1f s)",
                 a_drift / 1e9);
    return;
  }

  obs_audio.timestamp = static_cast<uint64_t>(audio_computed);

  // Always feed the dashboard preview before the mute gate so the operator
  // can still monitor the source audio remotely after muting the OBS output.
  send_preview_audio(af);

  // Dashboard mute: drop the frame on the floor for OBS output. The
  // listener (canvas / stream) goes silent; the mute icon overlay drawn in
  // output_video_frame makes the state visible.
  if (dashboard_muted_.load()) {
    return;
  }

  // Output pacing REMOVED: Let OBS handle its own audio buffering.
  // During network drops, we want audio frames to accumulate so catch-up
  // can play them at high speed when the drop ends. Sleeping here prevents
  // buffer accumulation and causes audio desync.
  //
  // OBS has a ~960ms audio buffer and will handle timing naturally.
  // If we overflow, OBS will restart audio (acceptable for extreme cases).

  obs_source_output_audio(obs_source_, &obs_audio);
}

// Dashboard live-preview: push a downscaled JPEG of the current decoded
// frame to all connected websocket clients. The encoder no-ops when no
// client is connected, so per-frame cost is just an atomic count check.
void LiveStreamSource::send_preview_video(const AVFrame *frame) {
  if (!preview_video_enabled_.load()) return;
  if (WsStatsServer::instance().get_client_count() == 0)
    return;
  preview_video_buf_.clear();
  if (preview_encoder_.encode_video_jpeg(frame, preview_video_buf_)) {
    WsStatsServer::instance().send_binary(preview_video_buf_.data(),
                                          preview_video_buf_.size());
  }
}

// Audio preview path: resample to 16 kHz mono Float32 and push as a binary
// chunk. Sent unconditionally (no throttling) so monitoring stays smooth.
void LiveStreamSource::send_preview_audio(const DecodedAudioFrame &af) {
  if (!preview_audio_enabled_.load()) return;
  if (WsStatsServer::instance().get_client_count() == 0)
    return;
  if (!af.data || af.frames == 0) return;

  // The decoder produces Float planar (one plane per channel) at out_rate_.
  // Build the plane pointer array from the contiguous af.data buffer.
  const int bps = sizeof(float);
  const int plane_size = af.frames * bps;
  const uint8_t *planes[8] = {nullptr};
  for (int c = 0; c < af.channels && c < 8; ++c) {
    planes[c] = af.data + c * plane_size;
  }

  preview_audio_buf_.clear();
  if (preview_encoder_.encode_audio_pcm(planes, af.frames, af.sample_rate,
                                         af.channels, AV_SAMPLE_FMT_FLTP,
                                         preview_audio_buf_)) {
    WsStatsServer::instance().send_binary(preview_audio_buf_.data(),
                                          preview_audio_buf_.size());
  }
}

// Privacy pixelate: aggressive ~1/16 downscale (box-area average) → upscale
// with nearest-neighbour, producing crisp ~16-px pixelation blocks. Output
// is always I420 so OBS never hits an NV12/I444 alignment edge case.
void LiveStreamSource::apply_blur_inplace(obs_source_frame2 &obs_frame,
                                           video_format obs_fmt,
                                           AVFrame *frame) {
  AVPixelFormat in_avfmt;
  if      (obs_fmt == VIDEO_FORMAT_I420) in_avfmt = AV_PIX_FMT_YUV420P;
  else if (obs_fmt == VIDEO_FORMAT_I444) in_avfmt = AV_PIX_FMT_YUV444P;
  else if (obs_fmt == VIDEO_FORMAT_NV12) in_avfmt = AV_PIX_FMT_NV12;
  else return;

  const int w = frame->width;
  const int h = frame->height;
  if (w <= 0 || h <= 0 || !obs_frame.data[0]) return;
  if ((w & 1) || (h & 1)) return; // YUV420P needs even dims

  // Aggressive scale-down: ~1/32, rounded up to a multiple of 16 so the
  // intermediate YUV420P stride/height stay well-aligned. At 1080p this
  // yields a 64×36 grid (~30 px blocks) — large headlines, faces, and
  // logos all disappear into solid colour squares.
  auto align16_up = [](int v) { return (v + 15) & ~15; };
  const int sw = std::max(32, align16_up(w / 32));
  const int sh = std::max(32, align16_up(h / 32));

  if (!blur_sws_down_ || !blur_sws_up_ || blur_cached_w_ != w ||
      blur_cached_h_ != h || blur_cached_fmt_ != (int)in_avfmt) {
    if (blur_sws_down_) { sws_freeContext(blur_sws_down_); blur_sws_down_ = nullptr; }
    if (blur_sws_up_)   { sws_freeContext(blur_sws_up_);   blur_sws_up_   = nullptr; }
    // SWS_AREA = box-area averaging (proper down-sample, no aliasing).
    // SWS_POINT = nearest-neighbour (gives the crisp pixel-block look).
    blur_sws_down_ = sws_getContext(w, h, in_avfmt, sw, sh, AV_PIX_FMT_YUV420P,
                                    SWS_AREA, nullptr, nullptr, nullptr);
    blur_sws_up_   = sws_getContext(sw, sh, AV_PIX_FMT_YUV420P, w, h,
                                    AV_PIX_FMT_YUV420P,
                                    SWS_POINT, nullptr, nullptr, nullptr);
    if (!blur_sws_down_ || !blur_sws_up_) {
      if (blur_sws_down_) { sws_freeContext(blur_sws_down_); blur_sws_down_ = nullptr; }
      if (blur_sws_up_)   { sws_freeContext(blur_sws_up_);   blur_sws_up_   = nullptr; }
      return;
    }
    blur_cached_w_   = w;
    blur_cached_h_   = h;
    blur_cached_fmt_ = (int)in_avfmt;
  }

  // Intermediate tiny YUV420P buffer (Y + U + V, contiguous).
  const int suv_w = sw / 2;
  const int suv_h = sh / 2;
  const size_t small_total =
      (size_t)sw * sh + (size_t)suv_w * suv_h * 2;
  if (blur_small_buf_.size() < small_total) blur_small_buf_.resize(small_total);
  uint8_t *sy = blur_small_buf_.data();
  uint8_t *su = sy + (size_t)sw * sh;
  uint8_t *sv = su + (size_t)suv_w * suv_h;

  // Downscale full-res (input format) → tiny YUV420P.
  const uint8_t *dsrc[4] = {obs_frame.data[0], obs_frame.data[1],
                              obs_frame.data[2], nullptr};
  int dsrls[4] = {(int)obs_frame.linesize[0], (int)obs_frame.linesize[1],
                  (int)obs_frame.linesize[2], 0};
  uint8_t *ddst[4] = {sy, su, sv, nullptr};
  int ddls[4] = {sw, suv_w, suv_w, 0};
  sws_scale(blur_sws_down_, dsrc, dsrls, 0, h, ddst, ddls);

  // Upscale tiny → full-res YUV420P. Output stride = width (tight packing).
  const int out_ly = w;
  const int out_lu = w / 2;
  const int out_lv = w / 2;
  const size_t y_size  = (size_t)out_ly * h;
  const size_t uv_size = (size_t)out_lu * (h / 2);
  if (trans_y_buf_.size() < y_size)  trans_y_buf_.resize(y_size);
  if (trans_u_buf_.size() < uv_size) trans_u_buf_.resize(uv_size);
  if (trans_v_buf_.size() < uv_size) trans_v_buf_.resize(uv_size);

  const uint8_t *usrc[4] = {sy, su, sv, nullptr};
  int ulsz[4] = {sw, suv_w, suv_w, 0};
  uint8_t *udst[4] = {trans_y_buf_.data(), trans_u_buf_.data(),
                       trans_v_buf_.data(), nullptr};
  int udls[4] = {out_ly, out_lu, out_lv, 0};
  sws_scale(blur_sws_up_, usrc, ulsz, 0, sh, udst, udls);

  // Republish obs_frame as I420 regardless of the source format. OBS
  // accepts a per-frame format change; this avoids any NV12 / I444
  // alignment quirks in the downstream renderer.
  obs_frame.format = VIDEO_FORMAT_I420;
  obs_frame.data[0] = trans_y_buf_.data();
  obs_frame.data[1] = trans_u_buf_.data();
  obs_frame.data[2] = trans_v_buf_.data();
  obs_frame.linesize[0] = (uint32_t)out_ly;
  obs_frame.linesize[1] = (uint32_t)out_lu;
  obs_frame.linesize[2] = (uint32_t)out_lv;
}

// Burn a small "muted speaker" mark into the bottom-right corner of the Y
// plane. The decoder's frame->data[0] may be read-only (HW-mapped CPU
// surface) or reused on the next decode, so we ALWAYS copy the Y plane
// into trans_y_buf_ first and write into that copy. Chroma is left
// untouched → the icon shows as bright white with a slight hue tint from
// the underlying content, which reads clearly without patching U/V.
void LiveStreamSource::draw_mute_icon(obs_source_frame2 &obs_frame,
                                       video_format obs_fmt,
                                       AVFrame *frame) {
  if (obs_fmt != VIDEO_FORMAT_I420 && obs_fmt != VIDEO_FORMAT_I444 &&
      obs_fmt != VIDEO_FORMAT_NV12)
    return;

  const int w = frame->width;
  const int h = frame->height;
  const int ly = obs_frame.linesize[0];
  if (!obs_frame.data[0] || w <= 0 || h <= 0 || ly <= 0) return;

  // If obs_frame.data[0] doesn't already point at our scratch Y buffer
  // (e.g. fade-in or blur ran first), copy the current Y plane into
  // trans_y_buf_ and redirect — never write through to AVFrame memory.
  const size_t y_size = (size_t)ly * (size_t)h;
  if (trans_y_buf_.size() < y_size) trans_y_buf_.resize(y_size);
  if (obs_frame.data[0] != trans_y_buf_.data()) {
    memcpy(trans_y_buf_.data(), obs_frame.data[0], y_size);
    obs_frame.data[0] = trans_y_buf_.data();
  }

  // Proper muted-speaker icon: dark rounded-rect background, white speaker
  // body + cone, bright slash with dark border for contrast on any content.
  const int dim_min = (w < h) ? w : h;
  const int icon_d  = std::max(36, dim_min / 14);
  const int pad     = std::max(8, dim_min / 45);
  const int x0      = w - icon_d - pad;
  const int y0      = h - icon_d - pad;

  uint8_t *y_plane = trans_y_buf_.data();

  // Pass 1: semi-transparent dark rounded-rect background.
  const int bg = icon_d / 5;
  for (int yy = y0 - bg; yy < y0 + icon_d + bg && yy < h; ++yy) {
    if (yy < 0) continue;
    for (int xx = x0 - bg; xx < x0 + icon_d + bg && xx < w; ++xx) {
      if (xx < 0) continue;
      // Rounded corners via distance to nearest corner centre.
      float bx = (float)(xx - (x0 - bg)) / (icon_d + 2 * bg);
      float by = (float)(yy - (y0 - bg)) / (icon_d + 2 * bg);
      float cx = (bx < 0.5f) ? 0.18f : 0.82f;
      float cy2 = (by < 0.5f) ? 0.18f : 0.82f;
      float dx = bx - cx, dy2 = by - cy2;
      bool in_bg = (bx >= 0.18f && bx <= 0.82f) ||
                   (by >= 0.18f && by <= 0.82f) ||
                   (dx * dx + dy2 * dy2 <= 0.18f * 0.18f);
      if (in_bg) {
        uint8_t orig = y_plane[yy * ly + xx];
        y_plane[yy * ly + xx] = (uint8_t)(orig * 0.15f + 18.0f * 0.85f);
      }
    }
  }

  // Pass 2: draw speaker shape then slash over it.
  for (int yy = y0; yy < y0 + icon_d && yy < h; ++yy) {
    if (yy < 0) continue;
    for (int xx = x0; xx < x0 + icon_d && xx < w; ++xx) {
      if (xx < 0) continue;
      const float nx = (float)(xx - x0) / icon_d;
      const float ny = (float)(yy - y0) / icon_d;

      // Speaker body: rect 0.06–0.38 wide, 0.32–0.68 tall.
      bool in_body = (nx >= 0.06f && nx < 0.38f &&
                      ny >= 0.32f && ny < 0.68f);

      // Speaker cone: trapezoid expanding right from body.
      bool in_cone = false;
      if (nx >= 0.38f && nx < 0.72f) {
        float t = (nx - 0.38f) / 0.34f;
        float top = 0.32f - t * 0.24f;
        float bot = 0.68f + t * 0.24f;
        in_cone = (ny >= top && ny <= bot);
      }

      // Diagonal slash from (0.87, 0.07) to (0.13, 0.93).
      const float sdx = 0.13f - 0.87f, sdy = 0.93f - 0.07f;
      const float slen = sqrtf(sdx * sdx + sdy * sdy);
      float dist_px = fabsf((nx - 0.87f) * sdy - (ny - 0.07f) * sdx)
                      / slen * icon_d;

      if (dist_px < 1.8f) {
        // Slash core — bright so it reads on both dark and white speaker.
        y_plane[yy * ly + xx] = 235;
      } else if (dist_px < 3.2f) {
        // Dark border around slash for contrast on bright areas.
        y_plane[yy * ly + xx] = 16;
      } else if (in_body || in_cone) {
        y_plane[yy * ly + xx] = 228;
      }
    }
  }
}

// Thin wrapper for the fade-in path: feeds AVFrame's plane pointers to the
// shared overlay implementation.
void LiveStreamSource::apply_transition_overlay(obs_source_frame2 &obs_frame,
                                                 video_format obs_fmt,
                                                 AVFrame *frame,
                                                 double alpha) {
  const uint8_t *sv = (obs_fmt == VIDEO_FORMAT_NV12) ? nullptr : frame->data[2];
  const int      lv = (obs_fmt == VIDEO_FORMAT_NV12) ? 0       : frame->linesize[2];
  apply_noise_overlay_raw(obs_frame, obs_fmt,
                          frame->width, frame->height,
                          frame->data[0], frame->linesize[0],
                          frame->data[1], frame->linesize[1],
                          sv, lv, alpha);
}

// Shared overlay implementation, takes raw plane pointers so it can serve
// both fade-in (AVFrame → trans buffers) and fade-out (cached YUV → trans
// buffers). Writes always go to trans_*_buf_, and obs_frame is redirected
// to those buffers on return.
void LiveStreamSource::apply_noise_overlay_raw(obs_source_frame2 &obs_frame,
                                                video_format obs_fmt,
                                                int w, int h,
                                                const uint8_t *src_y, int ly,
                                                const uint8_t *src_u, int lu,
                                                const uint8_t *src_v, int lv,
                                                double alpha) {
  // Overlays decaying TV-static noise on the decoded video frame so the
  // shimmer dissolves smoothly into the picture instead of cutting hard.
  // - Y plane: blend each pixel with high-frequency hash noise.
  // - Chroma (U/V or NV12 UV): pull toward 128 (gray) by `alpha`, so
  //   colour fades in as the noise fades out.
  // Operates on copies (trans_*_buf_) so we never touch the decoder's
  // AVFrame data, then redirects obs_frame.data to those copies.

  if (alpha <= 0.0)
    return;

  // Only YUV-family formats expose a luma plane at data[0] that we can
  // additively blend with noise. RGB/BGRA layouts pack colour bytes
  // together, so the same byte-write would corrupt hue, not just luma —
  // skip the overlay for those (rare path; picture just cuts in).
  const bool is_yuv_planar3 = (obs_fmt == VIDEO_FORMAT_I420 ||
                                obs_fmt == VIDEO_FORMAT_I444);
  const bool is_yuv_nv12   = (obs_fmt == VIDEO_FORMAT_NV12);
  if (!is_yuv_planar3 && !is_yuv_nv12)
    return;

  if (w <= 0 || h <= 0)
    return;

  if (ly <= 0 || !src_y)
    return;

  const int alpha_int = (int)(alpha * 256.0 + 0.5);
  const int inv_alpha = 256 - alpha_int;
  const int64_t fid = now_ms();

  auto fhash = [](uint32_t x) -> uint32_t {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
  };

  // Y plane: noise blend
  const size_t y_size = (size_t)ly * (size_t)h;
  if (trans_y_buf_.size() < y_size)
    trans_y_buf_.resize(y_size);

  for (int yi = 0; yi < h; yi++) {
    const uint8_t *src = src_y + (size_t)yi * ly;
    uint8_t *dst = trans_y_buf_.data() + (size_t)yi * ly;
    for (int xi = 0; xi < w; xi++) {
      uint32_t hv = fhash(((uint32_t)xi * 73856093u) ^
                          ((uint32_t)yi * 19349663u) ^
                          ((uint32_t)fid * 83492791u));
      int n = (int)(hv & 0xFF);
      // Contrast curve: high-frequency static character (mostly dark with
      // bright sparkle pixels). Same family of noise the shimmer uses.
      // n2 = n^3 / 65535 → favours dark, occasional bright.
      int n_curved = (n * n) >> 8;          // [0..255]
      n_curved = (n_curved * (255 + n)) >> 9;  // bias toward dark + sparkle
      if (n > 248) n_curved = 240;          // rare bright fleck
      int luma = src[xi];
      dst[xi] = (uint8_t)((n_curved * alpha_int + luma * inv_alpha) >> 8);
    }
  }
  obs_frame.data[0] = trans_y_buf_.data();
  obs_frame.linesize[0] = (uint32_t)ly;

  // Chroma fade: pull U/V planes toward 128 (gray) by `alpha`.
  if (is_yuv_planar3) {
    // I420 → chroma is half-height (4:2:0). I444 → chroma is full height (4:4:4).
    const int uv_h = (obs_fmt == VIDEO_FORMAT_I420) ? (h / 2) : h;
    const int uv_w = (obs_fmt == VIDEO_FORMAT_I420) ? (w / 2) : w;
    if (lu > 0 && lv > 0 && src_u && src_v) {
      const size_t u_size = (size_t)lu * (size_t)uv_h;
      const size_t v_size = (size_t)lv * (size_t)uv_h;
      if (trans_u_buf_.size() < u_size) trans_u_buf_.resize(u_size);
      if (trans_v_buf_.size() < v_size) trans_v_buf_.resize(v_size);
      for (int yi = 0; yi < uv_h; yi++) {
        const uint8_t *su = src_u + (size_t)yi * lu;
        const uint8_t *sv2 = src_v + (size_t)yi * lv;
        uint8_t *du = trans_u_buf_.data() + (size_t)yi * lu;
        uint8_t *dv = trans_v_buf_.data() + (size_t)yi * lv;
        for (int xi = 0; xi < uv_w; xi++) {
          du[xi] = (uint8_t)((128 * alpha_int + su[xi] * inv_alpha) >> 8);
          dv[xi] = (uint8_t)((128 * alpha_int + sv2[xi] * inv_alpha) >> 8);
        }
      }
      obs_frame.data[1] = trans_u_buf_.data();
      obs_frame.data[2] = trans_v_buf_.data();
      obs_frame.linesize[1] = (uint32_t)lu;
      obs_frame.linesize[2] = (uint32_t)lv;
    }
  } else if (is_yuv_nv12) {
    const int uv_h = h / 2;
    if (lu > 0 && src_u) {
      const size_t uv_size = (size_t)lu * (size_t)uv_h;
      if (trans_u_buf_.size() < uv_size) trans_u_buf_.resize(uv_size);
      // NV12: UV is interleaved (U0 V0 U1 V1 ...), so we treat the plane
      // as raw bytes and ease every byte toward 128.
      for (int yi = 0; yi < uv_h; yi++) {
        const uint8_t *suv = src_u + (size_t)yi * lu;
        uint8_t *duv = trans_u_buf_.data() + (size_t)yi * lu;
        const int row_bytes = w;  // 2 bytes per UV pair × w/2 pairs
        for (int xi = 0; xi < row_bytes; xi++) {
          duv[xi] = (uint8_t)((128 * alpha_int + suv[xi] * inv_alpha) >> 8);
        }
      }
      obs_frame.data[1] = trans_u_buf_.data();
      obs_frame.linesize[1] = (uint32_t)lu;
    }
  }
  // For packed YUV (YUY2/UYVY) and RGB/BGRA we return early above —
  // transition is a no-op (rare path; picture just cuts in).
}

// Cache the decoded planes of the most recent live frame so the shimmer
// thread can paint over it when the stream drops. Called from the worker
// thread after each successful obs_source_output_video2.
void LiveStreamSource::cache_last_frame(AVFrame *frame, video_format fmt,
                                         bool full_range) {
  if (fmt != VIDEO_FORMAT_I420 && fmt != VIDEO_FORMAT_I444 &&
      fmt != VIDEO_FORMAT_NV12)
    return;  // unsupported — fade-out will fall through to hard cut
  if (!frame || !frame->data[0] || frame->width <= 0 || frame->height <= 0)
    return;
  const int ly = frame->linesize[0];
  if (ly <= 0) return;

  std::lock_guard<std::mutex> lk(last_frame_mutex_);

  const size_t y_size = (size_t)ly * (size_t)frame->height;
  if (last_y_buf_.size() < y_size) last_y_buf_.resize(y_size);
  memcpy(last_y_buf_.data(), frame->data[0], y_size);

  if (fmt == VIDEO_FORMAT_I420 || fmt == VIDEO_FORMAT_I444) {
    const int lu = frame->linesize[1];
    const int lv = frame->linesize[2];
    const int uv_h = (fmt == VIDEO_FORMAT_I420) ? (frame->height / 2)
                                                 : frame->height;
    if (lu > 0 && lv > 0 && frame->data[1] && frame->data[2]) {
      const size_t u_size = (size_t)lu * (size_t)uv_h;
      const size_t v_size = (size_t)lv * (size_t)uv_h;
      if (last_u_buf_.size() < u_size) last_u_buf_.resize(u_size);
      if (last_v_buf_.size() < v_size) last_v_buf_.resize(v_size);
      memcpy(last_u_buf_.data(), frame->data[1], u_size);
      memcpy(last_v_buf_.data(), frame->data[2], v_size);
      last_frame_lu_ = lu;
      last_frame_lv_ = lv;
    }
  } else {  // NV12: single interleaved UV plane
    const int luv = frame->linesize[1];
    const int uv_h = frame->height / 2;
    if (luv > 0 && frame->data[1]) {
      const size_t uv_size = (size_t)luv * (size_t)uv_h;
      if (last_u_buf_.size() < uv_size) last_u_buf_.resize(uv_size);
      memcpy(last_u_buf_.data(), frame->data[1], uv_size);
      last_frame_lu_ = luv;
      last_frame_lv_ = 0;
    }
  }

  last_frame_w_ = frame->width;
  last_frame_h_ = frame->height;
  last_frame_fmt_ = fmt;
  last_frame_ly_ = ly;
  last_frame_full_range_ = full_range;
  has_last_frame_ = true;
}

// Output one fade-out frame: cached YUV + rising noise overlay. Reads under
// the cache mutex (so worker can't realloc the cached buffers mid-read) and
// reuses apply_noise_overlay_raw to produce the composite.
void LiveStreamSource::output_fadeout_frame(double alpha) {
  std::lock_guard<std::mutex> lk(last_frame_mutex_);
  if (!has_last_frame_) return;

  obs_source_frame2 obs_frame = {};
  obs_frame.width  = (uint32_t)last_frame_w_;
  obs_frame.height = (uint32_t)last_frame_h_;
  obs_frame.format = last_frame_fmt_;
  obs_frame.range  = last_frame_full_range_ ? VIDEO_RANGE_FULL
                                            : VIDEO_RANGE_PARTIAL;
  // Reuse the same colour-matrix cache that output_video_frame populates.
  // If cache hasn't been primed yet, fall back to recomputing here.
  if (!cached_color_valid_ ||
      cached_color_format_ != (int)last_frame_fmt_ ||
      cached_color_range_ != (int)obs_frame.range) {
    enum video_colorspace cs = (last_frame_h_ >= 720) ? VIDEO_CS_709
                                                       : VIDEO_CS_601;
    video_format_get_parameters_for_format(
        cs, obs_frame.range, last_frame_fmt_, cached_color_matrix_,
        cached_color_range_min_, cached_color_range_max_);
    cached_color_height_ = (last_frame_h_ >= 720) ? 720 : 0;
    cached_color_format_ = (int)last_frame_fmt_;
    cached_color_range_  = (int)obs_frame.range;
    cached_color_valid_  = true;
  }
  memcpy(obs_frame.color_matrix,    cached_color_matrix_,
         sizeof(obs_frame.color_matrix));
  memcpy(obs_frame.color_range_min, cached_color_range_min_,
         sizeof(obs_frame.color_range_min));
  memcpy(obs_frame.color_range_max, cached_color_range_max_,
         sizeof(obs_frame.color_range_max));

  // Pre-fill obs_frame with cached plane pointers BEFORE the overlay call.
  // apply_noise_overlay_raw will override these to point at trans_*_buf_
  // when alpha > 0, but at alpha=0 (the very first fade-out frame, fired
  // microseconds after the transition was armed) it returns early without
  // touching obs_frame — and a NULL data[0] there gets rendered by OBS as
  // a brief green flash before the next iteration produces real data.
  obs_frame.data[0] = last_y_buf_.data();
  obs_frame.linesize[0] = (uint32_t)last_frame_ly_;
  if (last_frame_fmt_ == VIDEO_FORMAT_I420 ||
      last_frame_fmt_ == VIDEO_FORMAT_I444) {
    obs_frame.data[1] = last_u_buf_.data();
    obs_frame.linesize[1] = (uint32_t)last_frame_lu_;
    obs_frame.data[2] = last_v_buf_.data();
    obs_frame.linesize[2] = (uint32_t)last_frame_lv_;
  } else if (last_frame_fmt_ == VIDEO_FORMAT_NV12) {
    obs_frame.data[1] = last_u_buf_.data();
    obs_frame.linesize[1] = (uint32_t)last_frame_lu_;
  }

  const uint8_t *sv  = (last_frame_fmt_ == VIDEO_FORMAT_NV12) ? nullptr
                                                              : last_v_buf_.data();
  const int      lv  = (last_frame_fmt_ == VIDEO_FORMAT_NV12) ? 0
                                                              : last_frame_lv_;
  apply_noise_overlay_raw(obs_frame, last_frame_fmt_,
                          last_frame_w_, last_frame_h_,
                          last_y_buf_.data(), last_frame_ly_,
                          last_u_buf_.data(), last_frame_lu_,
                          sv, lv, alpha);

  obs_frame.timestamp = os_gettime_ns();
  obs_source_output_video2(obs_source_, &obs_frame);
}

void LiveStreamSource::output_shimmer_frame() {
  int w = width_.load();
  int h = height_.load();
  if (w <= 0 || h <= 0) {
    w = 1920;
    h = 1080;
  }

  const int linesize = w * 4;
  const size_t req = (size_t)linesize * h;
  if (shimmer_buffer_.size() < req)
    shimmer_buffer_.resize(req);
  uint8_t *buf = shimmer_buffer_.data();

  // Analog CRT "dead channel" static.
  //   - High-density per-pixel grayscale noise (every pixel changes every frame).
  //   - Two-octave hash noise → fine-grain texture ("karıncalanma").
  //   - Sigmoid-like contrast curve → mostly mid-grays with bright sparkle.
  //   - Per-row scanline darkening (CRT line gap).
  //   - Subtle chroma deviation per pixel → analog colour-burst error.
  //   - Multiple horizontal-sync tears each frame (lines shifted ±few px).
  //   - Vertical-sync roll: bright sync bar slowly drifting upward.
  //   - Radial vignette → tube curvature.
  //   - Brightness pulse → power-supply ripple at ~1.5 Hz.
  // All integer hashes — no transcendental math in the hot loop.

  const double tt  = (double)now_ms() / 1000.0;
  const int64_t fid = (int64_t)(tt * 30.0);
  const double cx  = w * 0.5;
  const double cy  = h * 0.5;
  const double dim = (double)(w < h ? w : h);

  auto fhash = [](uint32_t x) -> uint32_t {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
  };

  // === Horizontal-sync tears: 4 short bands per frame, lines shifted ±dx px.
  constexpr int NTEAR = 4;
  int tear_y[NTEAR], tear_h_[NTEAR], tear_dx[NTEAR];
  const int dx_max = (int)(dim * 0.025);
  for (int s = 0; s < NTEAR; s++) {
    uint32_t a = fhash((uint32_t)(s * 1009) ^ (uint32_t)(fid * 31337));
    tear_y[s]  = (int)(a % (uint32_t)h);
    tear_h_[s] = 1 + (int)((a >> 12) % 4);  // 1–4 lines tall
    int sign   = (a & 0x10000) ? 1 : -1;
    tear_dx[s] = sign * (int)((a >> 17) % (uint32_t)(dx_max + 1));
  }

  // === Vertical-sync error: intermittent V-hold drift. The TV is stable for
  // ~10 s then loses lock for ~4.5 s during which a dark V-blanking band
  // drifts down the screen at constant speed (real V-hold rolls linearly —
  // there's no inertia in a misaligned hold potentiometer). The leading-edge
  // bright sync-pulse line sits at the top of the band (signal-order: VSYNC
  // pulse fires first, then the V-blanking gap, then the next frame).
  constexpr double v_cycle_s = 14.5;
  constexpr double v_sweep_s = 4.5;
  const double v_band_h      = h * 0.085;     // 8.5 % of frame height
  const double v_sync_thk    = 2.0;           // bright sync-pulse thickness (px)
  const double v_in_cycle    = fmod(tt, v_cycle_s);
  const bool   v_active      = v_in_cycle < v_sweep_s;
  double v_band_top          = 1e9;           // off-screen by default
  if (v_active) {
    // Linear motion — constant V-hold drift rate
    const double tn = v_in_cycle / v_sweep_s;
    v_band_top = -v_band_h + (h + 2.0 * v_band_h) * tn;
  }
  const double v_band_bot    = v_band_top + v_band_h;

  // === Vignette: radial darkening toward corners.
  const double inv_vig_r2 = 1.0 / ((dim * 0.75) * (dim * 0.75));

  // === Brightness pulse: subtle ripple (1.5 Hz), ±3 %.
  const double pulse = 0.97 + 0.03 * sin(tt * 9.4);  // sin used once per frame only

  // Integer pre-multipliers for hash mixing
  const uint32_t hx_mul = 73856093u;
  const uint32_t hy_mul = 19349663u;
  const uint32_t ht_mul = 83492791u;

  for (int y = 0; y < h; y++) {
    uint8_t *row = buf + (size_t)y * linesize;
    const double dy  = (double)y - cy;
    const double dy2 = dy * dy;

    // Apply a tear offset if this row falls inside a tear band
    int tear_off = 0;
    for (int s = 0; s < NTEAR; s++) {
      if (y >= tear_y[s] && y < tear_y[s] + tear_h_[s]) {
        tear_off = tear_dx[s];
        break;
      }
    }

    // V-blanking band: nearly uniform dark, soft fade at the trailing edge.
    // Only active during the brief roll event each cycle.
    double roll_mul  = 1.0;
    bool   sync_line = false;
    if (v_active && (double)y >= v_band_top - 1 && (double)y < v_band_bot + 1) {
      const double dy = (double)y - v_band_top;
      if (dy >= 0.0 && dy < v_sync_thk) {
        // Bright horizontal sync pulse — white line at the top of V-blanking
        sync_line = true;
      } else if (dy >= v_sync_thk && dy < v_band_h) {
        // Uniform deep dark with a short soft fade at the trailing edge only.
        // (The leading edge is hard-cut by the sync pulse line itself.)
        const double body_t = (dy - v_sync_thk) / (v_band_h - v_sync_thk);
        double dark_amt = 1.0;
        if (body_t > 0.78) {
          // Fade out over the last 22 % of the band
          dark_amt = (1.0 - body_t) / 0.22;
        }
        roll_mul = 1.0 - 0.85 * dark_amt;  // up to 85 % darker
      }
    }

    // Scanline darkening (CRT line gap): every other row darker
    const double scan = (y & 1) ? 0.82 : 1.00;

    for (int x = 0; x < w; x++) {
      // Sample noise from offset position so tears actually shift content
      int sx = x + tear_off;
      if (sx < 0)      sx = 0;
      else if (sx >= w) sx = w - 1;

      // Two-octave grayscale noise per pixel
      uint32_t h1 = fhash(((uint32_t)sx * hx_mul) ^ ((uint32_t)y * hy_mul) ^ ((uint32_t)fid * ht_mul));
      uint32_t h2 = fhash((((uint32_t)sx >> 1) * hx_mul) ^ (((uint32_t)y >> 1) * hy_mul) ^ ((uint32_t)(fid * 2 + 1) * ht_mul));

      // Blend: dominant fine grain + softer coarse grain
      double n  = (h1 & 0xFF) / 255.0;
      double n2 = (h2 & 0xFF) / 255.0;
      double v  = n * 0.75 + n2 * 0.25;

      // Contrast curve: cube → emphasises bright sparkle, keeps most pixels mid-low.
      // This is the "snow" character: lots of dark gray with periodic bright pops.
      v = v * v * (3.0 - 2.0 * v);  // smoothstep, gives natural-looking distribution
      v = 0.08 + v * 0.82;          // map to [0.08 .. 0.90] base range

      // Rare bright sparkle pixel (~1 % of pixels at near-white)
      if ((h1 & 0xFF) > 248) v = 0.97;

      // Vignette (corner darkening)
      const double dx_c = (double)x - cx;
      const double d2   = dx_c * dx_c + dy2;
      double vig_t = d2 * inv_vig_r2;
      if (vig_t > 1.0) vig_t = 1.0;
      const double vig = 1.0 - vig_t * 0.38;

      // Compose grayscale
      double gray = v * scan * vig * roll_mul * pulse;

      // Subtle chroma deviation per pixel — analog colour-burst phase error.
      // Use a separate hash so chroma noise is independent of luma noise.
      uint32_t cv = fhash(((uint32_t)sx * 1103515245u) ^ ((uint32_t)y * 12345u) ^ ((uint32_t)fid * 2654435761u));
      // ±~5 % R/B variance, G mostly stable (analog NTSC/PAL behaviour)
      double cr = 1.0 + ((int)((cv >> 0) & 0x1F) - 16) * 0.003;   // ±4.8 %
      double cb = 1.0 + ((int)((cv >> 8) & 0x1F) - 16) * 0.003;

      double r = gray * cr;
      double g = gray;
      double b = gray * cb;

      // Sync pulse: thin bright white line at the top of the V-blanking band
      if (sync_line) {
        r = g = b = 0.95;
      }

      if (r > 1.0) r = 1.0; else if (r < 0.0) r = 0.0;
      if (g > 1.0) g = 1.0; else if (g < 0.0) g = 0.0;
      if (b > 1.0) b = 1.0; else if (b < 0.0) b = 0.0;

      row[x * 4 + 0] = (uint8_t)(b * 255.0);
      row[x * 4 + 1] = (uint8_t)(g * 255.0);
      row[x * 4 + 2] = (uint8_t)(r * 255.0);
      row[x * 4 + 3] = 255;
    }
  }

  obs_source_frame2 frame = {};
  frame.data[0]   = buf;
  frame.linesize[0] = linesize;
  frame.width     = w;
  frame.height    = h;
  frame.format    = VIDEO_FORMAT_BGRA;
  frame.timestamp = os_gettime_ns();
  obs_source_output_video2(obs_source_, &frame);
}

void LiveStreamSource::handle_freeze_skip_recovery(int64_t gap_ms,
                                                    bool set_cooldown) {
  lss_log_warn("Skip-resync (gap=%lld ms%s)",
               (long long)gap_ms,
               set_cooldown ? "" : ", manual");

  // Tear down any active catch-up before skip-resync.
  if (orchestrator_) {
    orchestrator_->force_exit();
  }
  audio_dec_.set_tempo(1.0);

  if (set_cooldown) {
    // Suppress catchup re-entry for a window — prevents catchup→skip
    // loops during sustained bad-network conditions (e.g. continuous
    // packet drop). For manual user skip we want instant resume so
    // the cooldown is bypassed.
    int64_t cooldown_ms = CATCHUP_COOLDOWN_MS;
    catchup_cooldown_until_ms_ = now_ms() + cooldown_ms;
    
    // For automatic skips on RTMP, enable packet discard mode to reach live edge
    bool is_rtmp = (url_.rfind("rtmp://", 0) == 0 ||
                    url_.rfind("rtmps://", 0) == 0);
    if (is_rtmp) {
      rtmp_skip_to_live_ = true;
      skip_start_ms_ = now_ms();
      skip_rate_ema_ = 5.0;
      skip_prev_pts_ms_ = 0;
      skip_prev_wall_ms_ = 0;
    }
  } else {
    // Make sure no leftover cooldown from a previous automatic skip
    // is still suppressing emissions.
    catchup_cooldown_until_ms_ = 0;
  }

  // Reset PTS anchor — dual-wait logic in output_*_frame will re-anchor
  // when both video and audio post-freeze frames arrive.
  has_pts_offset_ = false;
  pts_to_obs_offset_ns_ = 0;
  first_video_pts_ns_ = 0;
  first_audio_pts_ns_ = 0;
  last_video_pts_us_.store(0);
  last_audio_pts_us_.store(0);

  // Drop any pending audio still sitting in the queue (decoded before
  // the freeze, would otherwise play with the new wall-clock anchor).
  if (audio_queue_) {
    DecodedAudioFrame stale;
    while (audio_queue_->pop(stale))
      stale.free_buffers();
  }

  // Drain audio decoder's internal frames + resampler residual.
  audio_dec_.reset_state();

  if (rtmp_skip_to_live_) {
    // Flush video decoder so stale frames don't leak out after we resume
    video_dec_.flush([](AVFrame *) { /* discard */ });
  }

  // Reset stream-delay reference so the new wall/pts baseline starts clean.
  has_delay_ref_ = false;

  // HLS catchup uses these as anchors — reset only for HLS so we don't
  // disturb the HLS pacing engine's separate logic.
  bool is_hls = (stream_type_ == StreamType::HLS) ||
                (stream_type_ == StreamType::AmazonIVS);
  if (is_hls) {
    catchup_first_wall_ms_ = 0;
    catchup_first_pts_ms_ = 0;
  }
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
      // Background refresh signal from the UI thread (prevents UI freeze)
      // Moved to top of loop so it works even when disconnected/reconnecting
      if (pending_refresh_.exchange(false)) {
        lss_log_info("Worker: performing background refresh/restart");
        
        // Interrupt any current blocked network calls (read OR open_input)
        demuxer_.request_abort();
        
        // Wait for background connection thread if it's running
        if (connect_thread_.joinable()) {
          lss_log_debug("Worker: joining connect_thread before refresh");
          connect_thread_.join();
        }

        connected_.store(false);
        first_frame_received_.store(false);
        demuxer_.close();
        video_dec_.close();
        audio_dec_.close();
        
        // Reset state for clean restart
        has_pts_offset_ = false;
        pts_to_obs_offset_ns_ = 0;
        first_video_pts_ns_ = 0;
        first_audio_pts_ns_ = 0;
        last_video_pts_us_.store(0);
        last_audio_pts_us_.store(0);
        has_delay_ref_ = false;

        if (audio_queue_) audio_queue_->flush();
        if (video_queue_) video_queue_->flush();
        
        // Reset demuxer abort so the next open attempt can succeed
        demuxer_.reset_abort();
        reconnect_mgr_.reset();
        
        lss_log_info("Worker: refresh reset complete, starting fresh connection");
        continue;
      }

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
            bool is_hls = (stream_type_ == StreamType::HLS) ||
                          (stream_type_ == StreamType::AmazonIVS);
            catching_up_ = is_hls && auto_catchup_;
            catchup_first_wall_ms_ = 0;
            catchup_first_pts_ms_ = 0;
            // Hide disconnect source on successful reconnect
            update_source_toggles();
          } else {
            reconnect_mgr_.mark_failed();
          }
          connection_in_progress_.store(false);
        });

        continue;
      }

      if (pending_skip_to_live_.exchange(false)) {
        lss_log_info("Skip to live: in-place reconnect");
        demuxer_.close();
        video_dec_.close();
        audio_dec_.close();
        // Reset PTS state so the first post-reconnect frame anchors
        // to wall_now and audio drift gets cleared.
        has_pts_offset_ = false;
        pts_to_obs_offset_ns_ = 0;
        first_video_pts_ns_ = 0;
        first_audio_pts_ns_ = 0;
        last_video_pts_us_.store(0);
        last_audio_pts_us_.store(0);
        has_delay_ref_ = false;
        catchup_cooldown_until_ms_ = 0;

        if (audio_queue_) {
          DecodedAudioFrame stale;
          while (audio_queue_->pop(stale))
            stale.free_buffers();
        }
        // Reopen demuxer + decoders. NOTE: connected_ stays true,
        // first_frame_received_ stays true → no shimmer.
        if (demuxer_.open(url_, stream_type_) >= 0) {
          if (demuxer_.video_stream_index() >= 0) {
            video_dec_.init(demuxer_.video_codecpar(), hw_decode_);
            video_dec_.set_stream_time_base(demuxer_.video_time_base());
          }
          if (demuxer_.audio_stream_index() >= 0) {
            audio_dec_.init(demuxer_.audio_codecpar());
            audio_dec_.set_stream_time_base(demuxer_.audio_time_base());
          }
          last_pkt_recv_ms_ = now_ms();
          lss_log_info("Skip to live: reconnect successful");
        } else {
          lss_log_warn("Skip to live: reconnect failed — falling back");
          connected_.store(false); // triggers normal async-reconnect path
        }
        av_packet_unref(pkt);
        continue;
      }

      int64_t read_start = now_ms();
      int ret = demuxer_.read_packet(pkt);
      int64_t read_end = now_ms();

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
        if (empty_buffer_start_ms_ == 0) {
          empty_buffer_start_ms_ = read_start;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      int64_t network_wait_ms = read_end - read_start;
      if (empty_buffer_start_ms_ > 0) {
        network_wait_ms += (read_start - empty_buffer_start_ms_);
        empty_buffer_start_ms_ = 0;
      }

      // Stamp network arrival time here — before decode — so the freeze gap
      // measures only inter-packet delivery time, not decode latency.
      int64_t pkt_recv_now = now_ms();

      if (pkt->stream_index != demuxer_.video_stream_index() &&
          pkt->stream_index != demuxer_.audio_stream_index()) {
        av_packet_unref(pkt);
        continue;
      }

      // Soft-freeze detection based strictly on true network wait time.
      // This ignores any delays caused by our own decoding or pacing sleeps,
      // completely eliminating false positive freezes.
      // RTMP uses a higher threshold because TCP burst delivery at
      // 10+ Mbps causes 200-800ms inter-packet gaps as normal behavior.
      bool is_rtmp_stream = (url_.rfind("rtmp://", 0) == 0 ||
                             url_.rfind("rtmps://", 0) == 0);
      int64_t freeze_threshold = is_rtmp_stream
                                     ? FREEZE_RECOVERY_THRESHOLD_RTMP_MS
                                     : FREEZE_RECOVERY_THRESHOLD_MS;
      bool in_cooldown_now = (catchup_cooldown_until_ms_ > 0 &&
                              now_ms() < catchup_cooldown_until_ms_);
      
      // DON'T start catch-up immediately when freeze is detected!
      // At that moment, the buffer is EMPTY (no packets arrived during freeze).
      // Instead, just log the freeze. Catch-up will be triggered later by
      // PTS drift detection in output_video_frame() once buffer has refilled.
      if (network_wait_ms > freeze_threshold && first_frame_received_.load() &&
          !catching_up_ && !in_cooldown_now) {
        if (network_wait_ms >= CATCHUP_MIN_DRIFT_MS &&
            network_wait_ms <= CATCHUP_MAX_DRIFT_MS) {
          lss_log_info("Freeze detected: gap=%lld ms — resetting PTS anchor for clean re-sync",
                       (long long)network_wait_ms);

          // Reset the shared PTS anchor so audio is suppressed until video
          // re-anchors at the live edge (has_pts_offset_ = false guard).
          has_pts_offset_ = false;
          pts_to_obs_offset_ns_ = 0;
          first_video_pts_ns_ = 0;
          first_audio_pts_ns_ = 0;
          last_video_pts_us_.store(0);
          last_audio_pts_us_.store(0);
          has_delay_ref_ = false;

          // Drain stale pre-freeze audio and flush decoder pipeline.
          if (audio_queue_) {
            DecodedAudioFrame stale;
            while (audio_queue_->pop(stale))
              stale.free_buffers();
          }
          audio_dec_.reset_state();

          // Large drops (>2s): enable burst-skip to reach live edge quickly.
          // The worker discards burst packets until stream_pts ≥ target (live edge
          // estimate), then reanchors. Achieves <1s recovery vs 10-30s with tempo.
          if (network_wait_ms >= lss::BufferManager::INSTANT_JUMP_THRESHOLD_MS &&
              !burst_skip_active_) {
            if (pkt->stream_index == demuxer_.video_stream_index() &&
                pkt->pts != AV_NOPTS_VALUE) {
              AVRational tb = demuxer_.video_time_base();
              int64_t first_pts_ms = av_rescale_q(pkt->pts, tb, {1, 1000});
              // Target: first burst PTS + gap duration - 300ms buffer for re-anchor headroom
              burst_skip_target_pts_ms_ = first_pts_ms + network_wait_ms - 300;
              burst_skip_active_ = true;
              burst_skip_start_ms_ = now_ms();
              lss_log_info("Burst-skip enabled: first_pts=%lld ms, target=%lld ms (gap=%lld ms)",
                           (long long)first_pts_ms,
                           (long long)burst_skip_target_pts_ms_,
                           (long long)network_wait_ms);
            }
          }

        } else if (network_wait_ms > CATCHUP_MAX_DRIFT_MS) {
          handle_freeze_skip_recovery(network_wait_ms, true);
        }
      }
      last_pkt_recv_ms_ = pkt_recv_now;

      if (rtmp_skip_to_live_) {
        int64_t wall_now = now_ms();
        int64_t elapsed = wall_now - skip_start_ms_;

        // Safety timeout: if stuck for >8s, force exit on any video pkt.
        bool force_exit = (elapsed > 8000);

        if (pkt->stream_index == demuxer_.video_stream_index()) {
          // Track input rate using video DTS
          if (pkt->dts != AV_NOPTS_VALUE) {
            AVRational tb = demuxer_.video_time_base();
            int64_t pts_ms = av_rescale_q(pkt->dts, tb, {1, 1000});
            if (skip_prev_wall_ms_ > 0 && skip_prev_pts_ms_ > 0) {
              int64_t wall_delta = wall_now - skip_prev_wall_ms_;
              int64_t pts_delta = pts_ms - skip_prev_pts_ms_;
              if (wall_delta > 0 && pts_delta > 0) {
                double rate = (double)pts_delta / (double)wall_delta;
                skip_rate_ema_ = 0.3 * rate + 0.7 * skip_rate_ema_;
              }
            }
            skip_prev_pts_ms_ = pts_ms;
            skip_prev_wall_ms_ = wall_now;
          }

          bool is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
          bool rate_ok = (elapsed > 300 && skip_rate_ema_ < 1.5);

          if ((rate_ok && is_key) || (force_exit && is_key) || force_exit) {
            lss_log_info("RTMP skip-to-live complete: elapsed=%lld ms, "
                         "rate=%.2f, key=%d, forced=%d",
                         (long long)elapsed, skip_rate_ema_,
                         (int)is_key, (int)force_exit);
            rtmp_skip_to_live_ = false;
            
            // Re-anchor clocks right at the exit point so the new frames
            // play immediately at the current wall clock.
            has_pts_offset_ = false;
            pts_to_obs_offset_ns_ = 0;
            first_video_pts_ns_ = 0;
            first_audio_pts_ns_ = 0;
            last_video_pts_us_.store(0);
            last_audio_pts_us_.store(0);
            has_delay_ref_ = false;

            if (is_key) {
              goto decode_packet;
            } else {
              av_packet_unref(pkt);
              continue;
            }
          }
        }
        av_packet_unref(pkt);
        continue;
      }
      decode_packet:

      // Burst-skip: discard packets until stream PTS reaches the live-edge estimate.
      // Triggered by large drops (>2s). Gives <1s recovery by discarding buffered burst
      // at packet level (no decode cost). Audio is also suppressed via has_pts_offset_=false.
      if (burst_skip_active_) {
        // Safety timeout: if target not reached within 5s, abort.
        if (now_ms() - burst_skip_start_ms_ > 5000) {
          lss_log_warn("Burst-skip timeout — aborting (target=%lld ms not reached)",
                       (long long)burst_skip_target_pts_ms_);
          burst_skip_active_ = false;
        } else if (pkt->stream_index == demuxer_.video_stream_index() &&
                   pkt->pts != AV_NOPTS_VALUE) {
          AVRational tb = demuxer_.video_time_base();
          int64_t pts_ms = av_rescale_q(pkt->pts, tb, {1, 1000});
          if (pts_ms < burst_skip_target_pts_ms_) {
            av_packet_unref(pkt);
            continue; // discard — not at live edge yet
          }
          // Reached target. Reanchor at this keyframe (or close to it).
          burst_skip_active_ = false;
          has_pts_offset_ = false;
          pts_to_obs_offset_ns_ = 0;
          first_video_pts_ns_ = 0;
          first_audio_pts_ns_ = 0;
          last_video_pts_us_.store(0);
          last_audio_pts_us_.store(0);
          has_delay_ref_ = false;
          lss_log_info("Burst-skip complete: pts=%lld ms, target=%lld ms, elapsed=%lld ms",
                       (long long)pts_ms, (long long)burst_skip_target_pts_ms_,
                       (long long)(now_ms() - burst_skip_start_ms_));
          // Fall through to decode this first live-edge packet.
        } else if (pkt->stream_index == demuxer_.audio_stream_index()) {
          av_packet_unref(pkt);
          continue; // discard audio during burst-skip
        }
      }

      bitrate_mon_.record_bytes(pkt->size);

      // Old HLS catch-up system: skip packets to reach live edge
      // This is ONLY for HLS streams, NOT for RTMP/standard streams!
      bool is_hls = (stream_type_ == StreamType::HLS) ||
                    (stream_type_ == StreamType::AmazonIVS);
      
      if (catching_up_ && is_hls && pkt->stream_index == demuxer_.video_stream_index()) {
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
      } else if (catching_up_ && is_hls &&
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

        // Late initialization for streams that start with 0x0 resolution
        if (!video_dec_.is_initialized() && demuxer_.video_width() > 0) {
          lss_log_info("Late video decoder initialization: %dx%d",
                       demuxer_.video_width(), demuxer_.video_height());
          video_dec_.init(demuxer_.video_codecpar(), hw_decode_);
          video_dec_.set_stream_time_base(demuxer_.video_time_base());
          video_codec_name_ =
              avcodec_get_name(demuxer_.video_codecpar()->codec_id);
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

      av_packet_unref(pkt);


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
  bool prev_seen = first_frame_received_.load();
  while (running_.load()) {
    const bool cur_seen = first_frame_received_.load();

    // Detect live → disconnected transition. Arm the fade-out so the next
    // few frames blend rising noise over the cached last live frame instead
    // of cutting hard to shimmer.
    if (prev_seen && !cur_seen && has_last_frame_) {
      fade_out_start_ms_.store(now_ms());
    }
    // Reconnected mid-fade: cancel pending fade-out (fade-in path takes over).
    if (cur_seen && fade_out_start_ms_.load() > 0) {
      fade_out_start_ms_.store(0);
    }
    prev_seen = cur_seen;

    if (show_shimmer_ && !cur_seen) {
      const int64_t fo_start = fade_out_start_ms_.load();
      const int64_t elapsed = fo_start > 0 ? (now_ms() - fo_start) : INT64_MAX;
      if (elapsed < FADEOUT_DURATION_MS) {
        // Ease-in (quadratic): noise grows slowly at first, fast at the end.
        // Mirrors the fade-in's ease-out so disappear/appear feel symmetric.
        double a = (double)elapsed / (double)FADEOUT_DURATION_MS;
        a = a * a;
        output_fadeout_frame(a);
      } else {
        if (fo_start > 0) fade_out_start_ms_.store(0);  // fade-out complete
        output_shimmer_frame();
      }
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
  prev_low_bitrate_.store(false);
  last_low_bitrate_time_ms_.store(0);
  {
    std::lock_guard<std::mutex> lock(toggle_mutex_);
    disconnect_overlay_.reset(false, now_ms());
    loading_overlay_.reset(false, now_ms());
    prev_disconnected_shown_ = false;
    prev_loading_shown_ = false;
  }

  if (stream_type_ == StreamType::AmazonIVS ||
      stream_type_ == StreamType::HLS) {
    lss_log_info("HLS/IVS detected: Disabling low-latency catchup logic");
  }

  if (!low_bitrate_src_name_.empty())
    toggle_source_visibility(low_bitrate_src_name_, false);
  if (!disconnect_src_name_.empty())
    toggle_source_visibility(disconnect_src_name_, false);
  // No grace period on start — nothing is playing yet.
  if (!loading_src_name_.empty()) {
    toggle_source_visibility(loading_src_name_, true);
    std::lock_guard<std::mutex> lock(toggle_mutex_);
    loading_overlay_.reset(true, now_ms());
    prev_loading_shown_ = true;
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
  {
    std::lock_guard<std::mutex> lock(toggle_mutex_);
    loading_overlay_.reset(false, now_ms());
    prev_loading_shown_ = false;
  }
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

  // Drop any in-flight catch-up state — fresh connection starts at 1.0x.


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
  // Called from both the worker and shimmer threads — serialize.
  std::lock_guard<std::mutex> lock(toggle_mutex_);

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

  bool dis_raw = false;
  bool loading_raw = false;

  // 1. We are connected but buffering (no frame yet)
  bool buffering = connected && !first_frame;

  // 2. Initial grace period: only at the very beginning (no frames ever received)
  bool initial_connecting = !ever_frame && (attempts <= 2);

  if (buffering) {
    // Connected but waiting for first frame - show loading
    loading_raw = true;
  } else if (initial_connecting) {
    // Initial connection attempts - show loading
    loading_raw = true;
  } else if (!connected) {
    // Disconnected after receiving frames - show disconnect
    dis_raw = true;
  }

  // Hysteresis — the raw states above bounce every second or two on a bad
  // connection, and each flip would strobe an overlay and queue a UI task.
  const int64_t grace = disconnect_grace_ms_.load();
  bool dis_now =
      disconnect_overlay_.step(dis_raw, now, grace, DISCONNECT_MIN_VISIBLE_MS);

  // Loading stays down while disconnect is up, so a reconnect that fails
  // again doesn't ping-pong between the two.
  bool loading_now = loading_overlay_.step(loading_raw && !dis_now, now, grace,
                                           LOADING_MIN_VISIBLE_MS);

  bool dis_showing = prev_disconnected_shown_;
  if (dis_now != dis_showing) {
    lss_log_info("[TOGGLE] Disconnect state changed: connected=%d dis_now=%d "
                  "dis_showing=%d ever_frame=%d attempts=%d src='%s'",
                  connected, dis_now, dis_showing, ever_frame, attempts,
                  disconnect_src_name_.c_str());

    if (dis_now)
      obs_source_output_video2(obs_source_, nullptr);

    if (!disconnect_src_name_.empty())
      toggle_source_visibility(disconnect_src_name_, dis_now);
    prev_disconnected_shown_ = dis_now;
  }

  if (loading_now != prev_loading_shown_) {
    lss_log_debug(
        "[TOGGLE] Loading state changed: loading_now=%d connected=%d "
        "first_frame=%d ever_frame=%d src='%s'",
        loading_now, connected, first_frame, ever_frame,
        loading_src_name_.c_str());
    if (!loading_src_name_.empty())
      toggle_source_visibility(loading_src_name_, loading_now);
    prev_loading_shown_ = loading_now;
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

  // Keep handler registered under the current name (handles late naming / renames)
  if (src_name && *src_name)
    WsStatsServer::instance().register_command_handler(
        source_name, [this](const std::string &cmd) { handle_remote_command(cmd); });

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
  bool orch_active = orchestrator_ && orchestrator_->is_active();
  CatchupState orch_state = orchestrator_ ? orchestrator_->get_state() : CatchupState::NORMAL;
  double orch_tempo = orchestrator_ ? orchestrator_->get_tempo() : 1.0;
  int64_t orch_drift_ms = buffer_manager_.get_state().drift_from_live_ms;
  static const char *kStateNames[] = {"Normal", "Starting", "Active", "Exiting", "Cooldown"};
  ss << "\"catchup_active\":" << (orch_active ? "true" : "false") << ",";
  ss << "\"catchup_state\":\"" << kStateNames[(int)orch_state] << "\",";
  ss << "\"catchup_tempo\":" << std::fixed << std::setprecision(3) << orch_tempo << ",";
  ss << "\"catchup_drift_ms\":" << orch_drift_ms << ",";
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
    std::string cmd = "open '" + url + "'";
    system(cmd.c_str());
  }).detach();
#else
  std::thread([url]() {
    std::string cmd = "xdg-open '" + url + "'";
    system(cmd.c_str());
  }).detach();
#endif
  return false;
}

bool LiveStreamSource::on_show_window_stats_clicked(obs_properties_t *,
                                                    obs_property_t *, void *) {
  PluginDialog::show_instance();
  return false;
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

  return false;
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

  return false;
}

bool LiveStreamSource::on_refresh_clicked(obs_properties_t *, obs_property_t *,
                                          void *data) {
  auto *self = static_cast<LiveStreamSource *>(data);
  if (!self)
    return false;

  // Don't restart directly from the UI thread  - unsafe!
  self->pending_refresh_.store(true);

  obs_source_update(self->obs_source_, nullptr);

  return false;
}

bool LiveStreamSource::on_skip_to_live_clicked(obs_properties_t *,
                                                obs_property_t *, void *data) {
  auto *self = static_cast<LiveStreamSource *>(data);
  if (!self)
    return false;

  // In-place demuxer reconnect handled by the worker thread. The UI
  // state (first_frame_received_, OBS source frame) is NOT cleared,
  // so OBS keeps showing the current frame until the new connection
  // delivers fresh content — no shimmer / loading overlay.
  lss_log_info("Skip to live edge requested");
  self->pending_skip_to_live_.store(true);
  return false;
}

void LiveStreamSource::handle_remote_command(const std::string &cmd) {
  if (cmd == "refresh") {
    lss_log_info("Remote command: refresh");
    pending_refresh_.store(true);
    // obs_source_update must run on the OBS main thread
    obs_queue_task(OBS_TASK_UI, [](void *param) {
      auto *self = static_cast<LiveStreamSource *>(param);
      obs_source_update(self->obs_source_, nullptr);
    }, this, false);
  } else if (cmd == "live_to_edge") {
    lss_log_info("Remote command: live to edge");
    pending_skip_to_live_.store(true);
  } else if (cmd == "mute") {
    dashboard_muted_.store(true);
    lss_log_info("Remote command: mute ON");
  } else if (cmd == "unmute") {
    dashboard_muted_.store(false);
    lss_log_info("Remote command: mute OFF");
  } else if (cmd == "blur") {
    dashboard_blurred_.store(true);
    lss_log_info("Remote command: blur ON");
  } else if (cmd == "unblur") {
    dashboard_blurred_.store(false);
    lss_log_info("Remote command: blur OFF");
  } else if (cmd == "preview_video_on") {
    preview_video_enabled_.store(true);
  } else if (cmd == "preview_video_off") {
    preview_video_enabled_.store(false);
  } else if (cmd == "preview_audio_on") {
    preview_audio_enabled_.store(true);
  } else if (cmd == "preview_audio_off") {
    preview_audio_enabled_.store(false);
  }
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
  obs_properties_add_button(props, PROP_SKIP_TO_LIVE,
                            obs_module_text("SkipToLiveBtn"),
                            on_skip_to_live_clicked);
  obs_properties_add_bool(props, PROP_HW_DECODE, obs_module_text("HwDecode"));
  obs_properties_add_bool(props, PROP_SHOW_SHIMMER, obs_module_text("ShowShimmer"));
  obs_properties_add_int(props, PROP_LOW_BITRATE,
                         obs_module_text("LowBitrateThreshold"), 10, 10000, 10);

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

  obs_property_t *grace = obs_properties_add_int(
      props, PROP_DISCONNECT_GRACE, obs_module_text("DisconnectGrace"),
      MIN_DISCONNECT_GRACE_MS, MAX_DISCONNECT_GRACE_MS, 100);
  obs_property_int_set_suffix(grace, " ms");
  obs_property_set_long_description(grace,
                                    obs_module_text("DisconnectGrace.Desc"));

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
  obs_data_set_default_int(settings, PROP_DISCONNECT_GRACE,
                           DEFAULT_DISCONNECT_GRACE_MS);
  obs_data_set_default_int(settings, PROP_WHEP_MODE,
                           static_cast<int>(WhepClient::WhepMode::Auto));
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

} // namespace lss

