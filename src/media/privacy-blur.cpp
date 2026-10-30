// privacy-blur.cpp
#include "media/privacy-blur.h"

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lss {

static constexpr int KERNEL_ONE = 1 << 16;
static constexpr uint8_t BLACK_LUMA = 16;
static constexpr uint8_t NEUTRAL_CHROMA = 128;

static std::vector<int> make_kernel(double sigma) {
  const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0)));
  std::vector<double> weights(radius * 2 + 1);
  double total = 0.0;
  for (int i = -radius; i <= radius; ++i) {
    weights[i + radius] = std::exp(-(i * i) / (2.0 * sigma * sigma));
    total += weights[i + radius];
  }

  std::vector<int> kernel(weights.size());
  int sum = 0;
  for (size_t i = 0; i < weights.size(); ++i) {
    kernel[i] = static_cast<int>(std::lround(weights[i] / total * KERNEL_ONE));
    sum += kernel[i];
  }
  // Rounding must not brighten or darken the picture.
  kernel[radius] += KERNEL_ONE - sum;
  return kernel;
}

void gaussian_blur_plane(uint8_t *plane, int width, int height, int stride, double sigma,
                         std::vector<uint8_t> &scratch) {
  if (width <= 0 || height <= 0 || sigma <= 0.0)
    return;

  const std::vector<int> kernel = make_kernel(sigma);
  const int radius = static_cast<int>(kernel.size()) / 2;
  scratch.resize(static_cast<size_t>(width) * height);

  for (int y = 0; y < height; ++y) {
    const uint8_t *row = plane + static_cast<size_t>(y) * stride;
    uint8_t *out = scratch.data() + static_cast<size_t>(y) * width;
    for (int x = 0; x < width; ++x) {
      int64_t sum = KERNEL_ONE / 2;
      for (int k = -radius; k <= radius; ++k)
        sum += static_cast<int64_t>(kernel[k + radius]) * row[std::clamp(x + k, 0, width - 1)];
      out[x] = static_cast<uint8_t>(sum >> 16);
    }
  }

  for (int x = 0; x < width; ++x) {
    for (int y = 0; y < height; ++y) {
      int64_t sum = KERNEL_ONE / 2;
      for (int k = -radius; k <= radius; ++k) {
        const int from = std::clamp(y + k, 0, height - 1);
        sum += static_cast<int64_t>(kernel[k + radius]) *
               scratch[static_cast<size_t>(from) * width + x];
      }
      plane[static_cast<size_t>(y) * stride + x] = static_cast<uint8_t>(sum >> 16);
    }
  }
}

PrivacyBlur::~PrivacyBlur() { release(); }

void PrivacyBlur::release() {
  if (down_)
    sws_freeContext(down_);
  if (up_)
    sws_freeContext(up_);
  down_ = nullptr;
  up_ = nullptr;
}

bool PrivacyBlur::prepare(AVPixelFormat format, int width, int height) {
  if (down_ && up_ && format_ == format && width_ == width && height_ == height)
    return true;

  release();
  // swscale aborts the process on a format it doesn't know, it doesn't return null.
  if (!sws_isSupportedInput(format))
    return false;

  small_width_ = std::max(16, (width / REDUCTION + 1) & ~1);
  small_height_ = std::max(16, (height / REDUCTION + 1) & ~1);

  // Area averaging on the way down leaves no fine detail for the blur to smear
  // around, and the blur is what makes the way back up smooth.
  down_ = sws_getContext(width, height, format, small_width_, small_height_,
                         AV_PIX_FMT_YUV420P, SWS_AREA, nullptr, nullptr, nullptr);
  up_ = sws_getContext(small_width_, small_height_, AV_PIX_FMT_YUV420P, width, height,
                       AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!down_ || !up_) {
    release();
    return false;
  }

  format_ = format;
  width_ = width;
  height_ = height;
  return true;
}

bool PrivacyBlur::apply(const uint8_t *const src[4], const int src_linesize[4],
                        AVPixelFormat format, int width, int height, uint8_t *y,
                        uint8_t *u, uint8_t *v) {
  // The frame keeps its range flag, so converting it here would wash out blacks.
  if (format == AV_PIX_FMT_YUVJ420P)
    format = AV_PIX_FMT_YUV420P;
  else if (format == AV_PIX_FMT_YUVJ444P)
    format = AV_PIX_FMT_YUV444P;

  const int chroma_width = (width + 1) / 2;
  const int chroma_height = (height + 1) / 2;
  const size_t chroma_size = static_cast<size_t>(chroma_width) * chroma_height;

  if (!prepare(format, width, height)) {
    std::memset(y, BLACK_LUMA, static_cast<size_t>(width) * height);
    std::memset(u, NEUTRAL_CHROMA, chroma_size);
    std::memset(v, NEUTRAL_CHROMA, chroma_size);
    return false;
  }

  const int small_chroma_width = small_width_ / 2;
  const int small_chroma_height = small_height_ / 2;
  const size_t luma_size = static_cast<size_t>(small_width_) * small_height_;
  const size_t small_chroma_size = static_cast<size_t>(small_chroma_width) * small_chroma_height;
  small_.resize(luma_size + small_chroma_size * 2);

  uint8_t *small_y = small_.data();
  uint8_t *small_u = small_y + luma_size;
  uint8_t *small_v = small_u + small_chroma_size;

  uint8_t *small_planes[4] = {small_y, small_u, small_v, nullptr};
  const int small_linesize[4] = {small_width_, small_chroma_width, small_chroma_width, 0};
  sws_scale(down_, src, src_linesize, 0, height, small_planes, small_linesize);

  const double sigma = static_cast<double>(std::max(width, height)) / SIGMA_DIVISOR / REDUCTION;
  gaussian_blur_plane(small_y, small_width_, small_height_, small_width_, sigma, scratch_);
  gaussian_blur_plane(small_u, small_chroma_width, small_chroma_height, small_chroma_width,
                      sigma / 2.0, scratch_);
  gaussian_blur_plane(small_v, small_chroma_width, small_chroma_height, small_chroma_width,
                      sigma / 2.0, scratch_);

  const uint8_t *const small_source[4] = {small_y, small_u, small_v, nullptr};
  uint8_t *out_planes[4] = {y, u, v, nullptr};
  const int out_linesize[4] = {width, chroma_width, chroma_width, 0};
  sws_scale(up_, small_source, small_linesize, 0, small_height_, out_planes, out_linesize);
  return true;
}

} // namespace lss
