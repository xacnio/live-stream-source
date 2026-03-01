// whep-signaling.h - HTTP-based WHEP offer/answer exchange
#pragma once

#include <string>

namespace lss {

struct WhepSession {
  std::string sdp_answer;   // SDP answer body from server
  std::string resource_url; // Location header -> used for DELETE teardown
  int http_status = 0;      // HTTP status code (201 = success)
  std::string error;        // Error message if failed
};

class WhepSignaling {
public:
  static WhepSession offer(const std::string &whep_url,
                           const std::string &sdp_offer,
                           const std::string &bearer_token = "");

  static bool teardown(const std::string &resource_url,
                       const std::string &bearer_token = "");
};

} // namespace lss
