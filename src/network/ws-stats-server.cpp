#include "network/ws-stats-server.h"
#include "core/common.h"
#include <sstream>
#include <vector>
extern "C" {
#include <libavutil/base64.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#else
#include <cerrno>
#define closesocket close
#define WSAGetLastError() errno
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

namespace lss {

static const char *WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

WsStatsServer &WsStatsServer::instance() {
  static WsStatsServer s;
  return s;
}

WsStatsServer::WsStatsServer() {
#ifdef _WIN32
  WSADATA wsa;
  int res = WSAStartup(MAKEWORD(2, 2), &wsa);
  if (res != 0) {
    lss_log_error("WebSocket Server: WSAStartup failed with error %d", res);
  }
#endif
}

WsStatsServer::~WsStatsServer() {
  stop();
#ifdef _WIN32
  WSACleanup();
#endif
}

void WsStatsServer::add_ref() {
  std::lock_guard<std::mutex> lock(ref_mutex_);
  ref_count_++;
  if (ref_count_ == 1) {
    start();
  }
}

void WsStatsServer::configure(int port, const std::string &bind_ip) {
  std::lock_guard<std::mutex> lock(ref_mutex_);
  bool changed = (port_ != port || bind_ip_ != bind_ip);
  port_ = port;
  bind_ip_ = bind_ip;

  if (changed && running_) {
    stop();
    start();
  }
}

void WsStatsServer::release() {
  std::lock_guard<std::mutex> lock(ref_mutex_);
  if (ref_count_ > 0)
    ref_count_--;
  if (ref_count_ == 0) {
    stop();
  }
}

void WsStatsServer::start() {
  if (running_)
    return;
  running_ = true;
  thread_ = std::thread(&WsStatsServer::run, this);
}

void WsStatsServer::stop() {
  running_ = false;
  if (listen_socket_ != INVALID_SOCKET) {
    closesocket(listen_socket_);
    listen_socket_ = INVALID_SOCKET;
  }
  if (thread_.joinable())
    thread_.join();

  std::lock_guard<std::mutex> lock(clients_mutex_);
  for (auto s : clients_) {
    closesocket(s);
  }
  clients_.clear();

  std::lock_guard<std::mutex> slock(sources_mutex_);
  sources_.clear();
}

void WsStatsServer::update_source(const std::string &source_name,
                                  const std::string &json_data) {
  {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    sources_[source_name] = json_data;
  }
  broadcast_all();
}

void WsStatsServer::remove_source(const std::string &source_name) {
  std::lock_guard<std::mutex> lock(sources_mutex_);
  sources_.erase(source_name);
}

void WsStatsServer::register_command_handler(const std::string &source_name,
                                             CommandCallback cb) {
  std::lock_guard<std::mutex> lock(cmd_handlers_mutex_);
  cmd_handlers_[source_name] = std::move(cb);
}

void WsStatsServer::unregister_command_handler(const std::string &source_name) {
  std::lock_guard<std::mutex> lock(cmd_handlers_mutex_);
  cmd_handlers_.erase(source_name);
}

void WsStatsServer::register_connect_callback(const std::string &source_name,
                                               ConnectCallback cb) {
  std::lock_guard<std::mutex> lock(connect_cbs_mutex_);
  connect_callbacks_[source_name] = std::move(cb);
}

void WsStatsServer::unregister_connect_callback(const std::string &source_name) {
  std::lock_guard<std::mutex> lock(connect_cbs_mutex_);
  connect_callbacks_.erase(source_name);
}

std::string WsStatsServer::decode_ws_frame(const char *data, int len) {
  lss_log_debug("WsServer: decoding frame len=%d, b0=0x%02X b1=0x%02X",
               len, len > 0 ? (uint8_t)data[0] : 0,
               len > 1 ? (uint8_t)data[1] : 0);
  if (len < 2)
    return "";

  const uint8_t b0 = (uint8_t)data[0];
  const uint8_t b1 = (uint8_t)data[1];

  const int opcode = b0 & 0x0F;
  if (opcode == 0x8)
    return "";  // close frame
  if (opcode != 0x1 && opcode != 0x0)
    return "";  // not text or continuation

  const bool masked = (b1 & 0x80) != 0;
  uint64_t payload_len = b1 & 0x7F;
  int offset = 2;

  if (payload_len == 126) {
    if (len < 4)
      return "";
    payload_len = ((uint64_t)(uint8_t)data[2] << 8) | (uint8_t)data[3];
    offset = 4;
  } else if (payload_len == 127) {
    if (len < 10)
      return "";
    payload_len = 0;
    for (int i = 0; i < 8; i++)
      payload_len = (payload_len << 8) | (uint8_t)data[2 + i];
    offset = 10;
  }

  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked) {
    if (len < offset + 4)
      return "";
    mask[0] = (uint8_t)data[offset];
    mask[1] = (uint8_t)data[offset + 1];
    mask[2] = (uint8_t)data[offset + 2];
    mask[3] = (uint8_t)data[offset + 3];
    offset += 4;
  }

  if ((int64_t)len < (int64_t)(offset + payload_len))
    return "";

  std::string result(payload_len, '\0');
  for (uint64_t i = 0; i < payload_len; i++)
    result[i] = masked ? (char)((uint8_t)data[offset + i] ^ mask[i % 4])
                       : data[offset + i];

  return result;
}

void WsStatsServer::handle_client_message(const std::string &msg) {
  lss_log_debug("WsServer: incoming message: %s", msg.c_str());

  obs_data_t *obj = obs_data_create_from_json(msg.c_str());
  if (!obj) {
    lss_log_warn("WsServer: JSON parse failed for: %s", msg.c_str());
    return;
  }

  const char *cmd_raw = obs_data_get_string(obj, "command");
  const char *src_raw = obs_data_get_string(obj, "source");

  std::string cmd = cmd_raw ? cmd_raw : "";
  std::string src = src_raw ? src_raw : "";
  obs_data_release(obj);

  if (cmd.empty() || src.empty()) {
    lss_log_warn("WsServer: message missing 'command' or 'source'");
    return;
  }

  lss_log_info("WsServer: command='%s' source='%s'", cmd.c_str(), src.c_str());

  CommandCallback cb;
  {
    std::lock_guard<std::mutex> lock(cmd_handlers_mutex_);
    auto it = cmd_handlers_.find(src);
    if (it != cmd_handlers_.end())
      cb = it->second;
    else
      lss_log_warn("WsServer: no handler for source '%s' (registered: %d)",
                   src.c_str(), (int)cmd_handlers_.size());
  }
  if (cb)
    cb(cmd);
}

void WsStatsServer::broadcast_all() {
  std::string combined;
  {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    if (sources_.empty())
      return;

    obs_data_t *root = obs_data_create();
    obs_data_t *sources = obs_data_create();

    for (auto &kv : sources_) {
      obs_data_t *val = obs_data_create_from_json(kv.second.c_str());
      if (val) {
        obs_data_set_obj(sources, kv.first.c_str(), val);
        obs_data_release(val);
      }
    }

    obs_data_set_obj(root, "sources", sources);
    obs_data_release(sources);

    const char *dump = obs_data_get_json(root);
    if (dump) {
      combined = dump;
    }
    obs_data_release(root);
  }

  if (!combined.empty())
    send_ws_frame(combined);
}

void WsStatsServer::send_ws_frame_raw(const uint8_t *data, size_t len,
                                       uint8_t opcode) {
  // Shared frame writer used by both the text broadcast and the binary
  // preview-encoder pushes. opcode = 0x81 (FIN+Text) or 0x82 (FIN+Binary).
  std::lock_guard<std::mutex> lock(clients_mutex_);
  if (clients_.empty())
    return;

  std::vector<uint8_t> frame;
  frame.reserve(len + 10);
  frame.push_back(opcode);

  if (len < 126) {
    frame.push_back((uint8_t)len);
  } else if (len < 65536) {
    frame.push_back(126);
    frame.push_back((len >> 8) & 0xFF);
    frame.push_back(len & 0xFF);
  } else {
    frame.push_back(127);
    for (int i = 7; i >= 0; --i)
      frame.push_back((len >> (i * 8)) & 0xFF);
  }

  frame.insert(frame.end(), data, data + len);

  auto it = clients_.begin();
  while (it != clients_.end()) {
    SOCKET s = *it;
    const char *ptr = (const char *)frame.data();
    int remaining = (int)frame.size();
    bool ok = true;
    while (remaining > 0) {
#ifdef _WIN32
      int sent = send(s, ptr, remaining, 0);
#else
      int sent = send(s, ptr, remaining, MSG_NOSIGNAL);
#endif
      if (sent == SOCKET_ERROR) {
        ok = false;
        break;
      }
      ptr += sent;
      remaining -= sent;
    }
    if (!ok) {
      closesocket(s);
      it = clients_.erase(it);
    } else {
      ++it;
    }
  }
}

void WsStatsServer::send_ws_frame(const std::string &message) {
  send_ws_frame_raw(reinterpret_cast<const uint8_t *>(message.data()),
                    message.size(), 0x81 /* FIN + Text */);
}

void WsStatsServer::send_binary(const uint8_t *data, size_t len) {
  send_ws_frame_raw(data, len, 0x82 /* FIN + Binary */);
}

void WsStatsServer::run() {
  listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_socket_ == INVALID_SOCKET) {
    lss_log_error("WebSocket Server: Failed to create socket, error %d",
                  WSAGetLastError());
    running_ = false;
    return;
  }

  int opt = 1;
  setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt,
             sizeof(opt));

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  if (bind_ip_ == "0.0.0.0" || bind_ip_.empty()) {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
    addr.sin_addr.s_addr = inet_addr(bind_ip_.c_str());
  }
  addr.sin_port = htons(static_cast<unsigned short>(port_));

  if (bind(listen_socket_, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
    lss_log_warn("WebSocket Server: Failed to bind to port %d (error %d). Port "
                 "might be in use.",
                 port_, WSAGetLastError());
    closesocket(listen_socket_);
    listen_socket_ = INVALID_SOCKET;
    running_ = false;
    return;
  }

  if (listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR) {
    lss_log_error("WebSocket Server: Listen failed with error %d",
                  WSAGetLastError());
    closesocket(listen_socket_);
    listen_socket_ = INVALID_SOCKET;
    running_ = false;
    return;
  }

  lss_log_info("WebSocket Server: Listening on ws://%s:%d",
               bind_ip_.empty() ? "0.0.0.0" : bind_ip_.c_str(), port_);

  while (running_) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(listen_socket_, &read_fds);

    int nfds = (int)listen_socket_;

    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      for (SOCKET client : clients_) {
        FD_SET(client, &read_fds);
        if ((int)client > nfds) {
          nfds = (int)client;
        }
      }
    }
    nfds += 1;

    timeval timeout = {0, 500000}; // 500ms
    int activity = select(nfds, &read_fds, nullptr, nullptr, &timeout);

    if (activity == SOCKET_ERROR) {
      if (running_)
        lss_log_error("WebSocket Server: select() error %d", WSAGetLastError());
      break;
    }

    if (activity > 0) {
      if (FD_ISSET(listen_socket_, &read_fds)) {
        SOCKET client = accept(listen_socket_, nullptr, nullptr);
        if (client != INVALID_SOCKET) {
          lss_log_info("WebSocket Server: New connection accepted");
          handle_handshake(client);
        }
      }

      std::vector<std::string> incoming;
      {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.begin();
        while (it != clients_.end()) {
          SOCKET client = *it;
          if (FD_ISSET(client, &read_fds)) {
            char buf[4096];
            int bytes = recv(client, buf, sizeof(buf), 0);
            if (bytes <= 0) {
              closesocket(client);
              it = clients_.erase(it);
              continue;
            }
            std::string msg = decode_ws_frame(buf, bytes);
            if (!msg.empty())
              incoming.push_back(std::move(msg));
          }
          ++it;
        }
      }
      for (auto &msg : incoming)
        handle_client_message(msg);
    }
  }

  if (listen_socket_ != INVALID_SOCKET) {
    closesocket(listen_socket_);
    listen_socket_ = INVALID_SOCKET;
  }
  lss_log_info("WebSocket Server: Thread exiting");
}

