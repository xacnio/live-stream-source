// common.h - shared constants and macros
#pragma once

#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

//  FFmpeg headers (C linkage)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

//  Plugin-wide log tag
#define LSS_LOG_PREFIX "[LSS] "

#define lss_log_info(fmt, ...) blog(LOG_INFO, LSS_LOG_PREFIX fmt, ##__VA_ARGS__)
#define lss_log_warn(fmt, ...)                                                 \
  blog(LOG_WARNING, LSS_LOG_PREFIX fmt, ##__VA_ARGS__)
#define lss_log_error(fmt, ...)                                                \
  blog(LOG_ERROR, LSS_LOG_PREFIX fmt, ##__VA_ARGS__)
#ifdef NDEBUG
#define lss_log_debug(fmt, ...) ((void)0)
#else
#define lss_log_debug(fmt, ...)                                                \
  blog(LOG_DEBUG, LSS_LOG_PREFIX fmt, ##__VA_ARGS__)
#endif

//  Timing helpers
using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;
using Milliseconds = std::chrono::milliseconds;
using Microseconds = std::chrono::microseconds;

inline int64_t now_ms() {
  return std::chrono::duration_cast<Milliseconds>(
             SteadyClock::now().time_since_epoch())
      .count();
}

inline int64_t now_us() {
  return std::chrono::duration_cast<Microseconds>(
             SteadyClock::now().time_since_epoch())
      .count();
}

//  Default configuration values
namespace lss {

// A stream key sits in the path or query, and logs get pasted into forums.
inline std::string url_for_log(const std::string &url) {
  const size_t scheme = url.find("://");
  const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
  const size_t end = std::min(url.find_first_of("/?", start), url.size());
  const size_t login = url.rfind('@', end);
  const size_t host = login == std::string::npos || login < start ? start : login + 1;
  return url.substr(0, start) + url.substr(host, end - host) + "/...";
}

// Frame queue capacity: default initial buffer (used by audio_queue_
// pass-through path; not user-tunable).
constexpr int FRAME_QUEUE_CAPACITY = 60;

// Probesize: 512 KB - larger for high bitrate streams
constexpr int64_t PROBE_SIZE = 524288;

// Analyze duration: 1000 ms - better codec detection
constexpr int64_t ANALYZE_DURATION_US = 1000000;

// Default low-bitrate threshold in kbps (optimized for high quality streams)
constexpr int DEFAULT_LOW_BITRATE_KBPS = 5000;

// Bitrate sampling window in milliseconds (longer for stability)
constexpr int BITRATE_WINDOW_MS = 3000;

// Inter-packet wall-clock gap (ms) above which an HLS/IVS stream counts as
// frozen and gets an A/V resync. Standard streams don't use this: the playout
// engine sees a stall as its buffer running dry.
constexpr int64_t FREEZE_RECOVERY_THRESHOLD_MS = 250;

// Gaps in this range re-anchor the PTS mapping; longer ones skip-resync.
constexpr int64_t FREEZE_RESYNC_MIN_GAP_MS = 1000;
constexpr int64_t FREEZE_RESYNC_MAX_GAP_MS = 10000;

// Above this gap, burst-skip discards packets up to the live-edge estimate.
constexpr int64_t BURST_SKIP_MIN_GAP_MS = 2000;

// Suppresses another freeze recovery right after a skip-resync.
constexpr int64_t FREEZE_COOLDOWN_MS = 5000;

// Playout latency presets for Standard streams (ms).
constexpr int TARGET_LATENCY_LOW_MS = 250;
constexpr int TARGET_LATENCY_BALANCED_MS = 500;
constexpr int TARGET_LATENCY_STABLE_MS = 1000;
constexpr int TARGET_LATENCY_VERY_STABLE_MS = 2000;
constexpr int DEFAULT_TARGET_LATENCY_MS = TARGET_LATENCY_BALANCED_MS;

// Manual trim for a fixed delay difference between two synced sources (ms).
constexpr int MAX_SYNC_OFFSET_MS = 10000;
// How far apart sound matching looks. Wider finds false matches in rhythmic sound.
constexpr int SYNC_MATCH_RANGE_MS = 5000;

// How long the "Sync" button listens for shared sounds.
constexpr int64_t SYNC_LISTEN_US = 30000000;
constexpr int64_t SYNC_PANEL_STALE_US = 3000000;
constexpr int64_t SYNC_RESULT_SHOWN_US = 60000000;

// Status bar messages.
constexpr int STATUS_NOTICE_MS = 10000;

// Reconnection
constexpr int RECONNECT_DELAY_MS = 2000;
constexpr int RECONNECT_MAX_ATTEMPTS = 10;

// Overlay
constexpr int OVERLAY_FONT_SIZE = 32;
constexpr int OVERLAY_PADDING = 10;

// Overlay hysteresis: an overlay only appears once its condition has held
// for the grace period, then stays up for a minimum time. Keeps a flapping
// connection from strobing the overlays. See OverlayDebouncer.
constexpr int DEFAULT_DISCONNECT_GRACE_MS = 1500;
constexpr int MIN_DISCONNECT_GRACE_MS = 0;
constexpr int MAX_DISCONNECT_GRACE_MS = 60000;
constexpr int64_t DISCONNECT_MIN_VISIBLE_MS = 3000;
constexpr int64_t LOADING_MIN_VISIBLE_MS = 1000;

// Source ID
constexpr const char *SOURCE_ID = "live_stream_source";
constexpr const char *SOURCE_NAME = "Live Stream Source";

// Stats export interval
constexpr int STATS_WRITE_INTERVAL_MS = 1000;
constexpr int WS_STATS_PORT = 4477;

//  Stream type enum
enum class StreamType : int {
  Standard = 0,  // RTMP / RTMPS / FLV / SRT / RTSP
  AmazonIVS = 1, // Amazon IVS  - aggressive LL-HLS
  HLS = 2,       // Standard HLS
  WHEP = 3,      // WebRTC via WHEP endpoint
};

// Properties keys
constexpr const char *PROP_URL = "url";
constexpr const char *PROP_STREAM_TYPE = "stream_type";
constexpr const char *PROP_LOW_BITRATE = "low_bitrate_kbps";
constexpr const char *PROP_AUTO_CATCHUP = "auto_catchup";
constexpr const char *PROP_TARGET_LATENCY = "target_latency_ms";
constexpr const char *PROP_RECOVERY_MODE = "recovery_mode";
constexpr const char *PROP_HW_DECODE = "hw_decode";
constexpr const char *PROP_LOW_BITRATE_SOURCE = "low_bitrate_source_name";
constexpr const char *PROP_DISCONNECT_SOURCE = "disconnect_source_name";
constexpr const char *PROP_LOADING_SOURCE = "loading_source_name";
constexpr const char *PROP_DISCONNECT_GRACE = "disconnect_grace_ms";
constexpr const char *PROP_STATS_PATH_INFO = "stats_path_info";
constexpr const char *PROP_OPEN_STATS = "open_stats";
constexpr const char *PROP_ADD_OVERLAY_HORZ = "add_stats_overlay_horz";
constexpr const char *PROP_ADD_OVERLAY_VERT = "add_stats_overlay_vert";
constexpr const char *PROP_REFRESH = "refresh_stream";
constexpr const char *PROP_SKIP_TO_LIVE = "skip_to_live";
constexpr const char *PROP_STATS_TOGGLE = "stats_toggle";
constexpr const char *PROP_STATS_TEXT = "stats_text";
constexpr const char *PROP_STATS_REFRESH = "stats_refresh";

//  WHEP (WebRTC)
constexpr const char *PROP_WHEP_TOKEN = "whep_bearer_token";

} // namespace lss
