// live-stream-source.cpp
#include "core/live-stream-source.h"
#include "core/fallback-settings.h"
#include "core/fallback-template.h"
#include "core/plugin-settings.h"
#include "html-assets.h"
#include "ui/fallback-dialog.h"
#include "ui/sync-notice.h"
#include "ui/url-field-guard.h"
#include "util/platform.h"
#include "utils/reconnect-manager.h"
#include <cstring>
#include <map>
#include <set>
#include <utility>
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

// Bounds what the jitter buffer may hold if the decode side stops consuming.
static constexpr int64_t PLAYOUT_MAX_SPAN_US = 40000000;
static constexpr size_t PLAYOUT_MAX_BYTES = 256 * 1024 * 1024;

// WHEP: RTP vs sample-count disagreement (48 kHz samples) worth correcting.
static constexpr int64_t WHEP_AUDIO_RESYNC_SAMPLES = 4800;

LiveStreamSource::LiveStreamSource(obs_source_t *source, obs_data_t *settings)
    : obs_source_(source), packets_(PLAYOUT_MAX_SPAN_US, PLAYOUT_MAX_BYTES) {
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
  SyncRegistry::instance().set_start_handler(obs_source_get_uuid(obs_source_),
                                             [this]() { request_sync_listen(); });
  update(settings);
}

LiveStreamSource::~LiveStreamSource() {
  SyncRegistry::instance().remove_start_handler(obs_source_get_uuid(obs_source_));
  stop_stream();
  const char *uuid = obs_source_get_uuid(obs_source_);
  SyncRegistry::instance().stop_listening(uuid);
  SyncRegistry::instance().remove(uuid);
  const char *name = obs_source_get_name(obs_source_);
  if (name) {
    WsStatsServer::instance().unregister_command_handler(name);
    WsStatsServer::instance().unregister_connect_callback(name);
    WsStatsServer::instance().remove_source(name);
  }
  WsStatsServer::instance().release();
}

static FallbackSpec read_fallback(obs_data_t *settings, const char *prefix) {
  const auto text_of = [&](const char *field) -> std::string {
    return obs_data_get_string(settings, fallback_key(prefix, field).c_str());
  };
  const auto number_of = [&](const char *field) {
    return static_cast<int>(obs_data_get_int(settings, fallback_key(prefix, field).c_str()));
  };
  const auto color_of = [&](const char *field) {
    return static_cast<uint32_t>(obs_data_get_int(settings, fallback_key(prefix, field).c_str()));
  };

  FallbackSpec spec;
  spec.kind = static_cast<FallbackKind>(number_of("kind"));
  spec.text = text_of("text");
  if (obs_data_t *font = obs_data_get_obj(settings, fallback_key(prefix, "font").c_str())) {
    const char *json = obs_data_get_json(font);
    spec.font_json = json ? json : "";
    obs_data_release(font);
  }
  spec.text_color = color_of("text_color");
  spec.outline = obs_data_get_bool(settings, fallback_key(prefix, "outline").c_str());
  spec.outline_size = number_of("outline_size");
  spec.outline_color = color_of("outline_color");
  spec.file = text_of("file");
  spec.url = text_of("url");
  spec.template_name = text_of("template");
  spec.template_scale = number_of("scale");
  spec.fit = static_cast<FallbackFit>(number_of("fit"));
  spec.size_percent = number_of("size");
  spec.anchor = number_of("anchor");
  spec.keep_picture = obs_data_get_bool(settings, fallback_key(prefix, "keep").c_str());
  spec.background_color = color_of("background_color");
  spec.background_file = text_of("background_file");
  spec.anim_in = static_cast<FallbackAnim>(number_of("anim_in"));
  spec.anim_out = static_cast<FallbackAnim>(number_of("anim_out"));
  spec.anim_in_ms = number_of("anim_in_ms");
  spec.anim_out_ms = number_of("anim_out_ms");
  return spec;
}

// A source set up before this existed names a scene item: keep it working.
static void adopt_legacy_fallback(obs_data_t *settings, const char *prefix, const char *legacy_key) {
  const std::string key = fallback_key(prefix, "kind");
  const char *legacy = obs_data_get_string(settings, legacy_key);
  if (!obs_data_has_user_value(settings, key.c_str()) && legacy && *legacy)
    obs_data_set_int(settings, key.c_str(), static_cast<int>(FallbackKind::SceneSource));
}

// Runs on the UI thread, where creating the content is safe. The reference keeps
// the source alive until then.
struct FallbackTask {
  obs_source_t *source;
  LiveStreamSource *self;
  FallbackState state;
  FallbackSpec spec;
};

