#pragma once

#include <atomic>
#include <chrono>

namespace lss {

class ReconnectManager {
public:
  ReconnectManager() = default;

  void reset() {
    attempts_ = 0;
    last_attempt_ = std::chrono::steady_clock::now();
  }

  bool can_retry() {
    if (attempts_ == 0)
      return true;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_attempt_)
                       .count();

    // Aggressive reconnect with increasing delays
    int delays[] = {0, 200, 500, 1000, 2000};
    int idx = std::min(attempts_, 4);
    int delay = delays[idx];

    return elapsed >= delay;
  }

  void mark_failed() {
    attempts_++;
    last_attempt_ = std::chrono::steady_clock::now();
  }

  int get_attempts() const { return attempts_; }

private:
  int attempts_ = 0;
  std::chrono::steady_clock::time_point last_attempt_;
};

} // namespace lss
