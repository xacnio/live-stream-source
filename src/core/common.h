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
#define lss_log_debug(fmt, ...)                                                \
  blog(LOG_DEBUG, LSS_LOG_PREFIX fmt, ##__VA_ARGS__)

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

// Frame queue capacity: enough for 60fps without drops
constexpr int FRAME_QUEUE_CAPACITY = 5;

// Probesize: 32 KB  - absolute minimum to detect headers
constexpr int64_t PROBE_SIZE = 32768;

// Analyze duration: 500 ms max  - we want to start playing ASAP
constexpr int64_t ANALYZE_DURATION_US = 500000;

// Default low-bitrate threshold in kbps
constexpr int DEFAULT_LOW_BITRATE_KBPS = 200;

// Bitrate sampling window in milliseconds
constexpr int BITRATE_WINDOW_MS = 2000;

// Maximum A/V drift (ms) before catch-up engages
constexpr int64_t CATCHUP_DRIFT_THRESHOLD_MS = 150;

// Maximum A/V drift (ms) to exit catch-up
constexpr int64_t CATCHUP_STABLE_THRESHOLD_MS = 50;

// Catch-up playback speed range
constexpr double CATCHUP_SPEED_MIN = 1.05;
constexpr double CATCHUP_SPEED_MAX = 1.15;

// Reconnection
constexpr int RECONNECT_DELAY_MS = 2000;
constexpr int RECONNECT_MAX_ATTEMPTS = 10;

// Overlay
constexpr int OVERLAY_FONT_SIZE = 32;
constexpr int OVERLAY_PADDING = 10;

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
constexpr const char *PROP_HW_DECODE = "hw_decode";
constexpr const char *PROP_LOW_BITRATE_SOURCE = "low_bitrate_source_name";
constexpr const char *PROP_DISCONNECT_SOURCE = "disconnect_source_name";
constexpr const char *PROP_LOADING_SOURCE = "loading_source_name";
constexpr const char *PROP_STATS_PATH_INFO = "stats_path_info";
constexpr const char *PROP_OPEN_STATS = "open_stats";
constexpr const char *PROP_ADD_OVERLAY_HORZ = "add_stats_overlay_horz";
constexpr const char *PROP_ADD_OVERLAY_VERT = "add_stats_overlay_vert";
constexpr const char *PROP_REFRESH = "refresh_stream";
constexpr const char *PROP_STATS_TOGGLE = "stats_toggle";
constexpr const char *PROP_STATS_TEXT = "stats_text";
constexpr const char *PROP_STATS_REFRESH = "stats_refresh";

//  WHEP (WebRTC)
constexpr const char *PROP_WHEP_TOKEN = "whep_bearer_token";

} // namespace lss
