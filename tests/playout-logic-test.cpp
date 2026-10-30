// playout-logic-test.cpp - unit tests for the OBS-independent playout logic
//
// Build with -DLSS_BUILD_TESTS=ON, then run the playout-logic-test target.
#include "core/fallback-fit.h"
#include "core/fallback-template.h"
#include "media/privacy-blur.h"
#include "network/ws-protocol.h"
#include "playout/sound-match.h"
#include "playout/packet-buffer.h"
#include "playout/playout-clock.h"
#include "playout/recovery-policy.h"
#include "playout/sync-registry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace lss;

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static bool near_us(int64_t a, int64_t b, int64_t tol = 1) {
  return std::llabs(a - b) <= tol;
}

static void test_clock_maps_both_ways() {
  PlayoutClock clock;
  CHECK(!clock.valid());
  clock.anchor(1000000, 5000000, 1.0);
  CHECK(clock.valid());
  CHECK(clock.wall_of(1500000) == 5500000);
  CHECK(clock.media_at(5250000) == 1250000);
}

static void test_clock_rate_applies_from_write_head() {
  PlayoutClock clock;
  clock.anchor(0, 0, 1.0);
  clock.observe(1000000, 1000000, 1.1);
  // Before the write-head the old rate still holds.
  CHECK(clock.wall_of(500000) == 500000);
  // 1.1 s of media past it takes 1 s.
  CHECK(near_us(clock.wall_of(2100000), 2000000));
  CHECK(near_us(clock.media_at(2000000), 2100000));
}

static void test_clock_ignores_jitter_and_tracks_drift() {
  PlayoutClock clock;
  clock.anchor(0, 0, 1.0);
  // FLV timestamps are whole milliseconds: this must not create segments.
  clock.observe(100000, 101000, 1.0);
  CHECK(clock.wall_of(200000) == 200000);
  // A real offset moves the mapping.
  clock.observe(300000, 310000, 1.0);
  CHECK(clock.wall_of(400000) == 410000);
}

static void test_clock_ignores_old_media() {
  PlayoutClock clock;
  clock.anchor(1000000, 1000000, 1.0);
  clock.observe(500000, 900000, 1.5);
  CHECK(clock.wall_of(2000000) == 2000000);
}

static void test_timeline_keeps_holes() {
  MediaTimeline timeline;
  CHECK(timeline.map(0, 0, 0) == 0);
  CHECK(timeline.map(0, 40000, 40000) == 0);
  // 5 s outage, content missing: arrival gap explains the jump.
  CHECK(timeline.map(0, 5040000, 5040000) == 0);
}

static void test_timeline_keeps_backlog() {
  MediaTimeline timeline;
  timeline.map(0, 0, 0);
  // Stall, then the backlog lands with old timestamps.
  CHECK(timeline.map(0, 40000, 5000000) == 0);
  CHECK(timeline.map(0, 80000, 5000100) == 0);
}

static void test_timeline_rebases_encoder_restart() {
  MediaTimeline timeline;
  timeline.map(0, 100000000, 0);
  timeline.map(0, 100040000, 40000);
  // Timestamps restart at 0.
  const int64_t offset = timeline.map(0, 0, 80000);
  CHECK(offset != 0);
  CHECK(near_us(0 + offset, 100080000));
  // Later packets stay continuous.
  CHECK(timeline.map(0, 40000, 120000) == offset);
}

static void test_timeline_rebases_forward_wrap() {
  MediaTimeline timeline;
  timeline.map(0, 0, 0);
  timeline.map(0, 40000, 40000);
  // 26-hour jump with no arrival gap.
  const int64_t huge = 95443717688LL;
  const int64_t offset = timeline.map(0, huge, 80000);
  CHECK(near_us(huge + offset, 80000));
}

static void test_timeline_tolerates_av_skew() {
  MediaTimeline timeline;
  // Muxer holds audio 3 s behind video: no packet here is a discontinuity.
  int64_t offset = 0;
  for (int i = 0; i < 200; ++i) {
    const int64_t t = i * 20000LL;
    offset = timeline.map(0, 3000000 + t, t);
    CHECK(offset == 0);
    offset = timeline.map(1, t, t);
    CHECK(offset == 0);
  }
}

static void test_timeline_rebase_carries_to_other_stream() {
  MediaTimeline timeline;
  timeline.map(0, 100000000, 0);
  timeline.map(1, 100000000, 0);
  const int64_t offset = timeline.map(0, 0, 40000);
  CHECK(offset != 0);
  // Audio restarted with it and lands on the same shifted timeline.
  CHECK(timeline.map(1, 0, 40000) == offset);
}

static RecoveryPolicy make_policy(RecoveryMode mode = RecoveryMode::CutToLive,
                                  bool enabled = true) {
  RecoveryPolicy policy;
  policy.configure(500000, mode, enabled);
  return policy;
}

static const int64_t MIN_TARGET = 220000;

static double settle(RecoveryPolicy &policy, int64_t latency, int steps = 40) {
  int64_t now = 1000000;
  double rate = 1.0;
  for (int i = 0; i < steps; ++i, now += 100000)
    rate = policy.step(now, latency, 1.0, MIN_TARGET).rate;
  return rate;
}

static void test_policy_deadband() {
  RecoveryPolicy policy = make_policy();
  CHECK(settle(policy, 700000) == 1.0);
}

static void test_policy_small_excess_stays_inaudible() {
  RecoveryPolicy policy = make_policy();
  const double rate = settle(policy, 1500000);
  CHECK(rate > 1.0);
  CHECK(rate <= 1.10);
}

