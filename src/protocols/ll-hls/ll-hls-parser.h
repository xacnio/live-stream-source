// ll-hls-parser.h - HLS playlist parser (standard + LL-HLS extensions)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lss {

struct HlsPart {
  std::string uri;          // Resolved absolute URI
  double duration = 0.0;    // Duration in seconds
  bool independent = false; // INDEPENDENT=YES (contains keyframe)
  int64_t byterange_offset = -1;
  int64_t byterange_length = -1;
};

struct HlsSegment {
  std::string uri;       // Resolved absolute URI
  double duration = 0.0; // Duration in seconds (#EXTINF)
  std::vector<HlsPart> parts;
  int64_t media_sequence = 0;
};

struct HlsPreloadHint {
  std::string uri;
  std::string type; // "PART" or "MAP"
  int64_t byterange_offset = -1;
  int64_t byterange_length = -1;
  bool valid = false;
};

// IVS prefetch entry (#EXT-X-PREFETCH)
struct HlsPrefetch {
  std::string uri; // Resolved absolute URI
};

struct HlsServerControl {
  bool can_block_reload = false;
  double part_hold_back = 0.0; // seconds
  double hold_back = 0.0;      // seconds
  bool can_skip_until = false;
  double skip_until = 0.0; // seconds
};

struct HlsRendition {
  std::string uri;
  int bandwidth = 0;
  int width = 0;
  int height = 0;
  std::string codecs;
  std::string name;
};

class LLHlsPlaylist {
public:
  LLHlsPlaylist() = default;

  bool parse(const std::string &playlist_text, const std::string &base_url);

  int64_t media_sequence() const { return media_sequence_; }
  double target_duration() const { return target_duration_; }
  double part_target() const { return part_target_; }
  const HlsServerControl &server_control() const { return server_control_; }
  const std::vector<HlsSegment> &segments() const { return segments_; }
  const HlsPreloadHint &preload_hint() const { return preload_hint_; }
  const std::string &init_segment_uri() const { return init_segment_uri_; }

  const std::vector<HlsPrefetch> &prefetch_urls() const {
    return prefetch_urls_;
  }

  bool is_master() const { return is_master_; }
  const std::vector<HlsRendition> &renditions() const { return renditions_; }

  int64_t latest_msn() const;

  int latest_part_index() const;

  // Builds ?_HLS_msn=X&_HLS_part=Y for the next expected part.
  std::string blocking_reload_url(const std::string &base_url) const;

  bool is_ll_hls() const;

private:
  static std::string resolve_uri(const std::string &relative,
                                 const std::string &base);

  static std::string get_attribute(const std::string &line,
                                   const std::string &key);
  static double get_attribute_double(const std::string &line,
                                     const std::string &key,
                                     double default_val = 0.0);
  static int64_t get_attribute_int(const std::string &line,
                                   const std::string &key,
                                   int64_t default_val = -1);
  static bool get_attribute_bool(const std::string &line,
                                 const std::string &key);

  int64_t media_sequence_ = 0;
  double target_duration_ = 0.0;
  double part_target_ = 0.0;
  HlsServerControl server_control_;
  std::vector<HlsSegment> segments_;
  HlsPreloadHint preload_hint_;
  std::string init_segment_uri_;
  std::vector<HlsPrefetch> prefetch_urls_;

  bool is_master_ = false;
  std::vector<HlsRendition> renditions_;
};

} // namespace lss
