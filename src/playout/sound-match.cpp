// sound-match.cpp
#include "playout/sound-match.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace lss {

static constexpr int64_t HOP_US = 5000;
static constexpr double MIN_SCORE = 0.3;
static constexpr double MIN_MARGIN = 1.3;
static constexpr int MIN_SHARED_SOUNDS = 3;
static constexpr int RIVAL_EXCLUSION_HOPS = 10;  // 50 ms either side of the best
static constexpr int SAME_SOUND_HOPS = 4;        // 20 ms between microphones
static constexpr int MIN_SOUND_SPACING_HOPS = 20; // 100 ms
static constexpr double NOTABLE_SOUND_RATIO = 0.3;

void SoundTracer::reset() {
  trace_ = SoundTrace{};
  hop_sum_ = 0.0;
  hop_count_ = 0;
  previous_sample_ = 0.0;
  baseline_db_ = 0.0;
  baseline_ready_ = false;
}

void SoundTracer::feed(const float *left, const float *right, uint32_t frames,
                       int rate, int64_t timestamp_us) {
  if (!left || rate <= 0)
    return;

  // Steps sit on a shared grid of absolute time, so two traces line up exactly.
  const int64_t first_hop = timestamp_us / HOP_US;
  if (trace_.onsets.empty() && hop_count_ == 0) {
    trace_.start_us = first_hop * HOP_US;
  } else {
    const int64_t expected = trace_.start_us / HOP_US + static_cast<int64_t>(trace_.onsets.size());
    if (first_hop > expected + 1) {
      // Audio stopped for a while: keep the time axis honest.
      const size_t gap = static_cast<size_t>(
          std::min<int64_t>(first_hop - expected, MAX_HOPS));
      if (trace_.onsets.size() + gap <= MAX_HOPS)
        trace_.onsets.resize(trace_.onsets.size() + gap, 0.0f);
      hop_sum_ = 0.0;
      hop_count_ = 0;
      baseline_ready_ = false;
    }
  }

  const int hop_samples = rate * HOP_MS / 1000;
  for (uint32_t i = 0; i < frames; ++i) {
    const double sample = right ? (left[i] + right[i]) * 0.5 : left[i];
    const double emphasised = sample - PRE_EMPHASIS * previous_sample_;
    previous_sample_ = sample;
    hop_sum_ += emphasised * emphasised;
    if (++hop_count_ >= hop_samples)
      finish_hop();
  }
}

void SoundTracer::finish_hop() {
  const double level_db = 10.0 * std::log10(hop_sum_ / hop_count_ + 1e-10);
  hop_sum_ = 0.0;
  hop_count_ = 0;

  if (!baseline_ready_) {
    baseline_db_ = level_db;
    baseline_ready_ = true;
  }

  const double rise = level_db - baseline_db_;
  float onset = 0.0f;
  if (rise > MIN_RISE_DB && level_db > MIN_LEVEL_DB)
    onset = static_cast<float>(std::min(rise, MAX_RISE_DB) - MIN_RISE_DB);
  baseline_db_ += BASELINE_SMOOTHING * (level_db - baseline_db_);

  if (trace_.onsets.size() < MAX_HOPS)
    trace_.onsets.push_back(onset);
}

// Strongest step of each separate sound.
static std::vector<int> find_sounds(const std::vector<float> &onsets) {
  float strongest = 0.0f;
  for (float value : onsets)
    strongest = std::max(strongest, value);

  std::vector<int> sounds;
  const float notable = strongest * static_cast<float>(NOTABLE_SOUND_RATIO);
  for (int i = 0; i < static_cast<int>(onsets.size()); ++i) {
    if (onsets[i] <= 0.0f || onsets[i] < notable)
      continue;
    if (!sounds.empty() && i - sounds.back() < MIN_SOUND_SPACING_HOPS) {
      if (onsets[i] > onsets[sounds.back()])
        sounds.back() = i;
      continue;
    }
    sounds.push_back(i);
  }
  return sounds;
}

int count_sounds(const SoundTrace &trace) {
  return static_cast<int>(find_sounds(trace.onsets).size());
}

bool match_sounds(const SoundTrace &first, const SoundTrace &second,
                  int64_t max_delta_us, SoundMatch *out) {
  *out = SoundMatch{};
  if (first.onsets.empty() || second.onsets.empty())
    return false;

  // The same sound lands a step apart on two microphones: let neighbours count.
  const int second_size = static_cast<int>(second.onsets.size());
  std::vector<float> widened(second.onsets);
  for (int i = 0; i < second_size; ++i) {
    const float before = i > 0 ? second.onsets[i - 1] : 0.0f;
    const float after = i + 1 < second_size ? second.onsets[i + 1] : 0.0f;
    widened[i] += 0.5f * (before + after);
  }

  std::vector<int> active;
  double first_energy = 0.0;
  for (int i = 0; i < static_cast<int>(first.onsets.size()); ++i) {
    if (first.onsets[i] > 0.0f) {
      active.push_back(i);
      first_energy += static_cast<double>(first.onsets[i]) * first.onsets[i];
    }
  }
  double second_energy = 0.0;
  for (float value : widened)
    second_energy += static_cast<double>(value) * value;
  if (active.empty() || second_energy <= 0.0)
    return false;

  // A step in `first` at index i is at index i + base + lag in `second`.
  const int base = static_cast<int>((first.start_us - second.start_us) / HOP_US);
  const int max_lag = static_cast<int>(max_delta_us / HOP_US);
  std::vector<double> scores(max_lag * 2 + 1, 0.0);
  for (int lag = -max_lag; lag <= max_lag; ++lag) {
    double sum = 0.0;
    for (int i : active) {
      const int j = i + base + lag;
      if (j >= 0 && j < second_size)
        sum += first.onsets[i] * widened[j];
    }
    scores[lag + max_lag] = sum;
  }

  const double norm = std::sqrt(first_energy * second_energy);
  int best = 0;
  for (int i = 1; i < static_cast<int>(scores.size()); ++i) {
    if (scores[i] > scores[best])
      best = i;
  }
  if (scores[best] <= 0.0)
    return false;

  double rival = 0.0;
  for (int i = 0; i < static_cast<int>(scores.size()); ++i) {
    if (std::abs(i - best) > RIVAL_EXCLUSION_HOPS)
      rival = std::max(rival, scores[i]);
  }

  // Between steps, from the shape of the peak.
  double fraction = 0.0;
  if (best > 0 && best + 1 < static_cast<int>(scores.size())) {
    const double left = scores[best - 1];
    const double right = scores[best + 1];
    const double curve = left - 2.0 * scores[best] + right;
    if (curve < 0.0)
      fraction = std::clamp(0.5 * (left - right) / curve, -0.5, 0.5);
  }

  // The second stream may have heard some of them much more faintly, so anything
  // at the right moment counts there.
  const int best_lag = best - max_lag;
  for (int i : find_sounds(first.onsets)) {
    const int expected = i + base + best_lag;
    const int from = std::max(expected - SAME_SOUND_HOPS, 0);
    const int to = std::min(expected + SAME_SOUND_HOPS, second_size - 1);
    for (int j = from; j <= to; ++j) {
      if (second.onsets[j] > 0.0f) {
        ++out->shared;
        break;
      }
    }
  }

  out->delta_us = static_cast<int64_t>((best_lag + fraction) * HOP_US);
  out->score = scores[best] / norm;
  out->margin = rival > 0.0 ? scores[best] / rival : 100.0;

  return out->score >= MIN_SCORE && out->margin >= MIN_MARGIN &&
         out->shared >= MIN_SHARED_SOUNDS;
}

} // namespace lss