static void test_policy_never_exceeds_max_rate() {
  RecoveryPolicy policy = make_policy(RecoveryMode::Preserve);
  CHECK(settle(policy, 20000000, 200) <= 1.10);
}

static void test_policy_rate_is_slew_limited() {
  RecoveryPolicy policy = make_policy();
  double prev = 1.0;
  int64_t now = 1000000;
  for (int i = 0; i < 30; ++i, now += 100000) {
    const double rate = policy.step(now, 2000000, 1.0, MIN_TARGET).rate;
    CHECK(rate - prev <= 0.0101);
    prev = rate;
  }
}

static void test_policy_returns_to_unity() {
  RecoveryPolicy policy = make_policy();
  settle(policy, 1500000);
  CHECK(settle(policy, 500000) == 1.0);
}

static void test_policy_refills_when_starving() {
  RecoveryPolicy policy = make_policy();
  const double rate = settle(policy, 150000);
  CHECK(rate < 1.0);
  CHECK(rate >= 0.97);
}

static void test_policy_cuts_only_after_burst() {
  RecoveryPolicy policy = make_policy();
  CHECK(!policy.step(1000000, 5000000, 8.0, MIN_TARGET).cut);
  CHECK(policy.step(1100000, 5000000, 1.0, MIN_TARGET).cut);
}

static void test_policy_preserve_keeps_content() {
  RecoveryPolicy policy = make_policy(RecoveryMode::Preserve);
  CHECK(!policy.step(1000000, 8000000, 1.0, MIN_TARGET).cut);
  CHECK(policy.step(1100000, 35000000, 1.0, MIN_TARGET).cut);
}

static void test_policy_disabled_does_nothing() {
  RecoveryPolicy policy = make_policy(RecoveryMode::CutToLive, false);
  CHECK(settle(policy, 9000000) == 1.0);
  CHECK(!policy.wants_cut(9000000));
}

static void test_policy_cut_cooldown() {
  RecoveryPolicy policy = make_policy();
  policy.on_cut(1000000);
  CHECK(!policy.step(1500000, 5000000, 1.0, MIN_TARGET).cut);
  CHECK(policy.step(3100000, 5000000, 1.0, MIN_TARGET).cut);
}

static void test_policy_short_stalls_grow_target() {
  RecoveryPolicy policy = make_policy();
  CHECK(policy.target_us(MIN_TARGET) == 500000);
  policy.on_stall_finished(1000000, 400000);
  CHECK(policy.target_us(MIN_TARGET) == 600000);
  // An outage says nothing about jitter.
  policy.on_stall_finished(2000000, 5000000);
  CHECK(policy.target_us(MIN_TARGET) == 600000);
}

static void test_policy_forgets_learned_latency() {
  RecoveryPolicy policy = make_policy();
  policy.on_stall_finished(1000000, 400000);
  policy.reset();
  CHECK(policy.target_us(MIN_TARGET) == 600000);
  policy.forget_learned_latency();
  CHECK(policy.target_us(MIN_TARGET) == 500000);
}

static void test_policy_extra_latency_decays() {
  RecoveryPolicy policy = make_policy();
  policy.on_stall_finished(1000000, 400000);
  policy.step(40000000, 600000, 1.0, MIN_TARGET);
  CHECK(policy.target_us(MIN_TARGET) == 550000);
}

static void test_policy_target_respects_decoder_floor() {
  RecoveryPolicy policy;
  policy.configure(250000, RecoveryMode::CutToLive, true);
  CHECK(policy.target_us(400000) == 400000);
}

static double settle_following(RecoveryPolicy &policy, int64_t latency,
                               int64_t target, double lead_rate = 1.0,
                               int steps = 40) {
  int64_t now = 1000000;
  double rate = 1.0;
  policy.set_follow_target(target, lead_rate);
  for (int i = 0; i < steps; ++i, now += 100000)
    rate = policy.step(now, latency, 1.0, MIN_TARGET).rate;
  return rate;
}

static void test_follow_small_error_is_left_alone() {
  RecoveryPolicy policy = make_policy();
  CHECK(settle_following(policy, 800000, 840000) == 1.0);
}

static void test_follow_tighter_than_normal_deadband() {
  RecoveryPolicy policy = make_policy();
  // 150 ms ahead of the target: normal mode ignores this, follower doesn't.
  CHECK(settle_following(policy, 950000, 800000) > 1.0);
}

static void test_follow_slows_down_to_add_delay() {
  RecoveryPolicy policy = make_policy();
  const double rate = settle_following(policy, 500000, 1000000);
  CHECK(rate < 1.0);
  CHECK(rate >= 0.95);
}

static void test_follow_cannot_pass_decoder_floor() {
  RecoveryPolicy policy = make_policy();
  policy.set_follow_target(100000, 1.0);
  CHECK(policy.target_us(MIN_TARGET) == MIN_TARGET);
}

static void test_follow_matches_lead_rate_when_aligned() {
  RecoveryPolicy policy = make_policy();
  CHECK(settle_following(policy, 800000, 800000, 1.04) == 1.04);
}

static void test_follow_closes_gap_on_a_moving_lead() {
  RecoveryPolicy policy = make_policy();
  // Lead at 1.02 and we're 190 ms late: matching its speed would never close it.
  CHECK(settle_following(policy, 990000, 800000, 1.02) > 1.02);
}

static void test_follow_can_be_cleared() {
  RecoveryPolicy policy = make_policy();
  policy.set_follow_target(3000000, 1.0);
  policy.clear_follow();
  CHECK(policy.target_us(MIN_TARGET) == 500000);
}

