// live-stream-playout.cpp - playout engine for Standard streams
//
//   socket -> worker (never waits) -> PacketBuffer -> decode thread -> OBS audio
//                                                         |
//                                                         v
//                                                  present thread -> OBS video
//
// libobs can't be the jitter buffer: its async queue holds 30 frames and wipes
// itself on overflow, and in buffered mode it times video relative to the last
// frame shown, so every underrun left video permanently late against audio.
// The source therefore runs unbuffered and times delivery itself. Audio goes
// out ahead of time on an absolute timeline and pins PlayoutClock; video is
// handed over when the clock says it's due.
#include "core/live-stream-source.h"
#include "ui/sync-notice.h"

#include <algorithm>
#include <cstdio>

namespace lss {

namespace {

// Decode ahead of presentation. Frame-threaded decoders hold frames back, so
// the lead follows the measured delay. The floor must exceed SoundTouch's
// internal latency or entering a time-stretch would underrun OBS.
constexpr int64_t MIN_DECODE_LEAD_US = 120000;
constexpr int64_t MAX_DECODE_LEAD_US = 500000;
constexpr int64_t DECODE_LEAD_MARGIN_US = 60000;
// Network slack the target latency keeps on top of the decode lead.
constexpr int64_t MIN_BUFFER_HEADROOM_US = 100000;
constexpr int SYNC_REACH_TOLERANCE_MS = 30;
// The follower settles a little short of the buffer floor, and lines up exactly
// from whatever it's given, so asking for slightly too much is harmless.
constexpr int SYNC_HOLD_MARGIN_MS = 150;
// Long enough for a follower's offset on top of the main source's delay.
constexpr int64_t SYNC_HOLD_TIMEOUT_US = MAX_SYNC_OFFSET_MS * 2000LL + 2000000;
// Smaller moves than this are left to the playback rate.
constexpr int SHIFT_MIN_MS = 50;
// A spin box sends every step, and only the last one should move the picture.
constexpr int64_t SHIFT_SETTLE_US = 250000;
// A cut lands on a keyframe, which can be later than asked for.
constexpr int64_t SHIFT_CHECK_US = 800000;
// How long a new connection needs before its delay can be trusted: a server's cached
// keyframe arrives late, so the first packets say little.
constexpr int64_t SHIFT_START_CHECK_US = 2000000;
constexpr int SHIFT_CHECK_TOLERANCE_MS = 200;
// Past this a follower ahead of the main source pauses instead of drifting back.
constexpr int64_t SYNC_PAUSE_MIN_US = 300000;

constexpr int64_t METRICS_INTERVAL_US = 100000;
// A main source this quiet is stalled or gone: hold course until it returns.
// Longer than the longest sync hold, which keeps the main source from publishing.
constexpr int64_t SYNC_STALE_US = 6000000;
constexpr double SYNC_SMOOTHING = 0.2;
constexpr int64_t POLICY_INTERVAL_US = 100000;

// A stall is declared this close to running out of queued media.
constexpr int64_t UNDERRUN_SLACK_US = 30000;
// After a stall the buffer refills before playback resumes.
constexpr int64_t SETTLE_MIN_US = 200000;
constexpr int64_t SETTLE_MAX_US = 1500000;
constexpr int64_t SHORT_STALL_US = 1000000;
constexpr int64_t REFILL_TIMEOUT_US = 3000000;
// Past this, stop waiting on the connection and open a new one. FFmpeg's own
// rw_timeout isn't precise enough to rely on here.
constexpr int64_t STALL_RECONNECT_US = 12000000;
constexpr double BURST_RATE = 1.5;

constexpr int64_t KEYFRAME_WAIT_US = 5000000;
constexpr int64_t PRIME_TIMEOUT_US = 3000000;
constexpr int64_t AUDIO_WAIT_US = 300000;

constexpr size_t MAX_PRESENT_FRAMES = 90;
// A frame this late with a newer one waiting isn't worth showing.
constexpr int64_t LATE_DROP_US = 50000;

// Audio chunks further than this from their ideal position are a real hole.
constexpr int64_t AUDIO_GAP_US = 30000;
// SoundTouch's position is only known to within its internal buffering around
// a tempo change, and that isn't missing audio.
constexpr int64_t AUDIO_GAP_STRETCH_US = 120000;
constexpr int64_t AUDIO_MAX_FILL_US = 500000;
constexpr int64_t AUDIO_MIN_LEAD_US = 15000;
constexpr int AUDIO_RESUME_FADE_MS = 30;
// libobs merges timestamp gaps under 70 ms (TS_SMOOTHING_THRESHOLD) into the
// previous run, and the offset then sticks for the rest of the stream. A new
// run has to clear that or it would never land where it was aimed.
constexpr int64_t OBS_AUDIO_SMOOTHING_US = 75000;
// libobs mixes whole 1024-sample ticks and throws away a partial one as
// "audio stopped". Without padding, a fade-out at the end of a run never plays.
constexpr uint32_t OBS_MIX_TICK_PAD = 1100;
// On a jump above 2 s, libobs resets timing and places the next chunk at the
// current time however far ahead it was stamped. This silent chunk takes that
// hit so the first real audio lands on its timestamp.
constexpr uint32_t OBS_TIMING_RESET_CHUNK = 64;

int64_t wall_us() { return static_cast<int64_t>(os_gettime_ns() / 1000); }

// Split so the multiply can't overflow on a stream that runs for days.
uint64_t samples_to_ns(uint64_t samples, uint32_t rate) {
  return samples / rate * 1000000000ULL +
         (samples % rate) * 1000000000ULL / rate;
}

// Delivery time that lands a frame on the first OBS tick at or after it's due.
// Aiming mid-interval means scheduling jitter can't flip a frame onto the
// neighbouring tick, which is what shows up as judder when stream and canvas
// run at the same rate.
int64_t snapped_delivery(int64_t due_us) {
  const uint64_t tick_ns = obs_get_video_frame_time();
  const uint64_t interval_ns = obs_get_frame_interval_ns();
  if (tick_ns == 0 || interval_ns == 0)
    return due_us;

  const int64_t tick_us = static_cast<int64_t>(tick_ns / 1000);
  const int64_t interval_us = static_cast<int64_t>(interval_ns / 1000);
  int64_t ticks = (due_us - tick_us + interval_us - 1) / interval_us;
  if (due_us < tick_us)
    ticks = 0;
  return tick_us + ticks * interval_us - interval_us / 2;
}

void fade_in_planar(DecodedAudioFrame &af, uint32_t offset, uint32_t count,
                    uint32_t &left, uint32_t total) {
  float *planes[2] = {reinterpret_cast<float *>(af.data),
                      af.channels >= 2
                          ? reinterpret_cast<float *>(af.data) + af.frames
                          : nullptr};
  for (uint32_t i = 0; i < count && left > 0; ++i, --left) {
    const float gain = static_cast<float>(total - left) / total;
    planes[0][offset + i] *= gain;
    if (planes[1])
      planes[1][offset + i] *= gain;
  }
}

} // namespace

int64_t LiveStreamSource::playout_decode_lead_us() const {
  return std::clamp(run_.decoder_delay_us + DECODE_LEAD_MARGIN_US,
                    MIN_DECODE_LEAD_US, MAX_DECODE_LEAD_US);
}

//  Worker side

void LiveStreamSource::ingest_packet(AVPacket *pkt) {
  const bool is_video = pkt->stream_index == demuxer_.video_stream_index();
  const AVRational tb =
      is_video ? demuxer_.video_time_base() : demuxer_.audio_time_base();

  bitrate_mon_.record_bytes(pkt->size);
  if (is_video) {
    last_pkt_recv_us_.store(now_us());
    total_bytes_video_.fetch_add(pkt->size);
    if (pkt->dts != AV_NOPTS_VALUE)
      update_stream_delay(av_rescale_q(pkt->dts, tb, {1, 1000}));

    // Late initialization for streams that start with 0x0 resolution
    if (!video_dec_.is_initialized() && demuxer_.video_width() > 0) {
      lss_log_info("Late video decoder initialization: %dx%d",
                   demuxer_.video_width(), demuxer_.video_height());
      std::lock_guard<std::mutex> lock(decoder_mutex_);
      video_dec_.init(demuxer_.video_codecpar(), hw_decode_);
      video_dec_.set_stream_time_base(tb);
      video_codec_name_ = avcodec_get_name(demuxer_.video_codecpar()->codec_id);
    }
  } else {
    total_bytes_audio_.fetch_add(pkt->size);
  }

  if (pkt->dts == AV_NOPTS_VALUE)
    pkt->dts = pkt->pts;
  if (pkt->pts == AV_NOPTS_VALUE)
    pkt->pts = pkt->dts;
  if (pkt->dts == AV_NOPTS_VALUE)
    return;

  // Decoders pass timestamps through untouched, so rewriting them here gives
  // every frame downstream its position on the media timeline directly.
  av_packet_rescale_ts(pkt, tb, {1, 1000000});
  const int64_t arrival = wall_us();
  const int64_t offset = media_timeline_.map(is_video ? 0 : 1, pkt->dts, arrival);
  pkt->pts += offset;
  pkt->dts += offset;

  if (is_video || !playout_has_video_.load()) {
    arrival_baseline_.add(arrival, arrival - pkt->dts);
    arrival_base_us_.store(arrival_baseline_.value());
  }

  PacketInfo info;
  info.pts_us = pkt->pts;
  info.dts_us = pkt->dts;
  info.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;

  AVPacket *owned = av_packet_alloc();
  if (!owned)
    return;
  av_packet_move_ref(owned, pkt);
  packets_.push(is_video ? PacketKind::Video : PacketKind::Audio, owned, info);
}

void LiveStreamSource::update_stream_delay(int64_t dts_ms) {
  int64_t wall_ms = now_ms();
  if (!has_delay_ref_) {
    delay_ref_wall_ms_ = wall_ms;
    delay_ref_pts_ms_ = dts_ms;
    has_delay_ref_ = true;
    return;
  }

  // Delay includes: network latency + decode latency + buffer latency
  // This is the total end-to-end delay from stream to output
  int64_t delay = (wall_ms - delay_ref_wall_ms_) - (dts_ms - delay_ref_pts_ms_);
  if (delay < 0) {
    delay_ref_wall_ms_ = wall_ms;
    delay_ref_pts_ms_ = dts_ms;
    stream_delay_ms_.store(0);
  } else {
    stream_delay_ms_.store(delay);
  }
}

// Called by whichever thread is about to tear down or reconnect the stream.
// On return no frame from before the reset can reach OBS.
void LiveStreamSource::playout_reset() {
  std::lock_guard<std::mutex> decoders(decoder_mutex_);

  packets_.clear();
  media_timeline_.reset();
  arrival_baseline_.reset();
  arrival_base_us_.store(INT64_MAX);
  playout_clock_.reset();

  {
    std::lock_guard<std::mutex> output(present_output_mutex_);
    std::lock_guard<std::mutex> queue(present_queue_mutex_);
    for (PresentFrame &entry : present_queue_)
      av_frame_free(&entry.frame);
    present_queue_.clear();
  }

  if (audio_queue_) {
    DecodedAudioFrame stale;
    while (audio_queue_->pop(stale))
      stale.free_buffers();
  }

  playout_stalled_.store(false);
  playout_generation_.fetch_add(1);
}

//  Decode thread

void LiveStreamSource::playout_restart_state() {
  run_ = PlayoutRun{};
  recovery_policy_.reset();
  recovery_policy_.forget_learned_latency();
  recovery_policy_.clear_follow();
  sync_active_.store(false);
  sound_tracer_.reset();
  playout_rate_.store(1.0);
  playout_latency_ms_.store(0);
}

void LiveStreamSource::playout_decode_thread_func() {
  lss_log_debug("Playout decode thread started");
  uint64_t generation = playout_generation_.load();
  playout_restart_state();

  while (running_.load()) {
    if (!uses_playout()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    bool worked = false;
    {
      std::lock_guard<std::mutex> lock(decoder_mutex_);
      const uint64_t current = playout_generation_.load();
      if (current != generation) {
        generation = current;
        playout_restart_state();
      }

      const int64_t now = wall_us();
      playout_update_metrics(now);
      worked = (run_.phase == PlayoutPhase::Buffering)
                   ? playout_step_buffering(now)
                   : playout_step_playing(now);
    }

    if (!worked)
      packets_.wait_for_data(2);
  }
  lss_log_debug("Playout decode thread stopped");
}

void LiveStreamSource::playout_update_metrics(int64_t now_us) {
  if (now_us < run_.next_metrics_us)
    return;
  run_.next_metrics_us = now_us + METRICS_INTERVAL_US;

  apply_main_delay_request(now_us);
  if (run_.sync_hold)
    SyncRegistry::instance().touch(obs_source_get_uuid(obs_source_), now_us);

  // The main source's delay, or in Fixed mode a follower's own offset. Follow mode
  // steers followers with a target of its own further down.
  const SyncConfig sync = SyncRegistry::instance().config();
  const std::string self = obs_source_get_uuid(obs_source_);
  int sync_extra_ms = 0;
  if (sync.is_main(self))
    sync_extra_ms = sync.main_delay_ms;
  else if (sync.mode == SyncMode::Fixed && sync.is_follower(self))
    sync_extra_ms = sync.offset_of(self);

  recovery_policy_.configure((target_latency_ms_.load() + sync_extra_ms) * 1000LL,
                             static_cast<RecoveryMode>(recovery_mode_.load()),
                             auto_catchup_);

  const PacketBufferStats stats = packets_.stats();
  if (stats.newest_us == INT64_MIN)
    return;

  // Stream seconds arriving per wall second: well above 1 while a backlog is
  // still landing, near 0 during a stall.
  if (run_.metrics_at_us > 0) {
    const double dt = (now_us - run_.metrics_at_us) / 1000000.0;
    const double got =
        std::max<int64_t>(0, stats.newest_us - run_.metrics_newest_us) / 1000000.0;
    if (dt > 0)
      run_.ingest_rate = 0.5 * run_.ingest_rate + 0.5 * (got / dt);
  }
  run_.metrics_at_us = now_us;
  run_.metrics_newest_us = stats.newest_us;

  // A stream coming back has no picture to steer by yet, so it starts where the group is.
  SyncState lead;
  if (run_.starting && sync.mode == SyncMode::Follow && sync.is_follower(self) &&
      SyncRegistry::instance().lookup(sync.main_uuid, lead) &&
      now_us - lead.updated_us < SYNC_STALE_US)
    recovery_policy_.set_follow_target(lead.delay_us + sync.offset_of(self) * 1000LL, 1.0);

  const int64_t min_target = playout_decode_lead_us() + MIN_BUFFER_HEADROOM_US;
  playout_target_ms_.store(recovery_policy_.target_us(min_target) / 1000);
  if (playout_clock_.valid()) {
    const int64_t latency = stats.newest_us - playout_clock_.media_at(now_us);
    playout_latency_ms_.store(latency / 1000);
    pipeline_latency_ms_.store(std::max<int64_t>(0, latency / 1000));
    if (run_.phase == PlayoutPhase::Playing) {
      playout_update_sync(now_us, latency);
      playout_apply_shift(now_us, latency, min_target, sync);
    }
  }
  playout_update_listen(now_us);
}

void LiveStreamSource::publish_sync_progress() {
  SyncProgress progress;
  progress.state = static_cast<SyncListenState>(sync_listen_state_.load());
  progress.seconds_left = sync_listen_left_s_.load();
  progress.heard_main = sync_heard_main_.load();
  progress.heard_own = sync_heard_own_.load();
  progress.delta_ms = static_cast<int>(sync_listen_delta_ms_.load());
  progress.main_delay_ms = sync_listen_held_ms_.load();
  progress.short_ms = sync_listen_short_ms_.load();
  progress.expires_us = sync_result_until_us_.load();
  SyncRegistry::instance().set_progress(obs_source_get_uuid(obs_source_), progress);
}

void LiveStreamSource::finish_sync_listen(SyncListenState result) {
  sync_listen_left_s_.store(0);
  sync_result_until_us_.store(wall_us() + SYNC_RESULT_SHOWN_US);
  sync_listen_state_.store(static_cast<int>(result));
  publish_sync_progress();
}

void LiveStreamSource::request_sync_listen() {
  SyncRegistry &registry = SyncRegistry::instance();
  const SyncConfig config = registry.config();
  const std::string self = obs_source_get_uuid(obs_source_);
  if (!config.is_follower(self))
    return;

  sync_heard_main_.store(0);
  sync_heard_own_.store(0);
  sync_listen_delta_ms_.store(0);
  sync_listen_held_ms_.store(0);
  sync_listen_short_ms_.store(0);

  // Neither one would ever hear a thing, so say so now instead of in 30 seconds.
  const int64_t now = wall_us();
  SyncState own, main_state;
  const bool playing = registry.lookup(self, own) && now - own.updated_us < SYNC_PANEL_STALE_US &&
                       registry.lookup(config.main_uuid, main_state) &&
                       now - main_state.updated_us < SYNC_PANEL_STALE_US;
  if (!playing) {
    finish_sync_listen(SyncListenState::NoStream);
    show_status_notice(obs_module_text("SyncListenNoStream"));
    return;
  }

  const int64_t until = now + SYNC_LISTEN_US;
  registry.start_listening(config.main_uuid, until);
  registry.start_listening(self, until);
  sync_listen_base_delay_ms_.store(config.main_delay_ms);
  sync_listen_until_us_.store(until);
  sync_listen_left_s_.store(static_cast<int>(SYNC_LISTEN_US / 1000000));
  sync_result_until_us_.store(until + SYNC_RESULT_SHOWN_US);
  sync_listen_state_.store(static_cast<int>(SyncListenState::Listening));
  publish_sync_progress();
  show_status_notice(obs_module_text("SyncListenStarted"));
}

// Runs on both sources. Each one listens to itself while asked; the follower
// that started it compares the two and ends the session.
void LiveStreamSource::playout_update_listen(int64_t now_us) {
  SyncRegistry &registry = SyncRegistry::instance();
  const std::string self = obs_source_get_uuid(obs_source_);

  const bool listen = registry.listening_until(self) > now_us;
  if (listen != run_.sound_listening) {
    run_.sound_listening = listen;
    sound_tracer_.reset();
  }
  if (listen)
    registry.set_trace(self, sound_tracer_.trace());

  const int state = sync_listen_state_.load();
  if (state != static_cast<int>(SyncListenState::Listening)) {
    if (state != static_cast<int>(SyncListenState::Idle) && now_us > sync_result_until_us_.load())
      sync_listen_state_.store(static_cast<int>(SyncListenState::Idle));
    return;
  }

  const SyncConfig config = registry.config();
  if (!config.is_follower(self)) {
    registry.stop_listening(self);
    sync_listen_state_.store(static_cast<int>(SyncListenState::Idle));
    publish_sync_progress();
    return;
  }

  const SoundTrace main_trace = registry.trace(config.main_uuid);
  const SoundTrace own_trace = registry.trace(self);

  SoundMatch match;
  const bool matched = match_sounds(main_trace, own_trace,
                                    SYNC_MATCH_RANGE_MS * 1000LL, &match);
  const int main_sounds = count_sounds(main_trace);
  const int own_sounds = count_sounds(own_trace);
  // The count can drop when a louder sound arrives, which would read as losing one.
  sync_heard_main_.store(std::max(sync_heard_main_.load(), main_sounds));
  sync_heard_own_.store(std::max(sync_heard_own_.load(), own_sounds));

  if (!matched && now_us < sync_listen_until_us_.load()) {
    sync_listen_left_s_.store(static_cast<int>((sync_listen_until_us_.load() - now_us) / 1000000 + 1));
    publish_sync_progress();
    return;
  }

  // The main source runs out by itself: another follower may still need it.
  registry.stop_listening(self);

  lss_log_info("Sync: %s. Sounds heard: main %d, this source %d. Best delay %lld ms, "
               "score %.2f, margin %.2f, shared %d",
               matched ? "matched" : "no match", main_sounds, own_sounds,
               static_cast<long long>(match.delta_us / 1000), match.score,
               match.margin, match.shared);

  if (matched) {
    apply_sync_offset(match.delta_us, config, now_us);
  } else {
    const bool silent = main_sounds == 0 || own_sounds == 0;
    finish_sync_listen(silent ? SyncListenState::NoSound : SyncListenState::NotFound);
    show_status_notice(obs_module_text(silent ? "SyncListenNoSound" : "SyncListenNotFound"));
  }
}

void LiveStreamSource::apply_sync_offset(int64_t delta_us, const SyncConfig &config,
                                         int64_t now_us) {
  SyncRegistry &registry = SyncRegistry::instance();
  const std::string self = obs_source_get_uuid(obs_source_);

  // delta is the true on-screen error right now. The running follower error
  // says how far playback still is from the current offset, and counting it
  // stops a correction in progress from being applied twice.
  const int64_t pending_ms = sync_active_.load() ? sync_error_ms_.load() : 0;
  const int offset = static_cast<int>(
      std::clamp<int64_t>(config.offset_of(self) + pending_ms - delta_us / 1000,
                          -MAX_SYNC_OFFSET_MS, MAX_SYNC_OFFSET_MS));

  registry.set_offset(self, offset);
  sync_listen_delta_ms_.store(delta_us / 1000);
  SyncConfig settled = registry.config();

  // Behind the main source, this one can only move up to its buffer floor. The
  // rest has to come from the main source waiting, once, for the picture to line
  // up straight away. Ahead of it, this source can do the waiting itself.
  const int64_t floor_us = playout_decode_lead_us() + MIN_BUFFER_HEADROOM_US;
  const int64_t latency_us = playout_latency_ms_.load() * 1000LL;
  const int short_ms = static_cast<int>((floor_us - (latency_us - delta_us)) / 1000);

  char text[256];
  if (short_ms > SYNC_REACH_TOLERANCE_MS && config.may_delay_main) {
    const int hold_ms = short_ms + SYNC_HOLD_MARGIN_MS;
    lss_log_info("Sync: this source is %d ms past its limit, delaying the main source by %d ms",
                 short_ms, hold_ms);
    registry.request_main_delay(config.main_uuid, sync_listen_base_delay_ms_.load() + hold_ms);
    settled.main_delay_ms = sync_listen_base_delay_ms_.load() + hold_ms;
    sync_listen_held_ms_.store(hold_ms);
    snprintf(text, sizeof(text), obs_module_text("SyncListenHoldMain"),
             static_cast<int>(delta_us / 1000), hold_ms);
  } else if (short_ms > SYNC_REACH_TOLERANCE_MS) {
    lss_log_warn("Sync: this source can't get %d ms closer to the main one", short_ms);
    sync_listen_short_ms_.store(short_ms);
    snprintf(text, sizeof(text), obs_module_text("SyncListenTooFar"), short_ms);
  } else {
    if (delta_us < -SYNC_PAUSE_MIN_US && run_.phase == PlayoutPhase::Playing) {
      if (sync_active_.load())
        recovery_policy_.set_follow_target(latency_us - delta_us, 1.0);
      playout_hold(now_us);
    }
    snprintf(text, sizeof(text), obs_module_text("SyncListenFound"),
             static_cast<int>(delta_us / 1000), offset);
  }
  // This change has been dealt with above, in its own way.
  run_.shift_applied_ms = run_.shift_seen_ms = settled.shift_of(self);
  finish_sync_listen(SyncListenState::Found);
  show_status_notice(text);
}

void LiveStreamSource::playout_update_sync(int64_t now_us, int64_t latency_us) {
  const int64_t base = arrival_base_us_.load();
  if (base == INT64_MAX)
    return;

  // How far behind our own fastest delivery the playhead sits right now.
  const int64_t delay =
      (now_us - playout_clock_.media_at(now_us)) - base;

  SyncRegistry &registry = SyncRegistry::instance();
  const SyncConfig config = registry.config();
  const std::string self = obs_source_get_uuid(obs_source_);

  SyncState main_state;
  const bool follow = config.mode == SyncMode::Follow && config.is_follower(self) &&
                      registry.lookup(config.main_uuid, main_state) &&
                      now_us - main_state.updated_us < SYNC_STALE_US;

  SyncState own;
  own.delay_us = delay;
  own.updated_us = now_us;
  own.following = follow;
  own.rate = run_.rate;
  own.move_id = sync_move_id_;

  if (!follow) {
    // On its way to a cut, what's been announced is more true than what's measured.
    if (run_.force_cut)
      registry.touch(self, now_us);
    else
      registry.publish(self, own);
    recovery_policy_.clear_follow();
    sync_active_.store(false);
    run_.main_delay_valid = false;
    return;
  }

  // A move of the main source is a jump to make, not something to glide after.
  if (!run_.main_delay_valid || main_state.move_id != run_.main_move_id) {
    run_.jump_pending = run_.main_delay_valid;
    run_.main_move_id = main_state.move_id;
    run_.main_delay_us = static_cast<double>(main_state.delay_us);
    run_.main_delay_valid = true;
  }
  run_.main_delay_us +=
      SYNC_SMOOTHING * (main_state.delay_us - run_.main_delay_us);

  const int64_t error = delay - config.offset_of(self) * 1000LL -
                        static_cast<int64_t>(run_.main_delay_us);
  recovery_policy_.set_follow_target(latency_us - error, main_state.rate);
  sync_active_.store(true);
  sync_error_ms_.store(error / 1000);

  own.error_ms = static_cast<int>(error / 1000);
  registry.publish(self, own);
}

// Lands on the current target at once: waiting adds delay without a gap in the stream,
// and a cut takes it away. Drifting there at an inaudible rate takes a minute or more
// for a few seconds.
void LiveStreamSource::playout_land(int64_t now_us, int64_t missing_us, int tolerance_ms) {
  if (missing_us > tolerance_ms * 1000LL) {
    playout_hold(now_us);
  } else if (missing_us < -tolerance_ms * 1000LL) {
    run_.force_cut = true;
  }
}

// Followers have to make the same jump, and they can't wait for the measured delay:
// while this source waits it isn't measuring anything.
void LiveStreamSource::announce_move(int64_t now_us, int64_t by_us) {
  SyncRegistry &registry = SyncRegistry::instance();
  const std::string self = obs_source_get_uuid(obs_source_);
  SyncState state;
  if (!registry.lookup(self, state))
    return;
  state.delay_us += by_us;
  state.updated_us = now_us;
  state.move_id = ++sync_move_id_;
  registry.publish(self, state);
}

void LiveStreamSource::playout_apply_shift(int64_t now_us, int64_t latency_us,
                                           int64_t min_target_us, const SyncConfig &config) {
  const std::string self = obs_source_get_uuid(obs_source_);
  const int wanted = config.shift_of(self);
  const int64_t missing_us = recovery_policy_.target_us(min_target_us) - latency_us;

  if (!run_.shift_known) {
    run_.shift_known = true;
    run_.shift_applied_ms = run_.shift_seen_ms = wanted;
    // Not a change, but a stream that just came back may not have landed in place.
    if (config.is_main(self) || config.is_follower(self))
      run_.shift_check_us = now_us + SHIFT_START_CHECK_US;
    return;
  }

  if (run_.shift_check_us > 0 && now_us >= run_.shift_check_us && !run_.force_cut) {
    run_.shift_check_us = 0;
    playout_land(now_us, missing_us, SHIFT_CHECK_TOLERANCE_MS);
    return;
  }

  if (run_.jump_pending) {
    run_.jump_pending = false;
    lss_log_info("Sync: following the main source by %lld ms", (long long)(missing_us / 1000));
    playout_land(now_us, missing_us, SHIFT_CHECK_TOLERANCE_MS);
    return;
  }

  if (wanted != run_.shift_seen_ms) {
    run_.shift_seen_ms = wanted;
    run_.shift_seen_us = now_us;
    return;
  }
  if (wanted == run_.shift_applied_ms || now_us - run_.shift_seen_us < SHIFT_SETTLE_US)
    return;

  run_.shift_applied_ms = wanted;
  lss_log_info("Sync: moving this source by %lld ms", (long long)(missing_us / 1000));
  if (config.is_main(self))
    announce_move(now_us, missing_us);
  playout_land(now_us, missing_us, SHIFT_MIN_MS);
  if (run_.force_cut)
    run_.shift_check_us = now_us + SHIFT_CHECK_US;
}

// The main source's half of a sync. Runs here because that's the thread that owns
// playback.
void LiveStreamSource::apply_main_delay_request(int64_t now_us) {
  SyncRegistry &registry = SyncRegistry::instance();
  const int wanted = registry.take_main_delay_request(obs_source_get_uuid(obs_source_));
  if (wanted == 0)
    return;

  const int before = registry.config().main_delay_ms;
  const int after = std::min(wanted, MAX_SYNC_OFFSET_MS);
  if (after <= before)
    return;

  registry.set_main_delay(after);
  run_.shift_applied_ms = run_.shift_seen_ms = after;
  lss_log_info("Sync: delaying this source by %d ms for a follower, %d ms in total",
               after - before, after);

  if (run_.phase == PlayoutPhase::Playing) {
    announce_move(now_us, (after - before) * 1000LL);
    playout_hold(now_us);
  }
}

bool LiveStreamSource::playout_step_buffering(int64_t now_us) {
  const PacketBufferStats stats = packets_.stats();
  if (!stats.has_data()) {
    run_.data_since_us = 0;
    if (!run_.reconnect_requested && run_.stall_start_us > 0 &&
        now_us - run_.stall_start_us >= STALL_RECONNECT_US) {
      lss_log_warn("Playout: no data for %lld ms, reconnecting",
                   (long long)((now_us - run_.stall_start_us) / 1000));
      run_.reconnect_requested = true;
      demuxer_.request_abort();
    }
    return false;
  }
  if (run_.data_since_us == 0)
    run_.data_since_us = now_us;

  const int64_t waited = now_us - run_.data_since_us;
  const int64_t min_target = playout_decode_lead_us() + MIN_BUFFER_HEADROOM_US;
  const int64_t target = recovery_policy_.target_us(min_target);
  const int64_t fill = stats.playable_us();

  // Cutting while a backlog is still landing would aim at a stale edge. A short
  // stall can't have queued enough to need a cut, so it doesn't wait.
  const bool short_stall = run_.stall_start_us > 0 &&
                           now_us - run_.stall_start_us < SHORT_STALL_US;
  const bool settled =
      short_stall ||
      (waited >= SETTLE_MIN_US &&
       (run_.ingest_rate < BURST_RATE || waited >= SETTLE_MAX_US));
  // A delayed source has that much stream to collect before it can start.
  const int64_t refill_timeout = run_.sync_hold ? SYNC_HOLD_TIMEOUT_US : REFILL_TIMEOUT_US + target;
  if (!(fill >= target && settled) && waited < refill_timeout)
    return false;

  const bool has_video = playout_has_video_.load();
  const bool need_cut = run_.starting || stats.has_gap ||
                        recovery_policy_.wants_cut(fill - target);

  if (need_cut) {
    const int64_t target_media = stats.newest_us - target;
    CutPoint cut;
    const bool found = has_video && packets_.find_cut_point(target_media, cut);
    // No decodable entry point yet. Hold rather than start on a broken frame.
    if (has_video && !found && waited < KEYFRAME_WAIT_US)
      return false;

    lss_log_info("Playout: %s (backlog=%lld ms, target=%lld ms, keyframe=%d)",
                 run_.starting ? "starting" : "cutting to live",
                 (long long)(fill / 1000), (long long)(target / 1000),
                 (int)found);
    playout_cut(now_us, found ? &cut : nullptr, target_media);
  }

  if (run_.stall_start_us > 0) {
    const int64_t stall = now_us - run_.stall_start_us;
    lss_log_info("Playout: resumed after %lld ms stall (backlog=%lld ms, cut=%d)",
                 (long long)(stall / 1000), (long long)(fill / 1000),
                 (int)need_cut);
    // A pause on purpose says nothing about how healthy the stream is.
    if (!run_.sync_hold)
      recovery_policy_.on_stall_finished(now_us, stall);
    run_.sync_hold = false;
    run_.stall_start_us = 0;
  }

  playout_stalled_.store(false);
  run_.phase = PlayoutPhase::Playing;
  run_.prime_start_us = now_us;
  run_.next_policy_us = now_us + POLICY_INTERVAL_US;
  return true;
}

bool LiveStreamSource::playout_step_playing(int64_t now_us) {
  if (now_us >= run_.next_policy_us) {
    run_.next_policy_us = now_us + POLICY_INTERVAL_US;
    playout_run_policy(now_us);
  }

  PacketInfo video;
  PacketInfo audio;
  const bool have_video = packets_.peek(PacketKind::Video, video);
  const bool have_audio = packets_.peek(PacketKind::Audio, audio);
  const int64_t lead = playout_decode_lead_us();

  bool clock_ok = playout_clock_.valid();

  // Video reached the cut target but no audio turned up to pin the clock.
  if (!clock_ok && run_.video_primed &&
      now_us - run_.prime_start_us >= AUDIO_WAIT_US) {
    std::lock_guard<std::mutex> lock(present_queue_mutex_);
    if (!present_queue_.empty()) {
      playout_clock_.anchor(present_queue_.front().media_us, now_us + lead,
                            run_.rate);
      clock_ok = true;
    }
  }

  if (!run_.video_primed && now_us - run_.prime_start_us >= PRIME_TIMEOUT_US)
    run_.video_primed = true;

  bool video_ready = have_video;
  bool audio_ready = have_audio;

  if (clock_ok) {
    video_ready = have_video && playout_clock_.wall_of(video.dts_us) - lead <= now_us;
    audio_ready = have_audio && playout_clock_.wall_of(audio.pts_us) - lead <= now_us;
  } else if (!run_.video_primed && !run_.need_keyframe) {
    // Audio pins the clock, so it waits until video has decoded up to the cut
    // target. Starting it sooner would leave the first frames already late.
    audio_ready = false;
  }

  if (video_ready) {
    std::lock_guard<std::mutex> lock(present_queue_mutex_);
    if (present_queue_.size() >= MAX_PRESENT_FRAMES)
      video_ready = false;
  }

  if (!video_ready && !audio_ready) {
    if (have_video || have_audio)
      return false;

    if (!clock_ok) {
      if (now_us - run_.prime_start_us >= AUDIO_WAIT_US)
        playout_enter_buffering(now_us);
      return false;
    }

    // Both queues are dry. Stall just before what's already queued runs out,
    // so the outgoing audio can still end on a fade.
    int64_t end_us = 0;
    if (run_.audio_rate > 0)
      end_us = static_cast<int64_t>(
          (run_.audio_base_ns + samples_to_ns(run_.audio_samples, run_.audio_rate)) / 1000);
    if (run_.last_frame_media_us != 0)
      end_us = std::max(end_us, playout_clock_.wall_of(run_.last_frame_media_us));
    if (end_us - now_us < UNDERRUN_SLACK_US)
      playout_enter_buffering(now_us);
    return false;
  }

  if (video_ready && (!audio_ready || video.dts_us <= audio.pts_us))
    playout_feed_video(now_us);
  else
    playout_feed_audio();
  return true;
}

void LiveStreamSource::playout_run_policy(int64_t now_us) {
  if (!playout_clock_.valid())
    return;

  const PacketBufferStats stats = packets_.stats();
  if (stats.newest_us == INT64_MIN)
    return;

  const int64_t min_target = playout_decode_lead_us() + MIN_BUFFER_HEADROOM_US;
  const int64_t latency = stats.newest_us - playout_clock_.media_at(now_us);
  const RecoveryDecision decision = recovery_policy_.step(
      now_us, latency, run_.ingest_rate, min_target);

  if (decision.rate != run_.rate) {
    lss_log_debug("Playout: rate %.2fx -> %.2fx (latency=%lld ms)", run_.rate,
                  decision.rate, (long long)(latency / 1000));
    run_.rate = decision.rate;
    playout_rate_.store(decision.rate);
    audio_dec_.set_tempo(decision.rate);
    // Normally the audio write-head carries the change to the clock.
    if (now_us - run_.last_audio_wall_us > AUDIO_WAIT_US)
      playout_clock_.set_rate(now_us, decision.rate);
  }

  if (!decision.cut && !run_.force_cut)
    return;

  const bool has_video = playout_has_video_.load();
  const int64_t target_media = stats.newest_us - recovery_policy_.target_us(min_target);
  CutPoint cut;
  const bool found = has_video && packets_.find_cut_point(target_media, cut);
  if (has_video && !found)
    return;

  run_.force_cut = false;
  lss_log_info("Playout: cutting to live (latency=%lld ms)",
               (long long)(latency / 1000));
  audio_dec_.finish_segment(*audio_queue_);
  playout_drain_audio();
  playout_pad_audio();
  playout_cut(now_us, found ? &cut : nullptr, target_media);
  run_.prime_start_us = now_us;
}

void LiveStreamSource::playout_enter_buffering(int64_t now_us) {
  lss_log_info("Playout: stalled, buffer empty");
  playout_pause(now_us);
  playout_stalled_.store(true);
}

// Stops the playhead while the stream keeps arriving, which adds delay in one
// step with no gap in the picture. No stall overlay: this was asked for.
void LiveStreamSource::playout_hold(int64_t now_us) {
  lss_log_info("Playout: holding to add delay");
  playout_pause(now_us);
  run_.sync_hold = true;
}

void LiveStreamSource::playout_pause(int64_t now_us) {
  audio_dec_.finish_segment(*audio_queue_);
  playout_drain_audio();
  playout_pad_audio();

  run_.rate = 1.0;
  playout_rate_.store(1.0);
  audio_dec_.set_tempo(1.0);
  recovery_policy_.reset();
  playout_clock_.reset();

  run_.phase = PlayoutPhase::Buffering;
  run_.stall_start_us = now_us;
  run_.data_since_us = 0;
  run_.sync_hold = false;
}

// Jumps to a keyframe near the live edge. With no keyframe available, video
// stays frozen until one arrives while audio carries on from the target.
void LiveStreamSource::playout_cut(int64_t now_us, const CutPoint *cut,
                                   int64_t target_us) {
  video_dec_.reset_state();
  audio_dec_.reset_state();

  if (audio_queue_) {
    DecodedAudioFrame stale;
    while (audio_queue_->pop(stale))
      stale.free_buffers();
  }

  {
    std::lock_guard<std::mutex> lock(present_queue_mutex_);
    for (PresentFrame &entry : present_queue_)
      av_frame_free(&entry.frame);
    present_queue_.clear();
  }

  playout_clock_.reset();

  const bool has_video = playout_has_video_.load();
  const int64_t preroll = cut ? std::max(cut->key_pts_us, target_us) : target_us;
  if (cut)
    packets_.drop_video_before(cut->key_dts_us);
  packets_.drop_audio_before(preroll);

  // Frames between the keyframe and the target are decoded but never shown,
  // which lands playback on the target rather than up to a GOP before it.
  run_.preroll_us = preroll;
  run_.video_primed = !has_video;
  run_.need_keyframe = has_video && !cut;
  run_.key_wait_start_us = now_us;

  run_.rate = 1.0;
  playout_rate_.store(1.0);
  recovery_policy_.on_cut(now_us);

  if (!run_.starting)
    playout_cut_fade_pending_.store(true);
  run_.starting = false;
}

void LiveStreamSource::playout_feed_video(int64_t now_us) {
  PacketInfo info;
  AVPacket *pkt = packets_.pop(PacketKind::Video, &info);
  if (!pkt)
    return;

  // Packets went missing: P-frames past this point reference pictures the
  // decoder never saw and would smear until the next keyframe.
  if (info.after_gap && !info.keyframe && !run_.need_keyframe) {
    run_.need_keyframe = true;
    run_.key_wait_start_us = now_us;
  }

  if (run_.need_keyframe) {
    if (!info.keyframe && now_us - run_.key_wait_start_us < KEYFRAME_WAIT_US) {
      av_packet_free(&pkt);
      return;
    }
    run_.need_keyframe = false;
    video_dec_.reset_state();
  }

  run_.last_fed_dts_us = info.dts_us;
  if (video_dec_.is_initialized()) {
    int res = video_dec_.decode(
        pkt, [this](AVFrame *f) { playout_on_video_frame(f); });
    if (res == 0)
      total_frames_decoded_.fetch_add(1);
    else
      total_frames_dropped_decode_.fetch_add(1);
  }
  av_packet_free(&pkt);
}

void LiveStreamSource::playout_on_video_frame(AVFrame *frame) {
  if (!frame || !frame->data[0])
    return;

  int64_t media = frame->pts;
  if (media == AV_NOPTS_VALUE)
    media = frame->best_effort_timestamp;
  if (media == AV_NOPTS_VALUE)
    media = run_.last_frame_media_us + run_.frame_duration_us;

  if (media < run_.preroll_us)
    return;
  if (!run_.video_primed) {
    run_.video_primed = true;
    run_.prime_start_us = wall_us(); // audio gets its turn from here
  }

  const int64_t step = media - run_.last_frame_media_us;
  if (run_.last_frame_media_us != 0 && step > 2000 && step < 200000)
    run_.frame_duration_us = step;
  run_.last_frame_media_us = media;

  // How far behind the feed head frames come out, peak-held with a slow decay.
  const int64_t delay = run_.last_fed_dts_us - media;
  run_.decoder_delay_us = std::max(delay, run_.decoder_delay_us -
                                              run_.decoder_delay_us / 256);

  const int64_t now = wall_us();
  if (!playout_clock_.valid() && !playout_has_audio_.load())
    playout_clock_.anchor(media, now + playout_decode_lead_us(), run_.rate);

  AVFrame *held = av_frame_clone(frame);
  if (!held)
    return;

  {
    std::lock_guard<std::mutex> lock(present_queue_mutex_);
    present_queue_.push_back({held, media});
  }
  present_cv_.notify_one();
}

void LiveStreamSource::playout_feed_audio() {
  PacketInfo info;
  AVPacket *pkt = packets_.pop(PacketKind::Audio, &info);
  if (!pkt)
    return;

  if (info.pts_us >= run_.preroll_us) {
    audio_dec_.decode(pkt, *audio_queue_);
    playout_drain_audio();
  }
  av_packet_free(&pkt);
}

void LiveStreamSource::playout_drain_audio() {
  DecodedAudioFrame af;
  while (audio_queue_->pop(af)) {
    playout_emit_audio(af);
    af.free_buffers();
  }
}

void LiveStreamSource::playout_start_audio_timeline(int64_t wall_us_value,
                                                    int sample_rate) {
  if (run_.audio_rate > 0) {
    // Whatever OBS already holds plays out first; the new run goes after it.
    const int64_t queued_end = static_cast<int64_t>(
        (run_.audio_base_ns + samples_to_ns(run_.audio_samples, run_.audio_rate)) / 1000);
    wall_us_value = std::max(wall_us_value, queued_end + OBS_AUDIO_SMOOTHING_US);
  }

  run_.audio_base_ns = static_cast<uint64_t>(wall_us_value) * 1000ULL;
  run_.audio_samples = 0;
  run_.audio_rate = sample_rate;
  run_.audio_fade_total = static_cast<uint32_t>(sample_rate * AUDIO_RESUME_FADE_MS / 1000);
  run_.audio_fade_left = run_.audio_fade_total;

  const std::vector<float> silence(OBS_TIMING_RESET_CHUNK, 0.0f);
  playout_write_audio(silence.data(), silence.data(), OBS_TIMING_RESET_CHUNK);
}

void LiveStreamSource::playout_write_audio(const float *left, const float *right,
                                           uint32_t frames) {
  obs_source_audio obs_audio = {};
  obs_audio.data[0] = reinterpret_cast<const uint8_t *>(left);
  if (right)
    obs_audio.data[1] = reinterpret_cast<const uint8_t *>(right);
  obs_audio.frames = frames;
  obs_audio.speakers = right ? SPEAKERS_STEREO : SPEAKERS_MONO;
  obs_audio.samples_per_sec = static_cast<uint32_t>(run_.audio_rate);
  obs_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
  obs_audio.timestamp =
      run_.audio_base_ns + samples_to_ns(run_.audio_samples, run_.audio_rate);
  if (!dashboard_muted_.load())
    obs_source_output_audio(obs_source_, &obs_audio);
  run_.audio_samples += frames;
}

void LiveStreamSource::playout_pad_audio() {
  if (run_.audio_rate <= 0)
    return;
  const std::vector<float> silence(OBS_MIX_TICK_PAD, 0.0f);
  playout_write_audio(silence.data(), silence.data(), OBS_MIX_TICK_PAD);
}

// Writes one chunk to OBS. Timestamps come from a sample counter, so the output
// is gapless by construction and libobs' 70 ms smoothing never engages; the
// chunk's media position only serves to detect holes and to pin the clock.
void LiveStreamSource::playout_emit_audio(DecodedAudioFrame &af) {
  if (!af.data || af.frames == 0 || af.sample_rate <= 0)
    return;

  const int rate = af.sample_rate;
  const int64_t now = wall_us();
  run_.last_audio_wall_us = now;

  auto next_write_us = [this]() {
    return static_cast<int64_t>(
        (run_.audio_base_ns + samples_to_ns(run_.audio_samples, run_.audio_rate)) / 1000);
  };

  uint32_t skip = 0;

  if (!playout_clock_.valid()) {
    playout_start_audio_timeline(now + playout_decode_lead_us(), rate);
    playout_clock_.anchor(af.pts_us, next_write_us(), af.tempo);
  } else if (run_.audio_rate != rate) {
    const int64_t ideal = playout_clock_.wall_of(af.pts_us);
    // Audio joining a clock video already started: wait for a chunk still ahead.
    if (ideal < now + AUDIO_MIN_LEAD_US)
      return;
    playout_start_audio_timeline(ideal, rate);
  } else {
    const int64_t error = playout_clock_.wall_of(af.pts_us) - next_write_us();
    const int64_t gap_limit = (af.tempo != 1.0 || af.after_transition)
                                  ? AUDIO_GAP_STRETCH_US
                                  : AUDIO_GAP_US;

    if (error > AUDIO_MAX_FILL_US) {
      playout_start_audio_timeline(next_write_us() + error, rate);
    } else if (error > gap_limit) {
      // Lost audio packets. Silence holds the timeline where the media says it
      // should be; left out, everything after would play early.
      const uint32_t fill = static_cast<uint32_t>(error * rate / 1000000);
      std::vector<float> silence(fill, 0.0f);
      playout_write_audio(silence.data(), af.channels >= 2 ? silence.data() : nullptr, fill);
      run_.audio_fade_total = static_cast<uint32_t>(rate / 200);
      run_.audio_fade_left = run_.audio_fade_total;
    } else if (error < -gap_limit) {
      const uint64_t behind = static_cast<uint64_t>(-error) * rate / 1000000;
      if (behind >= af.frames)
        return;
      skip = static_cast<uint32_t>(behind);
    }

    const int64_t pts_us = af.pts_us + skip * 1000000LL / rate;
    if (next_write_us() < now + AUDIO_MIN_LEAD_US) {
      // Decoding fell behind and OBS would play past this chunk.
      playout_start_audio_timeline(now + playout_decode_lead_us(), rate);
      playout_clock_.anchor(pts_us, next_write_us(), af.tempo);
    } else {
      playout_clock_.observe(pts_us, next_write_us(), af.tempo);
    }
  }

  if (run_.audio_fade_left > 0)
    fade_in_planar(af, skip, af.frames - skip, run_.audio_fade_left,
                   run_.audio_fade_total);

  // The dashboard monitors the source even while OBS output is muted.
  send_preview_audio(af);

  const float *left = reinterpret_cast<const float *>(af.data);
  const float *right = af.channels >= 2 ? left + af.frames : nullptr;

  if (run_.sound_listening) {
    const int64_t first_sample_us = static_cast<int64_t>(
        (run_.audio_base_ns + samples_to_ns(run_.audio_samples, rate)) / 1000);
    sound_tracer_.feed(left + skip, right ? right + skip : nullptr,
                       af.frames - skip, rate, first_sample_us);
  }

  playout_write_audio(left + skip, right ? right + skip : nullptr, af.frames - skip);
}

//  Present thread

void LiveStreamSource::playout_present_thread_func() {
  lss_log_debug("Playout present thread started");

  while (running_.load()) {
    if (!uses_playout() || !playout_clock_.valid()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    int64_t media = 0;
    int64_t next_media = 0;
    bool has_next = false;
    {
      std::unique_lock<std::mutex> lock(present_queue_mutex_);
      if (present_queue_.empty()) {
        present_cv_.wait_for(lock, std::chrono::milliseconds(5));
        continue;
      }
      media = present_queue_.front().media_us;
      has_next = present_queue_.size() > 1;
      if (has_next)
        next_media = present_queue_[1].media_us;
    }

    const int64_t due = playout_clock_.wall_of(media);
    const int64_t deliver_at = snapped_delivery(due);
    const int64_t now = wall_us();

    if (deliver_at > now) {
      const int64_t wait_ms = std::min<int64_t>((deliver_at - now) / 1000 + 1, 4);
      std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
      continue;
    }

    PresentFrame entry;
    {
      std::lock_guard<std::mutex> lock(present_queue_mutex_);
      if (present_queue_.empty() || present_queue_.front().media_us != media)
        continue; // flushed by a cut or reset while we waited
      entry = present_queue_.front();
      present_queue_.pop_front();
    }

    // OBS keeps one frame per tick, so an earlier one is copied for nothing.
    const bool superseded =
        has_next &&
        snapped_delivery(playout_clock_.wall_of(next_media)) == deliver_at;
    const bool too_late = has_next && now - due > LATE_DROP_US;

    if (superseded || too_late) {
      if (too_late)
        total_frames_dropped_latency_.fetch_add(1);
      av_frame_free(&entry.frame);
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(present_output_mutex_);
      if (playout_clock_.valid())
        submit_video_frame(entry.frame, static_cast<uint64_t>(due) * 1000ULL);
    }
    av_frame_free(&entry.frame);
  }

  lss_log_debug("Playout present thread stopped");
}

} // namespace lss
