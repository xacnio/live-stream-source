// fallback-layer.cpp
#include "core/fallback-layer.h"
#include "core/common.h"
#include "core/fallback-template.h"
#include "core/live-stream-source.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <algorithm>
#include <cstring>

namespace lss {

static constexpr const char *FILTER_ID = "lss_fallback_filter";
static constexpr int BROWSER_WIDTH = 1920;
static constexpr int BROWSER_HEIGHT = 1080;

#ifdef _WIN32
static constexpr const char *TEXT_SOURCE_ID = "text_gdiplus";
#else
static constexpr const char *TEXT_SOURCE_ID = "text_ft2_source";
#endif

// Same as the shader of the default effect, plus a fade.
static constexpr const char *FADE_EFFECT = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float alpha;

sampler_state def_sampler {
  Filter = Linear;
  AddressU = Clamp;
  AddressV = Clamp;
};

struct VertInOut {
  float4 pos : POSITION;
  float2 uv : TEXCOORD0;
};

VertInOut VSDefault(VertInOut vert_in) {
  VertInOut vert_out;
  vert_out.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);
  vert_out.uv = vert_in.uv;
  return vert_out;
}

float4 PSDraw(VertInOut vert_in) : TARGET {
  return image.Sample(def_sampler, vert_in.uv) * alpha;
}

technique Draw {
  pass {
    vertex_shader = VSDefault(vert_in);
    pixel_shader = PSDraw(vert_in);
  }
}
)";

// Text and colour go straight into the page, so each state gets its own copy.
static std::string render_template(const FallbackSpec &spec, const char *owner_uuid, int state) {
  const std::string relative = "fallback-templates/" + spec.template_name + ".html";
  char *source_path = obs_module_file(relative.c_str());
  char *html = source_path ? os_quick_read_utf8_file(source_path) : nullptr;
  bfree(source_path);
  if (!html) {
    lss_log_warn("Fallback: template '%s' is missing", spec.template_name.c_str());
    return std::string();
  }

  const std::string page = render_fallback_template(html, spec.text, spec.text_color, spec.template_scale);
  bfree(html);

  char *folder = obs_module_config_path("fallback");
  std::string target;
  if (folder) {
    os_mkdirs(folder);
    target = std::string(folder) + "/" + owner_uuid + "-" + std::to_string(state) + ".html";
    bfree(folder);
    if (!os_quick_write_utf8_file(target.c_str(), page.c_str(), page.size(), false)) {
      lss_log_warn("Fallback: couldn't write '%s'", target.c_str());
      target.clear();
    }
  }
  return target;
}

static obs_source_t *create_background(const FallbackSpec &spec) {
  if (spec.background_file.empty())
    return nullptr;
  obs_data_t *settings = obs_data_create();
  obs_data_set_string(settings, "file", spec.background_file.c_str());
  obs_data_set_bool(settings, "unload", true);
  obs_source_t *child = obs_source_create_private("image_source", "lss-fallback-background", settings);
  obs_data_release(settings);
  return child;
}

