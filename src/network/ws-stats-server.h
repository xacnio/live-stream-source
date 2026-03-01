#pragma once

#include <atomic>
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

class WsStatsServer {
public:
  static WsStatsServer &instance();

  void add_ref();
  void release();

  void configure(int port, const std::string &bind_ip);

  void update_source(const std::string &source_name,
                     const std::string &json_data);

  void remove_source(const std::string &source_name);

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

  void run();
  void handle_handshake(SOCKET client);

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
};

} // namespace lss
