// ll-hls-fetcher.cpp
#include "protocols/ll-hls/ll-hls-fetcher.h"
#include "core/common.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <sstream>

namespace lss {

static constexpr int READ_CHUNK_SIZE = 32768; // 32 KB

LLHlsFetcher::LLHlsFetcher() {
  user_agent_ = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/121.0.0.0 Safari/537.36";
}

LLHlsFetcher::~LLHlsFetcher() { abort(); }


void LLHlsFetcher::set_user_agent(const std::string &ua) { user_agent_ = ua; }

void LLHlsFetcher::set_timeout_us(int64_t timeout_us) {
  timeout_us_ = timeout_us;
}

void LLHlsFetcher::abort() { abort_.store(true); }

bool LLHlsFetcher::is_aborted() const { return abort_.load(); }

void LLHlsFetcher::reset_abort() { abort_.store(false); }


int LLHlsFetcher::interrupt_cb(void *opaque) {
  auto *self = static_cast<LLHlsFetcher *>(opaque);
  return self->abort_.load() ? 1 : 0;
}


std::string LLHlsFetcher::build_blocking_url(const std::string &url,
                                             int64_t msn, int part) {
  if (msn < 0 || part < 0)
    return url;

  std::string result = url;
  char sep = (result.find('?') == std::string::npos) ? '?' : '&';
  result += sep;
  result += "_HLS_msn=" + std::to_string(msn);
  result += "&_HLS_part=" + std::to_string(part);
  return result;
}


AVDictionary *LLHlsFetcher::build_http_options(int64_t byterange_offset,
                                               int64_t byterange_length) const {
  AVDictionary *opts = nullptr;

  if (!user_agent_.empty())
    av_dict_set(&opts, "user_agent", user_agent_.c_str(), 0);

  av_dict_set(&opts, "timeout", std::to_string(timeout_us_).c_str(), 0);

  av_dict_set(&opts, "tcp_nodelay", "1", 0);

  av_dict_set(&opts, "http_persistent", "1", 0);

  av_dict_set(&opts, "reconnect", "1", 0);
  av_dict_set(&opts, "reconnect_streamed", "1", 0);
  av_dict_set(&opts, "reconnect_delay_max", "2", 0);

  if (byterange_offset >= 0 && byterange_length > 0) {
    std::string range = "bytes=" + std::to_string(byterange_offset) + "-" +
                        std::to_string(byterange_offset + byterange_length - 1);
    av_dict_set(&opts, "headers", ("Range: " + range + "\r\n").c_str(), 0);
  }

  return opts;
}


bool LLHlsFetcher::fetch_playlist(const std::string &url, std::string &out_text,
                                  int64_t msn, int part) {
  if (abort_.load())
    return false;

  std::string fetch_url = build_blocking_url(url, msn, part);
  out_text.clear();

  AVDictionary *opts = build_http_options();
  AVIOContext *avio_ctx = nullptr;

  AVIOInterruptCB int_cb;
  int_cb.callback = interrupt_cb;
  int_cb.opaque = this;

  int ret =
      avio_open2(&avio_ctx, fetch_url.c_str(), AVIO_FLAG_READ, &int_cb, &opts);
  av_dict_free(&opts);

  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    lss_log_error("[LL-HLS Fetcher] Failed to open playlist URL: %s (err=%s)",
                  fetch_url.c_str(), errbuf);
    return false;
  }

  std::vector<uint8_t> buf(READ_CHUNK_SIZE);
  std::ostringstream ss;

  while (!abort_.load()) {
    int bytes_read =
        avio_read(avio_ctx, buf.data(), static_cast<int>(buf.size()));
    if (bytes_read <= 0)
      break;
    ss.write(reinterpret_cast<const char *>(buf.data()), bytes_read);
  }

  avio_closep(&avio_ctx);

  if (abort_.load())
    return false;

  out_text = ss.str();
  return !out_text.empty();
}


bool LLHlsFetcher::fetch_data(const std::string &url,
                              std::vector<uint8_t> &out_buffer,
                              int64_t byterange_offset,
                              int64_t byterange_length) {
  if (abort_.load())
    return false;

  out_buffer.clear();

  AVDictionary *opts = build_http_options(byterange_offset, byterange_length);
  AVIOContext *avio_ctx = nullptr;

  AVIOInterruptCB int_cb;
  int_cb.callback = interrupt_cb;
  int_cb.opaque = this;

  int ret = avio_open2(&avio_ctx, url.c_str(), AVIO_FLAG_READ, &int_cb, &opts);
  av_dict_free(&opts);

  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    lss_log_error("[LL-HLS Fetcher] Failed to open data URL: %s (err=%s)",
                  url.c_str(), errbuf);
    return false;
  }

  std::vector<uint8_t> chunk(READ_CHUNK_SIZE);

  while (!abort_.load()) {
    int bytes_read =
        avio_read(avio_ctx, chunk.data(), static_cast<int>(chunk.size()));
    if (bytes_read <= 0)
      break;
    out_buffer.insert(out_buffer.end(), chunk.data(),
                      chunk.data() + bytes_read);
  }

  avio_closep(&avio_ctx);

  if (abort_.load())
    return false;

  return !out_buffer.empty();
}

} // namespace lss