static obs_source_t *create_child(const FallbackSpec &spec, const char *owner_uuid, int state) {
  obs_data_t *settings = obs_data_create();
  obs_source_t *child = nullptr;

  switch (spec.kind) {
  case FallbackKind::Card: {
    obs_data_set_string(settings, "text", spec.text.c_str());
    if (obs_data_t *font = obs_data_create_from_json(spec.font_json.c_str())) {
      obs_data_set_obj(settings, "font", font);
      obs_data_release(font);
    }
    // The Windows text source takes colour and opacity apart, the other one doesn't.
    obs_data_set_int(settings, "color", spec.text_color & 0xFFFFFF);
    obs_data_set_int(settings, "opacity", ((spec.text_color >> 24) & 0xFF) * 100 / 255);
    obs_data_set_int(settings, "color1", spec.text_color);
    obs_data_set_int(settings, "color2", spec.text_color);
    obs_data_set_string(settings, "align", "center");
    obs_data_set_bool(settings, "outline", spec.outline);
    obs_data_set_int(settings, "outline_size", spec.outline_size);
    obs_data_set_int(settings, "outline_color", spec.outline_color & 0xFFFFFF);
    obs_data_set_int(settings, "outline_opacity", ((spec.outline_color >> 24) & 0xFF) * 100 / 255);
    const char *id = obs_get_latest_input_type_id(TEXT_SOURCE_ID);
    child = obs_source_create_private(id ? id : TEXT_SOURCE_ID, "lss-fallback-card", settings);
    break;
  }
  case FallbackKind::Image:
    obs_data_set_string(settings, "file", spec.file.c_str());
    obs_data_set_bool(settings, "unload", true);
    child = obs_source_create_private("image_source", "lss-fallback-image", settings);
    break;
  case FallbackKind::Video:
    obs_data_set_bool(settings, "is_local_file", true);
    obs_data_set_string(settings, "local_file", spec.file.c_str());
    obs_data_set_bool(settings, "looping", true);
    obs_data_set_bool(settings, "restart_on_activate", true);
    obs_data_set_bool(settings, "close_when_inactive", true);
    child = obs_source_create_private("ffmpeg_source", "lss-fallback-video", settings);
    break;
  case FallbackKind::Browser: {
    const std::string page = spec.uses_template() ? render_template(spec, owner_uuid, state) : spec.file;
    obs_data_set_bool(settings, "is_local_file", spec.uses_template() || spec.url.empty());
    obs_data_set_string(settings, "local_file", page.c_str());
    obs_data_set_string(settings, "url", spec.url.c_str());
    obs_data_set_int(settings, "width", BROWSER_WIDTH);
    obs_data_set_int(settings, "height", BROWSER_HEIGHT);
    // A page only exists while it's needed. Its sound goes straight to the speakers,
    // past OBS, so one left running in the background would stay audible everywhere.
    obs_data_set_bool(settings, "shutdown", true);
    // OBS's default stylesheet wipes a page's background colour.
    if (spec.uses_template())
      obs_data_set_string(settings, "css", "");
    child = obs_source_create_private("browser_source", "lss-fallback-browser", settings);
    break;
  }
  default:
    break;
  }

  obs_data_release(settings);
  return child;
}

static void find_filter(obs_source_t *, obs_source_t *filter, void *param) {
  if (std::strcmp(obs_source_get_unversioned_id(filter), FILTER_ID) == 0)
    *static_cast<bool *>(param) = true;
}

// One saved with the source comes back on load, so this only ever adds the first.
static void ensure_filter(obs_source_t *owner) {
  bool found = false;
  obs_source_enum_filters(owner, find_filter, &found);
  if (found)
    return;

  obs_source_t *filter = obs_source_create(FILTER_ID, obs_module_text("FallbackFilter"), nullptr, nullptr);
  if (filter) {
    obs_source_filter_add(owner, filter);
    obs_source_release(filter);
  }
}

static void release_layer(obs_source_t *child, obs_source_t *background, bool ready, bool active) {
  for (obs_source_t *source : {child, background}) {
    if (!source)
      continue;
    if (ready)
      obs_source_dec_showing(source);
    if (active)
      obs_source_dec_active(source);
  }
  obs_source_release(child);
  obs_source_release(background);
}

FallbackLayer::~FallbackLayer() {
  for (Layer &layer : layers_)
    release_layer(layer.child, layer.background, layer.ready, layer.active);
  if (texrender_ || fade_effect_) {
    obs_enter_graphics();
    gs_texrender_destroy(texrender_);
    gs_effect_destroy(fade_effect_);
    obs_leave_graphics();
  }
}

void FallbackLayer::configure(obs_source_t *owner, FallbackState state, const FallbackSpec &spec) {
  if (state == FallbackState::None)
    return;

  const int index = static_cast<int>(state) - 1;
  obs_source_t *old_child = nullptr;
  obs_source_t *old_background = nullptr;
  bool was_ready = false;
  bool was_active = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Layer &layer = layers_[index];
    const bool same_content = layer.spec.content() == spec.content();
    layer.spec = spec;
    if (same_content)
      return;
    old_child = layer.child;
    old_background = layer.background;
    was_ready = layer.ready;
    was_active = layer.active;
    layer.child = nullptr;
    layer.background = nullptr;
    layer.ready = false;
    layer.active = false;
  }
  release_layer(old_child, old_background, was_ready, was_active);

  if (!spec.draws_itself())
    return;

  obs_source_t *child = create_child(spec, obs_source_get_uuid(owner), index);
  obs_source_t *background = create_background(spec);
  if (!child)
    lss_log_warn("Fallback: couldn't create the content, is that OBS plugin installed?");

  {
    std::lock_guard<std::mutex> lock(mutex_);
    layers_[index].child = child;
    layers_[index].background = background;
    // Whatever a tick decided in the meantime had nothing to act on.
    layers_[index].ready = false;
    layers_[index].active = false;
  }
  ensure_filter(owner);
}

