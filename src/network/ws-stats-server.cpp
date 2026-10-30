#include "network/ws-stats-server.h"
#include "core/common.h"
#include "network/ws-protocol.h"
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
#define SD_BOTH SHUT_RDWR
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

namespace lss {

static const char *WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// How long a new client has to present the access key.
static constexpr int64_t AUTH_TIMEOUT_MS = 5000;

// After a wrong key, that address waits this long. The key can't be guessed anyway,
// but this also stops anyone burning CPU on attempts.
static constexpr int64_t AUTH_BACKOFF_MS = 1000;

// More than any panel needs, and well inside what select() can watch.
static constexpr size_t MAX_CLIENTS = 32;

// The handshake is a short HTTP request.
static constexpr size_t MAX_REQUEST_BYTES = 8 * 1024;

// How long a client that stopped reading can hold up the sender, then it's
// dropped. Only other clients ever wait on this, never the stream.
static constexpr int SEND_TIMEOUT_MS = 500;

// Frames waiting to go out. Past this the oldest go: a client behind by this
// much wants the current state, not history.
static constexpr size_t MAX_QUEUED_FRAMES = 16;

// The chosen address can come back: a VPN adapter may only come up after OBS does.
static constexpr int BIND_RETRY_MS = 2000;

// Also how long stopping can take, so this stays short.
static constexpr int SELECT_WAIT_MS = 100;

// Clients only send short commands. Anything larger isn't a panel.
static constexpr size_t MAX_PENDING_BYTES = 64 * 1024;

static void send_all(SOCKET socket, const char *data, int length) {
#ifdef _WIN32
  send(socket, data, length, 0);
#else
  send(socket, data, length, MSG_NOSIGNAL);
#endif
}

static void set_socket_timeout(SOCKET socket, int option, int milliseconds) {
#ifdef _WIN32
  DWORD value = static_cast<DWORD>(milliseconds);
#else
  timeval value = {milliseconds / 1000, (milliseconds % 1000) * 1000};
#endif
  setsockopt(socket, SOL_SOCKET, option, reinterpret_cast<const char *>(&value),
             sizeof(value));
}

static std::vector<uint8_t> build_frame(const uint8_t *data, size_t len,
                                        uint8_t opcode) {
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
  return frame;
}

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

void WsStatsServer::set_page_token(const std::string &token) {
  std::lock_guard<std::mutex> lock(ref_mutex_);
  if (!running_)
    page_token_ = token;
}

void WsStatsServer::add_ref() {
  std::lock_guard<std::mutex> lock(ref_mutex_);
  ref_count_++;
  if (ref_count_ == 1) {
    start();
  }
}

void WsStatsServer::configure(int port, const std::string &bind_ip,
                              const std::string &access_key) {
  std::lock_guard<std::mutex> lock(ref_mutex_);
  if (port_ == port && bind_ip_ == bind_ip && access_key_ == access_key)
    return;

  // The server's threads read these, so they have to be gone first.
  stop();
  port_ = port;
  bind_ip_ = bind_ip;
  access_key_ = access_key;
  if (ref_count_ > 0)
    start();
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
  sender_ = std::thread(&WsStatsServer::run_sender, this);
  thread_ = std::thread(&WsStatsServer::run, this);
}

void WsStatsServer::stop() {
  running_ = false;
  // The server thread owns its sockets and closes them on its way out.
  if (thread_.joinable())
    thread_.join();
  send_cv_.notify_all();
  if (sender_.joinable())
    sender_.join();
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_queue_.clear();
  }

