// whep-client.h - WebRTC (WHEP) playback client
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef HAS_LIBDATACHANNEL
#include <rtc/rtc.hpp>
#endif

namespace lss {

class WhepClient {
public:
  // Video: receives complete H.264 NAL units (with Annex-B start codes)
  using VideoFrameCallback =
      std::function<void(const uint8_t *data, size_t len, uint64_t timestamp)>;

  // Audio: receives Opus frames (one per RTP packet, typically 20ms)
  using AudioFrameCallback =
      std::function<void(const uint8_t *data, size_t len, uint64_t timestamp)>;

  using StateCallback = std::function<void(const std::string &state)>;

  WhepClient();
  ~WhepClient();

  WhepClient(const WhepClient &) = delete;
  WhepClient &operator=(const WhepClient &) = delete;

  enum class WhepMode { Auto, VideoAudio, VideoOnly, AudioOnly };

  bool start(const std::string &whep_url, const std::string &bearer_token = "",
             WhepMode mode = WhepMode::Auto);
  void stop();
  bool is_connected() const;
  bool is_running() const;

  void set_video_callback(VideoFrameCallback cb);
  void set_audio_callback(AudioFrameCallback cb);
  void set_state_callback(StateCallback cb);

private:
#ifdef HAS_LIBDATACHANNEL
  bool setup_peer_connection(bool video, bool audio);
  bool start_internal(bool video, bool audio);
  void on_track(std::shared_ptr<rtc::Track> track);
  void on_video_frame(const uint8_t *data, size_t len, uint64_t ts);
  void on_audio_frame(const uint8_t *data, size_t len, uint64_t ts);
  void handle_h264_nalu(const uint8_t *data, size_t len, uint64_t ts);

  std::shared_ptr<rtc::PeerConnection> pc_;
  std::shared_ptr<rtc::Track> video_track_;
  std::shared_ptr<rtc::Track> audio_track_;
  std::shared_ptr<rtc::H264RtpDepacketizer> video_depacketizer_;
  std::shared_ptr<rtc::OpusRtpDepacketizer> audio_depacketizer_;
#endif

  VideoFrameCallback video_cb_;
  AudioFrameCallback audio_cb_;
  StateCallback state_cb_;
  std::mutex cb_mutex_;

  std::string whep_url_;
  std::string bearer_token_;
  std::string resource_url_; // for teardown

  std::atomic<bool> connected_{false};
  std::atomic<bool> running_{false};
  std::thread pli_thread_;
};

} // namespace lss
