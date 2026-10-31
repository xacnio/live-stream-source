// fallback-layer.h - what a source shows when its stream is in trouble
//
// A source draws its own fallback, so there's nothing to add to a scene or to line
// up by hand: it sits exactly on the stream, in every scene, and one stream's
// trouble can't show up on another. The stream arrives as raw frames, which can't
// have anything drawn over them, so a small filter does the drawing.
#pragma once

#include "core/fallback-fit.h"

#include <obs.h>

#include <atomic>
#include <mutex>
#include <string>
#include <tuple>

namespace lss {

enum class FallbackState : int { None, LowBitrate, Loading, Disconnected };

enum class FallbackKind : int { None, Card, Image, Video, Browser, SceneSource };

struct FallbackSpec {
  FallbackKind kind = FallbackKind::None;

  std::string text;
  std::string font_json;
  uint32_t text_color = 0xFFFFFFFF; // all colours are 0xAABBGGRR
  bool outline = false;
  int outline_size = 2;
  uint32_t outline_color = 0xFF000000;

  std::string file;
  std::string url;
  std::string template_name;
  int template_scale = 100;

  FallbackFit fit = FallbackFit::Fit;
  int size_percent = 100;
  int anchor = 4;
  bool keep_picture = false; // draw over the stream instead of replacing it

  uint32_t background_color = 0;
  std::string background_file;

  FallbackAnim anim_in = FallbackAnim::Fade;
  FallbackAnim anim_out = FallbackAnim::Fade;
  int anim_in_ms = 300;
  int anim_out_ms = 300;

  bool draws_itself() const {
    return kind != FallbackKind::None && kind != FallbackKind::SceneSource;
  }

  // Shipped templates lay themselves out, so they always get the whole source.
  bool uses_template() const { return kind == FallbackKind::Browser && !template_name.empty(); }

  // Loaded ahead while the source is on screen, so one screen can take over from another
  // with no gap. A page of your own may have sound, and a video starts over anyway.
  bool preloads() const {
    return kind == FallbackKind::Card || kind == FallbackKind::Image || uses_template();
  }

  // Changing any of these means building the content again. The rest can change on air.
  auto content() const {
    return std::tie(kind, text, font_json, text_color, outline, outline_size, outline_color, file,
                    url, template_name, template_scale, background_file);
  }
};

class FallbackLayer {
public:
  FallbackLayer() = default;
  ~FallbackLayer();

  FallbackLayer(const FallbackLayer &) = delete;
  FallbackLayer &operator=(const FallbackLayer &) = delete;

  static void register_filter();

  // UI thread: creating a browser or media source anywhere else isn't safe.
  void configure(obs_source_t *owner, FallbackState state, const FallbackSpec &spec);

  void show(FallbackState state) { shown_.store(static_cast<int>(state)); }

  // Graphics thread, from the filter.
  void tick(float seconds, bool visible);
  void render(obs_source_t *filter, uint32_t width, uint32_t height);

private:
  static constexpr int STATES = 3;

  struct Layer {
    FallbackSpec spec;
    obs_source_t *child = nullptr;
    obs_source_t *background = nullptr;
    bool ready = false;
    bool active = false;
    bool rising = false;
    float progress = 0.0f; // 0 is gone, 1 is fully in place
  };

  void set_refs(Layer &layer, bool ready, bool active);
  void draw_layer(const FallbackSpec &spec, obs_source_t *child, obs_source_t *background,
                  float progress, bool rising, uint32_t width, uint32_t height);

  std::mutex mutex_;
  Layer layers_[STATES];
  std::atomic<int> shown_{0};

  // Graphics thread.
  gs_texrender_t *texrender_ = nullptr;
  gs_effect_t *fade_effect_ = nullptr;
};

} // namespace lss
