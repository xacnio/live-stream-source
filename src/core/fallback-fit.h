// fallback-fit.h - where fallback content lands inside a source, and how it moves
#pragma once

#include <algorithm>

namespace lss {

enum class FallbackFit : int { Fit, Fill, Stretch, Original };

// A side means where content comes from on the way in, and goes to on the way out.
enum class FallbackAnim : int { None, Fade, SideBottom, SideTop, SideRight, SideLeft, Zoom, Pop };

struct FallbackRect {
  float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};

struct FallbackMotion {
  float dx = 0.0f, dy = 0.0f;
  float scale = 1.0f;
  float alpha = 1.0f;
};

// The part of the source the fallback may use. anchor counts left to right, top to
// bottom: 0 is the top left corner, 4 the centre, 8 bottom right.
inline FallbackRect fallback_box(int size_percent, int anchor, float source_width,
                                 float source_height) {
  const float share = static_cast<float>(size_percent) / 100.0f;
  const int column = std::clamp(anchor, 0, 8) % 3;
  const int row = std::clamp(anchor, 0, 8) / 3;

  FallbackRect box;
  box.width = source_width * share;
  box.height = source_height * share;
  box.x = (source_width - box.width) * static_cast<float>(column) / 2.0f;
  box.y = (source_height - box.height) * static_cast<float>(row) / 2.0f;
  return box;
}

inline FallbackRect fallback_rect(FallbackFit fit, int size_percent, int anchor,
                                  float content_width, float content_height,
                                  float source_width, float source_height) {
  FallbackRect rect;
  if (content_width <= 0.0f || content_height <= 0.0f)
    return rect;

  const FallbackRect box = fallback_box(size_percent, anchor, source_width, source_height);

  switch (fit) {
  case FallbackFit::Stretch:
    rect.width = box.width;
    rect.height = box.height;
    break;
  case FallbackFit::Original: {
    const float share = static_cast<float>(size_percent) / 100.0f;
    rect.width = content_width * share;
    rect.height = content_height * share;
    break;
  }
  case FallbackFit::Fill:
  case FallbackFit::Fit: {
    const float across = box.width / content_width;
    const float down = box.height / content_height;
    const float scale = fit == FallbackFit::Fill ? std::max(across, down) : std::min(across, down);
    rect.width = content_width * scale;
    rect.height = content_height * scale;
    break;
  }
  }

  const int column = std::clamp(anchor, 0, 8) % 3;
  const int row = std::clamp(anchor, 0, 8) / 3;
  rect.x = box.x + (box.width - rect.width) * static_cast<float>(column) / 2.0f;
  rect.y = box.y + (box.height - rect.height) * static_cast<float>(row) / 2.0f;
  return rect;
}

// Card text keeps its point size relative to 1080p, and only shrinks when it would
// leave the box.
inline FallbackRect fallback_text_rect(const FallbackRect &box, float text_width,
                                       float text_height, float source_height) {
  FallbackRect rect;
  if (text_width <= 0.0f || text_height <= 0.0f)
    return rect;

  const float natural = source_height / 1080.0f;
  const float scale = std::min({natural, box.width / text_width, box.height / text_height});
  rect.width = text_width * scale;
  rect.height = text_height * scale;
  rect.x = box.x + (box.width - rect.width) / 2.0f;
  rect.y = box.y + (box.height - rect.height) / 2.0f;
  return rect;
}

// progress runs from 0 (off screen) to 1 (in place), in both directions.
inline FallbackMotion fallback_motion(FallbackAnim anim, float progress, float source_width,
                                      float source_height) {
  const float t = std::clamp(progress, 0.0f, 1.0f);
  const float rest = 1.0f - t;
  const float eased = 1.0f - rest * rest * rest; // quick start, soft landing

  FallbackMotion motion;
  switch (anim) {
  case FallbackAnim::None:
    motion.alpha = t > 0.0f ? 1.0f : 0.0f;
    break;
  case FallbackAnim::Fade:
    motion.alpha = eased;
    break;
  case FallbackAnim::SideBottom:
    motion.dy = (1.0f - eased) * source_height;
    break;
  case FallbackAnim::SideTop:
    motion.dy = -(1.0f - eased) * source_height;
    break;
  case FallbackAnim::SideRight:
    motion.dx = (1.0f - eased) * source_width;
    break;
  case FallbackAnim::SideLeft:
    motion.dx = -(1.0f - eased) * source_width;
    break;
  case FallbackAnim::Zoom:
    motion.scale = 0.8f + 0.2f * eased;
    motion.alpha = eased;
    break;
  case FallbackAnim::Pop: {
    // Overshoots slightly before settling.
    const float back = t - 1.0f;
    motion.scale = std::max(0.0f, 1.0f + 2.7f * back * back * back + 1.7f * back * back);
    motion.alpha = std::min(1.0f, t * 3.0f);
    break;
  }
  }
  return motion;
}

} // namespace lss
