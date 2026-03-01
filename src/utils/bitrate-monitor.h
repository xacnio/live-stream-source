// bitrate-monitor.h
// Sliding-window bitrate tracker with low-bitrate detection.
#pragma once

#include "core/common.h"
#include <deque>

namespace lss {

class BitrateMonitor {
public:
  BitrateMonitor() = default;
  ~BitrateMonitor() = default;

  void set_threshold_kbps(int kbps);
  void record_bytes(int64_t bytes);
  double current_kbps() const;
  bool is_low() const;
  void reset();
  int64_t total_bytes_all() const;

private:
  struct Sample {
    int64_t timestamp_ms;
    int64_t bytes;
  };

  mutable std::mutex mutex_;
  mutable std::deque<Sample> samples_;
  int threshold_kbps_ = DEFAULT_LOW_BITRATE_KBPS;
  mutable int64_t total_bytes_ = 0;
  mutable int64_t total_bytes_all_ = 0; // cumulative (never evicted)
  mutable double cached_kbps_ = 0.0;
};

} // namespace lss
