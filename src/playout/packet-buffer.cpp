// packet-buffer.cpp
#include "playout/packet-buffer.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace lss {

PacketBuffer::PacketBuffer(int64_t max_span_us, size_t max_bytes)
    : max_span_us_(max_span_us), max_bytes_(max_bytes) {}

PacketBuffer::~PacketBuffer() { clear(); }

void PacketBuffer::push(PacketKind kind, AVPacket *pkt, const PacketInfo &info) {
  PacketInfo stored = info;
  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (kind == PacketKind::Video) {
      if (last_video_dts_us_ != INT64_MIN &&
          info.dts_us - last_video_dts_us_ > GAP_THRESHOLD_US)
        video_jump_us_ = info.dts_us;
      last_video_dts_us_ = info.dts_us;
    } else {
      if (last_audio_pts_us_ != INT64_MIN &&
          info.pts_us - last_audio_pts_us_ > GAP_THRESHOLD_US)
        audio_jump_us_ = info.pts_us;
      last_audio_pts_us_ = info.pts_us;
    }

    queue(kind).push_back({pkt, stored});
    bytes_ += static_cast<size_t>(pkt->size);
    newest_us_ = std::max(newest_us_, info.dts_us);
    confirm_gap_if_both_jumped();
    enforce_limits();
  }
  cv_.notify_one();
}

bool PacketBuffer::peek(PacketKind kind, PacketInfo &out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto &q = queue(kind);
  if (q.empty())
    return false;
  out = q.front().info;
  return true;
}

AVPacket *PacketBuffer::pop(PacketKind kind, PacketInfo *info) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto &q = queue(kind);
  if (q.empty())
    return nullptr;

  Entry entry = q.front();
  q.pop_front();
  bytes_ -= static_cast<size_t>(entry.pkt->size);
  if (info)
    *info = entry.info;
  if (kind == PacketKind::Video && entry.info.dts_us >= gap_us_)
    has_gap_ = false;
  return entry.pkt;
}

PacketBufferStats PacketBuffer::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);

  PacketBufferStats s;
  s.has_video = !video_.empty();
  s.has_audio = !audio_.empty();
  s.has_gap = has_gap_;
  s.gap_us = gap_us_;
  s.bytes = bytes_;
  s.newest_us = newest_us_;

  if (s.has_video && s.has_audio)
    s.head_us = std::min(video_.front().info.dts_us, audio_.front().info.pts_us);
  else if (s.has_video)
    s.head_us = video_.front().info.dts_us;
  else if (s.has_audio)
    s.head_us = audio_.front().info.pts_us;

  return s;
}

bool PacketBuffer::find_cut_point(int64_t target_us, CutPoint &out) const {
  std::lock_guard<std::mutex> lock(mutex_);

  const int64_t floor_us = has_gap_ ? gap_us_ : INT64_MIN;
  bool found = false;

  for (const Entry &e : video_) {
    if (!e.info.keyframe || e.info.dts_us < floor_us)
      continue;
    if (e.info.dts_us > target_us) {
      if (!found) {
        out = {e.info.dts_us, e.info.pts_us};
        found = true;
      }
      break;
    }
    out = {e.info.dts_us, e.info.pts_us};
    found = true;
  }

  return found;
}

void PacketBuffer::drop_video_before(int64_t dts_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  while (!video_.empty() && video_.front().info.dts_us < dts_us)
    drop_front(video_);
  if (dts_us >= gap_us_)
    has_gap_ = false;
}

void PacketBuffer::drop_audio_before(int64_t pts_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  while (!audio_.empty() && audio_.front().info.pts_us < pts_us)
    drop_front(audio_);
}

void PacketBuffer::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  while (!video_.empty())
    drop_front(video_);
  while (!audio_.empty())
    drop_front(audio_);
  last_video_dts_us_ = INT64_MIN;
  last_audio_pts_us_ = INT64_MIN;
  newest_us_ = INT64_MIN;
  video_jump_us_ = INT64_MIN;
  audio_jump_us_ = INT64_MIN;
  has_gap_ = false;
  gap_us_ = 0;
}

// The two streams land in either order, so this runs after every push.
void PacketBuffer::confirm_gap_if_both_jumped() {
  if (video_jump_us_ == INT64_MIN || audio_jump_us_ == INT64_MIN)
    return;
  if (std::llabs(video_jump_us_ - audio_jump_us_) > GAP_MATCH_US)
    return;

  gap_us_ = video_jump_us_;
  has_gap_ = true;
  for (auto it = video_.rbegin(); it != video_.rend(); ++it) {
    if (it->info.dts_us == gap_us_) {
      it->info.after_gap = true;
      break;
    }
  }
  video_jump_us_ = INT64_MIN;
  audio_jump_us_ = INT64_MIN;
}

void PacketBuffer::wait_for_data(int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms));
}

void PacketBuffer::drop_front(std::deque<Entry> &q) {
  Entry entry = q.front();
  q.pop_front();
  bytes_ -= static_cast<size_t>(entry.pkt->size);
  av_packet_free(&entry.pkt);
}

int64_t PacketBuffer::span_us() const {
  if (newest_us_ == INT64_MIN)
    return 0;
  if (!video_.empty())
    return newest_us_ - video_.front().info.dts_us;
  if (!audio_.empty())
    return newest_us_ - audio_.front().info.pts_us;
  return 0;
}

// Memory safety net. The recovery policy cuts long before this in normal use;
// reaching it means the decode side has stopped consuming.
void PacketBuffer::enforce_limits() {
  bool dropped_video = false;
  while ((span_us() > max_span_us_ || bytes_ > max_bytes_) && !video_.empty()) {
    dropped_video = true;
    drop_front(video_);
    // Keep going to the next keyframe, a partial GOP can't be decoded.
    while (!video_.empty() && !video_.front().info.keyframe)
      drop_front(video_);
  }
  if (dropped_video && !video_.empty()) {
    const int64_t video_head = video_.front().info.dts_us;
    while (!audio_.empty() && audio_.front().info.pts_us < video_head - 1000000)
      drop_front(audio_);
  }
  while (video_.empty() && !audio_.empty() &&
         (span_us() > max_span_us_ || bytes_ > max_bytes_))
    drop_front(audio_);
}

} // namespace lss
