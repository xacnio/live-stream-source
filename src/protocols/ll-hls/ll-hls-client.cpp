// ll-hls-client.cpp
#include "protocols/ll-hls/ll-hls-client.h"
#include "core/common.h"

extern "C" {
#include <libavutil/error.h>
}

#include <algorithm>

namespace lss {

LLHlsClient::LLHlsClient() : ring_buffer_(RING_CAPACITY, 0) {}

LLHlsClient::~LLHlsClient() { stop(); }

int LLHlsClient::avio_read_callback(void *opaque, uint8_t *buf, int buf_size) {
  if (!opaque) {
    lss_log_error("[LL-HLS AVIO] opaque is NULL!");
    return AVERROR(EIO);
  }
  auto *self = static_cast<LLHlsClient *>(opaque);
  int ret = self->read(buf, buf_size);
  static int log_count = 0;
  if (log_count < 10 || ret < 0) {
    lss_log_debug(
        "[LL-HLS AVIO] read callback: requested=%d, returned=%d, running=%d",
        buf_size, ret, self->running_.load());
    log_count++;
  }
  return ret;
}

bool LLHlsClient::start(const std::string &url) {
  if (running_.load())
    stop();

  lss_log_debug("[LL-HLS Client] Starting with URL: %s", url.c_str());

  fetcher_.reset_abort();
  running_.store(true);
  started_.store(false);
  init_sent_.store(false);

  {
    std::lock_guard<std::mutex> lock(buf_mutex_);
    ring_read_pos_ = 0;
    ring_write_pos_ = 0;
    ring_data_size_ = 0;
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    download_queue_.clear();
  }

  playlist_url_ = url;
  auto last_slash = url.rfind('/');
  if (last_slash != std::string::npos) {
    base_url_ = url.substr(0, last_slash + 1);
  } else {
    base_url_ = url;
  }

  std::string playlist_text;
  if (!fetcher_.fetch_playlist(playlist_url_, playlist_text)) {
    lss_log_error("[LL-HLS Client] Failed to fetch initial playlist");
    running_.store(false);
    return false;
  }

  if (!playlist_.parse(playlist_text, base_url_)) {
    lss_log_error("[LL-HLS Client] Failed to parse initial playlist");
    running_.store(false);
    return false;
  }

  if (playlist_.is_master()) {
    lss_log_debug("[LL-HLS Client] Detected master playlist, resolving...");
    if (!resolve_media_playlist(url)) {
      lss_log_error("[LL-HLS Client] Failed to resolve media playlist");
      running_.store(false);
      return false;
    }
  }

  lss_log_debug("[LL-HLS Client] Playlist parsed: MSN=%lld, parts=%d, "
                "prefetch=%d, ll_hls=%s, init=%s",
                (long long)playlist_.latest_msn(),
                playlist_.latest_part_index(),
                (int)playlist_.prefetch_urls().size(),
                playlist_.is_ll_hls() ? "YES" : "NO",
                playlist_.init_segment_uri().empty()
                    ? "NONE"
                    : playlist_.init_segment_uri().c_str());

  if (!playlist_.is_ll_hls()) {
    lss_log_warn("[LL-HLS Client] Playlist does not appear to be LL-HLS "
                 "(no EXT-X-PART, EXT-X-PREFETCH, or PRELOAD-HINT found). "
                 "Falling back will be needed.");
    running_.store(false);
    return false;
  }

  playlist_thread_ = std::thread(&LLHlsClient::playlist_worker, this);
  download_thread_ = std::thread(&LLHlsClient::download_worker, this);

  return true;
}

void LLHlsClient::stop() {

  bool was_running = running_.exchange(false);

  if (was_running) {
    lss_log_debug("[LL-HLS Client] Stopping... (signaling fetcher)");
    fetcher_.abort();

    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      queue_cv_.notify_all();
    }
    {
      std::lock_guard<std::mutex> lock(buf_mutex_);
      buf_cv_.notify_all();
    }
  }

  if (playlist_thread_.joinable()) {
    lss_log_debug("[LL-HLS Client] Joining playlist thread...");
    playlist_thread_.join();
    lss_log_debug("[LL-HLS Client] Playlist thread joined.");
  }
  if (download_thread_.joinable()) {
    lss_log_debug("[LL-HLS Client] Joining download thread...");
    download_thread_.join();
    lss_log_debug("[LL-HLS Client] Download thread joined.");
  }

  if (was_running)
    lss_log_debug("[LL-HLS Client] Stopped completely.");
}

