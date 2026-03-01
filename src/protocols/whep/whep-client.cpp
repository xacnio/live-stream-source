// whep-client.cpp
#include "protocols/whep/whep-client.h"
#include "core/common.h"
#include "protocols/whep/whep-signaling.h"

#include <cstring>

namespace lss {

WhepClient::WhepClient() = default;

WhepClient::~WhepClient() { stop(); }

bool WhepClient::is_connected() const { return connected_.load(); }
bool WhepClient::is_running() const { return running_.load(); }

void WhepClient::set_video_callback(VideoFrameCallback cb) {
  std::lock_guard<std::mutex> lock(cb_mutex_);
  video_cb_ = std::move(cb);
}

void WhepClient::set_audio_callback(AudioFrameCallback cb) {
  std::lock_guard<std::mutex> lock(cb_mutex_);
  audio_cb_ = std::move(cb);
}

void WhepClient::set_state_callback(StateCallback cb) {
  std::lock_guard<std::mutex> lock(cb_mutex_);
  state_cb_ = std::move(cb);
}

#ifdef HAS_LIBDATACHANNEL

bool WhepClient::start(const std::string &whep_url,
                       const std::string &bearer_token, WhepMode mode) {
  if (running_.load()) {
    lss_log_warn("[WHEP Client] Already running");
    return false;
  }

  whep_url_ = whep_url;
  bearer_token_ = bearer_token;
  running_.store(true);

  lss_log_info("[WHEP Client] Starting  - URL: %s (Mode: %d)", whep_url.c_str(),
               (int)mode);

  if (mode == WhepMode::Auto) {
    if (start_internal(true, true))
      return true;
    lss_log_warn(
        "[WHEP Client] Video+Audio handshake failed, trying VIDEO only...");
    if (start_internal(true, false))
      return true;
    lss_log_warn(
        "[WHEP Client] Video-only handshake failed, trying AUDIO only...");
    if (start_internal(false, true))
      return true;
  } else if (mode == WhepMode::VideoAudio) {
    if (start_internal(true, true))
      return true;
  } else if (mode == WhepMode::VideoOnly) {
    if (start_internal(true, false))
      return true;
  } else if (mode == WhepMode::AudioOnly) {
    if (start_internal(false, true))
      return true;
  }

  lss_log_error(
      "[WHEP Client] All connection attempts failed. WHEP start aborted.");
  running_.store(false);
  return false;
}

bool WhepClient::start_internal(bool video, bool audio) {
  if (pc_) {
    pc_->close();
    pc_.reset();
  }
  video_track_.reset();
  audio_track_.reset();
  connected_.store(false);

  lss_log_info("[WHEP Client] Attempting connection with Video=%d, Audio=%d",
               video, audio);

  if (!setup_peer_connection(video, audio)) {
    lss_log_error("[WHEP Client] Failed to create PeerConnection");
    return false;
  }

  pc_->setLocalDescription(rtc::Description::Type::Offer);

  std::mutex gather_mtx;
  std::condition_variable gather_cv;
  bool gathering_done = false;

  pc_->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
    if (state == rtc::PeerConnection::GatheringState::Complete) {
      std::lock_guard<std::mutex> lk(gather_mtx);
      gathering_done = true;
      gather_cv.notify_all();
    }
  });

  {
    std::unique_lock<std::mutex> lk(gather_mtx);
    gather_cv.wait_for(lk, std::chrono::seconds(5),
                       [&] { return gathering_done; });
  }

  auto local_desc = pc_->localDescription();
  if (!local_desc) {
    lss_log_error("[WHEP Client] No local description generated");
    return false;
  }

  std::string sdp_offer = std::string(*local_desc);
  lss_log_debug("[WHEP Client] SDP offer generated (%zu bytes)",
                sdp_offer.size());

  auto session = WhepSignaling::offer(whep_url_, sdp_offer, bearer_token_);
  if (session.http_status != 201 && session.http_status != 200) {
    lss_log_error("[WHEP Client] WHEP offer failed: HTTP %d, body: %s",
                  session.http_status, session.error.c_str());
    return false;
  }

  resource_url_ = session.resource_url;
  lss_log_info("[WHEP Client] WHEP session established, resource: %s",
               resource_url_.c_str());

  lss_log_debug("[WHEP Client] SDP Answer received:\n%s",
                session.sdp_answer.c_str());

  try {
    rtc::Description answer(session.sdp_answer, rtc::Description::Type::Answer);
    pc_->setRemoteDescription(answer);
  } catch (const std::exception &e) {
    lss_log_error("[WHEP Client] Failed to set remote description: %s",
                  e.what());
    return false;
  }

  lss_log_debug("[WHEP Client] Remote description set  - waiting for ICE+DTLS");

  if (!pli_thread_.joinable()) {
    pli_thread_ = std::thread([this]() {
      while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (connected_.load() && pc_ && video_track_ &&
            video_track_->isOpen()) {
          video_track_->requestKeyframe();
          video_track_->requestBitrate(5000000);
        }
      }
    });
  }

  return true;
}