// Showing keeps content loaded. Active also counts as showing, and runs it: a video
// starts from the top and a page that wasn't loaded opens from scratch.
void FallbackLayer::set_refs(Layer &layer, bool ready, bool active) {
  for (obs_source_t *source : {layer.child, layer.background}) {
    if (!source)
      continue;
    if (ready != layer.ready)
      ready ? obs_source_inc_showing(source) : obs_source_dec_showing(source);
    if (active != layer.active)
      active ? obs_source_inc_active(source) : obs_source_dec_active(source);
  }
  layer.ready = ready;
  layer.active = active;
}

void FallbackLayer::tick(float seconds, bool visible) {
  const int shown = shown_.load();
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < STATES; ++i) {
    Layer &layer = layers_[i];
    const bool wanted = i + 1 == shown && layer.spec.draws_itself();
    layer.rising = wanted;

    const int duration_ms = wanted ? layer.spec.anim_in_ms : layer.spec.anim_out_ms;
    const float step = duration_ms > 0 ? seconds * 1000.0f / duration_ms : 1.0f;
    layer.progress = std::clamp(layer.progress + (wanted ? step : -step), 0.0f, 1.0f);

    // Stays alive through its way out, unless the source itself has left the screen.
    // Sources tick in every scene, so a stream that's down elsewhere still gets here.
    set_refs(layer, visible && layer.spec.preloads(), visible && (wanted || layer.progress > 0.0f));
  }
}

static void fill_rect(const FallbackRect &rect, uint32_t abgr) {
  if (rect.width <= 0.0f || (abgr >> 24) == 0)
    return;
  vec4 color;
  vec4_from_rgba(&color, abgr);
  gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
  gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
  gs_matrix_push();
  gs_matrix_translate3f(rect.x, rect.y, 0.0f);
  while (gs_effect_loop(effect, "Solid"))
    gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(rect.width), static_cast<uint32_t>(rect.height));
  gs_matrix_pop();
}

static void draw_source(obs_source_t *source, const FallbackRect &rect) {
  const float width = static_cast<float>(obs_source_get_width(source));
  const float height = static_cast<float>(obs_source_get_height(source));
  if (rect.width <= 0.0f || width <= 0.0f || height <= 0.0f)
    return;
  gs_matrix_push();
  gs_matrix_translate3f(rect.x, rect.y, 0.0f);
  gs_matrix_scale3f(rect.width / width, rect.height / height, 1.0f);
  obs_source_video_render(source);
  gs_matrix_pop();
}

void FallbackLayer::draw_layer(const FallbackSpec &spec, obs_source_t *child,
                               obs_source_t *background, float progress, bool rising,
                               uint32_t width, uint32_t height) {
  const float source_width = static_cast<float>(width);
  const float source_height = static_cast<float>(height);
  const FallbackMotion motion = fallback_motion(rising ? spec.anim_in : spec.anim_out, progress,
                                                source_width, source_height);
  if (motion.alpha <= 0.0f)
    return;

  if (!texrender_)
    texrender_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
  if (!fade_effect_)
    fade_effect_ = gs_effect_create(FADE_EFFECT, "lss-fallback-fade", nullptr);
  gs_texrender_reset(texrender_);

  // Through a texture so that moving or oversized content stays inside the source.
  if (!gs_texrender_begin(texrender_, width, height))
    return;

  vec4 clear;
  vec4_zero(&clear);
  gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
  gs_ortho(0.0f, source_width, 0.0f, source_height, -100.0f, 100.0f);

  gs_blend_state_push();
  gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE,
                             GS_BLEND_INVSRCALPHA);
  gs_matrix_push();
  gs_matrix_translate3f(source_width / 2.0f + motion.dx, source_height / 2.0f + motion.dy, 0.0f);
  gs_matrix_scale3f(motion.scale, motion.scale, 1.0f);
  gs_matrix_translate3f(-source_width / 2.0f, -source_height / 2.0f, 0.0f);

  const FallbackRect whole{0.0f, 0.0f, source_width, source_height};
  const FallbackRect box = spec.uses_template()
                               ? whole
                               : fallback_box(spec.size_percent, spec.anchor, source_width, source_height);
  if (!spec.keep_picture)
    fill_rect(whole, 0xFF000000);
  fill_rect(box, spec.background_color);
  if (background)
    draw_source(background, box);

  if (child) {
    const float child_width = static_cast<float>(obs_source_get_width(child));
    const float child_height = static_cast<float>(obs_source_get_height(child));
    const FallbackRect rect =
        spec.uses_template()              ? whole
        : spec.kind == FallbackKind::Card ? fallback_text_rect(box, child_width, child_height, source_height)
                                          : fallback_rect(spec.fit, spec.size_percent, spec.anchor, child_width,
                                                          child_height, source_width, source_height);
    draw_source(child, rect);
  }

  gs_matrix_pop();
  gs_blend_state_pop();
  gs_texrender_end(texrender_);

  if (!fade_effect_)
    return;
  gs_texture_t *texture = gs_texrender_get_texture(texrender_);
  gs_effect_set_texture(gs_effect_get_param_by_name(fade_effect_, "image"), texture);
  gs_effect_set_float(gs_effect_get_param_by_name(fade_effect_, "alpha"), motion.alpha);
  gs_blend_state_push();
  gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
  while (gs_effect_loop(fade_effect_, "Draw"))
    gs_draw_sprite(texture, 0, width, height);
  gs_blend_state_pop();
}