static void test_baseline_takes_fastest_delivery() {
  ArrivalBaseline baseline;
  baseline.add(0, 500);
  baseline.add(1000000, 200);
  baseline.add(2000000, 900);
  CHECK(baseline.value() == 200);
}

static void test_baseline_forgets_after_window() {
  ArrivalBaseline baseline;
  baseline.add(0, 100);
  // Path got permanently slower: after the window that's the new normal.
  for (int i = 1; i <= 26; ++i)
    baseline.add(i * 5000000LL, 900);
  CHECK(baseline.value() == 900);
}

static void test_registry_roundtrip() {
  SyncRegistry &registry = SyncRegistry::instance();
  registry.publish("a", {700, 5, false});
  SyncState state;
  CHECK(registry.lookup("a", state));
  CHECK(state.delay_us == 700);
  registry.remove("a");
  CHECK(!registry.lookup("a", state));
}

static const int RATE = 48000;

static std::vector<float> quiet_audio(double seconds) {
  std::vector<float> out(static_cast<size_t>(seconds * RATE));
  unsigned state = 12345;
  for (float &s : out) {
    state = state * 1103515245u + 12345u;
    s = (static_cast<int>((state >> 16) & 0x7fff) / 16384.0f - 1.0f) * 0.002f;
  }
  return out;
}

static void add_clap(std::vector<float> &audio, double at_s) {
  unsigned state = 777u + static_cast<unsigned>(at_s * 1000);
  const size_t start = static_cast<size_t>(at_s * RATE);
  for (size_t i = 0; i < 2400 && start + i < audio.size(); ++i) {
    state = state * 1103515245u + 12345u;
    const float noise = static_cast<int>((state >> 16) & 0x7fff) / 16384.0f - 1.0f;
    audio[start + i] += noise * 0.6f * std::exp(-static_cast<float>(i) / 300.0f);
  }
}

static SoundTrace trace_of(const std::vector<float> &audio, int64_t start_us = 1000000000) {
  SoundTracer tracer;
  constexpr uint32_t BLOCK = 1024;
  for (size_t i = 0; i + BLOCK <= audio.size(); i += BLOCK)
    tracer.feed(&audio[i], nullptr, BLOCK, RATE,
                start_us + static_cast<int64_t>(i) * 1000000 / RATE);
  return tracer.trace();
}

static std::vector<float> with_claps(std::vector<double> times, double seconds = 8) {
  std::vector<float> audio = quiet_audio(seconds);
  for (double t : times)
    add_clap(audio, t);
  return audio;
}

static void test_sound_match_measures_delay() {
  const auto a = trace_of(with_claps({2.0, 2.5, 3.1}));
  const auto b = trace_of(with_claps({2.73, 3.23, 3.83}));
  SoundMatch match;
  CHECK(match_sounds(a, b, 5000000, &match));
  CHECK(std::llabs(match.delta_us - 730000) < 6000);
}

static void test_sound_match_handles_negative_delay() {
  const auto a = trace_of(with_claps({2.73, 3.23, 3.83}));
  const auto b = trace_of(with_claps({2.0, 2.5, 3.1}));
  SoundMatch match;
  CHECK(match_sounds(a, b, 5000000, &match));
  CHECK(std::llabs(match.delta_us + 730000) < 6000);
}

static void test_sound_match_uses_absolute_time() {
  // Same moment in the room, but the second stream's audio is 2 s behind on the clock.
  const auto a = trace_of(with_claps({2.0, 2.5, 3.1}), 1000000000);
  const auto b = trace_of(with_claps({2.0, 2.5, 3.1}), 1002000000);
  SoundMatch match;
  CHECK(match_sounds(a, b, 5000000, &match));
  CHECK(std::llabs(match.delta_us - 2000000) < 6000);
}

static void test_sound_match_survives_extra_sounds() {
  // A cough next to the claps ruled out the whole set in the old detector.
  auto noisy = with_claps({2.0, 2.5, 3.1, 3.3, 4.2, 1.2});
  const auto a = trace_of(with_claps({2.0, 2.5, 3.1}));
  const auto b = trace_of(noisy);
  SoundMatch match;
  CHECK(match_sounds(a, b, 5000000, &match));
  CHECK(std::llabs(match.delta_us) < 6000);
}

static void test_sound_match_needs_more_than_one_sound() {
  // Any two lone bangs line up at some delay.
  SoundMatch match;
  CHECK(!match_sounds(trace_of(with_claps({2.0})), trace_of(with_claps({3.4})), 5000000, &match));
}

static void test_sound_match_rejects_unrelated_sounds() {
  SoundMatch match;
  CHECK(!match_sounds(trace_of(with_claps({1.5, 2.1, 3.4})),
                      trace_of(with_claps({1.2, 4.0, 4.9})), 5000000, &match));
}

static void test_sound_match_rejects_silence() {
  const auto a = trace_of(with_claps({2.0, 2.5, 3.1}));
  const auto b = trace_of(quiet_audio(8));
  SoundMatch match;
  CHECK(!match_sounds(a, b, 5000000, &match));
  CHECK(count_sounds(a) == 3);
  CHECK(count_sounds(b) == 0);
}

static void test_sound_match_respects_limit() {
  const auto a = trace_of(with_claps({2.0, 2.5, 3.1}));
  const auto b = trace_of(with_claps({2.0, 2.5, 3.1}), 1009000000);
  SoundMatch match;
  CHECK(!match_sounds(a, b, 5000000, &match));
}

static void test_sound_trace_keeps_time_across_a_gap() {
  SoundTracer tracer;
  std::vector<float> block(1024, 0.001f);
  tracer.feed(block.data(), nullptr, 1024, RATE, 1000000000);
  tracer.feed(block.data(), nullptr, 1024, RATE, 1003000000);
  // Three seconds of 5 ms steps, not two blocks' worth.
  CHECK(tracer.trace().onsets.size() >= 600);
}