void WhepClient::stop() {
  if (!running_.exchange(false))
    return;

  lss_log_info("[WHEP Client] Stopping...");

  if (pli_thread_.joinable())
    pli_thread_.join();

  if (pc_) {
    pc_->close();
    pc_.reset();
  }

  video_track_.reset();
  audio_track_.reset();
  connected_.store(false);

  if (!resource_url_.empty()) {
    WhepSignaling::teardown(resource_url_, bearer_token_);
    resource_url_.clear();
  }

  {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    if (state_cb_)
      state_cb_("disconnected");
  }

  lss_log_info("[WHEP Client] Stopped");
}

bool WhepClient::setup_peer_connection(bool video, bool audio) {
  static std::once_flag log_init_flag;
  std::call_once(log_init_flag, []() {
    rtc::InitLogger(rtc::LogLevel::Verbose,
                    [](rtc::LogLevel level, std::string message) {
                      lss_log_debug("[libdatachannel] %s", message.c_str());
                    });
  });

  rtc::Configuration config;
  config.iceServers.emplace_back("stun:stun.l.google.com:19302");
  config.iceServers.emplace_back("stun:stun1.l.google.com:19302");
  config.disableAutoNegotiation = true;

  try {
    pc_ = std::make_shared<rtc::PeerConnection>(config);
  } catch (const std::exception &e) {
    lss_log_error("[WHEP Client] Exception creating PeerConnection: %s",
                  e.what());
    return false;
  }

  pc_->onStateChange([this](rtc::PeerConnection::State state) {
    std::string state_str;
    switch (state) {
    case rtc::PeerConnection::State::New:
      state_str = "new";
      break;
    case rtc::PeerConnection::State::Connecting:
      state_str = "connecting";
      break;
    case rtc::PeerConnection::State::Connected:
      state_str = "connected";
      break;
    case rtc::PeerConnection::State::Disconnected:
      state_str = "disconnected";
      break;
    case rtc::PeerConnection::State::Failed:
      state_str = "failed";
      break;
    case rtc::PeerConnection::State::Closed:
      state_str = "closed";
      break;
    default:
      state_str = "unknown";
      break;
    }

    lss_log_debug("[WHEP Client] PeerConnection state: %s", state_str.c_str());

    if (state == rtc::PeerConnection::State::Connected) {
      connected_.store(true);
    } else if (state == rtc::PeerConnection::State::Disconnected ||
               state == rtc::PeerConnection::State::Failed ||
               state == rtc::PeerConnection::State::Closed) {
      connected_.store(false);
    }

    std::lock_guard<std::mutex> lock(cb_mutex_);
    if (state_cb_)
      state_cb_(state_str);
  });

  pc_->onTrack([this](std::shared_ptr<rtc::Track> track) {
    lss_log_debug("[WHEP Client] Remote track received");
    this->on_track(track);
  });

  if (video) {
    rtc::Description::Video video_desc("video",
                                       rtc::Description::Direction::RecvOnly);
    video_desc.addH264Codec(96);
    video_track_ = pc_->addTrack(video_desc);
  }

  if (audio) {
    rtc::Description::Audio audio_desc("audio",
                                       rtc::Description::Direction::RecvOnly);
    audio_desc.addOpusCodec(111);
    audio_track_ = pc_->addTrack(audio_desc);
  }

  if (video_track_) {
    lss_log_debug("[WHEP Client] Attaching handlers to local VIDEO track");

    auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
    auto rtp_session = std::make_shared<rtc::RtcpReceivingSession>();

    // chainMediaHandler prepends, so order matters:
    // Incoming -> RtcpReceivingSession -> H264RtpDepacketizer -> onMessage
    video_track_->setMediaHandler(depacketizer);
    video_track_->chainMediaHandler(rtp_session);

    video_track_->onOpen([this]() {
      lss_log_debug("[WHEP Client] VIDEO track OPENED (isOpen=%d)",
                    video_track_ ? (int)video_track_->isOpen() : -1);
    });
    video_track_->onClosed(
        [this]() { lss_log_warn("[WHEP Client] VIDEO track CLOSED"); });
    video_track_->onError([](std::string error) {
      lss_log_error("[WHEP Client] VIDEO track ERROR: %s", error.c_str());
    });

    video_track_->onMessage([this](rtc::message_variant msg) {
      if (std::holds_alternative<rtc::binary>(msg)) {
        auto &data = std::get<rtc::binary>(msg);
      }
    });

    video_track_->onFrame([this](auto data, auto info) {
      this->on_video_frame(reinterpret_cast<const uint8_t *>(data.data()),
                           data.size(), (uint64_t)info.timestamp);
    });
  }

  if (audio_track_) {
    lss_log_debug("[WHEP Client] Attaching handlers to local AUDIO track");

    auto depacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
    auto rtp_session = std::make_shared<rtc::RtcpReceivingSession>();
    audio_track_->setMediaHandler(depacketizer);
    audio_track_->chainMediaHandler(rtp_session);

    audio_track_->onOpen([this]() {
      lss_log_debug("[WHEP Client] AUDIO track OPENED (isOpen=%d)",
                    audio_track_ ? (int)audio_track_->isOpen() : -1);
    });
    audio_track_->onClosed(
        [this]() { lss_log_warn("[WHEP Client] AUDIO track CLOSED"); });
    audio_track_->onError([](std::string error) {
      lss_log_error("[WHEP Client] AUDIO track ERROR: %s", error.c_str());
    });

    audio_track_->onMessage([this](rtc::message_variant msg) {});

    audio_track_->onFrame([this](auto data, auto info) {
      this->on_audio_frame(reinterpret_cast<const uint8_t *>(data.data()),
                           data.size(), (uint64_t)info.timestamp);
    });
  }

  return true;
}

