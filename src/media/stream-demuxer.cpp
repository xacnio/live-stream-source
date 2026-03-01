// stream-demuxer.cpp
#include "media/stream-demuxer.h"

extern "C" {
#include <libavformat/avio.h>
}

namespace lss {

StreamDemuxer::~StreamDemuxer() { close(); }

int StreamDemuxer::open(const std::string &url, StreamType type) {
  close();
  abort_.store(false);

  bool is_ivs = (type == StreamType::AmazonIVS);
  bool is_hls = (type == StreamType::HLS);

  if (is_ivs) {
    int ivs_ret = open_ivs(url);
    if (ivs_ret == 0)
      return 0;
    lss_log_warn("Custom LL-HLS client failed (ret=%d), "
                 "falling back to FFmpeg HLS demuxer",
                 ivs_ret);
  }

  fmt_ctx_ = avformat_alloc_context();
  if (!fmt_ctx_) {
    lss_log_error("Failed to allocate AVFormatContext");
    return AVERROR(ENOMEM);
  }

  fmt_ctx_->interrupt_callback.callback = interrupt_callback;
  fmt_ctx_->interrupt_callback.opaque = this;

  bool is_srt = (url.rfind("srt://", 0) == 0);
  // NOBUFFER for everything except HLS/IVS (segment buffering needed).
  // SRT MUST have NOBUFFER or av_read_frame stalls.
  if (!is_hls && !is_ivs) {
    fmt_ctx_->flags |= AVFMT_FLAG_NOBUFFER;
  }
  if (!is_srt) {
    fmt_ctx_->flags |= AVFMT_FLAG_DISCARD_CORRUPT;
  }

  fmt_ctx_->probesize = PROBE_SIZE;
  fmt_ctx_->max_analyze_duration = ANALYZE_DURATION_US;

  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "tcp_nodelay", "1", 0);
  av_dict_set(&opts, "rw_timeout", "5000000", 0);

  if (is_srt) {
    av_dict_set(&opts, "fflags", "nobuffer+genpts", 0);
  } else if (is_hls || is_ivs) {
    av_dict_set(&opts, "fflags", "discardcorrupt", 0);
  } else {
    av_dict_set(&opts, "fflags", "nobuffer+discardcorrupt", 0);
  }

  if (is_ivs) {
    av_dict_set(&opts, "live_start_index", "-1", 0);
    av_dict_set(&opts, "http_persistent", "1", 0);
    av_dict_set(&opts, "http_multiple", "1", 0);
    av_dict_set(&opts, "http_keepalive", "1", 0);
    av_dict_set(&opts, "prefer_audio_seek", "1", 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "2", 0);
    av_dict_set(&opts, "http_seekable", "0", 0);
    av_dict_set(&opts, "seg_max_retry", "8", 0);
    av_dict_set(&opts, "rw_timeout", "5000000", 0);
    av_dict_set(&opts, "allowed_extensions", "m3u8,ts,aac,mp4,m4s,key", 0);
    av_dict_set(&opts, "user_agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                "(KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36",
                0);
    av_dict_set(&opts, "hls_flags", "low_latency+independent_segments", 0);
    av_dict_set(&opts, "prefer_x_start", "1", 0);
    av_dict_set(&opts, "fflags", "nobuffer+igndts+flush_packets", 0);
    av_dict_set(&opts, "fpsprobesize", "0", 0);
    fmt_ctx_->probesize = 256 * 1024;
    fmt_ctx_->max_analyze_duration = 300000;
    lss_log_debug("Protocol: Amazon IVS");

  } else if (is_hls) {
    av_dict_set(&opts, "live_start_index", "-1", 0);
    av_dict_set(&opts, "http_persistent", "1", 0);
    av_dict_set(&opts, "http_multiple", "1", 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "2", 0);
    av_dict_set(&opts, "http_seekable", "0", 0);
    av_dict_set(&opts, "seg_max_retry", "3", 0);
    av_dict_set(&opts, "rw_timeout", "10000000", 0); // 10s
    fmt_ctx_->probesize = 2 * 1024 * 1024;           // 2MB
    fmt_ctx_->max_analyze_duration = 3000000;        // 3s
    lss_log_debug("Protocol: HLS");

  } else if (url.rfind("rtmp://", 0) == 0 || url.rfind("rtmps://", 0) == 0) {
    av_dict_set(&opts, "rtmp_live", "live", 0);
    av_dict_set(&opts, "rtmp_buffer", "0", 0);
    lss_log_debug("Protocol: RTMP");

  } else if (url.rfind("srt://", 0) == 0) {
    // ARQ enabled (no hardcoded latency=0) so SRT can recover dropped
    // packets and avoid permanent H.264 smearing/pixelation.
    av_dict_set(&opts, "rcvbuf", "4000000", 0);
    av_dict_set(&opts, "rw_timeout", "5000000", 0);
    fmt_ctx_->probesize = 512 * 1024;         // 512 KB
    fmt_ctx_->max_analyze_duration = 1000000; // 1s
    lss_log_debug("Protocol: SRT");

  } else if (url.rfind("rtsp://", 0) == 0 || url.rfind("rtsps://", 0) == 0) {
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0);
    lss_log_debug("Protocol: RTSP");

  } else {
    // Don't use FFmpeg's 'reconnect' here — it blocks av_read_frame ~20s.
    // ReconnectManager handles it asynchronously instead.
    lss_log_debug("Protocol: HTTP/FLV");
  }