void LiveStreamSource::apply_fallback_settings(obs_data_t *settings) {
  adopt_legacy_fallback(settings, FALLBACK_DISCONNECT, PROP_DISCONNECT_SOURCE);
  adopt_legacy_fallback(settings, FALLBACK_LOW, PROP_LOW_BITRATE_SOURCE);
  adopt_legacy_fallback(settings, FALLBACK_LOADING, PROP_LOADING_SOURCE);

  const std::pair<FallbackState, const char *> layers[] = {
      {FallbackState::Disconnected, FALLBACK_DISCONNECT},
      {FallbackState::LowBitrate, FALLBACK_LOW},
      {FallbackState::Loading, FALLBACK_LOADING},
  };
  disconnect_fallback_.store(read_fallback(settings, FALLBACK_DISCONNECT).draws_itself());
  loading_fallback_.store(read_fallback(settings, FALLBACK_LOADING).draws_itself());

  for (const auto &layer : layers) {
    obs_source_t *ref = obs_source_get_ref(obs_source_);
    if (!ref)
      return;
    obs_queue_task(
        OBS_TASK_UI,
        [](void *param) {
          auto *task = static_cast<FallbackTask *>(param);
          task->self->fallback_layer().configure(task->source, task->state, task->spec);
          obs_source_release(task->source);
          delete task;
        },
        new FallbackTask{ref, this, layer.first, read_fallback(settings, layer.second)}, false);
  }
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
  int new_target = static_cast<int>(obs_data_get_int(settings, PROP_TARGET_LATENCY));
  target_latency_ms_.store(new_target > 0 ? new_target : DEFAULT_TARGET_LATENCY_MS);
  recovery_mode_.store(static_cast<int>(obs_data_get_int(settings, PROP_RECOVERY_MODE)));
  stream_type_ = new_stream_type;
  const bool was_covered = fallback_covers_picture();
  apply_fallback_settings(settings);

  // Clear what's on screen if nothing is left to draw while the stream is down.
  if (was_covered && !first_frame_received_.load())
    drop_picture();
  const auto legacy_name = [settings](const char *prefix, const char *name) {
    const bool scene_source = obs_data_get_int(settings, fallback_key(prefix, "kind").c_str()) ==
                              static_cast<int>(FallbackKind::SceneSource);
    return scene_source && name ? std::string(name) : std::string();
  };
  low_bitrate_src_name_ = legacy_name(FALLBACK_LOW, lb_src);
  disconnect_src_name_ = legacy_name(FALLBACK_DISCONNECT, dc_src);
  loading_src_name_ = legacy_name(FALLBACK_LOADING, ld_src);

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
      // A read blocked in an outage would otherwise hold this up.
      demuxer_.request_abort();
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

// HLS/IVS output path. Standard streams go through the playout engine.
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

  // 2. PTS Anchor & Jump Detection
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
  } else if (first_video_pts_ns_ == 0) {
    first_video_pts_ns_ = stream_pts_ns;
    int64_t headroom_ns = 250LL * 1000000LL;
    pts_to_obs_offset_ns_ = wall_now_ns + headroom_ns - stream_pts_ns;
    lss_log_info("PTS re-anchored from video (audio was first): pts_ns=%lld, offset=%lld",
                 (long long)stream_pts_ns, (long long)pts_to_obs_offset_ns_);
  }

  int64_t computed_ts = stream_pts_ns + pts_to_obs_offset_ns_;
  last_video_pts_us_.store(stream_pts_ns / 1000);

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

  submit_video_frame(frame, static_cast<uint64_t>(computed_ts));

  int64_t recv_t = last_pkt_recv_us_.load();
  if (recv_t > 0) {
    pipeline_latency_ms_.store((now_us() - recv_t) / 1000);
  }
}

