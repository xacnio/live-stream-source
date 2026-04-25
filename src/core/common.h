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

// Frame queue capacity: default initial buffer
constexpr int FRAME_QUEUE_CAPACITY = 60;

// Buffer size limits
constexpr int BUFFER_SIZE_MIN = 5;
constexpr int BUFFER_SIZE_MAX = 180;

// Buffer preset target durations (milliseconds)
constexpr int BUFFER_ULTRA_LOW_LATENCY_MS = 150;
constexpr int BUFFER_LOW_LATENCY_MS = 300;
constexpr int BUFFER_BALANCED_MS = 700;
constexpr int BUFFER_STABLE_MS = 1200;
constexpr int BUFFER_MAX_STABILITY_MS = 2000;

// Buffer monitoring interval
constexpr int64_t BUFFER_MONITOR_INTERVAL_MS = 10000;  // 10 seconds
constexpr double BUFFER_ADJUST_THRESHOLD = 0.2;  // 20% change triggers adjustment

// Bitrate thresholds for smart buffer calculation (kbps)
constexpr double BITRATE_ULTRA_HIGH = 15000;
constexpr double BITRATE_HIGH = 8000;
constexpr double BITRATE_MEDIUM_HIGH = 4000;
constexpr double BITRATE_MEDIUM = 2000;
constexpr double BITRATE_LOW = 1000;

// Probesize: 512 KB - larger for high bitrate streams
constexpr int64_t PROBE_SIZE = 524288;

// Analyze duration: 1000 ms - better codec detection
constexpr int64_t ANALYZE_DURATION_US = 1000000;

// Default low-bitrate threshold in kbps (optimized for high quality streams)
constexpr int DEFAULT_LOW_BITRATE_KBPS = 5000;

// Bitrate sampling window in milliseconds (longer for stability)
constexpr int BITRATE_WINDOW_MS = 3000;

// Maximum A/V drift (ms) before catch-up engages (relaxed for 60fps)
constexpr int64_t CATCHUP_DRIFT_THRESHOLD_MS = 200;

// Maximum A/V drift (ms) to exit catch-up
constexpr int64_t CATCHUP_STABLE_THRESHOLD_MS = 80;

// Catch-up playback speed range (gentler for 60fps smoothness)
constexpr double CATCHUP_SPEED_MIN = 1.03;
constexpr double CATCHUP_SPEED_MAX = 1.10;

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

// Buffer preset enum
enum class BufferPreset : int {
  Auto = 0,           // Smart multi-factor optimization
  UltraLowLatency = 1,
  LowLatency = 2,
  Balanced = 3,
  Stable = 4,
  MaxStability = 5,
  Custom = 6
};

// Properties keys
constexpr const char *PROP_URL = "url";
constexpr const char *PROP_STREAM_TYPE = "stream_type";
constexpr const char *PROP_LOW_BITRATE = "low_bitrate_kbps";
constexpr const char *PROP_AUTO_CATCHUP = "auto_catchup";
constexpr const char *PROP_HW_DECODE = "hw_decode";
constexpr const char *PROP_BUFFER_PRESET = "buffer_preset";
constexpr const char *PROP_BUFFER_CUSTOM = "buffer_custom_frames";
constexpr const char *PROP_LOW_BITRATE_SOURCE = "low_bitrate_source_name";
constexpr const char *PROP_DISCONNECT_SOURCE = "disconnect_source_name";
constexpr const char *PROP_LOADING_SOURCE = "loading_source_name";
constexpr const char *PROP_SHOW_SHIMMER = "show_shimmer";
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
