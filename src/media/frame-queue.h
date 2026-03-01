// frame-queue.h - thread-safe queues for decoded video/audio frames
#pragma once

#include "core/common.h"
#include <vector>

namespace lss {

struct DecodedVideoFrame {
  uint8_t *data[4] = {};
  int linesize[4] = {};
  int width = 0;
  int height = 0;
  int64_t pts_us = 0; // presentation timestamp in us
  bool keyframe = false;

  void free_buffers();
};

struct DecodedAudioFrame {
  uint8_t *data = nullptr;
  uint32_t frames = 0; // number of audio frames (samples per channel)
  int channels = 0;
  int sample_rate = 0;
  int64_t pts_us = 0;

  void free_buffers();
};

template <typename Frame> class FrameQueue {
public:
  explicit FrameQueue(int capacity = FRAME_QUEUE_CAPACITY);
  ~FrameQueue();

  // Pushes a frame, drops the oldest if full. Returns true if one was dropped.
  bool push(Frame &&frame);
  bool pop(Frame &out);
  bool peek(Frame &out) const;
  void flush();
  int size() const;
  bool empty() const;

private:
  mutable std::mutex mutex_;
  std::vector<Frame> ring_;
  int capacity_;
  int head_ = 0; // read position
  int tail_ = 0; // write position
  int count_ = 0;
};

using VideoFrameQueue = FrameQueue<DecodedVideoFrame>;
using AudioFrameQueue = FrameQueue<DecodedAudioFrame>;

} // namespace lss
