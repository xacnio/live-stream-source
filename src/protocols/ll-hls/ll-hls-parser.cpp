// ll-hls-parser.cpp
#include "protocols/ll-hls/ll-hls-parser.h"

#include <sstream>

namespace lss {


std::string LLHlsPlaylist::resolve_uri(const std::string &relative,
                                       const std::string &base) {
  if (relative.empty())
    return {};

  // Already absolute?
  if (relative.rfind("http://", 0) == 0 || relative.rfind("https://", 0) == 0)
    return relative;

  auto pos = base.rfind('/');
  if (pos == std::string::npos)
    return relative;

  return base.substr(0, pos + 1) + relative;
}


std::string LLHlsPlaylist::get_attribute(const std::string &line,
                                         const std::string &key) {
  // Look for KEY= or KEY="value"
  std::string search = key + "=";
  auto pos = line.find(search);
  if (pos == std::string::npos)
    return {};

  pos += search.length();
  if (pos >= line.length())
    return {};

  // Quoted value?
  if (line[pos] == '"') {
    auto end = line.find('"', pos + 1);
    if (end == std::string::npos)
      return line.substr(pos + 1);
    return line.substr(pos + 1, end - pos - 1);
  }

  // Unquoted  - read until ',' or end of line
  auto end = line.find(',', pos);
  if (end == std::string::npos)
    return line.substr(pos);
  return line.substr(pos, end - pos);
}

double LLHlsPlaylist::get_attribute_double(const std::string &line,
                                           const std::string &key,
                                           double default_val) {
  std::string val = get_attribute(line, key);
  if (val.empty())
    return default_val;
  try {
    return std::stod(val);
  } catch (...) {
    return default_val;
  }
}

int64_t LLHlsPlaylist::get_attribute_int(const std::string &line,
                                         const std::string &key,
                                         int64_t default_val) {
  std::string val = get_attribute(line, key);
  if (val.empty())
    return default_val;
  try {
    return std::stoll(val);
  } catch (...) {
    return default_val;
  }
}

bool LLHlsPlaylist::get_attribute_bool(const std::string &line,
                                       const std::string &key) {
  std::string val = get_attribute(line, key);
  return (val == "YES" || val == "yes" || val == "1");
}


bool LLHlsPlaylist::parse(const std::string &playlist_text,
                          const std::string &base_url) {
  segments_.clear();
  renditions_.clear();
  prefetch_urls_.clear();
  preload_hint_ = {};
  server_control_ = {};
  init_segment_uri_.clear();
  is_master_ = false;
  media_sequence_ = 0;
  target_duration_ = 0.0;
  part_target_ = 0.0;

  if (playlist_text.empty())
    return false;

  std::istringstream stream(playlist_text);
  std::string line;

  if (!std::getline(stream, line))
    return false;

  if (!line.empty() && line.back() == '\r')
    line.pop_back();

  if (line.find("#EXTM3U") == std::string::npos)
    return false;

  double pending_duration = -1.0;
  std::vector<HlsPart> pending_parts;
  int64_t current_msn = 0;
  bool has_msn = false;

  HlsRendition pending_rendition;
  bool has_pending_rendition = false;

  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line.empty())
      continue;

