// fallback-template.h - fills in the HTML templates shipped with the plugin
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace lss {

inline std::string html_escape(const std::string &text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '"': out += "&quot;"; break;
    case '\'': out += "&#39;"; break;
    default: out += c;
    }
  }
  return out;
}

// OBS keeps colours as 0xAABBGGRR.
inline std::string css_color(uint32_t abgr) {
  const unsigned alpha = ((abgr >> 24) & 0xFF) * 100 / 255;
  char text[40];
  std::snprintf(text, sizeof(text), "rgba(%u,%u,%u,%u.%02u)", abgr & 0xFF, (abgr >> 8) & 0xFF,
                (abgr >> 16) & 0xFF, alpha / 100, alpha % 100);
  return text;
}

inline std::string replace_all(std::string text, const std::string &token,
                               const std::string &value) {
  size_t at = 0;
  while ((at = text.find(token, at)) != std::string::npos) {
    text.replace(at, token.size(), value);
    at += value.size();
  }
  return text;
}

inline std::string render_fallback_template(const std::string &html, const std::string &text,
                                            uint32_t color, int scale_percent) {
  std::string page = replace_all(html, "{{text}}", html_escape(text));
  page = replace_all(page, "{{color}}", css_color(color));
  return replace_all(page, "{{scale}}", std::to_string(scale_percent));
}

} // namespace lss