  int ret = avformat_open_input(&fmt_ctx_, url.c_str(), nullptr, &opts);
  av_dict_free(&opts);

  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    lss_log_debug("avformat_open_input failed: %s (%s)", errbuf, url.c_str());
    fmt_ctx_ = nullptr;
    return ret;
  }

  ret = avformat_find_stream_info(fmt_ctx_, nullptr);
  if (ret < 0) {
    lss_log_error("avformat_find_stream_info failed: %d", ret);
    close();
    return ret;
  }

  video_idx_ =
      av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_idx_ >= 0) {
    AVStream *vs = fmt_ctx_->streams[video_idx_];
    lss_log_info("Selected Video Stream #%d: %dx%d, codec=%s", video_idx_,
                 vs->codecpar->width, vs->codecpar->height,
                 avcodec_get_name(vs->codecpar->codec_id));

    audio_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1,
                                     video_idx_, nullptr, 0);
    if (audio_idx_ < 0) {
      lss_log_warn("No audio stream found related to video #%d, falling back "
                   "to any audio",
                   video_idx_);
      audio_idx_ =
          av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }
  } else {
    lss_log_warn("No video stream found in %s", url.c_str());
    audio_idx_ =
        av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  }

  if (video_idx_ < 0)
    video_idx_ = -1;
  if (audio_idx_ < 0)
    audio_idx_ = -1;

  if (audio_idx_ >= 0) {
    AVStream *as = fmt_ctx_->streams[audio_idx_];
    lss_log_info("Selected Audio Stream #%d: %d Hz, %d ch, codec=%s",
                 audio_idx_, as->codecpar->sample_rate,
                 as->codecpar->ch_layout.nb_channels,
                 avcodec_get_name(as->codecpar->codec_id));
  }

  for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
    AVStream *st = fmt_ctx_->streams[i];
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      lss_log_info("  Available Video #%d: %dx%d, codec=%s%s", i,
                   st->codecpar->width, st->codecpar->height,
                   avcodec_get_name(st->codecpar->codec_id),
                   ((int)i == video_idx_) ? " [SELECTED]" : "");
    }
  }

  int discarded = 0;
  for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
    if ((int)i != video_idx_ && (int)i != audio_idx_) {
      fmt_ctx_->streams[i]->discard = AVDISCARD_ALL;
      discarded++;
    }
  }
  if (discarded > 0) {
    lss_log_info("Discarded %d unused streams (keeping video=#%d, audio=#%d)",
                 discarded, video_idx_, audio_idx_);
  }

  return 0;
}