void WsStatsServer::handle_handshake(SOCKET client) {
  char buffer[4096];
  int bytes = recv(client, buffer, sizeof(buffer) - 1, 0);
  if (bytes <= 0) {
    closesocket(client);
    return;
  }
  buffer[bytes] = '\0';
  std::string request(buffer);

  size_t key_pos = request.find("Sec-WebSocket-Key: ");
  if (key_pos == std::string::npos) {
    lss_log_warn("WebSocket Server: Invalid handshake request (no key)");
    closesocket(client);
    return;
  }

  size_t key_end = request.find("\r\n", key_pos);
  std::string key = request.substr(key_pos + 19, key_end - (key_pos + 19));

  std::string accept_input = key + WS_MAGIC;
  uint8_t hash[20];
  AVSHA *sha = av_sha_alloc();
  av_sha_init(sha, 160);
  av_sha_update(sha, (const uint8_t *)accept_input.c_str(),
                accept_input.length());
  av_sha_final(sha, hash);
  av_free(sha);

  char accept_key[128];
  av_base64_encode(accept_key, sizeof(accept_key), hash, 20);

  std::ostringstream response;
  response << "HTTP/1.1 101 Switching Protocols\r\n"
           << "Upgrade: websocket\r\n"
           << "Connection: Upgrade\r\n"
           << "Sec-WebSocket-Accept: " << accept_key << "\r\n\r\n";

  std::string resp_str = response.str();
#ifdef _WIN32
  send(client, resp_str.c_str(), (int)resp_str.length(), 0);
#else
  send(client, resp_str.c_str(), (int)resp_str.length(), MSG_NOSIGNAL);
#endif

  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.push_back(client);
  }
  lss_log_info("WebSocket Server: Handshake complete, client added");

  // Notify all sources so they can reset per-connection state (e.g. preview
  // flags). Callbacks run without any WsStatsServer lock held.
  std::vector<ConnectCallback> cbs;
  {
    std::lock_guard<std::mutex> lock(connect_cbs_mutex_);
    for (auto &kv : connect_callbacks_)
      cbs.push_back(kv.second);
  }
  for (auto &cb : cbs)
    cb();
}

} // namespace lss
