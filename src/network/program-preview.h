// program-preview.h
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

struct gs_stage_surface;
struct gs_texture_render;

namespace lss {

// Small JPEGs of what OBS is sending out, for the web panel. It draws its own
// small copy a few times a second, so watching adds no full-size GPU readback
// to a running stream. Costs nothing while nobody is watching.
class ProgramPreview {
public:
  static ProgramPreview &instance();

  void start();
  void stop();

  // The panel repeats this while it watches. A phone that drops off the network
  // can't say goodbye, so the work ends by itself when the requests stop.
  void keep_alive();
  void end();

private:
  ProgramPreview() = default;

  static void on_rendered(void *param);
  void rendered();
  bool ensure_gpu();
  void release_gpu();
  void read_back();
  void draw();
  void run();

  std::atomic<int64_t> lease_until_ms_{0};

  // Graphics thread only.
  gs_texture_render *texrender_ = nullptr;
  gs_stage_surface *stage_ = nullptr;
  uint32_t base_width_ = 0, base_height_ = 0;
  uint32_t width_ = 0, height_ = 0;
  bool staged_ = false;
  int64_t next_capture_ms_ = 0;

  // Handed from the graphics thread to the encoding thread. A frame that's
  // still being encoded means the next one is skipped, never queued.
  std::mutex frame_mutex_;
  std::condition_variable frame_cv_;
  std::vector<uint8_t> frame_;
  uint32_t frame_width_ = 0, frame_height_ = 0, frame_linesize_ = 0;
  bool frame_ready_ = false;
  bool running_ = false;
  std::thread worker_;
};

} // namespace lss