// Wraps a decoded frame for OBS and applies the on-frame effects. Shared by
// the HLS path and the playout present thread.
void LiveStreamSource::submit_video_frame(AVFrame *frame, uint64_t timestamp_ns) {
  static int log_count = 0;

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

  obs_frame.timestamp = timestamp_ns;

  if (log_count < 5) {
    lss_log_debug("output_video_frame: fmt=%d obs_fmt=%d %dx%d range=%d "
                  "obs_ts=%llu",
                  frame->format, (int)obs_fmt, frame->width, frame->height,
                  (int)obs_frame.range, (unsigned long long)obs_frame.timestamp);
    log_count++;
  }

  // Dissolve across a playout cut so the jump in content reads as a transition.
  if (playout_cut_fade_pending_.exchange(false))
    cut_fade_start_ms_ = now_ms();
  if (cut_fade_start_ms_ > 0) {
    const int64_t elapsed = now_ms() - cut_fade_start_ms_;
    if (elapsed < CUT_FADE_MS)
      apply_cut_crossfade(obs_frame, obs_fmt, frame,
                          1.0 - (double)elapsed / (double)CUT_FADE_MS);
    else
      cut_fade_start_ms_ = 0;
  }

  // Dashboard remote controls: blur the frame before submitting, then stamp
  // a mute icon in the bottom-right if the audio is muted. Both effects
  // operate on the trans_* buffers via in-place edits of obs_frame.data —
  // the decoder's AVFrame is never touched (cache_last_frame below relies
  // on that to remember the unaltered last live frame for the crossfade).
  const bool blurred = dashboard_blurred_.load();
  if (blurred)
    apply_blur_inplace(obs_frame, frame);
  if (dashboard_muted_.load()) {
    draw_mute_icon(obs_frame, obs_fmt, frame);
  }

  obs_source_output_video2(obs_source_, &obs_frame);
  // The cut crossfade blends from this cache, so it stays frozen until that
  // ends. It keeps the decoder's planes, not the trans buffers. While blurred
  // there's nothing to keep: the crossfade would put the original picture back.
  if (blurred)
    has_last_frame_ = false;
  else if (cut_fade_start_ms_ == 0)
    cache_last_frame(frame, obs_fmt, full_range);
  send_preview_video(frame);
  first_frame_received_.store(true);
  ever_received_frame_.store(true);

  if (width_.load() != frame->width || height_.load() != frame->height) {
    width_.store(frame->width);
    height_.store(frame->height);
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

// Always ends in I420 from the trans_* buffers, whatever came in, so the frame
// handed to OBS never needs a case of its own.
void LiveStreamSource::apply_blur_inplace(obs_source_frame2 &obs_frame,
                                           AVFrame *frame) {
  const int w = frame->width;
  const int h = frame->height;
  if (w <= 0 || h <= 0)
    return;

  const size_t y_size = static_cast<size_t>(w) * h;
  const int chroma_w = (w + 1) / 2;
  const size_t uv_size = static_cast<size_t>(chroma_w) * ((h + 1) / 2);
  if (trans_y_buf_.size() < y_size) trans_y_buf_.resize(y_size);
  if (trans_u_buf_.size() < uv_size) trans_u_buf_.resize(uv_size);
  if (trans_v_buf_.size() < uv_size) trans_v_buf_.resize(uv_size);

  const uint8_t *const src[4] = {obs_frame.data[0], obs_frame.data[1], obs_frame.data[2],
                                 nullptr};
  const int src_linesize[4] = {static_cast<int>(obs_frame.linesize[0]),
                               static_cast<int>(obs_frame.linesize[1]),
                               static_cast<int>(obs_frame.linesize[2]), 0};
  if (!privacy_blur_.apply(src, src_linesize, static_cast<AVPixelFormat>(frame->format), w, h,
                           trans_y_buf_.data(), trans_u_buf_.data(), trans_v_buf_.data()))
    lss_log_warn("Blur failed for format %d, sending black instead", frame->format);

  obs_frame.format = VIDEO_FORMAT_I420;
  obs_frame.data[0] = trans_y_buf_.data();
  obs_frame.data[1] = trans_u_buf_.data();
  obs_frame.data[2] = trans_v_buf_.data();
  obs_frame.linesize[0] = static_cast<uint32_t>(w);
  obs_frame.linesize[1] = static_cast<uint32_t>(chroma_w);
  obs_frame.linesize[2] = static_cast<uint32_t>(chroma_w);
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

// Blends the last frame shown before a cut over the first frames after it.
// alpha is the weight of the old picture. Writes to trans_*_buf_; a size or
// format change just skips the dissolve.
void LiveStreamSource::apply_cut_crossfade(obs_source_frame2 &obs_frame,
                                           video_format obs_fmt, AVFrame *frame,
                                           double alpha) {
  std::lock_guard<std::mutex> lk(last_frame_mutex_);
  if (!has_last_frame_ || last_frame_fmt_ != obs_fmt ||
      last_frame_w_ != frame->width || last_frame_h_ != frame->height ||
      last_frame_ly_ != frame->linesize[0])
    return;

  const int old_w = (int)(alpha * 256.0 + 0.5);
  const int new_w = 256 - old_w;

  auto blend = [&](std::vector<uint8_t> &dst, const uint8_t *src_new,
                   const uint8_t *src_old, int line, int rows) {
    const size_t size = (size_t)line * (size_t)rows;
    if (dst.size() < size)
      dst.resize(size);
    for (size_t i = 0; i < size; ++i)
      dst[i] = (uint8_t)((src_new[i] * new_w + src_old[i] * old_w) >> 8);
  };

  const int h = frame->height;
  if (obs_fmt == VIDEO_FORMAT_NV12) {
    if (last_frame_lu_ != frame->linesize[1])
      return;
    blend(trans_y_buf_, frame->data[0], last_y_buf_.data(), frame->linesize[0], h);
    blend(trans_u_buf_, frame->data[1], last_u_buf_.data(), frame->linesize[1], h / 2);
    obs_frame.data[0] = trans_y_buf_.data();
    obs_frame.data[1] = trans_u_buf_.data();
  } else if (obs_fmt == VIDEO_FORMAT_I420 || obs_fmt == VIDEO_FORMAT_I444) {
    if (last_frame_lu_ != frame->linesize[1] ||
        last_frame_lv_ != frame->linesize[2])
      return;
    const int uv_h = (obs_fmt == VIDEO_FORMAT_I420) ? h / 2 : h;
    blend(trans_y_buf_, frame->data[0], last_y_buf_.data(), frame->linesize[0], h);
    blend(trans_u_buf_, frame->data[1], last_u_buf_.data(), frame->linesize[1], uv_h);
    blend(trans_v_buf_, frame->data[2], last_v_buf_.data(), frame->linesize[2], uv_h);
    obs_frame.data[0] = trans_y_buf_.data();
    obs_frame.data[1] = trans_u_buf_.data();
    obs_frame.data[2] = trans_v_buf_.data();
  }
}

// Cache the decoded planes of the most recent live frame for the cut crossfade.
// Called from the worker thread after each successful obs_source_output_video2.
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

void LiveStreamSource::handle_freeze_skip_recovery(int64_t gap_ms) {
  lss_log_warn("Skip-resync (gap=%lld ms)", (long long)gap_ms);

  // Suppress re-entry for a window — prevents skip loops during sustained
  // bad-network conditions (e.g. continuous packet drop).
  catchup_cooldown_until_ms_ = now_ms() + FREEZE_COOLDOWN_MS;

  // Reset PTS anchor — output_*_frame will re-anchor when post-freeze frames
  // arrive.
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

  // Reset stream-delay reference so the new wall/pts baseline starts clean.
  has_delay_ref_ = false;

  catchup_first_wall_ms_ = 0;
  catchup_first_pts_ms_ = 0;
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
        playout_reset();
        first_frame_received_.store(false);
        demuxer_.close();
        {
          std::lock_guard<std::mutex> lock(decoder_mutex_);
          video_dec_.close();
          audio_dec_.close();
        }
        
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

        // Clear any abort left by the watchdog or a skip. Stop sets running_
        // before it aborts, so it can't be lost here.
        demuxer_.reset_abort();

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
        playout_reset();
        playout_cut_fade_pending_.store(true);
        {
          std::lock_guard<std::mutex> lock(decoder_mutex_);
          video_dec_.close();
          audio_dec_.close();
        }
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
        demuxer_.reset_abort();
        // Reopen demuxer + decoders. NOTE: connected_ stays true,
        // first_frame_received_ stays true → the picture stays up.
        if (demuxer_.open(url_, stream_type_) >= 0) {
          open_decoders();
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

      // Aborted on purpose: the flag is handled at the top of the loop, and
      // this isn't a disconnect, so the picture stays up.
      if (ret < 0 && ret != AVERROR(EAGAIN) &&
          (pending_refresh_.load() || pending_skip_to_live_.load())) {
        av_packet_unref(pkt);
        continue;
      }

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

        playout_reset();

        current_fps_.store(0.0);
        pipeline_latency_ms_.store(0);
        stream_delay_ms_.store(0);
        has_delay_ref_ = false;
        first_frame_received_.store(false);

        has_pts_offset_ = false;
        pts_to_obs_offset_ns_ = 0;
        first_video_pts_ns_ = 0;
        first_audio_pts_ns_ = 0;

        drop_picture();
        
        write_stats_json();
        update_source_toggles();

        av_packet_unref(pkt);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }

      if (ret == AVERROR(EAGAIN)) {
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

      // Standard streams: everything past this point is the playout engine's
      // job. The reader never waits on it, so the socket can't back up.
      if (uses_playout()) {
        last_pkt_recv_ms_ = pkt_recv_now;
        ingest_packet(pkt);
        av_packet_unref(pkt);
        update_periodic_stats();
        continue;
      }

      // Soft-freeze detection based strictly on true network wait time.
      // This ignores any delays caused by our own decoding or pacing sleeps,
      // completely eliminating false positive freezes.
      bool in_cooldown_now = (catchup_cooldown_until_ms_ > 0 &&
                              now_ms() < catchup_cooldown_until_ms_);

      if (network_wait_ms > FREEZE_RECOVERY_THRESHOLD_MS &&
          first_frame_received_.load() && !catching_up_ && !in_cooldown_now) {
        if (network_wait_ms >= FREEZE_RESYNC_MIN_GAP_MS &&
            network_wait_ms <= FREEZE_RESYNC_MAX_GAP_MS) {
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
          // estimate), then reanchors. Whichever stream delivered the first
          // packet after the gap sets the target: requiring video here left
          // the skip unarmed roughly every other drop.
          if (network_wait_ms >= BURST_SKIP_MIN_GAP_MS && !burst_skip_active_ &&
              pkt->pts != AV_NOPTS_VALUE) {
            AVRational tb = (pkt->stream_index == demuxer_.video_stream_index())
                                ? demuxer_.video_time_base()
                                : demuxer_.audio_time_base();
            int64_t first_pts_ms = av_rescale_q(pkt->pts, tb, {1, 1000});
            // Target: first burst PTS + gap duration - 300ms buffer for re-anchor headroom
            burst_skip_target_pts_ms_ = first_pts_ms + network_wait_ms - 300;
            burst_skip_active_ = true;
            burst_skip_wait_key_ = false;
            burst_skip_start_ms_ = now_ms();
            lss_log_info("Burst-skip enabled: first_pts=%lld ms, target=%lld ms (gap=%lld ms)",
                         (long long)first_pts_ms,
                         (long long)burst_skip_target_pts_ms_,
                         (long long)network_wait_ms);
          }

        } else if (network_wait_ms > FREEZE_RESYNC_MAX_GAP_MS) {
          handle_freeze_skip_recovery(network_wait_ms);
        }
      }
      last_pkt_recv_ms_ = pkt_recv_now;

      // Burst-skip: discard packets until stream PTS reaches the live-edge estimate.
      // Triggered by large drops (>2s). Gives <1s recovery by discarding buffered burst
      // at packet level (no decode cost). Audio is also suppressed via has_pts_offset_=false.
      if (burst_skip_active_) {
        // Safety timeout: if target not reached within 5s, abort.
        if (now_ms() - burst_skip_start_ms_ > 5000) {
          lss_log_warn("Burst-skip timeout — aborting (target=%lld ms not reached)",
                       (long long)burst_skip_target_pts_ms_);
          burst_skip_active_ = false;
          video_dec_.reset_state();
        } else if (pkt->stream_index == demuxer_.video_stream_index() &&
                   pkt->pts != AV_NOPTS_VALUE) {
          AVRational tb = demuxer_.video_time_base();
          int64_t pts_ms = av_rescale_q(pkt->pts, tb, {1, 1000});
          if (pts_ms >= burst_skip_target_pts_ms_)
            burst_skip_wait_key_ = true;
          // Resuming on anything but a keyframe feeds the decoder P-frames
          // whose references were just discarded.
          if (!burst_skip_wait_key_ || !(pkt->flags & AV_PKT_FLAG_KEY)) {
            av_packet_unref(pkt);
            continue; // discard — not at live edge yet
          }
          // Reached target. Reanchor at this keyframe.
          burst_skip_active_ = false;
          video_dec_.reset_state();
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

        if (pkt->dts != AV_NOPTS_VALUE)
          update_stream_delay(
              av_rescale_q(pkt->dts, demuxer_.video_time_base(), {1, 1000}));

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

      update_periodic_stats();
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

void LiveStreamSource::update_periodic_stats() {
  int64_t now = now_ms();
  if (now - stats_last_write_ms_ < STATS_WRITE_INTERVAL_MS)
    return;

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

// Keeps the overlays current even if the worker is blocked on a slow read.
void LiveStreamSource::overlay_thread_func() {
  lss_log_debug("Overlay thread started");
  while (running_.load()) {
    // A held frame was shown unblurred, so it can't stay once blur is on.
    if (!first_frame_received_.load() && dashboard_blurred_.load() && fallback_covers_picture()) {
      std::lock_guard<std::mutex> lk(last_frame_mutex_);
      if (has_last_frame_) {
        has_last_frame_ = false;
        obs_source_output_video2(obs_source_, nullptr);
      }
    }

    update_source_toggles();

    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  lss_log_debug("Overlay thread stopped");
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
  overlay_thread_ = std::thread(&LiveStreamSource::overlay_thread_func, this);
  if (stream_type_ != StreamType::WHEP) {
    playout_decode_thread_ =
        std::thread(&LiveStreamSource::playout_decode_thread_func, this);
    playout_present_thread_ =
        std::thread(&LiveStreamSource::playout_present_thread_func, this);
  }
}

void LiveStreamSource::stop_stream() {
  lss_log_debug("stop_stream: begin");
  running_.store(false);

  lss_log_debug("stop_stream: requesting abort");
  demuxer_.request_abort();

  lss_log_debug("stop_stream: joining worker thread");
  if (worker_thread_.joinable())
    worker_thread_.join();

  lss_log_debug("stop_stream: joining playout threads");
  present_cv_.notify_all();
  if (playout_decode_thread_.joinable())
    playout_decode_thread_.join();
  if (playout_present_thread_.joinable())
    playout_present_thread_.join();

  lss_log_debug("stop_stream: joining connect thread");
  if (connect_thread_.joinable())
    connect_thread_.join();

  lss_log_debug("stop_stream: joining overlay thread");
  if (overlay_thread_.joinable())
    overlay_thread_.join();

  playout_reset();

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
  playout_reset();
  {
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    video_dec_.close();
    audio_dec_.close();
  }

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

  open_decoders();

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

// Shared by connect and skip-to-live. Playout packets are rescaled to
// microseconds on ingest, the HLS path keeps stream time bases.
void LiveStreamSource::open_decoders() {
  const bool playout = stream_type_ == StreamType::Standard;
  obs_source_set_async_unbuffered(obs_source_, playout);

  std::lock_guard<std::mutex> lock(decoder_mutex_);
  playout_enabled_.store(playout);

  const bool has_video = demuxer_.video_stream_index() >= 0;
  const bool has_audio = demuxer_.audio_stream_index() >= 0;
  playout_has_video_.store(has_video);
  playout_has_audio_.store(has_audio);

  if (has_video) {
    video_dec_.init(demuxer_.video_codecpar(), hw_decode_);
    video_dec_.set_stream_time_base(demuxer_.video_time_base());
    video_codec_name_ = avcodec_get_name(demuxer_.video_codecpar()->codec_id);
  } else {
    video_codec_name_ = "none";
  }
  if (has_audio) {
    audio_dec_.init(demuxer_.audio_codecpar());
    audio_dec_.set_stream_time_base(playout ? AVRational{1, 1000000}
                                            : demuxer_.audio_time_base());
    audio_codec_name_ = avcodec_get_name(demuxer_.audio_codecpar()->codec_id);
  } else {
    audio_codec_name_ = "none";
  }
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

// A fallback arrives after the disconnect delay, or plays over the stream's return.
// Until then the last frame is what's on screen, so it can't go black.
bool LiveStreamSource::fallback_covers_picture() const {
  return ever_received_frame_.load() ? disconnect_fallback_.load() : loading_fallback_.load();
}

void LiveStreamSource::drop_picture() {
  if (!fallback_covers_picture())
    obs_source_output_video2(obs_source_, nullptr);
}

void LiveStreamSource::update_source_toggles() {
  // Called from both the worker and overlay threads — serialize.
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
  // The socket can stay open through an outage. Viewers don't care which it
  // is, a frozen picture past the grace period gets the same overlay.
  bool stalled = playout_stalled_.load() && first_frame;

  bool dis_raw = false;
  bool loading_raw = false;

  // 1. We are connected but buffering (no frame yet)
  bool buffering = connected && !first_frame;

  // 2. Initial grace period: only at the very beginning (no frames ever received)
  bool initial_connecting = !ever_frame && (attempts <= 2);

  if (buffering && ever_frame && disconnect_fallback_.load() && !loading_fallback_.load()) {
    // Nothing takes over, so disconnect waits for the picture itself
    dis_raw = true;
  } else if (buffering) {
    // Connected but waiting for first frame - show loading
    loading_raw = true;
  } else if (initial_connecting) {
    // Initial connection attempts - show loading
    loading_raw = true;
  } else if (!connected || stalled) {
    // Disconnected after receiving frames - show disconnect
    dis_raw = true;
  }

  // Hysteresis — the raw states above bounce every second or two on a bad
  // connection, and each flip would strobe an overlay and queue a UI task.
  const int64_t grace = disconnect_grace_ms_.load();
  bool dis_now =
      disconnect_overlay_.step(dis_raw, now, grace, DISCONNECT_MIN_VISIBLE_MS);

  // The stream is back: loading comes in as disconnect goes out, with no gap
  // between them.
  if (prev_disconnected_shown_ && loading_raw && loading_fallback_.load()) {
    disconnect_overlay_.reset(false, now);
    loading_overlay_.reset(true, now);
    dis_now = false;
  }

  // Otherwise loading stays down while disconnect is up, so a reconnect that
  // fails again doesn't ping-pong between the two.
  bool loading_now = loading_overlay_.step(loading_raw && !dis_now, now, grace,
                                           LOADING_MIN_VISIBLE_MS);

  fallback_layer_.show(dis_now       ? FallbackState::Disconnected
                       : loading_now ? FallbackState::Loading
                       : low_now     ? FallbackState::LowBitrate
                                     : FallbackState::None);

  bool dis_showing = prev_disconnected_shown_;
  if (dis_now != dis_showing) {
    lss_log_info("[TOGGLE] Disconnect state changed: connected=%d dis_now=%d "
                  "dis_showing=%d ever_frame=%d attempts=%d src='%s'",
                  connected, dis_now, dis_showing, ever_frame, attempts,
                  disconnect_src_name_.c_str());

    // With no overlay configured a stall keeps the frozen frame on screen
    // rather than dropping the source to transparent.
    if (dis_now && (!connected || !disconnect_src_name_.empty()))
      drop_picture();

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

  // Two streams can share a scene item, and one recovering mustn't hide it while
  // the other is still down.
  static std::mutex users_mutex;
  static std::map<std::string, std::set<LiveStreamSource *>> users;
  bool any_user;
  {
    std::lock_guard<std::mutex> lock(users_mutex);
    auto &list = users[name];
    if (visible)
      list.insert(this);
    else
      list.erase(this);
    any_user = !list.empty();
  }

  auto *data = new SourceTaskData{name, any_user};
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
  const double rate = playout_rate_.load();
  const bool stalled = playout_stalled_.load();
  const int64_t latency_ms = playout_latency_ms_.load();
  const int64_t target_ms = playout_target_ms_.load();
  const char *recovery_state = stalled        ? "Buffering"
                               : rate > 1.001 ? "Catching up"
                               : rate < 0.999 ? "Refilling"
                                              : "Normal";
  ss << "\"catchup_active\":" << (stalled || rate != 1.0 ? "true" : "false") << ",";
  ss << "\"catchup_state\":\"" << recovery_state << "\",";
  ss << "\"catchup_tempo\":" << std::fixed << std::setprecision(3) << rate << ",";
  ss << "\"catchup_drift_ms\":" << std::max<int64_t>(0, latency_ms - target_ms) << ",";
  ss << "\"target_latency_ms\":" << target_ms << ",";
  ss << "\"muted\":" << (dashboard_muted_.load() ? "true" : "false") << ",";
  ss << "\"blurred\":" << (dashboard_blurred_.load() ? "true" : "false") << ",";
  ss << "\"sync_active\":" << (sync_active_.load() ? "true" : "false") << ",";
  ss << "\"sync_error_ms\":" << sync_error_ms_.load() << ",";
  static const char *kListenStates[] = {"Idle",     "Listening", "Found",
                                        "NotFound", "NoSound",   "NoStream"};
  ss << "\"sync_listen_state\":\"" << kListenStates[sync_listen_state_.load()] << "\",";
  ss << "\"sync_listen_delta_ms\":" << sync_listen_delta_ms_.load() << ",";
  ss << "\"sync_listen_left_s\":" << sync_listen_left_s_.load() << ",";
  ss << "\"sync_heard_main\":" << sync_heard_main_.load() << ",";
  ss << "\"sync_heard_own\":" << sync_heard_own_.load() << ",";
  ss << "\"sync_listen_held_ms\":" << sync_listen_held_ms_.load() << ",";
  ss << "\"sync_listen_short_ms\":" << sync_listen_short_ms_.load() << ",";
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

// At module load, before any browser source in the scene collection opens these.
static void install_stats_pages() {
  char path[512];
  if (os_get_config_path(path, sizeof(path), "obs-studio/plugin_config/live-stream-source") <= 0)
    return;
  os_mkdirs(path);

  const std::string token = get_plugin_page_token();
  const std::pair<const char *, const char *> pages[] = {
      {"/dashboard.html", DASHBOARD_HTML},
      {"/overlay.html", OVERLAY_HTML},
  };
  for (const auto &page : pages) {
    std::ofstream out(std::string(path) + page.first);
    out << replace_all(page.second, "__LSS_PAGE_TOKEN__", token);
  }
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
  url += "?ip=127.0.0.1";
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
  url += "?ip=127.0.0.1";
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
  // delivers fresh content — no loading overlay.
  lss_log_info("Skip to live edge requested");
  self->request_skip_to_live();
  return false;
}

void LiveStreamSource::request_skip_to_live() {
  pending_skip_to_live_.store(true);
  // Only a connected stream has a read to interrupt.
  if (connected_.load() && uses_playout())
    demuxer_.request_abort();
}

void LiveStreamSource::handle_remote_command(const std::string &cmd) {
  if (cmd == "refresh") {
    lss_log_info("Remote command: refresh");
    pending_refresh_.store(true);
    // obs_source_update must run on the OBS main thread. The reference keeps the
    // source around until then.
    if (obs_source_t *ref = obs_source_get_ref(obs_source_)) {
      obs_queue_task(OBS_TASK_UI, [](void *param) {
        auto *source = static_cast<obs_source_t *>(param);
        obs_source_update(source, nullptr);
        obs_source_release(source);
      }, ref, false);
    }
  } else if (cmd == "live_to_edge") {
    lss_log_info("Remote command: live to edge");
    request_skip_to_live();
  } else if (cmd == "sync_listen") {
    SyncRegistry::instance().start_sync();
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
  std::vector<std::string> *names;
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
    d->names->push_back(name);

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

std::vector<std::string> LiveStreamSource::scene_source_names() const {
  std::vector<std::string> names;
  PopulateListData data{&names, obs_source_};
  obs_enum_scenes(find_scene_proc, &data);
  return names;
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
  protect_url_field(); // returning true below rebuilds the field

  auto type =
      static_cast<StreamType>(obs_data_get_int(settings, PROP_STREAM_TYPE));
  bool is_whep = (type == StreamType::WHEP);
  bool is_standard = (type == StreamType::Standard);

  obs_property_t *p_token = obs_properties_get(props, PROP_WHEP_TOKEN);
  obs_property_t *p_mode = obs_properties_get(props, PROP_WHEP_MODE);
  obs_property_t *p_latency = obs_properties_get(props, PROP_TARGET_LATENCY);
  obs_property_t *p_recovery = obs_properties_get(props, PROP_RECOVERY_MODE);

  if (p_token)
    obs_property_set_visible(p_token, is_whep);
  if (p_mode)
    obs_property_set_visible(p_mode, is_whep);
  if (p_latency)
    obs_property_set_visible(p_latency, is_standard);
  if (p_recovery)
    obs_property_set_visible(p_recovery, is_standard);

  return true;
}

const char *LiveStreamSource::s_get_name(void *) {
  return obs_module_text("LiveStreamSource");
}

bool LiveStreamSource::on_fallback_settings_clicked(obs_properties_t *, obs_property_t *,
                                                    void *data) {
  auto *self = static_cast<LiveStreamSource *>(data);
  obs_data_t *settings = obs_source_get_settings(self->obs_source_);
  open_fallback_dialog(settings, self->scene_source_names(), [self](obs_data_t *changes) {
    obs_source_update(self->obs_source_, changes);
  });
  obs_data_release(settings);
  return false;
}

obs_properties_t *LiveStreamSource::get_properties(void *data) {
  obs_properties_t *props = obs_properties_create();

  obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

  // Only a URL someone entered is hidden: the default one isn't worth an extra click.
  bool hide_url = false;
  if (auto *self = static_cast<LiveStreamSource *>(data)) {
    obs_data_t *settings = obs_source_get_settings(self->obs_source_);
    hide_url = obs_data_has_user_value(settings, PROP_URL) && *obs_data_get_string(settings, PROP_URL);
    obs_data_release(settings);
  }

  obs_properties_t *connection = obs_properties_create();
  obs_property_t *url = obs_properties_add_text(connection, PROP_URL, obs_module_text("StreamURL"),
                                                hide_url ? OBS_TEXT_PASSWORD : OBS_TEXT_DEFAULT);
  if (hide_url) {
    obs_property_set_long_description(url, obs_module_text("StreamURL.Desc"));
    protect_url_field();
  }

  obs_property_t *st = obs_properties_add_list(
      connection, PROP_STREAM_TYPE, obs_module_text("StreamType"),
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

  obs_properties_add_text(connection, PROP_WHEP_TOKEN, obs_module_text("WHEPToken"),
                          OBS_TEXT_PASSWORD);

  obs_property_t *wm = obs_properties_add_list(
      connection, PROP_WHEP_MODE, obs_module_text("WHEPMode"), OBS_COMBO_TYPE_LIST,
      OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(wm, obs_module_text("WHEPModeAuto"),
                            static_cast<int>(WhepClient::WhepMode::Auto));
  obs_property_list_add_int(wm, obs_module_text("WHEPModeVideoAudio"),
                            static_cast<int>(WhepClient::WhepMode::VideoAudio));
  obs_property_list_add_int(wm, obs_module_text("WHEPModeVideoOnly"),
                            static_cast<int>(WhepClient::WhepMode::VideoOnly));
  obs_property_list_add_int(wm, obs_module_text("WHEPModeAudioOnly"),
                            static_cast<int>(WhepClient::WhepMode::AudioOnly));

  obs_properties_add_button(connection, PROP_REFRESH, obs_module_text("RefreshBtn"),
                            on_refresh_clicked);
  obs_properties_add_button(connection, PROP_SKIP_TO_LIVE,
                            obs_module_text("SkipToLiveBtn"),
                            on_skip_to_live_clicked);
  obs_properties_add_group(props, "group_connection", obs_module_text("PropsConnection"), OBS_GROUP_NORMAL, connection);

  obs_properties_t *playback = obs_properties_create();
  obs_property_t *lat = obs_properties_add_list(
      playback, PROP_TARGET_LATENCY, obs_module_text("TargetLatency"),
      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(lat, obs_module_text("TargetLatencyLow"),
                            TARGET_LATENCY_LOW_MS);
  obs_property_list_add_int(lat, obs_module_text("TargetLatencyBalanced"),
                            TARGET_LATENCY_BALANCED_MS);
  obs_property_list_add_int(lat, obs_module_text("TargetLatencyStable"),
                            TARGET_LATENCY_STABLE_MS);
  obs_property_list_add_int(lat, obs_module_text("TargetLatencyVeryStable"),
                            TARGET_LATENCY_VERY_STABLE_MS);
  obs_property_set_long_description(lat, obs_module_text("TargetLatency.Desc"));

  obs_property_t *rec = obs_properties_add_list(
      playback, PROP_RECOVERY_MODE, obs_module_text("RecoveryMode"),
      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(rec, obs_module_text("RecoveryCutToLive"),
                            static_cast<int>(RecoveryMode::CutToLive));
  obs_property_list_add_int(rec, obs_module_text("RecoveryPreserve"),
                            static_cast<int>(RecoveryMode::Preserve));
  obs_property_set_long_description(rec, obs_module_text("RecoveryMode.Desc"));

  obs_properties_add_bool(playback, PROP_HW_DECODE, obs_module_text("HwDecode"));
  obs_properties_add_group(props, "group_playback", obs_module_text("PropsPlayback"), OBS_GROUP_NORMAL, playback);

  obs_properties_t *fallback = obs_properties_create();
  obs_properties_add_button(fallback, "fb_open_settings", obs_module_text("FallbackDialogOpen"),
                            on_fallback_settings_clicked);
  obs_properties_add_int(fallback, PROP_LOW_BITRATE,
                         obs_module_text("LowBitrateThreshold"), 10, 10000, 10);

  obs_property_t *grace = obs_properties_add_int(
      fallback, PROP_DISCONNECT_GRACE, obs_module_text("DisconnectGrace"),
      MIN_DISCONNECT_GRACE_MS, MAX_DISCONNECT_GRACE_MS, 100);
  obs_property_int_set_suffix(grace, " ms");
  obs_property_set_long_description(grace,
                                    obs_module_text("DisconnectGrace.Desc"));
  obs_properties_add_group(props, "group_fallback", obs_module_text("PropsFallback"), OBS_GROUP_NORMAL, fallback);

  obs_properties_t *stats = obs_properties_create();
  obs_properties_add_button(stats, PROP_OPEN_STATS,
                            obs_module_text("OpenStatsDash"),
                            on_open_stats_clicked);

  obs_properties_add_button(stats, "show_window_stats",
                            obs_module_text("ShowStatsWin"),
                            on_show_window_stats_clicked);

  obs_properties_add_button(stats, PROP_ADD_OVERLAY_HORZ,
                            obs_module_text("AddStatsOverlayHorz"),
                            on_add_overlay_horz_clicked);

  obs_properties_add_button(stats, PROP_ADD_OVERLAY_VERT,
                            obs_module_text("AddStatsOverlayVert"),
                            on_add_overlay_vert_clicked);
  obs_properties_add_group(props, "group_stats", obs_module_text("PropsStats"), OBS_GROUP_NORMAL, stats);

  return props;
}

void LiveStreamSource::get_defaults(obs_data_t *settings) {
  obs_data_set_default_string(settings, PROP_URL, "rtmp://localhost/live/test");
  obs_data_set_default_int(settings, PROP_STREAM_TYPE,
                           static_cast<int>(StreamType::Standard));
  obs_data_set_default_int(settings, PROP_LOW_BITRATE, DEFAULT_LOW_BITRATE_KBPS);
  obs_data_set_default_bool(settings, PROP_AUTO_CATCHUP, true);
  obs_data_set_default_int(settings, PROP_TARGET_LATENCY, DEFAULT_TARGET_LATENCY_MS);
  obs_data_set_default_int(settings, PROP_RECOVERY_MODE,
                           static_cast<int>(RecoveryMode::CutToLive));
  obs_data_set_default_bool(settings, PROP_HW_DECODE, true);
  const auto fallback_defaults = [settings](const char *prefix, FallbackKind kind, const char *text,
                                            const char *page, int size, int anchor) {
    obs_data_set_default_int(settings, fallback_key(prefix, "kind").c_str(), static_cast<int>(kind));
    obs_data_set_default_string(settings, fallback_key(prefix, "text").c_str(),
                                obs_module_text(text));
    obs_data_set_default_string(settings, fallback_key(prefix, "template").c_str(), page);
    obs_data_set_default_int(settings, fallback_key(prefix, "scale").c_str(), 100);
    obs_data_t *font = obs_data_create();
    obs_data_set_string(font, "face", "Arial");
    obs_data_set_int(font, "size", 72);
    obs_data_set_default_obj(settings, fallback_key(prefix, "font").c_str(), font);
    obs_data_release(font);
    obs_data_set_default_int(settings, fallback_key(prefix, "text_color").c_str(), 0xFFFFFFFF);
    obs_data_set_default_int(settings, fallback_key(prefix, "outline_size").c_str(), 2);
    obs_data_set_default_int(settings, fallback_key(prefix, "outline_color").c_str(), 0xFF000000);
    obs_data_set_default_int(settings, fallback_key(prefix, "background_color").c_str(), 0);
    obs_data_set_default_int(settings, fallback_key(prefix, "anim_in").c_str(), static_cast<int>(FallbackAnim::Fade));
    obs_data_set_default_int(settings, fallback_key(prefix, "anim_out").c_str(), static_cast<int>(FallbackAnim::Fade));
    obs_data_set_default_int(settings, fallback_key(prefix, "anim_in_ms").c_str(), 300);
    obs_data_set_default_int(settings, fallback_key(prefix, "anim_out_ms").c_str(), 300);
    obs_data_set_default_int(settings, fallback_key(prefix, "size").c_str(), size);
    obs_data_set_default_int(settings, fallback_key(prefix, "anchor").c_str(), anchor);
    obs_data_set_default_bool(settings, fallback_key(prefix, "keep").c_str(), true);
  };
  fallback_defaults(FALLBACK_DISCONNECT, FallbackKind::Browser, "FallbackTextDisconnect", "disconnect-signal", 60, 4);
  fallback_defaults(FALLBACK_LOW, FallbackKind::None, "FallbackTextLow", "low-badge", 30, 8);
  fallback_defaults(FALLBACK_LOADING, FallbackKind::Browser, "FallbackTextLoading", "loading-ring", 60, 4);

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

  // Frames here aren't timed by the playout engine: OBS has to pace them.
  playout_enabled_.store(false);
  obs_source_set_async_unbuffered(obs_source_, false);

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

          // A stall stops the sample counter while the RTP clock keeps
          // running, which left audio behind video by the length of every
          // freeze. Resync the counter whenever the two disagree.
          const uint32_t rtp_samples =
              (uint32_t)timestamp - audio_start_pts_rtp_;
          const int64_t sample_drift =
              (int64_t)rtp_samples - (int64_t)dec_audio_samples_;
          if (sample_drift > WHEP_AUDIO_RESYNC_SAMPLES ||
              sample_drift < -WHEP_AUDIO_RESYNC_SAMPLES) {
            lss_log_info("[WHEP] Audio resync: %lld ms",
                         (long long)(sample_drift / 48));
            dec_audio_samples_ = rtp_samples;
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
      drop_picture();

      // Reset PTS anchors for next connection
      video_base_pts_ns_ = 0;
      video_start_pts_rtp_ = 0;
      audio_base_pts_ns_ = 0;
      dec_audio_samples_ = 0;

      update_source_toggles();
    }
  });

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
      drop_picture();
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
        drop_picture();
        update_source_toggles();

        // Reset PTS anchors so re-connection starts fresh
        video_base_pts_ns_ = 0;
        video_start_pts_rtp_ = 0;
        audio_base_pts_ns_ = 0;
        dec_audio_samples_ = 0;
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
  drop_picture();
  update_source_toggles();
  lss_log_debug("[WHEP] Worker thread finished");
}
struct obs_source_info live_stream_source_info = {};

void register_live_stream_source() {
  install_stats_pages();
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

