// audio-decoder.cpp
#include "media/audio-decoder.h"

namespace lss {

AudioDecoder::~AudioDecoder() { close(); }

int AudioDecoder::init(const AVCodecParameters *par) {
  close();

  const AVCodec *codec = avcodec_find_decoder(par->codec_id);
  if (!codec) {
    lss_log_error("No audio decoder for %s", avcodec_get_name(par->codec_id));
    return AVERROR_DECODER_NOT_FOUND;
  }

  codec_ctx_ = avcodec_alloc_context3(codec);
  if (!codec_ctx_)
    return AVERROR(ENOMEM);

  int ret = avcodec_parameters_to_context(codec_ctx_, par);
  if (ret < 0) {
    close();
    return ret;
  }

  // Audio decoding is fast enough that threading only adds unnecessary latency.
  codec_ctx_->thread_count = 1;

  ret = avcodec_open2(codec_ctx_, codec, nullptr);
  if (ret < 0) {
    close();
    return ret;
  }

  AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;

  // Use input sample rate to avoid pitch shift
  out_rate_ = codec_ctx_->sample_rate;

  ret = swr_alloc_set_opts2(&swr_ctx_,
                            &out_layout,             // out channel layout
                            AV_SAMPLE_FMT_FLTP,      // out sample format
                            out_rate_,               // out sample rate
                            &codec_ctx_->ch_layout,  // in channel layout
                            codec_ctx_->sample_fmt,  // in sample format
                            codec_ctx_->sample_rate, // in sample rate
                            0, nullptr);
  if (ret < 0 || !swr_ctx_) {
    lss_log_error("swr_alloc_set_opts2 failed: %d", ret);
    close();
    return ret;
  }

  ret = swr_init(swr_ctx_);
  if (ret < 0) {
    lss_log_error("swr_init failed: %d", ret);
    close();
    return ret;
  }

  lss_log_info("Audio decoder opened: %s, %d Hz, %d ch", codec->name,
               codec_ctx_->sample_rate, codec_ctx_->ch_layout.nb_channels);
  return 0;
}

int AudioDecoder::decode(AVPacket *pkt, AudioFrameQueue &queue) {
  if (!codec_ctx_)
    return AVERROR(EINVAL);

  int ret = avcodec_send_packet(codec_ctx_, pkt);
  if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
    consecutive_errors_++;
    if (consecutive_errors_ >= ERROR_THRESHOLD) {
      lss_log_warn("Audio: %d consecutive errors, resetting decoder state",
                   consecutive_errors_);
      reset_state();
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
        lss_log_warn(
            "Audio: %d consecutive receive errors, resetting decoder state",
            consecutive_errors_);
        av_frame_free(&frame);
        reset_state();
        return ret;
      }
      av_frame_free(&frame);
      return ret;
    }

    decoded_any = true;
    push_resampled(frame, queue);
    av_frame_unref(frame);
  }

  if (decoded_any)
    consecutive_errors_ = 0;

  av_frame_free(&frame);
  return 0;
}

void AudioDecoder::flush(AudioFrameQueue &queue) {
  if (!codec_ctx_)
    return;
  avcodec_send_packet(codec_ctx_, nullptr);

  AVFrame *frame = av_frame_alloc();
  if (!frame)
    return;

  while (avcodec_receive_frame(codec_ctx_, frame) == 0) {
    push_resampled(frame, queue);
    av_frame_unref(frame);
  }
  av_frame_free(&frame);
}

void AudioDecoder::push_resampled(AVFrame *frame, AudioFrameQueue &queue) {
  if (!swr_ctx_)
    return;

  // Calculate output sample count
  int out_samples = swr_get_out_samples(swr_ctx_, frame->nb_samples);
  if (out_samples <= 0)
    return;

  // Allocate output buffer (float planar, stereo)
  uint8_t *out_buf[2] = {};
  int out_linesize = 0;
  int ret = av_samples_alloc(out_buf, &out_linesize, out_ch_, out_samples,
                             AV_SAMPLE_FMT_FLTP, 0);
  if (ret < 0)
    return;

  out_samples = swr_convert(swr_ctx_, out_buf, out_samples,
                            (const uint8_t **)frame->data, frame->nb_samples);
  if (out_samples <= 0) {
    av_freep(&out_buf[0]);
    return;
  }

  // Pack into a contiguous buffer for the queue
  int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_FLTP);
  int total_bytes = out_samples * out_ch_ * bytes_per_sample;

  DecodedAudioFrame af;
  af.data = static_cast<uint8_t *>(av_malloc(total_bytes));
  if (!af.data) {
    av_freep(&out_buf[0]);
    return;
  }

  // Copy planar channels contiguously
  int plane_size = out_samples * bytes_per_sample;
  for (int ch = 0; ch < out_ch_; ++ch) {
    memcpy(af.data + ch * plane_size, out_buf[ch], plane_size);
  }

  af.frames = static_cast<uint32_t>(out_samples);
  af.channels = out_ch_;
  af.sample_rate = out_rate_;
  af.pts_us = (frame->pts != AV_NOPTS_VALUE)
                  ? av_rescale_q(frame->pts, stream_tb_, {1, 1000000})
                  : now_us();

  queue.push(std::move(af));
  av_freep(&out_buf[0]);
}

void AudioDecoder::close() {
  if (swr_ctx_) {
    swr_free(&swr_ctx_);
    swr_ctx_ = nullptr;
  }
  if (codec_ctx_) {
    avcodec_free_context(&codec_ctx_);
    codec_ctx_ = nullptr;
  }
  consecutive_errors_ = 0;
}

void AudioDecoder::reset_state() {
  if (!codec_ctx_)
    return;

  // Flush the codec to clear any corrupted internal state
  avcodec_flush_buffers(codec_ctx_);

  // Drain the resampler's internal buffered samples
  if (swr_ctx_) {
    // Allocate a temporary buffer to drain residual samples
    uint8_t *tmp_buf[2] = {};
    int tmp_linesize = 0;
    int drain_samples = swr_get_out_samples(swr_ctx_, 0);
    if (drain_samples > 0) {
      if (av_samples_alloc(tmp_buf, &tmp_linesize, out_ch_, drain_samples,
                           AV_SAMPLE_FMT_FLTP, 0) >= 0) {
        swr_convert(swr_ctx_, tmp_buf, drain_samples, nullptr, 0);
        av_freep(&tmp_buf[0]);
      }
    }
  }

  consecutive_errors_ = 0;
  lss_log_debug("Audio decoder state reset");
}

} // namespace lss