    if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
      is_master_ = true;
      pending_rendition = {};
      pending_rendition.bandwidth =
          static_cast<int>(get_attribute_int(line, "BANDWIDTH", 0));
      std::string resolution = get_attribute(line, "RESOLUTION");
      if (!resolution.empty()) {
        auto xpos = resolution.find('x');
        if (xpos != std::string::npos) {
          try {
            pending_rendition.width = std::stoi(resolution.substr(0, xpos));
            pending_rendition.height = std::stoi(resolution.substr(xpos + 1));
          } catch (...) {
          }
        }
      }
      pending_rendition.codecs = get_attribute(line, "CODECS");
      pending_rendition.name = get_attribute(line, "NAME");
      has_pending_rendition = true;
      continue;
    }

    // EXT-X-TARGETDURATION
    if (line.rfind("#EXT-X-TARGETDURATION:", 0) == 0) {
      try {
        target_duration_ = std::stod(
            line.substr(std::string("#EXT-X-TARGETDURATION:").length()));
      } catch (...) {
      }
      continue;
    }

    // EXT-X-MEDIA-SEQUENCE
    if (line.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
      try {
        media_sequence_ = std::stoll(
            line.substr(std::string("#EXT-X-MEDIA-SEQUENCE:").length()));
        current_msn = media_sequence_;
        has_msn = true;
      } catch (...) {
      }
      continue;
    }

    // EXT-X-SERVER-CONTROL
    if (line.rfind("#EXT-X-SERVER-CONTROL:", 0) == 0) {
      server_control_.can_block_reload =
          get_attribute_bool(line, "CAN-BLOCK-RELOAD");
      server_control_.part_hold_back =
          get_attribute_double(line, "PART-HOLD-BACK", 0.0);
      server_control_.hold_back = get_attribute_double(line, "HOLD-BACK", 0.0);
      std::string skip_val = get_attribute(line, "CAN-SKIP-UNTIL");
      if (!skip_val.empty()) {
        server_control_.can_skip_until = true;
        try {
          server_control_.skip_until = std::stod(skip_val);
        } catch (...) {
          server_control_.skip_until = 0.0;
        }
      }
      continue;
    }

    // EXT-X-PART-INF
    if (line.rfind("#EXT-X-PART-INF:", 0) == 0) {
      part_target_ = get_attribute_double(line, "PART-TARGET", 0.0);
      continue;
    }

    // EXT-X-MAP (init segment)
    if (line.rfind("#EXT-X-MAP:", 0) == 0) {
      std::string uri = get_attribute(line, "URI");
      if (!uri.empty()) {
        init_segment_uri_ = resolve_uri(uri, base_url);
      }
      continue;
    }

    // EXT-X-PART
    if (line.rfind("#EXT-X-PART:", 0) == 0) {
      HlsPart part;
      std::string uri = get_attribute(line, "URI");
      part.uri = resolve_uri(uri, base_url);
      part.duration = get_attribute_double(line, "DURATION", 0.0);
      part.independent = get_attribute_bool(line, "INDEPENDENT");

      // BYTERANGE support
      std::string byterange = get_attribute(line, "BYTERANGE");
      if (!byterange.empty()) {
        auto at = byterange.find('@');
        if (at != std::string::npos) {
          try {
            part.byterange_length = std::stoll(byterange.substr(0, at));
            part.byterange_offset = std::stoll(byterange.substr(at + 1));
          } catch (...) {
          }
        } else {
          try {
            part.byterange_length = std::stoll(byterange);
          } catch (...) {
          }
        }
      }

      pending_parts.push_back(std::move(part));
      continue;
    }

    // EXT-X-PREFETCH (Amazon IVS)
    // Format: #EXT-X-PREFETCH:<url>
    // These are partial segments at the live edge.
    if (line.rfind("#EXT-X-PREFETCH:", 0) == 0) {
      std::string uri = line.substr(std::string("#EXT-X-PREFETCH:").length());
      // Trim whitespace
      while (!uri.empty() && (uri.back() == ' ' || uri.back() == '\t'))
        uri.pop_back();
      if (!uri.empty()) {
        HlsPrefetch pf;
        pf.uri = resolve_uri(uri, base_url);
        prefetch_urls_.push_back(std::move(pf));
      }
      continue;
    }

    // EXT-X-PRELOAD-HINT
    if (line.rfind("#EXT-X-PRELOAD-HINT:", 0) == 0) {
      preload_hint_ = {};
      preload_hint_.type = get_attribute(line, "TYPE");
      std::string uri = get_attribute(line, "URI");
      preload_hint_.uri = resolve_uri(uri, base_url);
      preload_hint_.byterange_offset =
          get_attribute_int(line, "BYTERANGE-START", -1);
      preload_hint_.byterange_length =
          get_attribute_int(line, "BYTERANGE-LENGTH", -1);
      preload_hint_.valid = !preload_hint_.uri.empty();
      continue;
    }

    // EXTINF (segment duration)
    if (line.rfind("#EXTINF:", 0) == 0) {
      std::string dur_str = line.substr(std::string("#EXTINF:").length());
      // Remove trailing comma + title
      auto comma = dur_str.find(',');
      if (comma != std::string::npos)
        dur_str = dur_str.substr(0, comma);
      try {
        pending_duration = std::stod(dur_str);
      } catch (...) {
        pending_duration = 0.0;
      }
      continue;
    }

    if (line[0] != '#') {
      if (has_pending_rendition) {
        // Master playlist URI line
        pending_rendition.uri = resolve_uri(line, base_url);
        renditions_.push_back(std::move(pending_rendition));
        has_pending_rendition = false;
        continue;
      }

      if (pending_duration >= 0.0) {
        // Media segment URI
        HlsSegment seg;
        seg.uri = resolve_uri(line, base_url);
        seg.duration = pending_duration;
        seg.media_sequence = current_msn;
        seg.parts = std::move(pending_parts);
        pending_parts.clear();
        pending_duration = -1.0;

        segments_.push_back(std::move(seg));
        current_msn++;
      }
      continue;
    }

  }

  // If there are trailing pending_parts (for the segment being built),
  // create a partial segment entry (no URI yet  - segment in progress)
  if (!pending_parts.empty()) {
    HlsSegment seg;
    seg.uri.clear(); // segment not yet complete
    seg.duration = 0.0;
    seg.media_sequence = current_msn;
    seg.parts = std::move(pending_parts);
    segments_.push_back(std::move(seg));
  }

  return true;
}


