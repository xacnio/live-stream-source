// ll-hls-client.h - custom LL-HLS client for Amazon IVS
#pragma once

#include "protocols/ll-hls/ll-hls-fetcher.h"
#include "protocols/ll-hls/ll-hls-parser.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lss {

struct PartRequest {
  std::string uri;
  int64_t byterange_offset = -1;
  int64_t byterange_length = -1;
  bool is_init = false; // true for init segment (moov atom)
};

class LLHlsClient {
public:
  LLHlsClient();
  ~LLHlsClient();

  LLHlsClient(const LLHlsClient &) = delete;
  LLHlsClient &operator=(const LLHlsClient &) = delete;

  bool start(const std::string &url);
  void stop();
  // Signals stop without joining threads. Call stop() to fully shut down.
  void signal_stop();
  int read(uint8_t *buf, int buf_size);
  static int avio_read_callback(void *opaque, uint8_t *buf, int buf_size);
  bool is_running() const;
  bool has_init_segment() const;

private:
  void playlist_worker();
  void download_worker();
  bool resolve_media_playlist(const std::string &master_url);
  void ring_write(const uint8_t *data, size_t len);
  size_t ring_read(uint8_t *data, size_t max_len);
  size_t ring_available() const;
  void enqueue_part(PartRequest req);
  bool dequeue_part(PartRequest &out);

  LLHlsFetcher fetcher_;
  LLHlsPlaylist playlist_;

  std::string base_url_;
  std::string playlist_url_;

  std::mutex buf_mutex_;
  std::condition_variable buf_cv_;
  std::vector<uint8_t> ring_buffer_;
  size_t ring_read_pos_ = 0;
  size_t ring_write_pos_ = 0;
  size_t ring_data_size_ = 0;
  static constexpr size_t RING_CAPACITY = 16 * 1024 * 1024; // 16 MB

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<PartRequest> download_queue_;

  int64_t current_msn_ = -1;
  int current_part_idx_ = -1;
  std::atomic<bool> init_sent_{false};

  std::thread playlist_thread_;
  std::thread download_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> started_{false};
};

} // namespace lss
