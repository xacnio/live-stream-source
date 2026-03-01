// bitrate-monitor.cpp
#include "utils/bitrate-monitor.h"

namespace lss {

void BitrateMonitor::set_threshold_kbps(int kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  threshold_kbps_ = kbps;
}

void BitrateMonitor::record_bytes(int64_t bytes) {
  std::lock_guard<std::mutex> lock(mutex_);

  int64_t now = now_ms();
  samples_.push_back({now, bytes});
  total_bytes_ += bytes;
  total_bytes_all_ += bytes;

  // Evict samples outside the window
  int64_t cutoff = now - BITRATE_WINDOW_MS;
  while (!samples_.empty() && samples_.front().timestamp_ms < cutoff) {
    total_bytes_ -= samples_.front().bytes;
    samples_.pop_front();
  }

  // Kbps = total_bytes_ * 8 / window_seconds / 1000
  if (!samples_.empty()) {
    int64_t span_ms = now - samples_.front().timestamp_ms;
    if (span_ms > 0) {
      cached_kbps_ = (static_cast<double>(total_bytes_) * 8.0) /
                     (static_cast<double>(span_ms) / 1000.0) / 1000.0;
    }
  }
}

double BitrateMonitor::current_kbps() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cached_kbps_;
}

bool BitrateMonitor::is_low() const {
  std::lock_guard<std::mutex> lock(mutex_);

  // Active eviction based on current time
  int64_t now = now_ms();
  int64_t cutoff = now - BITRATE_WINDOW_MS;

  while (!samples_.empty() && samples_.front().timestamp_ms < cutoff) {
    total_bytes_ -= samples_.front().bytes;
    samples_.pop_front();
  }

  if (samples_.empty()) {
    cached_kbps_ = 0.0;
    return false; // No data yet - don't consider it low
  } else {
    int64_t span_ms = now - samples_.front().timestamp_ms;
    if (span_ms > 0) {
      cached_kbps_ = (static_cast<double>(total_bytes_) * 8.0) /
                     (static_cast<double>(span_ms) / 1000.0) / 1000.0;
    }
  }

  return cached_kbps_ < static_cast<double>(threshold_kbps_);
}

void BitrateMonitor::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  samples_.clear();
  total_bytes_ = 0;
  total_bytes_all_ = 0;
  cached_kbps_ = 0.0;
}

int64_t BitrateMonitor::total_bytes_all() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_bytes_all_;
}

} // namespace lss