void LLHlsClient::signal_stop() {

  running_.store(false);
  fetcher_.abort();

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_cv_.notify_all();
  }
  {
    std::lock_guard<std::mutex> lock(buf_mutex_);
    buf_cv_.notify_all();
  }
}

bool LLHlsClient::is_running() const { return running_.load(); }

bool LLHlsClient::has_init_segment() const { return init_sent_.load(); }

int LLHlsClient::read(uint8_t *buf, int buf_size) {
  std::unique_lock<std::mutex> lock(buf_mutex_);

  buf_cv_.wait(lock,
               [this] { return ring_data_size_ > 0 || !running_.load(); });

  if (ring_data_size_ == 0) {
    if (!running_.load())
      return AVERROR_EOF;
    return AVERROR(EAGAIN);
  }

  size_t to_read = std::min(static_cast<size_t>(buf_size), ring_data_size_);
  size_t bytes_read = 0;

  while (bytes_read < to_read) {
    size_t chunk =
        std::min(to_read - bytes_read, RING_CAPACITY - ring_read_pos_);
    std::memcpy(buf + bytes_read, ring_buffer_.data() + ring_read_pos_, chunk);
    ring_read_pos_ = (ring_read_pos_ + chunk) % RING_CAPACITY;
    bytes_read += chunk;
  }

  ring_data_size_ -= bytes_read;
  return static_cast<int>(bytes_read);
}

void LLHlsClient::ring_write(const uint8_t *data, size_t len) {
  std::unique_lock<std::mutex> lock(buf_mutex_);

  // Must wait for space; dropping corrupts the TS byte stream.
  while (len + ring_data_size_ > RING_CAPACITY && running_.load()) {
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    lock.lock();
  }

  if (!running_.load())
    return;

  size_t written = 0;
  while (written < len) {
    size_t chunk = std::min(len - written, RING_CAPACITY - ring_write_pos_);
    std::memcpy(ring_buffer_.data() + ring_write_pos_, data + written, chunk);
    ring_write_pos_ = (ring_write_pos_ + chunk) % RING_CAPACITY;
    written += chunk;
  }

  ring_data_size_ += len;
  buf_cv_.notify_one();
}

size_t LLHlsClient::ring_available() const { return ring_data_size_; }

void LLHlsClient::enqueue_part(PartRequest req) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  download_queue_.push_back(std::move(req));
  queue_cv_.notify_one();
}

bool LLHlsClient::dequeue_part(PartRequest &out) {
  std::unique_lock<std::mutex> lock(queue_mutex_);

  queue_cv_.wait(
      lock, [this] { return !download_queue_.empty() || !running_.load(); });

  if (!running_.load() && download_queue_.empty())
    return false;

  if (download_queue_.empty())
    return false;

  out = std::move(download_queue_.front());
  download_queue_.pop_front();
  return true;
}

bool LLHlsClient::resolve_media_playlist(const std::string &master_url) {
  const auto &renditions = playlist_.renditions();
  if (renditions.empty()) {
    lss_log_error("[LL-HLS Client] Master playlist has no renditions");
    return false;
  }

  const HlsRendition *best = &renditions[0];
  for (const auto &r : renditions) {
    if (r.bandwidth > best->bandwidth)
      best = &r;
  }

  lss_log_debug("[LL-HLS Client] Selected rendition: %dx%d @ %d bps (%s)",
                best->width, best->height, best->bandwidth, best->name.c_str());

  playlist_url_ = best->uri;

  auto last_slash = playlist_url_.rfind('/');
  if (last_slash != std::string::npos) {
    base_url_ = playlist_url_.substr(0, last_slash + 1);
  }

  std::string playlist_text;
  if (!fetcher_.fetch_playlist(playlist_url_, playlist_text))
    return false;

  if (!playlist_.parse(playlist_text, base_url_))
    return false;

  return true;
}