void WhepClient::on_track(std::shared_ptr<rtc::Track> track) {
  lss_log_debug("[WHEP Client] Incoming track: mid=%s", track->mid().c_str());

  auto desc = track->description();
  bool is_video = false;
  bool is_audio = false;

  for (int pt : desc.payloadTypes()) {
    if (pt == 96)
      is_video = true;
    if (pt == 111)
      is_audio = true;
    if (const auto *rtp = desc.rtpMap(pt)) {
      if (rtp->format == "H264")
        is_video = true;
      if (rtp->format == "opus")
        is_audio = true;
    }
  }

  if (!is_video && !is_audio) {
    if (track->mid().find("video") != std::string::npos)
      is_video = true;
    if (track->mid().find("audio") != std::string::npos)
      is_audio = true;
  }

  if (is_video) {
    lss_log_debug("[WHEP Client] Identified VIDEO track  - attaching handlers");

    auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
    auto rtcp_session = std::make_shared<rtc::RtcpReceivingSession>();
    track->setMediaHandler(depacketizer);
    track->chainMediaHandler(rtcp_session);

    track->onFrame([this](auto data, auto info) {
      this->on_video_frame(reinterpret_cast<const uint8_t *>(data.data()),
                           data.size(), (uint64_t)info.timestamp);
    });

    video_track_ = track;

  } else if (is_audio) {
    lss_log_debug("[WHEP Client] Identified AUDIO track  - attaching handlers");

    auto depacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
    auto rtcp_session = std::make_shared<rtc::RtcpReceivingSession>();
    track->setMediaHandler(depacketizer);
    track->chainMediaHandler(rtcp_session);

    track->onFrame([this](auto data, auto info) {
      this->on_audio_frame(reinterpret_cast<const uint8_t *>(data.data()),
                           data.size(), (uint64_t)info.timestamp);
    });

    audio_track_ = track;
  } else {
    lss_log_warn("[WHEP Client] Unknown track type (mid=%s), ignoring",
                 track->mid().c_str());
  }
}

void WhepClient::on_video_frame(const uint8_t *data, size_t len, uint64_t ts) {
  static bool logged = false;
  if (!logged) {
    lss_log_debug("[WHEP Client] First video frame received");
    logged = true;
  }
  if (!data || len == 0)
    return;
  handle_h264_nalu(data, len, ts);
}

void WhepClient::handle_h264_nalu(const uint8_t *data, size_t len,
                                  uint64_t ts) {
  if (len == 0)
    return;

  // Prepend Annex-B start codes for FFmpeg if not already present
  static const uint8_t start_code[] = {0x00, 0x00, 0x00, 0x01};

  bool has_start_code =
      (len >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
       ((data[2] == 0x00 && data[3] == 0x01) || (data[2] == 0x01)));

  std::lock_guard<std::mutex> lock(cb_mutex_);
  if (!video_cb_)
    return;

  if (has_start_code) {
    video_cb_(data, len, ts);
  } else {
    std::vector<uint8_t> nalu_buf(4 + len);
    std::memcpy(nalu_buf.data(), start_code, 4);
    std::memcpy(nalu_buf.data() + 4, data, len);
    video_cb_(nalu_buf.data(), nalu_buf.size(), ts);
  }
}

void WhepClient::on_audio_frame(const uint8_t *data, size_t len, uint64_t ts) {
  if (!data || len == 0)
    return;

  std::lock_guard<std::mutex> lock(cb_mutex_);
  if (audio_cb_) {
    audio_cb_(data, len, ts);
  }
}

#else

// Stub (no libdatachannel)

bool WhepClient::start(const std::string &whep_url,
                       const std::string &bearer_token) {
  (void)whep_url;
  (void)bearer_token;
  lss_log_error("[WHEP Client] libdatachannel not available  - "
                "WHEP support disabled. Rebuild with HAS_LIBDATACHANNEL.");
  return false;
}

void WhepClient::stop() {}

#endif // HAS_LIBDATACHANNEL

} // namespace lss
