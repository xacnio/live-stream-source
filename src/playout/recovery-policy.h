// recovery-policy.h - decides how to return to the target latency
//
// Three tiers on the measured excess latency:
//   < 0.3 s   leave it alone
//   0.3 - 2 s drift back inaudibly, never above 1.10x
//   > 2 s     cut to a keyframe near the live edge
//
// Visible fast-forward does not exist here on purpose: past the point where
// speed-up is inaudible the content is skipped in one masked step.
#pragma once

#include <cstdint>

namespace lss {

enum class RecoveryMode : int {
  CutToLive = 0, // skip content above the cut threshold (default)
  Preserve = 1,  // keep everything, drain slowly
};

struct RecoveryDecision {
  double rate = 1.0;
  bool cut = false;
};

class RecoveryPolicy {
public:
  void configure(int64_t base_target_us, RecoveryMode mode, bool enabled);
  void reset();
  // For a new connection: its network owes nothing to the last one.
  void forget_learned_latency() { extra_us_ = 0; }

  // Follower mode: steer towards this latency, computed from another source,
  // with a tighter deadband since the goal is matching rather than staying low.
  // lead_rate is the other source's own rate: correcting on top of it is what
  // closes the gap, otherwise equal speeds would just hold it open.
  void set_follow_target(int64_t target_us, double lead_rate) {
    following_ = true;
    follow_target_us_ = target_us;
    follow_lead_rate_ = lead_rate;
  }
  void clear_follow() {
    following_ = false;
    follow_correcting_ = false;
  }
  bool following() const { return following_; }

  // Latency we steer towards: the configured value plus what jitter has
  // proven necessary. Never below min_target_us (decoder latency + headroom).
  int64_t target_us(int64_t min_target_us) const;

  // Call about every 100 ms while playing.
  RecoveryDecision step(int64_t now_us, int64_t latency_us, double ingest_rate,
                        int64_t min_target_us);

  // True when excess_us of backlog should be cut rather than played through.
  bool wants_cut(int64_t excess_us) const;

  // Short stalls mean the buffer is too small for this network, so the target
  // grows. Long ones are outages and say nothing about jitter.
  void on_stall_finished(int64_t now_us, int64_t stall_us);

  void on_cut(int64_t now_us);

  double rate() const { return rate_; }
  int64_t adaptive_extra_us() const { return extra_us_; }

  static constexpr int64_t CUT_EXCESS_US = 2000000;
  // Hard limit for Preserve mode: bounded by the packet buffer.
  static constexpr int64_t PRESERVE_MAX_EXCESS_US = 30000000;

private:
  static constexpr int64_t RATE_ENTER_US = 300000;
  static constexpr int64_t RATE_EXIT_US = 100000;
  static constexpr double MAX_RATE = 1.10;
  static constexpr double MIN_RATE = 0.97;
  static constexpr double RATE_GAIN_PER_SEC = 0.10;
  static constexpr double MAX_SLEW_PER_SEC = 0.05;
  static constexpr double MIN_ACTIVE_STEP = 0.02;
  static constexpr int64_t FOLLOW_ENTER_US = 60000;
  static constexpr int64_t FOLLOW_EXIT_US = 20000;
  static constexpr double FOLLOW_MIN_RATE = 0.95;
  static constexpr double FOLLOW_MAX_RATE = 1.08;
  static constexpr double FOLLOW_MIN_STEP = 0.01;
  static constexpr double FOLLOW_GAIN_PER_SEC = 0.25;
  // Rate of ingest (stream seconds per wall second) above which a burst is
  // still landing and the newest packet isn't the live edge yet.
  static constexpr double BURST_RATE = 1.5;
  static constexpr int64_t CUT_COOLDOWN_US = 2000000;
  static constexpr int64_t JITTER_STALL_MAX_US = 1500000;
  static constexpr int64_t EXTRA_STEP_US = 100000;
  static constexpr int64_t EXTRA_MAX_US = 1000000;
  static constexpr int64_t EXTRA_DECAY_STEP_US = 50000;
  static constexpr int64_t EXTRA_DECAY_AFTER_US = 30000000;

  int64_t base_target_us_ = 500000;
  RecoveryMode mode_ = RecoveryMode::CutToLive;
  bool enabled_ = true;
  bool following_ = false;
  int64_t follow_target_us_ = 0;
  double follow_lead_rate_ = 1.0;
  bool follow_correcting_ = false;

  double rate_ = 1.0;
  int64_t last_step_us_ = 0;
  int64_t last_cut_us_ = 0;
  int64_t extra_us_ = 0;
  int64_t last_stall_us_ = 0;
  int64_t last_decay_us_ = 0;
};

} // namespace lss
