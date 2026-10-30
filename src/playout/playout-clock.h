// playout-clock.h - media time <-> OBS time mapping shared by audio and video
//
// Audio is the master. Its output is a gapless sample stream, so every emitted
// chunk pins a (media, wall) pair and the video presenter reads the same
// mapping. A/V sync therefore can't drift: both sides derive their timing from
// one piecewise-linear function instead of each keeping its own offset.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>

namespace lss {

class PlayoutClock {
public:
  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    segments_.clear();
  }

  bool valid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !segments_.empty();
  }

  // Hard anchor: drops history. Used after a stall, a cut or a stream start.
  void anchor(int64_t media_us, int64_t wall_us, double rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    segments_.clear();
    segments_.push_back({media_us, wall_us, rate});
  }

  // Audio write-head report. The new rate takes effect exactly at media_us,
  // which is still in the future for the video presenter, so a tempo change
  // reaches both streams at the same media position.
  void observe(int64_t media_us, int64_t wall_us, double rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (segments_.empty()) {
      segments_.push_back({media_us, wall_us, rate});
      return;
    }
    const Segment &last = segments_.back();
    if (media_us <= last.media_us)
      return;
    const int64_t error = wall_of_locked(media_us) - wall_us;
    if (rate == last.rate && error < MAX_ERROR_US && error > -MAX_ERROR_US)
      return;
    segments_.push_back({media_us, wall_us, rate});
    while (segments_.size() > 2 && segments_[1].wall_us < wall_us - KEEP_US)
      segments_.pop_front();
  }

  // Rate change with no audio write-head to pin it to (video-only streams).
  void set_rate(int64_t wall_us, double rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (segments_.empty())
      return;
    const int64_t media_us = media_at_locked(wall_us);
    segments_.push_back({media_us, wall_us, rate});
    while (segments_.size() > 2 && segments_[1].wall_us < wall_us - KEEP_US)
      segments_.pop_front();
  }

  int64_t wall_of(int64_t media_us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wall_of_locked(media_us);
  }

  int64_t media_at(int64_t wall_us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return media_at_locked(wall_us);
  }

private:
  struct Segment {
    int64_t media_us;
    int64_t wall_us;
    double rate;
  };

  int64_t wall_of_locked(int64_t media_us) const {
    if (segments_.empty())
      return 0;
    size_t i = segments_.size() - 1;
    while (i > 0 && segments_[i].media_us > media_us)
      --i;
    const Segment &s = segments_[i];
    return s.wall_us +
           static_cast<int64_t>((media_us - s.media_us) / s.rate);
  }

  int64_t media_at_locked(int64_t wall_us) const {
    if (segments_.empty())
      return 0;
    size_t i = segments_.size() - 1;
    while (i > 0 && segments_[i].wall_us > wall_us)
      --i;
    const Segment &s = segments_[i];
    return s.media_us + static_cast<int64_t>((wall_us - s.wall_us) * s.rate);
  }

  // Well under one video frame, so a correction never shows.
  static constexpr int64_t MAX_ERROR_US = 6000;
  static constexpr int64_t KEEP_US = 2000000;

  mutable std::mutex mutex_;
  std::deque<Segment> segments_;
};

// Folds demuxer timestamps into one monotonic media timeline. Encoder restarts
// and 32/33-bit wraps jump the raw values; a jump that the arrival gap explains
// is a real hole in the content and stays on the timeline.
//
// Each stream is judged against its own history but they share the offset:
// muxers can hold audio and video seconds apart, and comparing across streams
// would read that skew as a jump on every packet.
class MediaTimeline {
public:
  void reset() {
    offset_us_ = 0;
    tracks_[0] = Track{};
    tracks_[1] = Track{};
  }

  // stream is 0 or 1. Returns the offset to add to this packet's timestamps.
  int64_t map(int stream, int64_t dts_us, int64_t arrival_us) {
    Track &track = tracks_[stream];
    if (!track.started) {
      track.started = true;
      track.high_water_us = dts_us + offset_us_;
      track.last_arrival_us = arrival_us;
      return offset_us_;
    }

    const int64_t arrival_gap = arrival_us - track.last_arrival_us;
    int64_t media = dts_us + offset_us_;
    const int64_t media_gap = media - track.high_water_us;

    if (media_gap < -MAX_BACKWARD_US || media_gap - arrival_gap > MAX_FORWARD_US) {
      offset_us_ += track.high_water_us + arrival_gap - media;
      media = dts_us + offset_us_;
    }

    if (media > track.high_water_us)
      track.high_water_us = media;
    track.last_arrival_us = arrival_us;
    return offset_us_;
  }

private:
  struct Track {
    bool started = false;
    int64_t high_water_us = 0;
    int64_t last_arrival_us = 0;
  };

  static constexpr int64_t MAX_BACKWARD_US = 2000000;
  static constexpr int64_t MAX_FORWARD_US = 5000000;

  int64_t offset_us_ = 0;
  Track tracks_[2];
};

} // namespace lss
