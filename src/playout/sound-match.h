// sound-match.h - measures the delay between two streams that heard the same sounds
//
// Runs only while the user has asked for a sync. Picking out claps in each stream
// separately doesn't hold up: a phone's noise suppression and AGC and the audio
// codec reshape them differently, and one cough next to them spoils the set.
// Instead each stream records when any sound started, and the two records are
// slid past each other to find the delay where they line up. Claps are simply the
// sharpest thing to line up on, and talking over them helps rather than hurts.
#pragma once

#include <cstdint>
#include <vector>

namespace lss {

// How strongly a sound started in each 5 ms step. Mostly zeros.
struct SoundTrace {
  int64_t start_us = 0;
  std::vector<float> onsets;
};

class SoundTracer {
public:
  void reset();

  // left: mono or left channel, right: may be null. timestamp_us is the time of
  // the first sample.
  void feed(const float *left, const float *right, uint32_t frames, int rate,
            int64_t timestamp_us);

  const SoundTrace &trace() const { return trace_; }

private:
  void finish_hop();

  static constexpr int HOP_MS = 5;
  static constexpr double BASELINE_SMOOTHING = 0.1; // about 50 ms
  static constexpr double MIN_RISE_DB = 6.0;
  static constexpr double MIN_LEVEL_DB = -65.0;
  static constexpr double MAX_RISE_DB = 40.0;
  static constexpr double PRE_EMPHASIS = 0.95; // favours claps over rumble and wind
  static constexpr size_t MAX_HOPS = 12000;    // one minute

  SoundTrace trace_;
  double hop_sum_ = 0.0;
  int hop_count_ = 0;
  double previous_sample_ = 0.0;
  double baseline_db_ = 0.0;
  bool baseline_ready_ = false;
};

struct SoundMatch {
  int64_t delta_us = 0; // how much later the second stream played the sounds
  double score = 0.0;   // 0..1, how well the two line up at that delay
  double margin = 0.0;  // how far ahead of the next best delay
  int shared = 0;       // distinct sounds both streams heard at that delay
};

// Always fills `out` with the best delay found, for logging. Returns whether it's
// good enough to act on.
bool match_sounds(const SoundTrace &first, const SoundTrace &second,
                  int64_t max_delta_us, SoundMatch *out);

// Distinct sounds in a trace, for telling "no audio" from "no match".
int count_sounds(const SoundTrace &trace);

} // namespace lss
