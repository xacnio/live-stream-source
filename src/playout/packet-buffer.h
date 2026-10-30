// packet-buffer.h - duration-bounded jitter buffer between demuxer and decoders
//
// The reader thread drains the socket into this buffer without ever pausing,
// so a backlog is always visible here as a measured duration instead of
// sitting unseen in the kernel. Audio and video queue separately because muxers
// can interleave them hundreds of milliseconds apart.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

extern "C" {
#include <libavcodec/packet.h>
}

namespace lss {

enum class PacketKind { Video, Audio };

struct PacketInfo {
  int64_t pts_us = 0;
  int64_t dts_us = 0;
  bool keyframe = false;
  // First video packet after content went missing: nothing decoded before it
  // can serve as a reference for what follows.
  bool after_gap = false;
};

struct PacketBufferStats {
  bool has_video = false;
  bool has_audio = false;
  bool has_gap = false;
  int64_t head_us = 0;   // oldest media time still queued
  int64_t newest_us = 0; // newest media time queued
  int64_t gap_us = 0;    // first media time after the most recent gap
  size_t bytes = 0;

  bool has_data() const { return has_video || has_audio; }
  // Content that plays back continuously: nothing before a gap counts.
  int64_t playable_us() const {
    return newest_us - (has_gap && gap_us > head_us ? gap_us : head_us);
  }
};

struct CutPoint {
  int64_t key_dts_us = 0;
  int64_t key_pts_us = 0;
};

class PacketBuffer {
public:
  PacketBuffer(int64_t max_span_us, size_t max_bytes);
  ~PacketBuffer();

  PacketBuffer(const PacketBuffer &) = delete;
  PacketBuffer &operator=(const PacketBuffer &) = delete;

  // Takes ownership of pkt.
  void push(PacketKind kind, AVPacket *pkt, const PacketInfo &info);

  bool peek(PacketKind kind, PacketInfo &out) const;
  // Caller owns the returned packet. nullptr when the queue is empty.
  AVPacket *pop(PacketKind kind, PacketInfo *info = nullptr);

  PacketBufferStats stats() const;

  // Latest keyframe at or before target_us that is decodable, meaning no gap
  // lies between it and the target. Falls back to the first keyframe past the
  // target. False if the buffer holds no usable keyframe.
  bool find_cut_point(int64_t target_us, CutPoint &out) const;

  void drop_video_before(int64_t dts_us);
  void drop_audio_before(int64_t pts_us);
  void clear();

  void wait_for_data(int timeout_ms);

private:
  struct Entry {
    AVPacket *pkt;
    PacketInfo info;
  };

  std::deque<Entry> &queue(PacketKind kind) {
    return kind == PacketKind::Video ? video_ : audio_;
  }
  const std::deque<Entry> &queue(PacketKind kind) const {
    return kind == PacketKind::Video ? video_ : audio_;
  }

  void drop_front(std::deque<Entry> &q);
  void enforce_limits();
  int64_t span_us() const;
  void confirm_gap_if_both_jumped();

  static constexpr int64_t GAP_THRESHOLD_US = 500000;
  static constexpr int64_t GAP_MATCH_US = 1000000;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Entry> video_;
  std::deque<Entry> audio_;
  int64_t max_span_us_;
  size_t max_bytes_;
  size_t bytes_ = 0;
  int64_t last_video_dts_us_ = INT64_MIN;
  int64_t last_audio_pts_us_ = INT64_MIN;
  int64_t newest_us_ = INT64_MIN;
  // A timestamp jump in one stream proves nothing: video frames are sparse on
  // a static scene, and audio stops on some encoders during silence. Content
  // is only missing when both streams jumped at the same point.
  int64_t video_jump_us_ = INT64_MIN;
  int64_t audio_jump_us_ = INT64_MIN;
  int64_t gap_us_ = 0;
  bool has_gap_ = false;
};

} // namespace lss
