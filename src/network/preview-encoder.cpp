// preview-encoder.cpp
// Implementation: downscale → MJPEG via FFmpeg's avcodec, plus a Float32
// 16 kHz mono audio resampler. Both paths reuse the FFmpeg libraries OBS
// already links, so this module adds no new dependency.

#include "network/preview-encoder.h"
#include "core/common.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace lss {

PreviewEncoder::~PreviewEncoder() { close(); }

void PreviewEncoder::close() {
  if (jpeg_ctx_) {
    avcodec_free_context(&jpeg_ctx_);
    jpeg_ctx_ = nullptr;
  }
  if (sws_ctx_) {
    sws_freeContext(sws_ctx_);
    sws_ctx_ = nullptr;
  }
  if (scaled_frame_) {
    av_frame_free(&scaled_frame_);
    scaled_frame_ = nullptr;
  }
  if (swr_ctx_) {
    swr_free(&swr_ctx_);
    swr_ctx_ = nullptr;
  }
}

bool PreviewEncoder::ensure_video_ctx(int src_w, int src_h,
                                      AVPixelFormat src_fmt) {
  if (jpeg_ctx_ && sws_ctx_ &&
      sws_in_w_ == src_w && sws_in_h_ == src_h && sws_in_fmt_ == src_fmt) {
    return true;
  }

  // Rebuild on resolution / format change.
  if (sws_ctx_) {
    sws_freeContext(sws_ctx_);
    sws_ctx_ = nullptr;
  }

  // MJPEG encoder expects YUVJ420P (full-range) input.
  sws_ctx_ = sws_getContext(src_w, src_h, src_fmt,
                            PREVIEW_WIDTH, PREVIEW_HEIGHT, AV_PIX_FMT_YUVJ420P,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!sws_ctx_) {
    lss_log_warn("preview: sws_getContext failed (%dx%d %d → %dx%d)",
                 src_w, src_h, (int)src_fmt, PREVIEW_WIDTH, PREVIEW_HEIGHT);
    return false;
  }
  sws_in_w_   = src_w;
  sws_in_h_   = src_h;
  sws_in_fmt_ = src_fmt;

  if (!scaled_frame_) {
    scaled_frame_ = av_frame_alloc();
    if (!scaled_frame_) return false;
    scaled_frame_->format = AV_PIX_FMT_YUVJ420P;
    scaled_frame_->width  = PREVIEW_WIDTH;
    scaled_frame_->height = PREVIEW_HEIGHT;
    if (av_frame_get_buffer(scaled_frame_, 32) < 0) {
      av_frame_free(&scaled_frame_);
      return false;
    }
  }

  if (!jpeg_ctx_) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
      lss_log_warn("preview: MJPEG encoder not found");
      return false;
    }
    jpeg_ctx_ = avcodec_alloc_context3(codec);
    if (!jpeg_ctx_) return false;
    jpeg_ctx_->pix_fmt   = AV_PIX_FMT_YUVJ420P;
    jpeg_ctx_->width     = PREVIEW_WIDTH;
    jpeg_ctx_->height    = PREVIEW_HEIGHT;
    jpeg_ctx_->time_base = {1, PREVIEW_FPS};
    jpeg_ctx_->framerate = {PREVIEW_FPS, 1};
    // qscale → quality. Lower = better quality, bigger file.
    jpeg_ctx_->flags        |= AV_CODEC_FLAG_QSCALE;
    jpeg_ctx_->global_quality = FF_QP2LAMBDA * PREVIEW_JPEG_QUALITY;
    if (avcodec_open2(jpeg_ctx_, codec, nullptr) < 0) {
      lss_log_warn("preview: MJPEG avcodec_open2 failed");
      avcodec_free_context(&jpeg_ctx_);
      return false;
    }
  }
  return true;
}