static void test_sync_group_cant_point_at_itself() {
  SyncConfig config;
  config.mode = SyncMode::Follow;
  config.add_follower("a", 0);
  config.add_follower("b", 120);
  config.set_main("a");
  CHECK(config.main_uuid == "a");
  CHECK(!config.is_follower("a"));
  CHECK(config.is_follower("b"));
  CHECK(config.offset_of("b") == 120);

  config.add_follower("a", 50); // the main source can't join its own followers
  CHECK(!config.is_follower("a"));

  // Swapping roles leaves one main source, never a loop.
  config.set_main("b");
  config.add_follower("a", 0);
  CHECK(config.is_main("b"));
  CHECK(config.is_follower("a"));
  CHECK(!config.is_follower("b"));
}

static void test_sync_shift_counts_what_each_source_adds() {
  SyncConfig config;
  config.mode = SyncMode::Follow;
  config.set_main("main");
  config.add_follower("cam", 300);
  config.main_delay_ms = 1000;
  CHECK(config.shift_of("main") == 1000);
  CHECK(config.shift_of("cam") == 300); // the main source's own moves reach it separately
  CHECK(config.shift_of("other") == 0);

  config.main_delay_ms = -200;
  CHECK(config.shift_of("main") == -200);

  config.mode = SyncMode::Off;
  CHECK(config.shift_of("main") == 0);
  CHECK(config.shift_of("cam") == 0);
}

static void test_sync_touch_keeps_a_waiting_source_alive() {
  SyncRegistry &registry = SyncRegistry::instance();
  SyncState state;
  state.delay_us = 123;
  state.updated_us = 1000;
  registry.publish("waiting", state);
  registry.touch("waiting", 9000);
  registry.touch("nobody", 9000);

  SyncState seen;
  CHECK(registry.lookup("waiting", seen));
  CHECK(seen.updated_us == 9000 && seen.delay_us == 123);
  CHECK(!registry.lookup("nobody", seen));
  registry.remove("waiting");
}

static void test_sync_group_needs_all_its_parts() {
  SyncConfig config;
  CHECK(!config.ready());
  config.set_main("a");
  config.add_follower("b", 0);
  CHECK(!config.ready()); // still Off
  CHECK(!config.is_follower("b"));
  CHECK(!config.is_main("a"));
  config.mode = SyncMode::Fixed;
  CHECK(config.ready());
  config.offsets_ms.clear();
  CHECK(!config.ready());
}

static void test_sync_main_delay_requests_dont_add_up() {
  SyncRegistry &registry = SyncRegistry::instance();
  CHECK(registry.take_main_delay_request("m") == 0);
  registry.request_main_delay("m", 900);
  registry.request_main_delay("m", 1400);
  registry.request_main_delay("m", 700);
  CHECK(registry.take_main_delay_request("m") == 1400);
  CHECK(registry.take_main_delay_request("m") == 0);
}

static void test_sync_start_reaches_only_followers() {
  SyncRegistry &registry = SyncRegistry::instance();
  int a = 0, b = 0, c = 0;
  registry.set_start_handler("a", [&]() { ++a; });
  registry.set_start_handler("b", [&]() { ++b; });
  registry.set_start_handler("c", [&]() { ++c; });

  SyncConfig config;
  registry.set_config(config);
  CHECK(!registry.start_sync());

  config.mode = SyncMode::Follow;
  config.set_main("a");
  config.add_follower("b", 0);
  registry.set_config(config);
  CHECK(registry.start_sync());
  CHECK(a == 0 && b == 1 && c == 0);

  registry.remove_start_handler("a");
  registry.remove_start_handler("b");
  registry.remove_start_handler("c");
  registry.set_config(SyncConfig{});
}

static void test_fallback_fit_keeps_shape() {
  const FallbackRect rect = fallback_rect(FallbackFit::Fit, 100, 4, 1000, 1000, 1920, 1080);
  CHECK(rect.width == 1080.0f && rect.height == 1080.0f);
  CHECK(rect.x == 420.0f && rect.y == 0.0f);
}

static void test_fallback_fill_covers_the_box() {
  const FallbackRect rect = fallback_rect(FallbackFit::Fill, 100, 4, 1000, 1000, 1920, 1080);
  CHECK(rect.width == 1920.0f && rect.height == 1920.0f);
  CHECK(rect.x == 0.0f && rect.y == -420.0f);
}

static void test_fallback_stretch_and_original() {
  const FallbackRect stretched = fallback_rect(FallbackFit::Stretch, 50, 0, 100, 100, 1920, 1080);
  CHECK(stretched.width == 960.0f && stretched.height == 540.0f);
  const FallbackRect original = fallback_rect(FallbackFit::Original, 50, 0, 200, 100, 1920, 1080);
  CHECK(original.width == 100.0f && original.height == 50.0f);
}

static void test_fallback_corners() {
  const FallbackRect corner = fallback_rect(FallbackFit::Stretch, 25, 8, 10, 10, 1000, 1000);
  CHECK(corner.x == 750.0f && corner.y == 750.0f);
  const FallbackRect top_left = fallback_rect(FallbackFit::Stretch, 25, 0, 10, 10, 1000, 1000);
  CHECK(top_left.x == 0.0f && top_left.y == 0.0f);
  const FallbackRect right = fallback_rect(FallbackFit::Stretch, 25, 5, 10, 10, 1000, 1000);
  CHECK(right.x == 750.0f && right.y == 375.0f);
}