  std::lock_guard<std::mutex> lock(clients_mutex_);
  for (const Client &client : clients_) {
    closesocket(client.socket);
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

void WsStatsServer::set_obs_state(const std::string &json) {
  {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    obs_state_ = json;
  }
  broadcast_all();
}

void WsStatsServer::set_audio_state(const std::string &json) {
  {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    audio_state_ = json;
  }
  broadcast_all();
}

void WsStatsServer::set_remote_handler(RemoteCallback cb) {
  std::lock_guard<std::mutex> lock(remote_handler_mutex_);
  remote_handler_ = std::move(cb);
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

bool WsStatsServer::is_authed(SOCKET client) {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  for (const Client &entry : clients_) {
    if (entry.socket == client)
      return entry.authed;
  }
  return false;
}

void WsStatsServer::drop_client(SOCKET client) {
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto it = clients_.begin(); it != clients_.end(); ++it) {
      if (it->socket == client) {
        clients_.erase(it);
        break;
      }
    }
  }
  close_sockets({client});
}

// deliver() sends on a copy of the client list, so a socket can't close under it.
void WsStatsServer::close_sockets(const std::vector<SOCKET> &sockets) {
  std::lock_guard<std::mutex> lock(delivery_mutex_);
  for (SOCKET socket : sockets)
    closesocket(socket);
}

void WsStatsServer::send_to_client(SOCKET client, const std::string &message) {
  const std::vector<uint8_t> frame = build_frame(
      reinterpret_cast<const uint8_t *>(message.data()), message.size(), 0x81);
  send_all(client, reinterpret_cast<const char *>(frame.data()),
           (int)frame.size());
}

bool WsStatsServer::has_key(SOCKET client) {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  for (const Client &entry : clients_) {
    if (entry.socket == client)
      return entry.has_key;
  }
  return false;
}

void WsStatsServer::mark_has_key(SOCKET client) {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  for (Client &entry : clients_) {
    if (entry.socket == client)
      entry.has_key = true;
  }
}

void WsStatsServer::drop_unauthenticated_clients() {
  const int64_t now = now_ms();
  std::vector<SOCKET> expired;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto it = clients_.begin(); it != clients_.end();) {
      if (!it->authed && now - it->connected_ms > AUTH_TIMEOUT_MS) {
        expired.push_back(it->socket);
        it = clients_.erase(it);
      } else {
        ++it;
      }
    }
  }
  close_sockets(expired);
}

bool WsStatsServer::is_same_machine_client(SOCKET client, uint32_t *peer) {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  for (const Client &entry : clients_) {
    if (entry.socket == client) {
      *peer = entry.peer;
      return entry.same_machine;
    }
  }
  return false;
}

void WsStatsServer::handle_client_message(SOCKET client, const std::string &msg) {
  // Message bodies stay out of the log: the first one holds the access key.
  obs_data_t *obj = obs_data_create_from_json(msg.c_str());
  if (!obj) {
    lss_log_warn("WsServer: ignoring a message that isn't JSON");
    return;
  }

  if (!is_authed(client)) {
    const std::string key = obs_data_get_string(obj, "auth");
    obs_data_release(obj);

    uint32_t peer = 0;
    const bool same_machine = is_same_machine_client(client, &peer);
    const bool has_access_key = !access_key_.empty() && access_key_matches(key, access_key_);
    const bool own_page = same_machine && !page_token_.empty() && access_key_matches(key, page_token_);
    if (has_access_key || own_page) {
      {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (Client &entry : clients_) {
          if (entry.socket == client) {
            entry.authed = true;
            entry.has_key = has_access_key;
          }
        }
      }
      lss_log_info("WebSocket Server: client authenticated");
      broadcast_all();
    } else {
      lss_log_warn("WebSocket Server: rejected a client with a wrong key");
      const int64_t now = now_ms();
      for (auto it = auth_blocked_until_ms_.begin(); it != auth_blocked_until_ms_.end();)
        it = it->second <= now ? auth_blocked_until_ms_.erase(it) : std::next(it);
      if (!same_machine)
        auth_blocked_until_ms_[peer] = now + AUTH_BACKOFF_MS;
      send_to_client(client, "{\"auth_error\":true}");
      drop_client(client);
    }
    return;
  }

  // The gateway always sends the key first, even to a server without one.
  if (obs_data_has_user_value(obj, "auth")) {
    const char *key = obs_data_get_string(obj, "auth");
    if (key && !access_key_.empty() && access_key_matches(key, access_key_))
      mark_has_key(client);
    obs_data_release(obj);
    return;
  }

  const char *cmd_raw = obs_data_get_string(obj, "command");
  const char *src_raw = obs_data_get_string(obj, "source");

  std::string cmd = cmd_raw ? cmd_raw : "";
  std::string src = src_raw ? src_raw : "";

  const char *target = obs_data_get_string(obj, cmd == "set_scene" ? "scene" : "name");
  RemoteCommand remote;
  remote.command = cmd;
  remote.name = target ? target : "";
  remote.value = obs_data_get_double(obj, "value");
  remote.muted = obs_data_get_bool(obj, "muted");
  obs_data_release(obj);

  const bool needs_key = cmd == "start_stream" || cmd == "stop_stream" ||
                         cmd == "set_volume" || cmd == "set_mute" ||
                         cmd == "preview_program_on" || cmd == "preview_program_off" ||
                         cmd == "audio_levels_on" || cmd == "audio_levels_off" ||
                         cmd == "sync_match";

  if (cmd == "set_scene" || needs_key) {
    if (needs_key && !has_key(client)) {
      lss_log_warn("WsServer: refused '%s' from a client without the key", cmd.c_str());
      return;
    }
    RemoteCallback cb;
    {
      std::lock_guard<std::mutex> lock(remote_handler_mutex_);
      cb = remote_handler_;
    }
    if (cb)
      cb(remote);
    return;
  }

  if (cmd.empty() || src.empty()) {
    lss_log_warn("WsServer: message missing 'command' or 'source'");
    return;
  }

  lss_log_info("WsServer: command='%s' source='%s'", cmd.c_str(), src.c_str());

  // Held through the call: a source waits here on its way out, so a command can't
  // run into one that's already gone.
  std::lock_guard<std::mutex> lock(cmd_handlers_mutex_);
  auto it = cmd_handlers_.find(src);
  if (it != cmd_handlers_.end())
    it->second(cmd);
  else
    lss_log_warn("WsServer: no handler for source '%s' (registered: %d)", src.c_str(),
                 (int)cmd_handlers_.size());
}