int64_t LLHlsPlaylist::latest_msn() const {
  if (segments_.empty())
    return -1;
  return segments_.back().media_sequence;
}

int LLHlsPlaylist::latest_part_index() const {
  if (segments_.empty())
    return -1;
  const auto &last_seg = segments_.back();
  if (last_seg.parts.empty())
    return -1;
  return static_cast<int>(last_seg.parts.size()) - 1;
}

std::string
LLHlsPlaylist::blocking_reload_url(const std::string &base_url) const {
  // Compute the next expected MSN and part index
  int64_t next_msn = latest_msn();
  int next_part = latest_part_index() + 1;

  if (next_msn < 0) {
    // No segments found  - just return base URL
    return base_url;
  }

  // If there's a preload hint, next is the preloaded part
  // Otherwise, next part after the latest known one

  std::string url = base_url;
  auto q = url.find('?');
  if (q != std::string::npos) {
    // Keep only non-_HLS params
    std::string base_part = url.substr(0, q);
    std::string query = url.substr(q + 1);
    std::string new_query;

    std::istringstream qs(query);
    std::string param;
    while (std::getline(qs, param, '&')) {
      if (param.rfind("_HLS_msn", 0) == 0 || param.rfind("_HLS_part", 0) == 0)
        continue;
      if (!new_query.empty())
        new_query += '&';
      new_query += param;
    }

    if (new_query.empty()) {
      url = base_part;
    } else {
      url = base_part + "?" + new_query;
    }
  }

  char sep = (url.find('?') == std::string::npos) ? '?' : '&';
  url += sep;
  url += "_HLS_msn=" + std::to_string(next_msn);
  url += "&_HLS_part=" + std::to_string(next_part);

  return url;
}

bool LLHlsPlaylist::is_ll_hls() const {
  // Has prefetch URLs (Amazon IVS), parts, or preload hint => LL-HLS
  if (!prefetch_urls_.empty())
    return true;
  if (preload_hint_.valid)
    return true;
  for (const auto &seg : segments_) {
    if (!seg.parts.empty())
      return true;
  }
  return false;
}

} // namespace lss