static void test_fallback_waits_for_content_size() {
  const FallbackRect rect = fallback_rect(FallbackFit::Fit, 100, 4, 0, 0, 1920, 1080);
  CHECK(rect.width == 0.0f);
}

static void test_fallback_box_sits_on_its_anchor() {
  const FallbackRect box = fallback_box(50, 8, 1000, 1000);
  CHECK(box.x == 500.0f && box.y == 500.0f && box.width == 500.0f);
  CHECK(fallback_box(100, 8, 1000, 1000).x == 0.0f);
}

static void test_fallback_text_keeps_its_size_until_it_cant() {
  const FallbackRect box{0, 0, 1920, 1080};
  const FallbackRect small = fallback_text_rect(box, 400, 100, 1080);
  CHECK(small.width == 400.0f && small.x == 760.0f);
  // The same text on a 720p source stays the same share of the picture.
  CHECK(fallback_text_rect({0, 0, 1280, 720}, 400, 100, 720).height < 67.0f);
  const FallbackRect wide = fallback_text_rect(box, 3840, 100, 1080);
  CHECK(wide.width == 1920.0f);
}

static void test_fallback_motion_ends_in_place() {
  for (FallbackAnim anim : {FallbackAnim::None, FallbackAnim::Fade, FallbackAnim::SideBottom,
                            FallbackAnim::SideTop, FallbackAnim::SideRight, FallbackAnim::SideLeft,
                            FallbackAnim::Zoom, FallbackAnim::Pop}) {
    const FallbackMotion done = fallback_motion(anim, 1.0f, 1920, 1080);
    CHECK(done.dx == 0.0f && done.dy == 0.0f && done.scale == 1.0f && done.alpha == 1.0f);
  }
}

static void test_fallback_motion_starts_off_screen() {
  CHECK(fallback_motion(FallbackAnim::SideBottom, 0.0f, 1920, 1080).dy == 1080.0f);
  CHECK(fallback_motion(FallbackAnim::SideTop, 0.0f, 1920, 1080).dy == -1080.0f);
  CHECK(fallback_motion(FallbackAnim::SideRight, 0.0f, 1920, 1080).dx == 1920.0f);
  CHECK(fallback_motion(FallbackAnim::SideLeft, 0.0f, 1920, 1080).dx == -1920.0f);
  CHECK(fallback_motion(FallbackAnim::Fade, 0.0f, 1920, 1080).alpha == 0.0f);
  CHECK(fallback_motion(FallbackAnim::None, 0.0f, 1920, 1080).alpha == 0.0f);
  CHECK(fallback_motion(FallbackAnim::Pop, 0.0f, 1920, 1080).alpha == 0.0f);
}

static void test_fallback_pop_overshoots() {
  float largest = 0.0f;
  for (float t = 0.0f; t <= 1.0f; t += 0.05f)
    largest = std::max(largest, fallback_motion(FallbackAnim::Pop, t, 1920, 1080).scale);
  CHECK(largest > 1.05f && largest < 1.2f);
}

static void test_fallback_template_fills_in_safely() {
  const std::string page = render_fallback_template(
      "<p style=\"color:{{color}};zoom:{{scale}}\">{{text}}</p><b>{{text}}</b>", "A<b>&\"'", 0x800000FF, 150);
  CHECK(page == "<p style=\"color:rgba(255,0,0,0.50);zoom:150\">A&lt;b&gt;&amp;&quot;&#39;</p>"
                "<b>A&lt;b&gt;&amp;&quot;&#39;</b>");
  CHECK(css_color(0xFFFFFFFF) == "rgba(255,255,255,1.00)");
}

static void test_only_programs_on_this_machine_skip_the_key() {
  CHECK(trusted_without_proof(true, ""));
  CHECK(!trusted_without_proof(false, ""));
  CHECK(!trusted_without_proof(true, "null")); // a local file, or a sandboxed frame on any site
  CHECK(!trusted_without_proof(true, "https://example.com"));
}

static void test_only_this_machine_skips_the_key() {
  constexpr uint32_t VPN = 0x0A000005;    // 10.0.0.5
  constexpr uint32_t SERVER = 0x0A000009; // 10.0.0.9
  CHECK(is_same_machine(0x7F000001, 0x7F000001));
  CHECK(is_same_machine(0x7F000001, VPN));
  CHECK(is_same_machine(VPN, VPN)); // an overlay dialing the VPN address
  CHECK(!is_same_machine(SERVER, VPN));
  CHECK(!is_same_machine(SERVER, 0)); // getsockname failed
}

static void test_blur_keeps_flat_areas() {
  std::vector<uint8_t> plane(64 * 64, 100);
  std::vector<uint8_t> scratch;
  gaussian_blur_plane(plane.data(), 64, 64, 64, 4.0, scratch);
  for (uint8_t value : plane)
    CHECK(value == 100);
}

static void test_blur_spreads_evenly() {
  std::vector<uint8_t> plane(65 * 65, 0);
  plane[32 * 65 + 32] = 255;
  std::vector<uint8_t> scratch;
  gaussian_blur_plane(plane.data(), 65, 65, 65, 1.5, scratch);
  CHECK(plane[32 * 65 + 32] < 40);
  CHECK(plane[32 * 65 + 30] == plane[32 * 65 + 34]);
  CHECK(plane[30 * 65 + 32] == plane[34 * 65 + 32]);
  CHECK(plane[32 * 65 + 30] == plane[30 * 65 + 32]);
}

// Text-sized detail: bars a few pixels wide, white on black.
static void draw_bars(uint8_t *plane, int width, int height) {
  for (int y = height / 3; y < height * 2 / 3; ++y)
    for (int x = width / 4; x < width * 3 / 4; ++x)
      plane[y * width + x] = (x / 6) % 2 ? 235 : 16;
}