void FallbackLayer::render(obs_source_t *filter, uint32_t width, uint32_t height) {
  // Always underneath: a fallback fading out has to reveal the stream.
  obs_source_skip_video_filter(filter);

  // What's leaving goes underneath, so what's arriving lands on top of it.
  for (const bool rising : {false, true}) {
    for (int i = 0; i < STATES; ++i) {
      FallbackSpec spec;
      obs_source_t *child = nullptr;
      obs_source_t *background = nullptr;
      float progress = 0.0f;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const Layer &layer = layers_[i];
        progress = layer.progress;
        if (progress <= 0.0f || layer.rising != rising || !layer.spec.draws_itself())
          continue;
        spec = layer.spec;
        child = obs_source_get_ref(layer.child);
        background = obs_source_get_ref(layer.background);
      }

      draw_layer(spec, child, background, progress, rising, width, height);
      obs_source_release(child);
      obs_source_release(background);
    }
  }
}

// The filter

struct FilterData {
  obs_source_t *self = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
};

static const char *filter_name(void *) {
  return obs_module_text("FallbackFilter");
}

static void *filter_create(obs_data_t *, obs_source_t *source) {
  auto *data = new FilterData;
  data->self = source;
  return data;
}

static void filter_destroy(void *param) {
  delete static_cast<FilterData *>(param);
}

static FallbackLayer *layer_of(obs_source_t *filter) {
  obs_source_t *target = obs_filter_get_target(filter);
  if (!target || std::strcmp(obs_source_get_unversioned_id(target), SOURCE_ID) != 0)
    return nullptr;
  auto *source = static_cast<LiveStreamSource *>(obs_obj_get_data(target));
  return source ? &source->fallback_layer() : nullptr;
}

// A source with no stream has no size, and the fallback still needs one. Keeping
// the last known size also stops the scene item from collapsing when a stream drops.
static void refresh_size(FilterData *data) {
  obs_source_t *target = obs_filter_get_target(data->self);
  const uint32_t width = target ? obs_source_get_base_width(target) : 0;
  const uint32_t height = target ? obs_source_get_base_height(target) : 0;
  if (width && height) {
    data->width = width;
    data->height = height;
  } else if (!data->width) {
    obs_video_info ovi = {};
    if (obs_get_video_info(&ovi)) {
      data->width = ovi.base_width;
      data->height = ovi.base_height;
    }
  }
}

static uint32_t filter_width(void *param) {
  auto *data = static_cast<FilterData *>(param);
  refresh_size(data);
  return data->width;
}

static uint32_t filter_height(void *param) {
  auto *data = static_cast<FilterData *>(param);
  refresh_size(data);
  return data->height;
}

static void filter_tick(void *param, float seconds) {
  obs_source_t *self = static_cast<FilterData *>(param)->self;
  if (FallbackLayer *layer = layer_of(self))
    layer->tick(seconds, obs_source_showing(obs_filter_get_target(self)));
}

static void filter_render(void *param, gs_effect_t *) {
  auto *data = static_cast<FilterData *>(param);
  FallbackLayer *layer = layer_of(data->self);
  refresh_size(data);
  if (layer && data->width && data->height)
    layer->render(data->self, data->width, data->height);
  else
    obs_source_skip_video_filter(data->self);
}

void FallbackLayer::register_filter() {
  obs_source_info info = {};
  info.id = FILTER_ID;
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CAP_DISABLED;
  info.get_name = filter_name;
  info.create = filter_create;
  info.destroy = filter_destroy;
  info.get_width = filter_width;
  info.get_height = filter_height;
  info.video_tick = filter_tick;
  info.video_render = filter_render;
  obs_register_source(&info);
}

} // namespace lss
