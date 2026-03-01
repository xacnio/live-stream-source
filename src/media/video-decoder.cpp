// video-decoder.cpp
#include "media/video-decoder.h"

namespace lss {

VideoDecoder::~VideoDecoder() { close(); }

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts) {
  const AVPixelFormat *required_fmt =
      static_cast<const AVPixelFormat *>(ctx->opaque);

  for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
    if (*p == *required_fmt)
      return *p;
  }

  lss_log_warn("HW pixel format not available, falling back to SW");
  return pix_fmts[0];
}

int VideoDecoder::init_hw_decoder(const AVCodec *codec,
                                  const AVCodecParameters *par) {
  static const AVHWDeviceType hw_types[] = {
#ifdef _WIN32
      AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2,
#elif defined(__APPLE__)
      AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#else
      AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_VDPAU,
#endif
      AV_HWDEVICE_TYPE_NONE};

  for (int i = 0; hw_types[i] != AV_HWDEVICE_TYPE_NONE; ++i) {
    AVHWDeviceType type = hw_types[i];

    AVPixelFormat found_fmt = AV_PIX_FMT_NONE;
    for (int j = 0;; j++) {
      const AVCodecHWConfig *config = avcodec_get_hw_config(codec, j);
      if (!config)
        break;
      if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
          config->device_type == type) {
        found_fmt = config->pix_fmt;
        break;
      }
    }

    if (found_fmt == AV_PIX_FMT_NONE)
      continue;

    int ret =
        av_hwdevice_ctx_create(&hw_device_ctx_, type, nullptr, nullptr, 0);
    if (ret < 0) {
      lss_log_warn("Failed to create HW device %s: %d",
                   av_hwdevice_get_type_name(type), ret);
      continue;
    }

    hw_pix_fmt_ = found_fmt;

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
      av_buffer_unref(&hw_device_ctx_);
      return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_to_context(codec_ctx_, par);
    if (ret < 0) {
      close();
      return ret;
    }

    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    codec_ctx_->thread_count = 1;
    codec_ctx_->opaque = &hw_pix_fmt_;
    codec_ctx_->get_format = get_hw_format;

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
      lss_log_warn("Failed to open HW decoder %s: %d",
                   av_hwdevice_get_type_name(type), ret);
      avcodec_free_context(&codec_ctx_);
      av_buffer_unref(&hw_device_ctx_);
      hw_pix_fmt_ = AV_PIX_FMT_NONE;
      continue;
    }

    hw_frame_ = av_frame_alloc();

    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;

    lss_log_info("HW video decoder opened: %s (%s), %dx%d", codec->name,
                 av_hwdevice_get_type_name(type), width_, height_);
    return 0;
  }

  return AVERROR(ENODEV);
}

int VideoDecoder::init(const AVCodecParameters *par, bool hw_accel) {
  close();

  const AVCodec *codec = avcodec_find_decoder(par->codec_id);
  if (!codec) {
    lss_log_error("No decoder found for codec %s",
                  avcodec_get_name(par->codec_id));
    return AVERROR_DECODER_NOT_FOUND;
  }

  // Try HW accel if requested and resolution is known
  if (hw_accel && par->width > 0 && par->height > 0) {
    int ret = init_hw_decoder(codec, par);

    if (ret == 0) {
      return 0;
    }
    lss_log_info("HW decode unavailable, falling back to software");
  }

  codec_ctx_ = avcodec_alloc_context3(codec);
  if (!codec_ctx_)
    return AVERROR(ENOMEM);

  int ret = avcodec_parameters_to_context(codec_ctx_, par);
  if (ret < 0) {
    lss_log_error("avcodec_parameters_to_context failed: %d", ret);
    close();
    return ret;
  }

  codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;

  // Single-thread decode to avoid crashes during resolution changes
  codec_ctx_->thread_count = 1;

  ret = avcodec_open2(codec_ctx_, codec, nullptr);
  if (ret < 0) {
    lss_log_error("avcodec_open2 failed: %d", ret);
    close();
    return ret;
  }

  width_ = codec_ctx_->width;
  height_ = codec_ctx_->height;

  lss_log_info("SW video decoder opened: %s, %dx%d", codec->name, width_,
               height_);

  return 0;
}

