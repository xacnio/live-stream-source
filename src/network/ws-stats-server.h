#pragma once

#include <atomic>
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

class WsStatsServer {
public:
  static WsStatsServer &instance();

  void add_ref();
  void release();

  void configure(int port, const std::string &bind_ip);

  void update_source(const std::string &source_name,
                     const std::string &json_data);

  void remove_source(const std::string &source_name);

  // Broadcast a single binary frame to all connected clients. Used by the
  // preview encoder for JPEG video frames and PCM audio chunks. The encoder
  // is expected to prefix its payload with a 1-byte type tag (0x01 = video,
  // 0x02 = audio) so the browser can demultiplex from one WS connection.
  void send_binary(const uint8_t *data, size_t len);

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
  bool is_running() const { return running_; }
  int get_client_count() {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    return (int)clients_.size();
  }

private:
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
  void handle_handshake(SOCKET client);
  std::string decode_ws_frame(const char *data, int len);
  void handle_client_message(const std::string &msg);

  SOCKET listen_socket_ = INVALID_SOCKET;
  int port_ = 4477;
  std::string bind_ip_ = "127.0.0.1";
  std::thread thread_;
  std::atomic<bool> running_{false};

  std::mutex ref_mutex_;
  int ref_count_ = 0;

  std::mutex clients_mutex_;
  std::vector<SOCKET> clients_;

  std::mutex sources_mutex_;
  std::map<std::string, std::string> sources_;

  std::mutex cmd_handlers_mutex_;
  std::map<std::string, CommandCallback> cmd_handlers_;

  std::mutex connect_cbs_mutex_;
  std::map<std::string, ConnectCallback> connect_callbacks_;
};

} // namespace lss
