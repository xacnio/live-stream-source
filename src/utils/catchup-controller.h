// catchup-controller.h - latency drift monitor
#pragma once

#include "core/common.h"

namespace lss {

class CatchupController {
public:
  CatchupController() = default;
  ~CatchupController() = default;

  void set_enabled(bool enabled);
  bool is_enabled() const { return enabled_; }

  void reset();
  // Feed each decoded frame here. Returns how fast to play back
  // (1.0 = realtime). If we're too far behind it'll flag drop_frame
  // so the caller can skip non-keyframes and catch up quicker.
  double update(int64_t pts_us, bool is_keyframe, bool &drop_frame);
  bool is_catching_up() const;
  int64_t drift_ms() const;

private:
  std::atomic<bool> enabled_{true};

  mutable std::mutex mutex_;
  bool active_ = false;
  bool initialised_ = false;
  int64_t first_pts_us_ = 0;
  int64_t first_wall_us_ = 0;
  int64_t drift_us_ = 0;
};

} // namespace lss
