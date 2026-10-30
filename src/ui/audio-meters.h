// audio-meters.h
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>

struct obs_volmeter;

namespace lss {

// Live levels of the sources in the mixer, for the web panel. The same meter the
// OBS mixer uses, so both show the same thing. Runs only while someone is
// watching.
class AudioMeters {
public:
  static AudioMeters &instance();

  // The panel repeats this while its audio screen is open. A phone that drops off
  // the network can't say goodbye, so the meters stop by themselves.
  void keep_alive();
  void end();

  // Graphics thread.
  void tick(float seconds);
  void stop();

private:
  AudioMeters() = default;

  struct Meter;
  void sync_meters();
  void publish();

  std::atomic<int64_t> lease_until_ms_{0};
  std::map<std::string, std::unique_ptr<Meter>> meters_; // graphics thread
  float elapsed_ = 0.0f;
  int rounds_ = 0;
};

} // namespace lss