int StreamDemuxer::open_ivs(const std::string &url) {
  lss_log_info("[IVS] Attempting custom LL-HLS client for: %s", url.c_str());

  llhls_client_ = std::make_unique<LLHlsClient>();
  if (!llhls_client_->start(url)) {
    lss_log_warn("[IVS] LLHlsClient::start() failed");
    llhls_client_.reset();
    return -1;
  }

  constexpr int BUFFER_SIZE = 32768;
  uint8_t *avio_buf =
      (unsigned char *)av_malloc(BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
  if (!avio_buf) {
    lss_log_error("[IVS] Failed to allocate AVIO buffer");
    llhls_client_->stop();
    llhls_client_.reset();
    return AVERROR(ENOMEM);
  }
  memset(avio_buf + BUFFER_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

  custom_avio_ctx_ =
      avio_alloc_context(avio_buf, BUFFER_SIZE, 0, llhls_client_.get(),
                         &LLHlsClient::avio_read_callback, nullptr, nullptr);

  if (!custom_avio_ctx_) {
    lss_log_error("[IVS] Failed to allocate custom AVIOContext");
    av_free(avio_buf);
    llhls_client_->stop();
    llhls_client_.reset();
    return AVERROR(ENOMEM);
  }

  custom_avio_ctx_->seekable = 0;

  fmt_ctx_ = avformat_alloc_context();
  if (!fmt_ctx_) {
    lss_log_error("[IVS] Failed to allocate AVFormatContext");
    avio_context_free(&custom_avio_ctx_);
    llhls_client_->stop();
    llhls_client_.reset();
    return AVERROR(ENOMEM);
  }

  fmt_ctx_->pb = custom_avio_ctx_;
  fmt_ctx_->flags |= AVFMT_FLAG_CUSTOM_IO;
  fmt_ctx_->interrupt_callback.callback = interrupt_callback;
  fmt_ctx_->interrupt_callback.opaque = this;

  fmt_ctx_->probesize = 1048576;            // 1 MB
  fmt_ctx_->max_analyze_duration = 3000000; // 3s
  fmt_ctx_->flags |= AVFMT_FLAG_DISCARD_CORRUPT;

  int ret = avformat_open_input(&fmt_ctx_, nullptr, nullptr, nullptr);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    lss_log_debug("[IVS] avformat_open_input failed: %s", errbuf);
    fmt_ctx_ = nullptr;
    custom_avio_ctx_ = nullptr;
    llhls_client_->stop();
    llhls_client_.reset();
    return ret;
  }

  ret = avformat_find_stream_info(fmt_ctx_, nullptr);
  if (ret < 0) {
    lss_log_error("[IVS] avformat_find_stream_info failed: %d", ret);
    close();
    return ret;
  }

  avformat_flush(fmt_ctx_);

  for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
    AVStream *st = fmt_ctx_->streams[i];
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      lss_log_info("[IVS]   Available Video #%d: %dx%d, codec=%s", i,
                   st->codecpar->width, st->codecpar->height,
                   avcodec_get_name(st->codecpar->codec_id));
    } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      lss_log_info("[IVS]   Available Audio #%d: %d Hz, %d ch, codec=%s", i,
                   st->codecpar->sample_rate,
                   st->codecpar->ch_layout.nb_channels,
                   avcodec_get_name(st->codecpar->codec_id));
    } else {
      lss_log_info("[IVS]   Available Unknown/Data #%d: type=%d", i,
                   st->codecpar->codec_type);
    }
  }

  video_idx_ =
      av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_idx_ >= 0) {
    AVStream *vs = fmt_ctx_->streams[video_idx_];
    lss_log_info("[IVS] Selected Video Stream #%d: %dx%d, codec=%s", video_idx_,
                 vs->codecpar->width, vs->codecpar->height,
                 avcodec_get_name(vs->codecpar->codec_id));
    audio_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1,
                                     video_idx_, nullptr, 0);
    if (audio_idx_ < 0) {
      lss_log_warn("[IVS] No audio stream found related to video #%d, falling "
                   "back to any audio",
                   video_idx_);
      audio_idx_ =
          av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }
  } else {
    lss_log_warn("[IVS] No video stream found");
    audio_idx_ =
        av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  }

  // Fallback: pick first audio stream if av_find_best_stream missed it
  if (audio_idx_ < 0) {
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
      if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        audio_idx_ = (int)i;
        lss_log_warn("[IVS] Fallback: selected Audio Stream #%d", audio_idx_);
        break;
      }
    }
  }

  if (video_idx_ < 0)
    video_idx_ = -1;
  if (audio_idx_ < 0)
    audio_idx_ = -1;

  if (audio_idx_ >= 0) {
    AVStream *as = fmt_ctx_->streams[audio_idx_];
    lss_log_info("[IVS] Selected Audio Stream #%d: %d Hz, %d ch, codec=%s",
                 audio_idx_, as->codecpar->sample_rate,
                 as->codecpar->ch_layout.nb_channels,
                 avcodec_get_name(as->codecpar->codec_id));
  }

  int discarded = 0;
  for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
    if ((int)i != video_idx_ && (int)i != audio_idx_) {
      fmt_ctx_->streams[i]->discard = AVDISCARD_ALL;
      discarded++;
    }
  }

  lss_log_info("[IVS] Custom LL-HLS client ready. "
               "video=#%d, audio=#%d, discarded=%d streams",
               video_idx_, audio_idx_, discarded);

  return 0;
}

