// ws-protocol.h - request parsing that doesn't depend on sockets
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lss {

// HTTP header names are case-insensitive, and proxies differ: nginx keeps
// Sec-WebSocket-Key, Go-based ones like Caddy send Sec-Websocket-Key.
inline std::string find_http_header(const std::string &request,
                                    const std::string &name) {
  std::string lower = request;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  std::string needle = "\r\n" + name + ":";
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });

  const size_t pos = lower.find(needle);
  if (pos == std::string::npos)
    return "";

  size_t start = pos + needle.size();
  const size_t end = request.find("\r\n", start);
  if (end == std::string::npos)
    return "";
  while (start < end && request[start] == ' ')
    ++start;
  return request.substr(start, end - start);
}

// Decodes one client frame from the front of data. Returns how many bytes it
// took, or 0 if the frame hasn't fully arrived. TCP keeps no message boundaries,
// so a single read can hold several frames or only part of one. Text ends up
// in *text; control frames consume their bytes and leave it empty.
inline size_t parse_ws_frame(const char *data, size_t len, std::string *text) {
  text->clear();
  if (len < 2)
    return 0;

  const uint8_t b0 = static_cast<uint8_t>(data[0]);
  const uint8_t b1 = static_cast<uint8_t>(data[1]);
  const int opcode = b0 & 0x0F;
  const bool masked = (b1 & 0x80) != 0;

  uint64_t payload_len = b1 & 0x7F;
  size_t offset = 2;

  if (payload_len == 126) {
    if (len < 4)
      return 0;
    payload_len = (static_cast<uint64_t>(static_cast<uint8_t>(data[2])) << 8) |
                  static_cast<uint8_t>(data[3]);
    offset = 4;
  } else if (payload_len == 127) {
    if (len < 10)
      return 0;
    payload_len = 0;
    for (int i = 0; i < 8; ++i)
      payload_len = (payload_len << 8) | static_cast<uint8_t>(data[2 + i]);
    offset = 10;
  }

  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked) {
    if (len < offset + 4)
      return 0;
    for (int i = 0; i < 4; ++i)
      mask[i] = static_cast<uint8_t>(data[offset + i]);
    offset += 4;
  }

  if (payload_len > len - offset)
    return 0;

  if (opcode == 0x1 || opcode == 0x0) {
    text->resize(static_cast<size_t>(payload_len));
    for (size_t i = 0; i < payload_len; ++i)
      (*text)[i] = static_cast<char>(static_cast<uint8_t>(data[offset + i]) ^ mask[i % 4]);
  }
  return offset + static_cast<size_t>(payload_len);
}

// Addresses in host order. A client on this machine shows up either with a loopback
// address or, when it dials one of the machine's own addresses, with that same address
// on both ends of the connection. Anyone else needs the access key.
inline bool is_same_machine(uint32_t peer, uint32_t local) {
  return (peer >> 24) == 127 || peer == local;
}

// Browsers put an Origin on every WebSocket, programs don't. Any site open on this
// machine can reach its loopback address, and a sandboxed frame on one sends the same
// "null" a local file does, so a browser's connection proves nothing by itself.
inline bool trusted_without_proof(bool same_machine, const std::string &origin) {
  return same_machine && origin.empty();
}

// Takes the same time whichever byte differs, so a wrong key reveals nothing.
inline bool access_key_matches(const std::string &given,
                               const std::string &expected) {
  if (given.size() != expected.size())
    return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < expected.size(); ++i)
    diff |= static_cast<unsigned char>(given[i] ^ expected[i]);
  return diff == 0;
}

} // namespace lss
