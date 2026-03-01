// frame-queue.cpp
#include "media/frame-queue.h"

namespace lss {

void DecodedVideoFrame::free_buffers() {
  if (data[0]) {
    av_freep(&data[0]);
  }
  memset(data, 0, sizeof(data));
  memset(linesize, 0, sizeof(linesize));
  width = 0;
  height = 0;
  pts_us = 0;
  keyframe = false;
}

void DecodedAudioFrame::free_buffers() {
  if (data) {
    av_free(data);
    data = nullptr;
  }
  frames = 0;
  channels = 0;
  sample_rate = 0;
  pts_us = 0;
}


template <typename Frame>
FrameQueue<Frame>::FrameQueue(int capacity) : capacity_(capacity) {
  ring_.resize(static_cast<size_t>(capacity));
}

template <typename Frame> FrameQueue<Frame>::~FrameQueue() { flush(); }

template <typename Frame> bool FrameQueue<Frame>::push(Frame &&frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  bool dropped = false;

  if (count_ == capacity_) {
    // Drop the oldest frame to make room
    ring_[static_cast<size_t>(head_)].free_buffers();
    head_ = (head_ + 1) % capacity_;
    --count_;
    dropped = true;
  }

  ring_[static_cast<size_t>(tail_)] = std::move(frame);
  tail_ = (tail_ + 1) % capacity_;
  ++count_;

  return dropped;
}

template <typename Frame> bool FrameQueue<Frame>::pop(Frame &out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (count_ == 0)
    return false;

  out = std::move(ring_[static_cast<size_t>(head_)]);
  head_ = (head_ + 1) % capacity_;
  --count_;
  return true;
}

template <typename Frame> bool FrameQueue<Frame>::peek(Frame &out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (count_ == 0)
    return false;

  out = ring_[static_cast<size_t>(head_)];
  return true;
}

template <typename Frame> void FrameQueue<Frame>::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < count_; ++i) {
    int idx = (head_ + i) % capacity_;
    ring_[static_cast<size_t>(idx)].free_buffers();
  }
  head_ = 0;
  tail_ = 0;
  count_ = 0;
}

template <typename Frame> int FrameQueue<Frame>::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_;
}

template <typename Frame> bool FrameQueue<Frame>::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_ == 0;
}

template class FrameQueue<DecodedVideoFrame>;
template class FrameQueue<DecodedAudioFrame>;

} // namespace lss