void WsStatsServer::broadcast_all() {
  std::string combined;
  {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    if (sources_.empty() && obs_state_.empty() && audio_state_.empty())
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

    if (!obs_state_.empty()) {
      obs_data_t *state = obs_data_create_from_json(obs_state_.c_str());
      if (state) {
        obs_data_set_obj(root, "obs", state);
        obs_data_release(state);
      }
    }

    if (!audio_state_.empty()) {
      obs_data_t *audio = obs_data_create_from_json(audio_state_.c_str());
      if (audio) {
        obs_data_set_obj(root, "audio", audio);
        obs_data_release(audio);
      }
    }

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
  // opcode = 0x81 (FIN+Text) or 0x82 (FIN+Binary). Called from the stream's own
  // threads, so this only ever queues.
  if (get_client_count() == 0)
    return;

  std::vector<uint8_t> frame = build_frame(data, len, opcode);
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (send_queue_.size() >= MAX_QUEUED_FRAMES)
      send_queue_.pop_front();
    send_queue_.push_back(std::move(frame));
  }
  send_cv_.notify_one();
}

void WsStatsServer::run_sender() {
  while (running_) {
    std::vector<uint8_t> frame;
    {
      std::unique_lock<std::mutex> lock(send_mutex_);
      send_cv_.wait_for(lock, std::chrono::milliseconds(200),
                        [this]() { return !send_queue_.empty() || !running_; });
      if (send_queue_.empty())
        continue;
      frame = std::move(send_queue_.front());
      send_queue_.pop_front();
    }
    deliver(frame);
  }
}