void StreamDemuxer::close() {
  if (llhls_client_) {
    llhls_client_->stop();
  }

  // Detach custom AVIO before closing format context — avformat_close_input
  // would otherwise double-free the buffer we own.
  if (fmt_ctx_) {
    if (custom_avio_ctx_ && fmt_ctx_->pb == custom_avio_ctx_) {
      fmt_ctx_->pb = nullptr;
    }
    avformat_close_input(&fmt_ctx_);
    fmt_ctx_ = nullptr;
  }

  if (custom_avio_ctx_) {
    avio_context_free(&custom_avio_ctx_);
    custom_avio_ctx_ = nullptr;
  }

  if (llhls_client_) {
    llhls_client_.reset();
  }

  video_idx_ = -1;
  audio_idx_ = -1;
}

int StreamDemuxer::read_packet(AVPacket *pkt) {
  if (!fmt_ctx_ || abort_.load())
    return AVERROR(EINVAL);
  if (!fmt_ctx_->pb) {
    lss_log_error("read_packet: fmt_ctx_->pb is NULL!");
    return AVERROR(EIO);
  }

  static int rp_log = 0;
  if (rp_log < 5) {
    lss_log_debug("read_packet: calling av_read_frame (pb=%p, pb->buffer=%p, "
                  "pb->buf_ptr=%p, pb->buf_end=%p, pb->opaque=%p)",
                  (void *)fmt_ctx_->pb, (void *)fmt_ctx_->pb->buffer,
                  (void *)fmt_ctx_->pb->buf_ptr, (void *)fmt_ctx_->pb->buf_end,
                  (void *)fmt_ctx_->pb->opaque);
    rp_log++;
  }

  int ret = av_read_frame(fmt_ctx_, pkt);

  if (rp_log <= 5 || ret < 0) {
    lss_log_debug("read_packet: av_read_frame returned %d", ret);
    rp_log++;
  }

  return ret;
}

void StreamDemuxer::request_abort() {
  abort_.store(true);
  // signal_stop() only, not stop() — stop() joins threads which would
  // deadlock because the worker is blocked inside av_read_frame -> AVIO ->
  // read()
  if (llhls_client_) {
    llhls_client_->signal_stop();
  }
}

void StreamDemuxer::reset_abort() { abort_.store(false); }

int StreamDemuxer::interrupt_callback(void *opaque) {
  auto *self = static_cast<StreamDemuxer *>(opaque);
  return self->abort_.load() ? 1 : 0;
}

AVCodecParameters *StreamDemuxer::video_codecpar() const {
  if (!fmt_ctx_ || video_idx_ < 0)
    return nullptr;
  return fmt_ctx_->streams[video_idx_]->codecpar;
}

AVCodecParameters *StreamDemuxer::audio_codecpar() const {
  if (!fmt_ctx_ || audio_idx_ < 0)
    return nullptr;
  return fmt_ctx_->streams[audio_idx_]->codecpar;
}

AVRational StreamDemuxer::video_time_base() const {
  if (!fmt_ctx_ || video_idx_ < 0)
    return {0, 1};
  return fmt_ctx_->streams[video_idx_]->time_base;
}

AVRational StreamDemuxer::audio_time_base() const {
  if (!fmt_ctx_ || audio_idx_ < 0)
    return {0, 1};
  return fmt_ctx_->streams[audio_idx_]->time_base;
}

int StreamDemuxer::video_width() const {
  auto *par = video_codecpar();
  return par ? par->width : 0;
}

int StreamDemuxer::video_height() const {
  auto *par = video_codecpar();
  return par ? par->height : 0;
}

} // namespace lss