static int bar_contrast(const uint8_t *plane, int width, int height) {
  int low = 255, high = 0;
  const int y = height / 2;
  for (int x = width * 3 / 8; x < width * 5 / 8; ++x) {
    low = std::min<int>(low, plane[y * width + x]);
    high = std::max<int>(high, plane[y * width + x]);
  }
  return high - low;
}

static void test_privacy_blur_hides_fine_detail() {
  constexpr int W = 1920, H = 1080;
  std::vector<uint8_t> y(W * H, 16), u(W * H / 4, 128), v(W * H / 4, 128);
  draw_bars(y.data(), W, H);
  CHECK(bar_contrast(y.data(), W, H) > 200);

  std::vector<uint8_t> out_y(W * H), out_u(W * H / 4), out_v(W * H / 4);
  const uint8_t *const src[4] = {y.data(), u.data(), v.data(), nullptr};
  const int linesize[4] = {W, W / 2, W / 2, 0};
  PrivacyBlur blur;
  CHECK(blur.apply(src, linesize, AV_PIX_FMT_YUV420P, W, H, out_y.data(), out_u.data(), out_v.data()));
  CHECK(bar_contrast(out_y.data(), W, H) < 4);
}

static void test_privacy_blur_covers_10_bit() {
  // Skipping a format would put the picture on air unblurred.
  constexpr int W = 640, H = 360;
  std::vector<uint16_t> luma(W * H, 64 << 6), chroma(W * H / 2, 512 << 6);
  for (int y = H / 3; y < H * 2 / 3; ++y)
    for (int x = W / 4; x < W * 3 / 4; ++x)
      luma[y * W + x] = static_cast<uint16_t>(((x / 6) % 2 ? 940 : 64) << 6);

  std::vector<uint8_t> out_y(W * H), out_u(W * H / 4), out_v(W * H / 4);
  const uint8_t *const src[4] = {reinterpret_cast<const uint8_t *>(luma.data()),
                                 reinterpret_cast<const uint8_t *>(chroma.data()), nullptr, nullptr};
  const int linesize[4] = {W * 2, W * 2, 0, 0};
  PrivacyBlur blur;
  CHECK(blur.apply(src, linesize, AV_PIX_FMT_P010LE, W, H, out_y.data(), out_u.data(), out_v.data()));
  CHECK(bar_contrast(out_y.data(), W, H) < 25);
}

static void test_privacy_blur_covers_odd_sizes() {
  constexpr int W = 641, H = 361, CW = 321, CH = 181;
  std::vector<uint8_t> y(W * H, 16), u(CW * CH, 128), v(CW * CH, 128);
  draw_bars(y.data(), W, H);
  std::vector<uint8_t> out_y(W * H), out_u(CW * CH), out_v(CW * CH);
  const uint8_t *const src[4] = {y.data(), u.data(), v.data(), nullptr};
  const int linesize[4] = {W, CW, CW, 0};
  PrivacyBlur blur;
  CHECK(blur.apply(src, linesize, AV_PIX_FMT_YUV420P, W, H, out_y.data(), out_u.data(), out_v.data()));
  CHECK(bar_contrast(out_y.data(), W, H) < 25);
}

static void test_privacy_blur_fails_to_black() {
  constexpr int W = 64, H = 64;
  std::vector<uint8_t> y(W * H, 200), u(W * H / 4, 200), v(W * H / 4, 200);
  const uint8_t *const src[4] = {y.data(), u.data(), v.data(), nullptr};
  const int linesize[4] = {W, W / 2, W / 2, 0};
  std::vector<uint8_t> out_y(W * H, 200), out_u(W * H / 4, 200), out_v(W * H / 4, 200);
  PrivacyBlur blur;
  CHECK(!blur.apply(src, linesize, AV_PIX_FMT_NONE, W, H, out_y.data(), out_u.data(), out_v.data()));
  for (uint8_t value : out_y)
    CHECK(value == 16);
  for (uint8_t value : out_u)
    CHECK(value == 128);
}

static void test_header_any_case() {
  const std::string base = "GET / HTTP/1.1\r\nHost: a\r\n";
  CHECK(find_http_header(base + "Sec-WebSocket-Key: abc==\r\n\r\n",
                         "Sec-WebSocket-Key") == "abc==");
  CHECK(find_http_header(base + "Sec-Websocket-Key: abc==\r\n\r\n",
                         "Sec-WebSocket-Key") == "abc==");
  CHECK(find_http_header(base + "sec-websocket-key:abc==\r\n\r\n",
                         "Sec-WebSocket-Key") == "abc==");
  CHECK(find_http_header(base + "\r\n", "Sec-WebSocket-Key").empty());
}

static std::string client_frame(const std::string &text) {
  const unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::string out;
  out.push_back(static_cast<char>(0x81));
  out.push_back(static_cast<char>(0x80 | text.size()));
  out.append(reinterpret_cast<const char *>(mask), 4);
  for (size_t i = 0; i < text.size(); ++i)
    out.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
  return out;
}

static void test_frames_in_one_read() {
  const std::string data = client_frame("first") + client_frame("second");
  std::string text;
  const size_t one = parse_ws_frame(data.data(), data.size(), &text);
  CHECK(text == "first");
  const size_t two = parse_ws_frame(data.data() + one, data.size() - one, &text);
  CHECK(text == "second");
  CHECK(one + two == data.size());
}

