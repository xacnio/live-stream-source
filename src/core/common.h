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

// Inter-packet wall-clock gap (ms) above which we treat a stream as having
// experienced a "soft freeze" and trigger A/V resync. Below the demuxer
// rw_timeout (5s) so we recover before a hard reconnect kicks in. Above
// normal jitter (~100ms) and B-frame reorder gaps (~50ms) to avoid false
// positives at high FPS.
constexpr int64_t FREEZE_RECOVERY_THRESHOLD_MS = 250;
// RTMP over TCP delivers data in bursts — especially at 10+ Mbps where
// Nagle batching + congestion-window dynamics cause 200-800ms inter-packet
// gaps that are perfectly normal. Using the generic 250ms threshold
// triggers false-positive freeze detection on every burst boundary,
// leading to constant recovery actions (PTS re-anchor / skip / cooldown)
// that manifest as visible stutter. 2000ms captures genuine stalls only.
constexpr int64_t FREEZE_RECOVERY_THRESHOLD_RTMP_MS = 2000;

// PTS drift (ms) above which the video path re-anchors the shared offset,
// or audio output clamps a single frame's timestamp to wall clock.
constexpr int64_t PTS_DRIFT_REANCHOR_MS = 200;

// Catch-up decision thresholds — when to start catch-up, when to skip,
// and when to exit catch-up mode. Used by CatchupOrchestrator + worker
// freeze recovery.

// Minimum drift from live edge to trigger catch-up (1 second)
// Rationale: Below 1s, drift is not noticeable to viewers. Starting catch-up
// too early causes unnecessary tempo changes and potential audio artifacts.
constexpr int64_t CATCHUP_MIN_DRIFT_MS = 1000;

// Maximum drift before skipping to live instead of catching up (10 seconds)
// Rationale: Above 10s, catch-up would take too long or require tempo > 2.0x
// which degrades audio quality. Skipping is faster and less disruptive.
constexpr int64_t CATCHUP_MAX_DRIFT_MS = 10000;

// Exit catch-up when deficit falls below this threshold (100ms)
// Rationale: Provides hysteresis to prevent rapid enter/exit cycles. 100ms
// is below perceptual threshold but provides stability margin.
constexpr int64_t CATCHUP_EXIT_DEFICIT_MS = 100;

// Target recovery time for catch-up (3 seconds)
// Rationale: Balances fast recovery with audio quality. Shorter recovery
// requires higher tempo (more artifacts). Longer recovery keeps viewers
// further from live edge. 3s provides good balance.
// Formula: tempo = 1.0 + (drift_ms / 1000.0) / TARGET_RECOVERY_SEC
constexpr double CATCHUP_TARGET_RECOVERY_SEC = 3.0;

// ----------------------------------------------------------------------------
// Tempo Limits
// ----------------------------------------------------------------------------
// These limits constrain the tempo range to maintain audio quality.

// Minimum tempo for catch-up (1.1x = 10% faster)
// Rationale: Below 1.1x, catch-up is too slow to be effective. This provides
// a floor for the tempo calculation to ensure meaningful progress.
constexpr double CATCHUP_TEMPO_MIN = 1.1;

// Maximum tempo for catch-up (2.0x = double speed)
// Rationale: SoundTouch maintains acceptable quality up to 2.0x. Above 2.0x,
// audio artifacts become noticeable (chipmunk effect, distortion). This is
// also the practical limit for viewer comprehension of speech/music.
constexpr double CATCHUP_TEMPO_MAX = 2.0;

// ----------------------------------------------------------------------------
// Catch-up Cooldown
// ----------------------------------------------------------------------------
// Prevents catch-up loops by enforcing a cooldown period after exit.

// Cooldown duration after catch-up exit (5 seconds)
// Rationale: Prevents rapid re-entry into catch-up mode which causes:
// - Repeated tempo changes (jarring for viewers)
// - Audio artifacts from frequent transitions
// - Unstable playback experience
// 5 seconds allows buffer to stabilize and network conditions to settle.
constexpr int64_t CATCHUP_COOLDOWN_MS = 5000;

// ----------------------------------------------------------------------------
// A/V Sync Configuration
// ----------------------------------------------------------------------------
// Controls the master-slave clock system for perfect audio/video sync.

// Snap audio clock to video clock if drift exceeds this threshold (10ms)
// Rationale: Video is the master clock, audio is slave. Small drift
// accumulates over time due to rounding errors and tempo adjustments.
// 10ms is below perceptual threshold (~50ms) but prevents gradual drift.
// Snapping ensures clocks stay tightly synchronized.
constexpr int64_t SYNC_DRIFT_SNAP_THRESHOLD_NS = 10 * 1000000LL;

// ----------------------------------------------------------------------------
// Buffer Management Configuration
// ----------------------------------------------------------------------------
// Controls buffer depth monitoring and catch-up eligibility.

// Minimum buffer frames required to start catch-up (~500ms at 60fps)
// Rationale: Catch-up requires sufficient buffer to:
// - Sustain playback during tempo ramp-up
// - Handle network jitter during catch-up
// - Prevent buffer underrun (which causes stuttering)
// 30 frames = 500ms at 60fps, 1000ms at 30fps - adequate safety margin.
constexpr int BUFFER_MIN_FRAMES = 30;

// Network delay threshold for stability (100ms)
// Rationale: High network delay indicates unstable connection. Starting
// catch-up during network instability risks buffer underrun. 100ms is
// typical for stable connections; above this suggests congestion/jitter.
constexpr int64_t BUFFER_NETWORK_STABLE_MS = 100;

// ----------------------------------------------------------------------------
// Tempo Ramping Configuration
// ----------------------------------------------------------------------------
// Controls smooth tempo transitions to prevent audible artifacts.

// Tempo ramp speed: 30% of remaining distance per frame
// Rationale: Exponential ramp provides smooth, natural-feeling transitions:
// - Fast initial change (responsive)
// - Gradual approach to target (smooth)
// - No abrupt jumps (no clicks/pops)
// Example: 1.0 → 1.5x takes ~5 frames (83ms at 60fps)
// Frame 1: 1.0 + (0.5 * 0.3) = 1.15
// Frame 2: 1.15 + (0.35 * 0.3) = 1.255
// Frame 3: 1.255 + (0.245 * 0.3) = 1.329
// Frame 4: 1.329 + (0.171 * 0.3) = 1.380
// Frame 5: 1.380 + (0.120 * 0.3) = 1.416
constexpr double TEMPO_RAMP_SPEED = 0.3;

// Tempo stability threshold: consider stable when within 1% of target
// Rationale: Prevents infinite ramping due to floating-point precision.
// 1% is imperceptible to listeners but provides clear convergence criterion.
// Example: At 1.5x target, stable when current tempo is 1.485 - 1.515
constexpr double TEMPO_STABILITY_THRESHOLD = 0.01;

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
constexpr const char *PROP_SHOW_SHIMMER = "show_shimmer";
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
