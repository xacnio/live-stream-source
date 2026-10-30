// fallback-settings.h - how each fallback is named in a source's settings
#pragma once

#include <string>

namespace lss {

inline constexpr const char *FALLBACK_DISCONNECT = "fb_disconnect";
inline constexpr const char *FALLBACK_LOW = "fb_low";
inline constexpr const char *FALLBACK_LOADING = "fb_loading";

struct FallbackTemplateEntry {
  const char *prefix;
  const char *file;
  const char *label;
};

inline constexpr FallbackTemplateEntry FALLBACK_TEMPLATES[] = {
    {FALLBACK_DISCONNECT, "disconnect-signal", "FallbackTemplateDisconnectSignal"},
    {FALLBACK_DISCONNECT, "disconnect-static", "FallbackTemplateDisconnectStatic"},
    {FALLBACK_DISCONNECT, "disconnect-card", "FallbackTemplateDisconnectCard"},
    {FALLBACK_LOW, "low-badge", "FallbackTemplateLowBadge"},
    {FALLBACK_LOW, "low-bars", "FallbackTemplateLowBars"},
    {FALLBACK_LOW, "low-banner", "FallbackTemplateLowBanner"},
    {FALLBACK_LOADING, "loading-ring", "FallbackTemplateLoadingRing"},
    {FALLBACK_LOADING, "loading-dots", "FallbackTemplateLoadingDots"},
    {FALLBACK_LOADING, "loading-bar", "FallbackTemplateLoadingBar"},
};

inline std::string fallback_key(const char *prefix, const char *field) {
  return std::string(prefix) + "_" + field;
}

} // namespace lss