static void test_frame_split_across_reads() {
  const std::string data = client_frame("hello");
  std::string text;
  for (size_t cut = 0; cut < data.size(); ++cut)
    CHECK(parse_ws_frame(data.data(), cut, &text) == 0);
  CHECK(parse_ws_frame(data.data(), data.size(), &text) == data.size());
  CHECK(text == "hello");
}

static void test_control_frame_is_skipped() {
  const std::string ping("\x89\x80\x00\x00\x00\x00", 6);
  std::string text = "stale";
  CHECK(parse_ws_frame(ping.data(), ping.size(), &text) == 6);
  CHECK(text.empty());
}

static void test_access_key() {
  CHECK(access_key_matches("secret", "secret"));
  CHECK(!access_key_matches("secreT", "secret"));
  CHECK(!access_key_matches("secret1", "secret"));
  CHECK(!access_key_matches("", "secret"));
}

static AVPacket *make_packet(int size) {
  AVPacket *pkt = av_packet_alloc();
  av_new_packet(pkt, size);
  return pkt;
}

static void push_video(PacketBuffer &buffer, int64_t dts, bool key) {
  PacketInfo info;
  info.dts_us = dts;
  info.pts_us = dts;
  info.keyframe = key;
  buffer.push(PacketKind::Video, make_packet(100), info);
}

static void push_audio(PacketBuffer &buffer, int64_t pts) {
  PacketInfo info;
  info.dts_us = pts;
  info.pts_us = pts;
  buffer.push(PacketKind::Audio, make_packet(10), info);
}

static PacketBuffer *make_buffer() {
  return new PacketBuffer(40000000, 64 * 1024 * 1024);
}

static void test_buffer_measures_duration() {
  PacketBuffer *buffer = make_buffer();
  for (int i = 0; i < 100; ++i)
    push_video(*buffer, i * 40000LL, i % 50 == 0);
  const PacketBufferStats stats = buffer->stats();
  CHECK(stats.has_video);
  CHECK(!stats.has_gap);
  CHECK(stats.playable_us() == 3960000);
  delete buffer;
}

static void test_buffer_finds_latest_keyframe() {
  PacketBuffer *buffer = make_buffer();
  for (int i = 0; i < 200; ++i)
    push_video(*buffer, i * 40000LL, i % 50 == 0);
  CutPoint cut;
  CHECK(buffer->find_cut_point(5000000, cut));
  CHECK(cut.key_dts_us == 4000000);
  // Target earlier than any key: first one after it.
  CHECK(buffer->find_cut_point(-1, cut));
  CHECK(cut.key_dts_us == 0);
  delete buffer;
}

static void test_buffer_cut_never_crosses_a_gap() {
  PacketBuffer *buffer = make_buffer();
  for (int i = 0; i < 30; ++i) {
    push_video(*buffer, i * 40000LL, i == 0);
    push_audio(*buffer, i * 40000LL);
  }
  // 5 s hole in both streams, then P-frames only.
  for (int i = 0; i < 30; ++i)
    push_video(*buffer, 6000000 + i * 40000LL, false);
  push_audio(*buffer, 6000000);

  PacketBufferStats stats = buffer->stats();
  CHECK(stats.has_gap);
  CHECK(stats.playable_us() == 1160000);

  CutPoint cut;
  // The only keyframe sits before the hole: unusable.
  CHECK(!buffer->find_cut_point(7000000, cut));

  push_video(*buffer, 7200000, true);
  CHECK(buffer->find_cut_point(9000000, cut));
  CHECK(cut.key_dts_us == 7200000);
  delete buffer;
}

static void test_buffer_drop_clears_gap() {
  PacketBuffer *buffer = make_buffer();
  push_video(*buffer, 0, true);
  push_audio(*buffer, 0);
  push_audio(*buffer, 6000000);
  push_video(*buffer, 6000000, true);
  CHECK(buffer->stats().has_gap);
  buffer->drop_video_before(6000000);
  CHECK(!buffer->stats().has_gap);
  PacketInfo info;
  CHECK(buffer->peek(PacketKind::Video, info));
  CHECK(info.dts_us == 6000000);
  delete buffer;
}

static void test_buffer_marks_packet_after_gap() {
  PacketBuffer *buffer = make_buffer();
  push_video(*buffer, 0, true);
  push_audio(*buffer, 0);
  push_video(*buffer, 40000, false);
  // Video lands first: confirmed when audio's jump follows.
  push_video(*buffer, 6000000, false);
  push_audio(*buffer, 6000000);
  PacketInfo info;
  AVPacket *pkt = buffer->pop(PacketKind::Video, &info);
  CHECK(!info.after_gap);
  av_packet_free(&pkt);
  pkt = buffer->pop(PacketKind::Video, &info);
  CHECK(!info.after_gap);
  av_packet_free(&pkt);
  pkt = buffer->pop(PacketKind::Video, &info);
  CHECK(info.after_gap);
  av_packet_free(&pkt);
  delete buffer;
}

static void test_buffer_sparse_video_is_not_a_gap() {
  PacketBuffer *buffer = make_buffer();
  // Static scene: 2 s between frames while audio keeps flowing.
  for (int i = 0; i < 100; ++i)
    push_audio(*buffer, i * 20000LL);
  push_video(*buffer, 0, true);
  push_video(*buffer, 2000000, false);
  CHECK(!buffer->stats().has_gap);
  delete buffer;
}

static void test_buffer_audio_only_jump_is_not_a_gap() {
  PacketBuffer *buffer = make_buffer();
  for (int i = 0; i < 50; ++i)
    push_video(*buffer, i * 40000LL, i == 0);
  // Encoder that stops sending audio during silence.
  push_audio(*buffer, 0);
  push_audio(*buffer, 1900000);
  CHECK(!buffer->stats().has_gap);
  delete buffer;
}