void LLHlsClient::playlist_worker() {
  lss_log_debug("[LL-HLS Client] Playlist worker started");

  const std::string &init_uri = playlist_.init_segment_uri();
  if (!init_uri.empty()) {
    lss_log_debug("[LL-HLS Client] Downloading init segment directly: %s",
                  init_uri.c_str());
    std::vector<uint8_t> init_data;
    if (fetcher_.fetch_data(init_uri, init_data)) {
      ring_write(init_data.data(), init_data.size());
      init_sent_.store(true);
      lss_log_debug("[LL-HLS Client] Init segment written to ring buffer "
                    "(%zu bytes)  - FFmpeg can start probing now",
                    init_data.size());
    } else {
      lss_log_error("[LL-HLS Client] Failed to download init segment");
      running_.store(false);
      return;
    }
  }

  const bool use_prefetch = !playlist_.prefetch_urls().empty();
  std::string last_prefetch_uri;

  if (use_prefetch) {
    const auto &prefetches = playlist_.prefetch_urls();
    lss_log_debug("[LL-HLS Client] IVS mode: %d prefetch URLs",
                  (int)prefetches.size());

    for (const auto &pf : prefetches) {
      PartRequest req;
      req.uri = pf.uri;
      enqueue_part(std::move(req));
      lss_log_debug("[LL-HLS Client] Enqueued prefetch: %s", pf.uri.c_str());
    }

    if (!prefetches.empty()) {
      last_prefetch_uri = prefetches.back().uri;
    }

    if (!playlist_.segments().empty()) {
      current_msn_ = playlist_.segments().back().media_sequence;
    }
  } else {

    const auto &segments = playlist_.segments();
    bool found_start = false;

    if (!segments.empty()) {
      const auto &last_seg = segments.back();

      for (int i = static_cast<int>(last_seg.parts.size()) - 1; i >= 0; --i) {
        if (last_seg.parts[i].independent) {
          // Enqueue from this part onwards
          for (int j = i; j < static_cast<int>(last_seg.parts.size()); ++j) {
            PartRequest req;
            req.uri = last_seg.parts[j].uri;
            req.byterange_offset = last_seg.parts[j].byterange_offset;
            req.byterange_length = last_seg.parts[j].byterange_length;
            enqueue_part(std::move(req));
          }
          current_msn_ = last_seg.media_sequence;
          current_part_idx_ = static_cast<int>(last_seg.parts.size()) - 1;
          found_start = true;
          lss_log_debug("[LL-HLS Client] Starting at MSN=%lld, part=%d "
                        "(independent part at index %d)",
                        (long long)current_msn_, current_part_idx_, i);
          break;
        }
      }

      if (!found_start && !last_seg.parts.empty()) {
        PartRequest req;
        req.uri = last_seg.parts.back().uri;
        req.byterange_offset = last_seg.parts.back().byterange_offset;
        req.byterange_length = last_seg.parts.back().byterange_length;
        enqueue_part(std::move(req));
        current_msn_ = last_seg.media_sequence;
        current_part_idx_ = static_cast<int>(last_seg.parts.size()) - 1;
        found_start = true;
        lss_log_warn("[LL-HLS Client] No independent part found, starting "
                     "from latest part");
      }
    }

    // Also enqueue preload hint if available
    if (playlist_.preload_hint().valid &&
        playlist_.preload_hint().type == "PART") {
      PartRequest req;
      req.uri = playlist_.preload_hint().uri;
      req.byterange_offset = playlist_.preload_hint().byterange_offset;
      req.byterange_length = playlist_.preload_hint().byterange_length;
      enqueue_part(std::move(req));
      lss_log_debug("[LL-HLS Client] Enqueued preload hint: %s",
                    playlist_.preload_hint().uri.c_str());
    }
  }

  started_.store(true);

  int consecutive_errors = 0;
  constexpr int MAX_CONSECUTIVE_ERRORS = 10;

  while (running_.load()) {
    std::string reload_url;
    std::string new_playlist_text;

    if (use_prefetch) {
      constexpr int POLL_MS = 200;

      std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));

      if (!running_.load())
        break;

      reload_url = playlist_url_;

      if (!fetcher_.fetch_playlist(reload_url, new_playlist_text)) {
        if (!running_.load())
          break;
        consecutive_errors++;
        lss_log_warn("[LL-HLS Client] Playlist reload failed (%d/%d)",
                     consecutive_errors, MAX_CONSECUTIVE_ERRORS);
        if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
          lss_log_error("[LL-HLS Client] Too many consecutive errors, "
                        "stopping.");
          break;
        }
        continue;
      }
    } else {

      int64_t next_msn = playlist_.latest_msn();
      int next_part = playlist_.latest_part_index() + 1;

      if (next_msn < 0) {
        lss_log_error("[LL-HLS Client] No MSN available for blocking reload");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }

      reload_url = playlist_.blocking_reload_url(playlist_url_);

      lss_log_debug("[LL-HLS Client] Blocking reload: MSN=%lld, part=%d, "
                    "URL=%s",
                    (long long)next_msn, next_part, reload_url.c_str());

      if (!fetcher_.fetch_playlist(reload_url, new_playlist_text)) {
        if (!running_.load())
          break;
        consecutive_errors++;
        lss_log_warn("[LL-HLS Client] Blocking reload failed (%d/%d)",
                     consecutive_errors, MAX_CONSECUTIVE_ERRORS);
        if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
          lss_log_error("[LL-HLS Client] Too many consecutive errors, "
                        "stopping.");
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
    }

    consecutive_errors = 0;

    LLHlsPlaylist new_playlist;
    if (!new_playlist.parse(new_playlist_text, base_url_)) {
      lss_log_warn("[LL-HLS Client] Failed to parse updated playlist");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    if (use_prefetch) {
      const auto &new_prefetches = new_playlist.prefetch_urls();

      bool found_last = last_prefetch_uri.empty();
      for (const auto &pf : new_prefetches) {
        if (!found_last) {
          if (pf.uri == last_prefetch_uri) {
            found_last = true;
          }
          continue; // Skip until we pass the last known one
        }

        // This is a new prefetch URL
        PartRequest req;
        req.uri = pf.uri;
        enqueue_part(std::move(req));
        lss_log_debug("[LL-HLS Client] Enqueued new prefetch: %s",
                      pf.uri.c_str());
      }

      if (!found_last && !new_prefetches.empty()) {
        lss_log_warn("[LL-HLS Client] Previous prefetch URI not found in "
                     "new playlist, enqueuing all %d prefetch URLs",
                     (int)new_prefetches.size());
        for (const auto &pf : new_prefetches) {
          PartRequest req;
          req.uri = pf.uri;
          enqueue_part(std::move(req));
        }
      }

      if (!new_prefetches.empty()) {
        last_prefetch_uri = new_prefetches.back().uri;
      }

      if (!new_playlist.segments().empty()) {
        current_msn_ = new_playlist.segments().back().media_sequence;
      }
    } else {

      int64_t new_latest_msn = new_playlist.latest_msn();
      int new_latest_part = new_playlist.latest_part_index();

      for (const auto &seg : new_playlist.segments()) {
        if (seg.media_sequence < current_msn_)
          continue;

        for (int i = 0; i < static_cast<int>(seg.parts.size()); ++i) {
          bool is_new = false;
          if (seg.media_sequence > current_msn_) {
            is_new = true;
          } else if (seg.media_sequence == current_msn_ &&
                     i > current_part_idx_) {
            is_new = true;
          }

          if (is_new) {
            PartRequest req;
            req.uri = seg.parts[i].uri;
            req.byterange_offset = seg.parts[i].byterange_offset;
            req.byterange_length = seg.parts[i].byterange_length;
            enqueue_part(std::move(req));

            lss_log_debug("[LL-HLS Client] Enqueued new part: MSN=%lld, "
                          "idx=%d, URI=%s",
                          (long long)seg.media_sequence, i,
                          seg.parts[i].uri.c_str());
          }
        }
      }

      if (new_playlist.preload_hint().valid &&
          new_playlist.preload_hint().type == "PART") {
        PartRequest req;
        req.uri = new_playlist.preload_hint().uri;
        req.byterange_offset = new_playlist.preload_hint().byterange_offset;
        req.byterange_length = new_playlist.preload_hint().byterange_length;
        enqueue_part(std::move(req));
      }

      current_msn_ = new_latest_msn;
      current_part_idx_ = new_latest_part;
    }

    playlist_ = std::move(new_playlist);
  }

  lss_log_debug("[LL-HLS Client] Playlist worker stopped");
}