bool PreviewEncoder::encode_video_jpeg(const AVFrame *frame,
                                       std::vector<uint8_t> &out) {
  if (!frame || frame->width <= 0 || frame->height <= 0) return false;

  // Throttle to PREVIEW_FPS. The source might run at 30/60 fps; we don't
  // need that bandwidth for a monitor view.
  int64_t now = now_ms();
  int min_interval_ms = 1000 / PREVIEW_FPS;
  if (last_video_ms_ != 0 && (now - last_video_ms_) < min_interval_ms)
    return false;

  if (!ensure_video_ctx(frame->width, frame->height,
                        (AVPixelFormat)frame->format))
    return false;

  // Scale source → 480p YUVJ420P
  uint8_t *dst[4] = {scaled_frame_->data[0], scaled_frame_->data[1],
                      scaled_frame_->data[2], nullptr};
  int dst_ls[4] = {scaled_frame_->linesize[0], scaled_frame_->linesize[1],
                    scaled_frame_->linesize[2], 0};
  sws_scale(sws_ctx_, frame->data, frame->linesize, 0, frame->height, dst,
            dst_ls);
  scaled_frame_->pts = now;

  // Encode → AVPacket
  if (avcodec_send_frame(jpeg_ctx_, scaled_frame_) < 0)
    return false;

  AVPacket *pkt = av_packet_alloc();
  if (!pkt) return false;

  int ret = avcodec_receive_packet(jpeg_ctx_, pkt);
  if (ret < 0) {
    av_packet_free(&pkt);
    return false;
  }

  // Prefix with 1-byte type tag (0x01 = video JPEG) and append payload.
  out.push_back(0x01);
  out.insert(out.end(), pkt->data, pkt->data + pkt->size);
  av_packet_free(&pkt);

  last_video_ms_ = now;
  return true;
}

bool PreviewEncoder::ensure_audio_ctx(int src_rate, int src_ch,
                                      int src_sample_fmt) {
  if (swr_ctx_ && swr_in_rate_ == src_rate && swr_in_ch_ == src_ch &&
      swr_in_fmt_ == src_sample_fmt) {
    return true;
  }
  if (swr_ctx_) {
    swr_free(&swr_ctx_);
    swr_ctx_ = nullptr;
  }

  AVChannelLayout in_ch_layout;
  AVChannelLayout out_ch_layout;
  av_channel_layout_default(&in_ch_layout, src_ch);
  av_channel_layout_default(&out_ch_layout, AUDIO_OUT_CH);

  int rc = swr_alloc_set_opts2(
      &swr_ctx_, &out_ch_layout, AV_SAMPLE_FMT_FLT, AUDIO_OUT_RATE,
      &in_ch_layout, (AVSampleFormat)src_sample_fmt, src_rate, 0, nullptr);
  av_channel_layout_uninit(&in_ch_layout);
  av_channel_layout_uninit(&out_ch_layout);
  if (rc < 0 || !swr_ctx_) return false;
  if (swr_init(swr_ctx_) < 0) {
    swr_free(&swr_ctx_);
    return false;
  }
  swr_in_rate_ = src_rate;
  swr_in_ch_   = src_ch;
  swr_in_fmt_  = src_sample_fmt;
  return true;
}

bool PreviewEncoder::encode_audio_pcm(const uint8_t *const *src_data,
                                      int src_samples, int src_rate,
                                      int src_channels, int src_sample_fmt,
                                      std::vector<uint8_t> &out) {
  if (src_samples <= 0) return false;
  if (!ensure_audio_ctx(src_rate, src_channels, src_sample_fmt))
    return false;

  // Estimate output sample count. Worst case: rate ratio plus a small pad.
  int max_out = (int)av_rescale_rnd(
      swr_get_delay(swr_ctx_, src_rate) + src_samples, AUDIO_OUT_RATE,
      src_rate, AV_ROUND_UP) + 16;

  size_t bytes_per_sample = sizeof(float) * AUDIO_OUT_CH;
  size_t tag_offset = out.size();
  out.push_back(0x02); // type tag = audio PCM Float32 mono @ 16 kHz
  size_t data_offset = out.size();
  out.resize(data_offset + max_out * bytes_per_sample);

  uint8_t *out_planes[1] = {out.data() + data_offset};
  int produced = swr_convert(swr_ctx_, out_planes, max_out,
                              const_cast<const uint8_t **>(src_data), src_samples);
  if (produced <= 0) {
    out.resize(tag_offset);
    return false;
  }
  out.resize(data_offset + produced * bytes_per_sample);
  return true;
}

} // namespace lss