static void test_buffer_audio_drop() {
  PacketBuffer *buffer = make_buffer();
  for (int i = 0; i < 10; ++i)
    push_audio(*buffer, i * 20000LL);
  buffer->drop_audio_before(100000);
  PacketInfo info;
  CHECK(buffer->peek(PacketKind::Audio, info));
  CHECK(info.pts_us == 100000);
  delete buffer;
}

static void test_buffer_limit_drops_whole_gops() {
  PacketBuffer buffer(1000000, 64 * 1024 * 1024);
  for (int i = 0; i < 200; ++i)
    push_video(buffer, i * 40000LL, i % 25 == 0);
  PacketInfo info;
  CHECK(buffer.peek(PacketKind::Video, info));
  CHECK(info.keyframe);
  CHECK(7960000 - info.dts_us <= 1000000);
}

static void test_buffer_keeps_audio_when_video_is_sparse() {
  PacketBuffer *buffer = make_buffer();
  push_audio(*buffer, 0);
  // Next video frame is 3 s past the queued audio, and nothing was dropped.
  push_video(*buffer, 3000000, true);
  PacketInfo info;
  CHECK(buffer->peek(PacketKind::Audio, info));
  CHECK(info.pts_us == 0);
  delete buffer;
}

static void test_buffer_clear_resets_discontinuity_state() {
  PacketBuffer *buffer = make_buffer();
  push_video(*buffer, 0, true);
  buffer->clear();
  // A reconnect restarts timestamps: this is not a gap.
  push_video(*buffer, 900000000, true);
  CHECK(!buffer->stats().has_gap);
  delete buffer;
}

int main() {
  test_clock_maps_both_ways();
  test_clock_rate_applies_from_write_head();
  test_clock_ignores_jitter_and_tracks_drift();
  test_clock_ignores_old_media();
  test_timeline_keeps_holes();
  test_timeline_keeps_backlog();
  test_timeline_rebases_encoder_restart();
  test_timeline_rebases_forward_wrap();
  test_timeline_tolerates_av_skew();
  test_timeline_rebase_carries_to_other_stream();
  test_policy_deadband();
  test_policy_small_excess_stays_inaudible();
  test_policy_never_exceeds_max_rate();
  test_policy_rate_is_slew_limited();
  test_policy_returns_to_unity();
  test_policy_refills_when_starving();
  test_policy_cuts_only_after_burst();
  test_policy_preserve_keeps_content();
  test_policy_disabled_does_nothing();
  test_policy_cut_cooldown();
  test_policy_short_stalls_grow_target();
  test_policy_forgets_learned_latency();
  test_policy_extra_latency_decays();
  test_policy_target_respects_decoder_floor();
  test_follow_small_error_is_left_alone();
  test_follow_tighter_than_normal_deadband();
  test_follow_slows_down_to_add_delay();
  test_follow_cannot_pass_decoder_floor();
  test_follow_matches_lead_rate_when_aligned();
  test_follow_closes_gap_on_a_moving_lead();
  test_follow_can_be_cleared();
  test_baseline_takes_fastest_delivery();
  test_baseline_forgets_after_window();
  test_registry_roundtrip();
  test_header_any_case();
  test_access_key();
  test_frames_in_one_read();
  test_frame_split_across_reads();
  test_control_frame_is_skipped();
  test_sound_match_measures_delay();
  test_sound_match_handles_negative_delay();
  test_sound_match_uses_absolute_time();
  test_sound_match_survives_extra_sounds();
  test_sound_match_needs_more_than_one_sound();
  test_sound_match_rejects_unrelated_sounds();
  test_sound_match_rejects_silence();
  test_sound_match_respects_limit();
  test_sound_trace_keeps_time_across_a_gap();
  test_sync_group_cant_point_at_itself();
  test_sync_group_needs_all_its_parts();
  test_sync_touch_keeps_a_waiting_source_alive();
  test_sync_shift_counts_what_each_source_adds();
  test_sync_main_delay_requests_dont_add_up();
  test_sync_start_reaches_only_followers();
  test_fallback_fit_keeps_shape();
  test_fallback_fill_covers_the_box();
  test_fallback_stretch_and_original();
  test_fallback_corners();
  test_fallback_waits_for_content_size();
  test_fallback_box_sits_on_its_anchor();
  test_fallback_text_keeps_its_size_until_it_cant();
  test_fallback_motion_ends_in_place();
  test_fallback_motion_starts_off_screen();
  test_fallback_pop_overshoots();
  test_fallback_template_fills_in_safely();
  test_only_this_machine_skips_the_key();
  test_only_programs_on_this_machine_skip_the_key();
  test_blur_keeps_flat_areas();
  test_blur_spreads_evenly();
  test_privacy_blur_hides_fine_detail();
  test_privacy_blur_covers_10_bit();
  test_privacy_blur_covers_odd_sizes();
  test_privacy_blur_fails_to_black();
  test_buffer_measures_duration();
  test_buffer_finds_latest_keyframe();
  test_buffer_cut_never_crosses_a_gap();
  test_buffer_drop_clears_gap();
  test_buffer_marks_packet_after_gap();
  test_buffer_sparse_video_is_not_a_gap();
  test_buffer_audio_only_jump_is_not_a_gap();
  test_buffer_audio_drop();
  test_buffer_limit_drops_whole_gops();
  test_buffer_keeps_audio_when_video_is_sparse();
  test_buffer_clear_resets_discontinuity_state();

  if (failures == 0)
    std::printf("All playout logic tests passed\n");
  return failures == 0 ? 0 : 1;
}
