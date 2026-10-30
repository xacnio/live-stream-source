// sync-registry.h - lets one source match its delay to another
//
// Two independent encoders share no clock, so the moment a frame was captured
// can't be recovered. What can be measured is how far behind its own best
// arrival each source is playing. Holding that equal cancels every delay change
// on either path (a bonded link slowing down, a phone switching towers) with no
// calibration. A fixed difference between the paths is invisible to this and is
// what the manual offset is for.
#pragma once

#include "playout/sound-match.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lss {

// Minimum of (arrival - media) over a sliding window: the fastest this stream
// has recently been delivered. The window also lets it follow encoder clock
// drift, and recover from timestamp holes a server may have collapsed.
class ArrivalBaseline {
public:
  void reset() {
    started_ = false;
    bucket_index_ = 0;
    bucket_start_us_ = 0;
  }

  void add(int64_t now_us, int64_t arrival_offset_us) {
    if (!started_) {
      started_ = true;
      bucket_start_us_ = now_us;
      for (int64_t &b : buckets_)
        b = INT64_MAX;
    }
    if (now_us - bucket_start_us_ >= BUCKET_US) {
      bucket_start_us_ = now_us;
      bucket_index_ = (bucket_index_ + 1) % BUCKETS;
      buckets_[bucket_index_] = INT64_MAX;
    }
    if (arrival_offset_us < buckets_[bucket_index_])
      buckets_[bucket_index_] = arrival_offset_us;
  }

  bool valid() const { return started_; }

  int64_t value() const {
    int64_t best = INT64_MAX;
    for (int64_t b : buckets_)
      if (b < best)
        best = b;
    return best;
  }

private:
  static constexpr int BUCKETS = 24;
  static constexpr int64_t BUCKET_US = 5000000; // 2 minutes in total

  bool started_ = false;
  int bucket_index_ = 0;
  int64_t bucket_start_us_ = 0;
  int64_t buckets_[BUCKETS] = {};
};

struct SyncState {
  int64_t delay_us = 0;   // playback delay beyond the best arrival
  int64_t updated_us = 0; // wall clock
  bool following = false;
  double rate = 1.0;      // current playback rate
  int error_ms = 0;       // a follower's distance from where it should be
  uint32_t move_id = 0; // goes up when the delay jumps on purpose
};

enum class SyncListenState : int { Idle, Listening, Found, NotFound, NoSound, NoStream };

// One follower's sync, for anything that shows it to the operator.
struct SyncProgress {
  SyncListenState state = SyncListenState::Idle;
  int seconds_left = 0;
  int heard_main = 0;
  int heard_own = 0;
  int delta_ms = 0;      // how far behind the main source this follower was
  int main_delay_ms = 0; // what the main source was delayed by for it
  int short_ms = 0;      // what it couldn't catch up, with main delay not allowed
  int64_t expires_us = 0;
};

enum class SyncMode : int {
  Off,    // every source plays on its own
  Follow, // followers keep tracking the main source's delay
  Fixed,  // nothing moves by itself, only the sync button and the offsets do
};

// One main source and its followers. Being a single group is what rules out two
// sources following each other: set_main() and add_follower() keep the main
// source out of the followers, whatever order things arrive in.
struct SyncConfig {
  SyncMode mode = SyncMode::Off;
  std::string main_uuid;
  std::map<std::string, int> offsets_ms; // followers, with their offset
  int main_delay_ms = 0;
  bool may_delay_main = true;

  void set_main(const std::string &uuid) {
    main_uuid = uuid;
    offsets_ms.erase(uuid);
  }

  void add_follower(const std::string &uuid, int offset_ms) {
    if (!uuid.empty() && uuid != main_uuid)
      offsets_ms[uuid] = offset_ms;
  }

  bool ready() const {
    return mode != SyncMode::Off && !main_uuid.empty() && !offsets_ms.empty();
  }

  bool is_main(const std::string &uuid) const { return ready() && uuid == main_uuid; }

  bool is_follower(const std::string &uuid) const {
    return ready() && offsets_ms.count(uuid) > 0;
  }

  // How far this source sits from where it would play outside the group.
  int shift_of(const std::string &uuid) const {
    if (is_main(uuid))
      return main_delay_ms;
    return is_follower(uuid) ? offset_of(uuid) : 0;
  }

