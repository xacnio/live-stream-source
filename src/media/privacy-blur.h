// privacy-blur.h - hides what's on screen while keeping the overall look of it
//
// A Gaussian blur, which leaves nothing to recognise. Pixelation doesn't: seen
// small, the eye merges the blocks back into shapes and text reads again. Working
// on a heavily reduced copy keeps a blur this wide cheap enough for every frame.
#pragma once

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <cstdint>
#include <vector>

struct SwsContext;

namespace lss {

// Separable Gaussian, in place. Edges repeat the outermost pixel.
void gaussian_blur_plane(uint8_t *plane, int width, int height, int stride, double sigma,
                         std::vector<uint8_t> &scratch);

class PrivacyBlur {
public:
  PrivacyBlur() = default;
  ~PrivacyBlur();

  PrivacyBlur(const PrivacyBlur &) = delete;
  PrivacyBlur &operator=(const PrivacyBlur &) = delete;

  // Writes the blurred picture as I420 into the three planes, each sized
  // width * height and (width + 1) / 2 * (height + 1) / 2. If that isn't possible
  // they're filled with black: this runs when a picture must not be seen, so
  // failing has to hide it as well. Returns whether the blur itself worked.
  bool apply(const uint8_t *const src[4], const int src_linesize[4], AVPixelFormat format,
             int width, int height, uint8_t *y, uint8_t *u, uint8_t *v);

private:
  bool prepare(AVPixelFormat format, int width, int height);
  void release();

  // One step of the small copy is this many pixels of the picture.
  static constexpr int REDUCTION = 16;
  // Blur width as a fraction of the longer side: 40 px at 1080p, much taller
  // than on-screen text has to be to stay readable.
  static constexpr int SIGMA_DIVISOR = 48;

  SwsContext *down_ = nullptr;
  SwsContext *up_ = nullptr;
  AVPixelFormat format_ = AV_PIX_FMT_NONE;
  int width_ = 0, height_ = 0;
  int small_width_ = 0, small_height_ = 0;
  std::vector<uint8_t> small_;
  std::vector<uint8_t> scratch_;
};

} // namespace lss
