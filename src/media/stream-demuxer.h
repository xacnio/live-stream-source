// stream-demuxer.h - avformat wrapper for live stream demuxing
#pragma once

#include "core/common.h"
#include "protocols/ll-hls/ll-hls-client.h"

namespace lss {

class StreamDemuxer {
public:
  StreamDemuxer() = default;
  ~StreamDemuxer();

  StreamDemuxer(const StreamDemuxer &) = delete;
  StreamDemuxer &operator=(const StreamDemuxer &) = delete;

  int open(const std::string &url, StreamType type = StreamType::Standard);
  void close();
  bool is_open() const { return fmt_ctx_ != nullptr; }

  void request_abort();
  void reset_abort();
  bool is_aborted() const { return abort_.load(); }

  int read_packet(AVPacket *pkt);

  int video_stream_index() const { return video_idx_; }
  int audio_stream_index() const { return audio_idx_; }

  AVCodecParameters *video_codecpar() const;
  AVCodecParameters *audio_codecpar() const;

  AVRational video_time_base() const;
  AVRational audio_time_base() const;

  int video_width() const;
  int video_height() const;

private:
  int open_ivs(const std::string &url);

  static int interrupt_callback(void *opaque);

  AVFormatContext *fmt_ctx_ = nullptr;
  int video_idx_ = -1;
  int audio_idx_ = -1;
  std::atomic<bool> abort_{false};

  std::unique_ptr<LLHlsClient> llhls_client_;
  AVIOContext *custom_avio_ctx_ = nullptr;
};

} // namespace lss
