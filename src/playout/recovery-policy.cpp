// recovery-policy.cpp
#include "playout/recovery-policy.h"

#include <algorithm>
#include <cmath>

namespace lss {

void RecoveryPolicy::configure(int64_t base_target_us, RecoveryMode mode,
                               bool enabled) {
  base_target_us_ = base_target_us;
  mode_ = mode;
  enabled_ = enabled;
}

void RecoveryPolicy::reset() {
  rate_ = 1.0;
  last_step_us_ = 0;
}

int64_t RecoveryPolicy::target_us(int64_t min_target_us) const {
  if (following_)
    return std::max(follow_target_us_, min_target_us);
  return std::max(base_target_us_ + extra_us_, min_target_us);
}

RecoveryDecision RecoveryPolicy::step(int64_t now_us, int64_t latency_us,
                                      double ingest_rate,
                                      int64_t min_target_us) {
  RecoveryDecision decision;

  const int64_t target = target_us(min_target_us);
  const int64_t error = latency_us - target;

  double want = 1.0;
  if (enabled_ && following_) {
    const int64_t magnitude = error < 0 ? -error : error;
    if (magnitude > FOLLOW_ENTER_US)
      follow_correcting_ = true;
    else if (magnitude < FOLLOW_EXIT_US)
      follow_correcting_ = false;

    want = follow_lead_rate_;
    if (follow_correcting_) {
      const double step = std::max(
          FOLLOW_MIN_STEP, FOLLOW_GAIN_PER_SEC * (magnitude / 1000000.0));
      want += error > 0 ? step : -step;
    }
    want = std::round(std::clamp(want, FOLLOW_MIN_RATE, FOLLOW_MAX_RATE) * 100.0) / 100.0;
  } else if (enabled_) {
    if (error > RATE_ENTER_US || (rate_ > 1.0 && error > RATE_EXIT_US)) {
      want = 1.0 + std::clamp(RATE_GAIN_PER_SEC * (error / 1000000.0),
                              MIN_ACTIVE_STEP, MAX_RATE - 1.0);
    } else if (error < -target / 2 ||
               (rate_ < 1.0 && error < -target / 6)) {
      // Sender clock slower than ours: refill before the buffer runs dry.
      want = MIN_RATE;
    }
  }

  const double dt =
      last_step_us_ > 0 ? (now_us - last_step_us_) / 1000000.0 : 0.1;
  last_step_us_ = now_us;
  const double max_step = MAX_SLEW_PER_SEC * dt;
  double next = rate_ + std::clamp(want - rate_, -max_step, max_step);
  // Quantised so SoundTouch sees a handful of settings, not a stream of them.
  next = std::round(next * 100.0) / 100.0;
  if (next == rate_ && want != rate_)
    next = rate_ + (want > rate_ ? 0.01 : -0.01);
  rate_ = std::clamp(next, following_ ? FOLLOW_MIN_RATE : MIN_RATE, MAX_RATE);

  decision.rate = rate_;
  decision.cut = ingest_rate < BURST_RATE && wants_cut(error) &&
                 (last_cut_us_ == 0 || now_us - last_cut_us_ >= CUT_COOLDOWN_US);

  // Extra latency earned from jitter is given back once the link has calmed.
  if (extra_us_ > 0 && now_us - last_stall_us_ >= EXTRA_DECAY_AFTER_US &&
      now_us - last_decay_us_ >= EXTRA_DECAY_AFTER_US) {
    extra_us_ = std::max<int64_t>(0, extra_us_ - EXTRA_DECAY_STEP_US);
    last_decay_us_ = now_us;
  }

  return decision;
}

bool RecoveryPolicy::wants_cut(int64_t excess_us) const {
  if (!enabled_)
    return false;
  return excess_us > (mode_ == RecoveryMode::CutToLive ? CUT_EXCESS_US
                                                       : PRESERVE_MAX_EXCESS_US);
}

void RecoveryPolicy::on_stall_finished(int64_t now_us, int64_t stall_us) {
  last_stall_us_ = now_us;
  if (enabled_ && stall_us <= JITTER_STALL_MAX_US)
    extra_us_ = std::min(extra_us_ + EXTRA_STEP_US, EXTRA_MAX_US);
}

void RecoveryPolicy::on_cut(int64_t now_us) {
  last_cut_us_ = now_us;
  reset();
}

} // namespace lss
