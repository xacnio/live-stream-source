// ll-hls-fetcher.h - HTTP fetcher for LL-HLS
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct AVIOContext;
struct AVDictionary;

namespace lss {

class LLHlsFetcher {
public:
  LLHlsFetcher();
  ~LLHlsFetcher();

  LLHlsFetcher(const LLHlsFetcher &) = delete;
  LLHlsFetcher &operator=(const LLHlsFetcher &) = delete;

  bool fetch_playlist(const std::string &url, std::string &out_text,
                      int64_t msn = -1, int part = -1);
  bool fetch_data(const std::string &url, std::vector<uint8_t> &out_buffer,
                  int64_t byterange_offset = -1, int64_t byterange_length = -1);
  void set_user_agent(const std::string &ua);
  void set_timeout_us(int64_t timeout_us);
  void abort();
  bool is_aborted() const;
  void reset_abort();

private:
  static std::string build_blocking_url(const std::string &url, int64_t msn,
                                        int part);
  AVDictionary *build_http_options(int64_t byterange_offset = -1,
                                   int64_t byterange_length = -1) const;
  static int interrupt_cb(void *opaque);

  std::atomic<bool> abort_{false};
  std::string user_agent_;
  int64_t timeout_us_ = 10000000; // 10s default
};

} // namespace lss
