// program-preview.cpp
#include "network/program-preview.h"
#include "core/common.h"
#include "network/preview-encoder.h"
#include "network/ws-stats-server.h"

#include <obs.h>

#include <cstring>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
}

namespace lss {

static constexpr int64_t CAPTURE_INTERVAL_MS = 200;
static constexpr int64_t LEASE_MS = 8000;
static constexpr uint32_t PREVIEW_WIDTH = 640;
static constexpr int JPEG_QUALITY = 8;
static constexpr uint8_t TAG_PROGRAM = 0x03;

ProgramPreview &ProgramPreview::instance() {
  static ProgramPreview preview;
  return preview;
}

void ProgramPreview::start() {
  running_ = true;
  worker_ = std::thread(&ProgramPreview::run, this);
  obs_add_main_rendered_callback(on_rendered, this);
}

void ProgramPreview::stop() {
  obs_remove_main_rendered_callback(on_rendered, this);
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    running_ = false;
  }
  frame_cv_.notify_all();
  if (worker_.joinable())
    worker_.join();

  obs_enter_graphics();
  release_gpu();
  obs_leave_graphics();
}

void ProgramPreview::keep_alive() {
  lease_until_ms_ = now_ms() + LEASE_MS;
}

void ProgramPreview::end() {
  lease_until_ms_ = 0;
}

void ProgramPreview::on_rendered(void *param) {
  static_cast<ProgramPreview *>(param)->rendered();
}

void ProgramPreview::rendered() {
  const int64_t now = now_ms();
  if (now > lease_until_ms_) {
    release_gpu();
    return;
  }
  if (now < next_capture_ms_)
    return;
  next_capture_ms_ = now + CAPTURE_INTERVAL_MS;

  if (!ensure_gpu())
    return;
  // Last round's copy is ready by now. Reading right after staging would stall
  // the graphics thread until the GPU catches up.
  if (staged_)
    read_back();
  draw();
}

bool ProgramPreview::ensure_gpu() {
  obs_video_info ovi = {};
  if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height)
    return false;

  if (texrender_ && (ovi.base_width != base_width_ || ovi.base_height != base_height_))
    release_gpu();
  if (texrender_)
    return true;

  base_width_ = ovi.base_width;
  base_height_ = ovi.base_height;
  width_ = PREVIEW_WIDTH;
  height_ = (PREVIEW_WIDTH * base_height_ / base_width_ + 1) & ~1u; // JPEG 4:2:0 wants even

  texrender_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
  stage_ = gs_stagesurface_create(width_, height_, GS_RGBA);
  if (!texrender_ || !stage_) {
    release_gpu();
    return false;
  }
  return true;
}

void ProgramPreview::release_gpu() {
  if (texrender_) {
    gs_texrender_destroy(texrender_);
    texrender_ = nullptr;
  }
  if (stage_) {
    gs_stagesurface_destroy(stage_);
    stage_ = nullptr;
  }
  staged_ = false;
}

void ProgramPreview::read_back() {
  staged_ = false;

  uint8_t *data = nullptr;
  uint32_t linesize = 0;
  if (!gs_stagesurface_map(stage_, &data, &linesize))
    return;

  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!frame_ready_) {
      frame_.resize(static_cast<size_t>(linesize) * height_);
      std::memcpy(frame_.data(), data, frame_.size());
      frame_width_ = width_;
      frame_height_ = height_;
      frame_linesize_ = linesize;
      frame_ready_ = true;
    }
  }
  gs_stagesurface_unmap(stage_);
  frame_cv_.notify_one();
}

void ProgramPreview::draw() {
  gs_texrender_reset(texrender_);
  if (!gs_texrender_begin_with_color_space(texrender_, width_, height_, GS_CS_SRGB))
    return;

  vec4 clear = {};
  gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
  gs_ortho(0.0f, static_cast<float>(base_width_), 0.0f,
           static_cast<float>(base_height_), -100.0f, 100.0f);

  gs_blend_state_push();
  gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
  obs_render_main_texture();
  gs_blend_state_pop();

  gs_texrender_end(texrender_);
  gs_stage_texture(stage_, gs_texrender_get_texture(texrender_));
  staged_ = true;
}

void ProgramPreview::run() {
  std::unique_ptr<PreviewEncoder> encoder;
  std::vector<uint8_t> pixels;
  uint32_t encoder_width = 0, encoder_height = 0;

  for (;;) {
    uint32_t width, height, linesize;
    {
      std::unique_lock<std::mutex> lock(frame_mutex_);
      frame_cv_.wait(lock, [this]() { return frame_ready_ || !running_; });
      if (!running_)
        return;
      pixels.swap(frame_);
      width = frame_width_;
      height = frame_height_;
      linesize = frame_linesize_;
      frame_ready_ = false;
    }

    if (!encoder || width != encoder_width || height != encoder_height) {
      encoder = std::make_unique<PreviewEncoder>(static_cast<int>(width),
                                                 static_cast<int>(height), JPEG_QUALITY);
      encoder_width = width;
      encoder_height = height;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame)
      continue;
    frame->format = AV_PIX_FMT_RGBA;
    frame->width = static_cast<int>(width);
    frame->height = static_cast<int>(height);
    frame->data[0] = pixels.data();
    frame->linesize[0] = static_cast<int>(linesize);

    std::vector<uint8_t> jpeg;
    if (encoder->encode_video_jpeg(frame, jpeg)) {
      jpeg[0] = TAG_PROGRAM;
      WsStatsServer::instance().send_binary(jpeg.data(), jpeg.size());
    }
    av_frame_free(&frame);
  }
}

} // namespace lss