void WsStatsServer::deliver(const std::vector<uint8_t> &frame) {
  // clients_mutex_ isn't held across send(): get_client_count() runs on the stream's
  // threads and would wait out a slow client's timeout otherwise.
  std::lock_guard<std::mutex> delivery(delivery_mutex_);
  std::vector<SOCKET> targets;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const Client &client : clients_) {
      if (client.authed)
        targets.push_back(client.socket);
    }
  }

  for (SOCKET socket : targets) {
    const char *ptr = reinterpret_cast<const char *>(frame.data());
    int remaining = static_cast<int>(frame.size());
    while (remaining > 0) {
#ifdef _WIN32
      int sent = send(socket, ptr, remaining, 0);
#else
      int sent = send(socket, ptr, remaining, MSG_NOSIGNAL);
#endif
      if (sent == SOCKET_ERROR) {
        // Not closed from here: the server thread may be in select() on it, and
        // this wakes it to clean up.
        shutdown(socket, SD_BOTH);
        break;
      }
      ptr += sent;
      remaining -= sent;
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

static bool from_same_machine(SOCKET client, const sockaddr_in &peer) {
  sockaddr_in local = {};
#ifdef _WIN32
  int local_len = sizeof(local);
#else
  socklen_t local_len = sizeof(local);
#endif
  getsockname(client, reinterpret_cast<sockaddr *>(&local), &local_len);
  return is_same_machine(ntohl(peer.sin_addr.s_addr), ntohl(local.sin_addr.s_addr));
}

bool WsStatsServer::is_refused(uint32_t peer, bool same_machine) {
  if (same_machine)
    return false;
  if (access_key_.empty())
    return true;
  const auto blocked = auth_blocked_until_ms_.find(peer);
  return blocked != auth_blocked_until_ms_.end() && now_ms() < blocked->second;
}

// Only takes the connection: the request is read as it arrives, with everyone else's,
// so a client that connects and says nothing holds up nobody.
void WsStatsServer::accept_client(SOCKET listener) {
  sockaddr_in peer = {};
#ifdef _WIN32
  int peer_len = sizeof(peer);
#else
  socklen_t peer_len = sizeof(peer);
#endif
  SOCKET client = accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_len);
  if (client == INVALID_SOCKET)
    return;

  Client entry;
  entry.socket = client;
  entry.connected_ms = now_ms();
  entry.peer = peer.sin_addr.s_addr;
  entry.same_machine = from_same_machine(client, peer);
  if (is_refused(entry.peer, entry.same_machine)) {
    closesocket(client);
    return;
  }

  set_socket_timeout(client, SO_SNDTIMEO, SEND_TIMEOUT_MS);
  std::lock_guard<std::mutex> lock(clients_mutex_);
  if (clients_.size() >= MAX_CLIENTS) {
    lss_log_warn("WebSocket Server: too many clients, refusing one");
    closesocket(client);
    return;
  }
  clients_.push_back(std::move(entry));
}

static SOCKET make_listener(uint32_t address, int port, int *error) {
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    *error = WSAGetLastError();
    return INVALID_SOCKET;
  }

  int opt = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = address;
  addr.sin_port = htons(static_cast<unsigned short>(port));

  if (bind(listener, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
      listen(listener, SOMAXCONN) == SOCKET_ERROR) {
    *error = WSAGetLastError();
    closesocket(listener);
    return INVALID_SOCKET;
  }
  return listener;
}

bool WsStatsServer::open_listeners(bool report_failure) {
  const bool any_address = bind_ip_.empty() || bind_ip_ == "0.0.0.0";
  int error = 0;
  listen_socket_ = make_listener(any_address ? htonl(INADDR_ANY) : inet_addr(bind_ip_.c_str()), port_, &error);
  if (listen_socket_ == INVALID_SOCKET) {
    if (report_failure)
      lss_log_warn("WebSocket Server: Can't listen on %s:%d (error %d), retrying", bind_ip_.c_str(), port_, error);
    return false;
  }
  lss_log_info("WebSocket Server: Listening on ws://%s:%d", any_address ? "0.0.0.0" : bind_ip_.c_str(), port_);

  // Overlays connect to this machine's loopback address, whichever address the server
  // is bound to for the outside world. Not being able to is no reason to stop.
  if (!any_address && bind_ip_.rfind("127.", 0) != 0) {
    loopback_socket_ = make_listener(htonl(INADDR_LOOPBACK), port_, &error);
    if (loopback_socket_ == INVALID_SOCKET)
      lss_log_warn("WebSocket Server: Couldn't listen on 127.0.0.1:%d, local overlays won't connect", port_);
    else
      lss_log_info("WebSocket Server: Also listening on ws://127.0.0.1:%d", port_);
  }

  listening_ = true;
  return true;
}

void WsStatsServer::run() {
  for (bool reported = false; running_ && !open_listeners(!reported); reported = true) {
    for (int waited = 0; running_ && waited < BIND_RETRY_MS; waited += SELECT_WAIT_MS)
      std::this_thread::sleep_for(std::chrono::milliseconds(SELECT_WAIT_MS));
  }

  bool select_failed = false;
  while (running_) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(listen_socket_, &read_fds);

    int nfds = (int)listen_socket_;
    if (loopback_socket_ != INVALID_SOCKET) {
      FD_SET(loopback_socket_, &read_fds);
      if ((int)loopback_socket_ > nfds)
        nfds = (int)loopback_socket_;
    }

    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      for (const Client &client : clients_) {
        FD_SET(client.socket, &read_fds);
        if ((int)client.socket > nfds) {
          nfds = (int)client.socket;
        }
      }
    }
    nfds += 1;

    timeval timeout = {0, SELECT_WAIT_MS * 1000};
    int activity = select(nfds, &read_fds, nullptr, nullptr, &timeout);

    if (activity == SOCKET_ERROR) {
      if (!select_failed)
        lss_log_error("WebSocket Server: select() error %d", WSAGetLastError());
      select_failed = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(SELECT_WAIT_MS));
      continue;
    }

    drop_unauthenticated_clients();

    if (activity > 0) {
      if (FD_ISSET(listen_socket_, &read_fds))
        accept_client(listen_socket_);
      if (loopback_socket_ != INVALID_SOCKET && FD_ISSET(loopback_socket_, &read_fds))
        accept_client(loopback_socket_);

      std::vector<std::pair<SOCKET, std::string>> incoming;
      std::vector<std::pair<SOCKET, std::string>> requests;
      std::vector<SOCKET> finished;
      {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.begin();
        while (it != clients_.end()) {
          SOCKET client = it->socket;
          if (FD_ISSET(client, &read_fds)) {
            char buf[4096];
            int bytes = recv(client, buf, sizeof(buf), 0);
            if (bytes <= 0) {
              finished.push_back(client);
              it = clients_.erase(it);
              continue;
            }
            it->pending.append(buf, static_cast<size_t>(bytes));
            if (!it->upgraded) {
              if (it->pending.find("\r\n\r\n") != std::string::npos) {
                it->upgraded = true;
                requests.emplace_back(client, std::move(it->pending));
                it->pending.clear();
              } else if (it->pending.size() > MAX_REQUEST_BYTES) {
                finished.push_back(client);
                it = clients_.erase(it);
                continue;
              }
              ++it;
              continue;
            }
            size_t used = 0;
            std::string msg;
            while (size_t taken = parse_ws_frame(it->pending.data() + used,
                                                 it->pending.size() - used, &msg)) {
              used += taken;
              if (!msg.empty())
                incoming.emplace_back(client, std::move(msg));
            }
            it->pending.erase(0, used);
            if (it->pending.size() > MAX_PENDING_BYTES) {
              finished.push_back(client);
              it = clients_.erase(it);
              continue;
            }
          }
          ++it;
        }
      }
      close_sockets(finished);
      for (auto &item : requests)
        finish_upgrade(item.first, item.second);
      for (auto &item : incoming)
        handle_client_message(item.first, item.second);
    }
  }

  if (listen_socket_ != INVALID_SOCKET) {
    closesocket(listen_socket_);
    listen_socket_ = INVALID_SOCKET;
  }
  if (loopback_socket_ != INVALID_SOCKET) {
    closesocket(loopback_socket_);
    loopback_socket_ = INVALID_SOCKET;
  }
  listening_ = false;
  lss_log_info("WebSocket Server: Thread exiting");
}

void WsStatsServer::finish_upgrade(SOCKET client, const std::string &request) {
  const std::string key = find_http_header(request, "Sec-WebSocket-Key");
  if (key.empty()) {
    lss_log_warn("WebSocket Server: Invalid handshake request (no key)");
    drop_client(client);
    return;
  }

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
  send_all(client, resp_str.c_str(), (int)resp_str.length());

  const std::string origin = find_http_header(request, "Origin");
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (Client &entry : clients_) {
      if (entry.socket == client)
        entry.authed = trusted_without_proof(entry.same_machine, origin);
    }
  }
  lss_log_info("WebSocket Server: Handshake complete, client added");

  // Notify all sources so they can reset per-connection state (e.g. preview
  // flags). Held through the calls: a source waits here on its way out.
  std::lock_guard<std::mutex> lock(connect_cbs_mutex_);
  for (auto &kv : connect_callbacks_)
    kv.second();
}

} // namespace lss