void LLHlsClient::download_worker() {
  lss_log_debug("[LL-HLS Client] Download worker started");

  while (running_.load()) {
    PartRequest req;
    if (!dequeue_part(req))
      break;

    if (!running_.load())
      break;

    std::vector<uint8_t> data;
    bool ok = fetcher_.fetch_data(req.uri, data, req.byterange_offset,
                                  req.byterange_length);

    if (!ok) {
      if (!running_.load())
        break;

      lss_log_warn("[LL-HLS Client] Failed to fetch %s: %s",
                   req.is_init ? "init segment" : "part", req.uri.c_str());

      if (req.is_init) {
        lss_log_error("[LL-HLS Client] Init segment fetch failed  - "
                      "cannot continue");
        running_.store(false);
        break;
      }
      continue;
    }

    if (!data.empty()) {
      ring_write(data.data(), data.size());

      if (req.is_init) {
        init_sent_.store(true);
        lss_log_debug("[LL-HLS Client] Init segment written to ring buffer "
                      "(%zu bytes)",
                      data.size());
      } else {
        lss_log_debug("[LL-HLS Client] Part written to ring buffer "
                      "(%zu bytes, URI=%s)",
                      data.size(), req.uri.c_str());
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(buf_mutex_);
    buf_cv_.notify_all();
  }

  lss_log_debug("[LL-HLS Client] Download worker stopped");
}

} // namespace lss
