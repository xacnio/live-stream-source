#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif

namespace lss {

using CommandCallback = std::function<void(const std::string &)>;
using ConnectCallback = std::function<void()>;

// A panel request that acts on OBS itself rather than on one source.
struct RemoteCommand {
  std::string command;
  std::string name; // scene or audio source
  double value = 0; // volume multiplier
  bool muted = false;
};
using RemoteCallback = std::function<void(const RemoteCommand &)>;

class WsStatsServer {
public:
  static WsStatsServer &instance();

  void add_ref();
  void release();

  // With an access_key set, a remote client gets nothing until it presents the
  // key. Without one, only this machine can connect.
  // Connections from this machine are exempt: the stats overlay runs as a
  // browser source and has no way to hold a key.
  void configure(int port, const std::string &bind_ip,
                 const std::string &access_key);

  void update_source(const std::string &source_name,
                     const std::string &json_data);

  void remove_source(const std::string &source_name);

  // Scene list and current scene, as JSON. Sent alongside the source stats.
  void set_obs_state(const std::string &json);
  // Audio sources with their volume and mute state, as JSON.
  void set_audio_state(const std::string &json);
  // Everything except scene switching needs the access key, even from this
  // machine: any web page open here can reach this port.
  void set_remote_handler(RemoteCallback cb);

  // Lets this machine's own pages in, see trusted_without_proof(). Before the server starts.
  void set_page_token(const std::string &token);

  // Broadcast a single binary frame to all connected clients. Used by the
  // preview encoder for JPEG video frames and PCM audio chunks. The encoder
  // is expected to prefix its payload with a 1-byte type tag (0x01 = video,
  // 0x02 = audio, 0x03 = program JPEG) so the browser can demultiplex from one
  // WS connection.
  void send_binary(const uint8_t *data, size_t len);

  // A small JSON message outside the regular stats, for fast-changing values.
  void send_text(const std::string &json) { send_ws_frame(json); }

  void register_command_handler(const std::string &source_name,
                                CommandCallback cb);
  void unregister_command_handler(const std::string &source_name);

  // Called whenever a new WebSocket client completes the handshake. Sources
  // register here to reset per-client state (e.g. preview flags) so a fresh
  // browser tab always starts in the default-off state.
  void register_connect_callback(const std::string &source_name,
                                 ConnectCallback cb);
  void unregister_connect_callback(const std::string &source_name);

  std::map<std::string, std::string> get_all_stats() {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    return sources_;
  }

  int get_port() const { return port_; }
  bool is_running() const { return listening_; }
  // Clients that have passed authentication.
  int get_client_count() {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    int count = 0;
    for (const Client &client : clients_)
      count += client.authed ? 1 : 0;
    return count;
  }

private:
  struct Client {
    SOCKET socket;
    int64_t connected_ms;
    uint32_t peer = 0;
    bool same_machine = false;
    bool upgraded = false; // past the HTTP handshake
    bool authed = false;
    bool has_key = false;
    std::string pending; // bytes of a request or frame still arriving
  };

  WsStatsServer();
  ~WsStatsServer();

  WsStatsServer(const WsStatsServer &) = delete;
  WsStatsServer &operator=(const WsStatsServer &) = delete;

  void start(); // Internal start
  void stop();
  void broadcast_all();
  void send_ws_frame(const std::string &message);
  void send_ws_frame_raw(const uint8_t *data, size_t len, uint8_t opcode);

  void run();
  void run_sender();
  void deliver(const std::vector<uint8_t> &frame);
  void finish_upgrade(SOCKET client, const std::string &request);
  void handle_client_message(SOCKET client, const std::string &msg);
  bool is_authed(SOCKET client);
  bool has_key(SOCKET client);
  bool is_same_machine_client(SOCKET client, uint32_t *peer);
  bool is_refused(uint32_t peer, bool same_machine);
  void mark_has_key(SOCKET client);
  void drop_client(SOCKET client);
  void send_to_client(SOCKET client, const std::string &message);
  void drop_unauthenticated_clients();
  void accept_client(SOCKET listener);
  bool open_listeners(bool report_failure);
  void close_sockets(const std::vector<SOCKET> &sockets);

  SOCKET listen_socket_ = INVALID_SOCKET;
  SOCKET loopback_socket_ = INVALID_SOCKET;
  std::mutex delivery_mutex_; // always taken before clients_mutex_
  int port_ = 4477;
  std::string bind_ip_ = "127.0.0.1";
  std::string access_key_;
  std::string page_token_;
  std::map<uint32_t, int64_t> auth_blocked_until_ms_; // by address, server thread only
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> listening_{false};

  std::mutex ref_mutex_;
  int ref_count_ = 0;

  std::mutex clients_mutex_;
  std::vector<Client> clients_;

  // Stats go out from the stream's reader thread and preview frames from its
  // present thread. Queueing here means neither can ever wait on a slow client.
  std::thread sender_;
  std::mutex send_mutex_;
  std::condition_variable send_cv_;
  std::deque<std::vector<uint8_t>> send_queue_;

  std::mutex sources_mutex_;
  std::map<std::string, std::string> sources_;
  std::string obs_state_;
  std::string audio_state_;

  std::mutex remote_handler_mutex_;
  RemoteCallback remote_handler_;

  std::mutex cmd_handlers_mutex_;
  std::map<std::string, CommandCallback> cmd_handlers_;

  std::mutex connect_cbs_mutex_;
  std::map<std::string, ConnectCallback> connect_callbacks_;
};

} // namespace lss