int VideoDecoder::decode(AVPacket *pkt,
                         std::function<void(AVFrame *)> callback) {
  if (!codec_ctx_)
    return AVERROR(EINVAL);

  static int decode_log_counter = 0;
  if (decode_log_counter < 5) {
    lss_log_debug(
        "[VideoDecoder] Decoding packet: size=%d, pts=%lld, dts=%lld, key=%d",
        pkt->size, (long long)pkt->pts, (long long)pkt->dts,
        (pkt->flags & AV_PKT_FLAG_KEY));
    decode_log_counter++;
  }

  int ret = avcodec_send_packet(codec_ctx_, pkt);
  if (ret < 0) {
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
      consecutive_errors_++;
      if (consecutive_errors_ >= ERROR_THRESHOLD) {
        lss_log_warn("Video: %d consecutive send errors (code=%d), resetting "
                     "decoder state",
                     consecutive_errors_, ret);
        reset_state();
      } else if (consecutive_errors_ < 5) {
        lss_log_warn("avcodec_send_packet failed: %d", ret);
      }
    }
    return ret;
  }

  AVFrame *frame = av_frame_alloc();
  if (!frame)
    return AVERROR(ENOMEM);

  bool decoded_any = false;
  while (true) {
    ret = avcodec_receive_frame(codec_ctx_, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      break;

    if (ret < 0) {
      consecutive_errors_++;
      if (consecutive_errors_ >= ERROR_THRESHOLD) {
        lss_log_warn("Video: %d consecutive receive errors (code=%d), "
                     "resetting decoder state",
                     consecutive_errors_, ret);
        reset_state();
      } else if (consecutive_errors_ < 5) {
        lss_log_warn("avcodec_receive_frame failed: %d", ret);
      }
      break;
    }

    decoded_any = true;

    if (hw_device_ctx_ && frame->format == hw_pix_fmt_) {
      // Transfer HW frame to system memory
      if (!hw_frame_)
        hw_frame_ = av_frame_alloc();

      ret = av_hwframe_transfer_data(hw_frame_, frame, 0);
      if (ret < 0) {
        lss_log_warn("HW frame transfer failed: %d", ret);
        av_frame_unref(frame);
        continue;
      }
      hw_frame_->pts = frame->pts;
      hw_frame_->pkt_dts = frame->pkt_dts;
      hw_frame_->flags = frame->flags; // keyframe info etc.

      callback(hw_frame_);

      av_frame_unref(hw_frame_);
    } else {
      // SW frame: pass directly
      callback(frame);
    }

    av_frame_unref(frame);
  }

  if (decoded_any) {
    consecutive_errors_ = 0;
  }

  av_frame_free(&frame);
  return 0;
}

void VideoDecoder::flush(std::function<void(AVFrame *)> callback) {
  if (!codec_ctx_)
    return;

  avcodec_send_packet(codec_ctx_, nullptr);

  AVFrame *frame = av_frame_alloc();
  if (!frame)
    return;

  while (avcodec_receive_frame(codec_ctx_, frame) == 0) {
    if (hw_device_ctx_ && frame->format == hw_pix_fmt_) {
      if (!hw_frame_)
        hw_frame_ = av_frame_alloc();
      if (av_hwframe_transfer_data(hw_frame_, frame, 0) == 0) {
        hw_frame_->pts = frame->pts;
        hw_frame_->pkt_dts = frame->pkt_dts;
        hw_frame_->flags = frame->flags;
        callback(hw_frame_);
        av_frame_unref(hw_frame_);
      }
    } else {
      callback(frame);
    }
    av_frame_unref(frame);
  }

  av_frame_free(&frame);
}

void VideoDecoder::close() {
  if (sws_ctx_) {
    sws_freeContext(sws_ctx_);
    sws_ctx_ = nullptr;
  }
  if (tmp_frame_) {
    av_frame_free(&tmp_frame_);
    tmp_frame_ = nullptr;
  }
  if (hw_frame_) {
    av_frame_free(&hw_frame_);
    hw_frame_ = nullptr;
  }
  if (codec_ctx_) {
    avcodec_free_context(&codec_ctx_);
    codec_ctx_ = nullptr;
  }
  if (hw_device_ctx_) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }
  hw_pix_fmt_ = AV_PIX_FMT_NONE;
  width_ = 0;
  height_ = 0;
  consecutive_errors_ = 0;
}

void VideoDecoder::reset_state() {
  if (!codec_ctx_)
    return;

  avcodec_flush_buffers(codec_ctx_);
  consecutive_errors_ = 0;
  lss_log_info("Video decoder state reset");
}

} // namespace lss