  int offset_of(const std::string &uuid) const {
    auto it = offsets_ms.find(uuid);
    return it == offsets_ms.end() ? 0 : it->second;
  }
};

class SyncRegistry {
public:
  static SyncRegistry &instance() {
    static SyncRegistry registry;
    return registry;
  }

  SyncConfig config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
  }

  void set_config(const SyncConfig &config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    ++revision_;
  }

  void set_offset(const std::string &uuid, int offset_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_.offsets_ms.find(uuid);
    if (it != config_.offsets_ms.end() && it->second != offset_ms) {
      it->second = offset_ms;
      ++revision_;
    }
  }

  void set_main_delay(int delay_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.main_delay_ms = delay_ms;
    ++revision_;
  }

  // Goes up with every change, so the panel knows when to redraw and save.
  uint64_t revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
  }

  void set_start_handler(const std::string &uuid, std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    start_handlers_[uuid] = std::move(handler);
  }

  void remove_start_handler(const std::string &uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    start_handlers_.erase(uuid);
  }

  // Asks every follower to start listening. Returns false with no group set up.
  bool start_sync() {
    std::vector<std::function<void()>> handlers;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!config_.ready())
        return false;
      for (const auto &follower : config_.offsets_ms) {
        auto it = start_handlers_.find(follower.first);
        if (it != start_handlers_.end())
          handlers.push_back(it->second);
      }
    }
    for (auto &handler : handlers)
      handler();
    return !handlers.empty();
  }

  void publish(const std::string &uuid, const SyncState &state) {
    std::lock_guard<std::mutex> lock(mutex_);
    states_[uuid] = state;
  }

  // A source waiting on purpose has nothing new to publish, but it hasn't gone away.
  void touch(const std::string &uuid, int64_t now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(uuid);
    if (it != states_.end())
      it->second.updated_us = now_us;
  }

  void remove(const std::string &uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    states_.erase(uuid);
  }

  bool lookup(const std::string &uuid, SyncState &out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(uuid);
    if (it == states_.end())
      return false;
    out = it->second;
    return true;
  }

  // A source listens only while someone has asked it to. The request has to
  // cross sources because the main one is never touched by its own settings.
  void start_listening(const std::string &uuid, int64_t until_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    listen_until_[uuid] = until_us;
    traces_.erase(uuid);
  }

  void stop_listening(const std::string &uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    listen_until_.erase(uuid);
    traces_.erase(uuid);
  }

  int64_t listening_until(const std::string &uuid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = listen_until_.find(uuid);
    return it == listen_until_.end() ? 0 : it->second;
  }

  // A source can't play what hasn't reached it yet, so when it's behind, the
  // only way to line up is for the main source to wait. The sync button asks for
  // that here, and the main source picks it up on its own thread. The delay asked
  // for is a total, so two followers in one sync don't add up.
  void request_main_delay(const std::string &uuid, int delay_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    int &wanted = hold_requests_[uuid];
    wanted = std::max(wanted, delay_ms);
  }

  int take_main_delay_request(const std::string &uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = hold_requests_.find(uuid);
    if (it == hold_requests_.end())
      return 0;
    const int delay_ms = it->second;
    hold_requests_.erase(it);
    return delay_ms;
  }

  void set_progress(const std::string &uuid, const SyncProgress &progress) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_[uuid] = progress;
  }

  SyncProgress progress(const std::string &uuid, int64_t now_us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = progress_.find(uuid);
    if (it == progress_.end() || now_us > it->second.expires_us)
      return SyncProgress{};
    return it->second;
  }

  void set_trace(const std::string &uuid, const SoundTrace &trace) {
    std::lock_guard<std::mutex> lock(mutex_);
    traces_[uuid] = trace;
  }

  SoundTrace trace(const std::string &uuid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = traces_.find(uuid);
    return it == traces_.end() ? SoundTrace{} : it->second;
  }

private:
  mutable std::mutex mutex_;
  SyncConfig config_;
  uint64_t revision_ = 0;
  std::map<std::string, std::function<void()>> start_handlers_;
  std::map<std::string, SyncProgress> progress_;
  std::map<std::string, SyncState> states_;
  std::map<std::string, int64_t> listen_until_;
  std::map<std::string, SoundTrace> traces_;
  std::map<std::string, int> hold_requests_;
};

} // namespace lss
