// catchup-controller.cpp
#include "utils/catchup-controller.h"

namespace lss {

void CatchupController::set_enabled(bool enabled) {
  enabled_.store(enabled, std::memory_order_relaxed);
  if (!enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
  }
}

void CatchupController::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  active_ = false;
  initialised_ = false;
  first_pts_us_ = 0;
  first_wall_us_ = 0;
  drift_us_ = 0;
}

double CatchupController::update(int64_t pts_us, bool is_keyframe,
                                 bool &drop_frame) {
  drop_frame = false;

  std::lock_guard<std::mutex> lock(mutex_);

  int64_t wall_us = now_us();

  if (!initialised_) {
    first_pts_us_ = pts_us;
    first_wall_us_ = wall_us;
    initialised_ = true;
    return 1.0;
  }

  int64_t wall_elapsed = wall_us - first_wall_us_;
  int64_t pts_elapsed = pts_us - first_pts_us_;

  drift_us_ = wall_elapsed - pts_elapsed;

  if (!enabled_.load(std::memory_order_relaxed))
    return 1.0;

  int64_t drift_ms = drift_us_ / 1000;

  if (!active_) {
    if (drift_ms > CATCHUP_DRIFT_THRESHOLD_MS) {
      active_ = true;
      lss_log_info("Catch-up engaged (drift: %lld ms)", (long long)drift_ms);
    } else {
      return 1.0;
    }
  }

  if (drift_ms <= CATCHUP_STABLE_THRESHOLD_MS) {
    active_ = false;
    lss_log_info("Catch-up disengaged (drift: %lld ms)", (long long)drift_ms);
    return 1.0;
  }

  // Scale playback speed proportionally to how far behind we are.
  // t=0 at the threshold, t=1 at 2x threshold, clamped above that.
  // Past 2x threshold we also start dropping non-keyframes.
  double t = static_cast<double>(drift_ms - CATCHUP_DRIFT_THRESHOLD_MS) /
             static_cast<double>(CATCHUP_DRIFT_THRESHOLD_MS);
  if (t > 1.0)
    t = 1.0;

  double speed =
      CATCHUP_SPEED_MIN + t * (CATCHUP_SPEED_MAX - CATCHUP_SPEED_MIN);

  if (drift_ms > 2 * CATCHUP_DRIFT_THRESHOLD_MS && !is_keyframe) {
    drop_frame = true;
  }

  return speed;
}

bool CatchupController::is_catching_up() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

int64_t CatchupController::drift_ms() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return drift_us_ / 1000;
}

} // namespace lss
