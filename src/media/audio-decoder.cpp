// audio-decoder.cpp
#include "media/audio-decoder.h"

#include <algorithm>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

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

  // Initialize AudioTimeStretcher
  stretcher_.initialize(out_rate_, out_ch_);
  stretcher_initialized_ = true;
  use_fallback_atempo_ = false;
  lss_log_info("AudioTimeStretcher initialized successfully");

  current_tempo_ = 1.0;
  pending_tempo_.store(1.0);

  lss_log_info("Audio decoder opened: %s, %d Hz, %d ch", codec->name,
               codec_ctx_->sample_rate, codec_ctx_->ch_layout.nb_channels);
  return 0;
}

int AudioDecoder::decode(AVPacket *pkt, AudioFrameQueue &queue) {
  if (!codec_ctx_)
    return AVERROR(EINVAL);

  // Apply any pending tempo change before processing this packet's output.
  apply_pending_tempo(queue);

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

  int64_t pts_us = (frame->pts != AV_NOPTS_VALUE)
                       ? av_rescale_q(frame->pts, stream_tb_, {1, 1000000})
                       : now_us();

  // Fast path: tempo == 1.0 and no time-stretching — emit directly.
  if (current_tempo_ == 1.0 && !use_fallback_atempo_) {
    int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_FLTP);
    int plane_size = out_samples * bytes_per_sample;
    int total_bytes = out_samples * out_ch_ * bytes_per_sample;

    DecodedAudioFrame af;
    af.data = static_cast<uint8_t *>(av_malloc(total_bytes));
    if (!af.data) {
      av_freep(&out_buf[0]);
      return;
    }
    for (int ch = 0; ch < out_ch_; ++ch) {
      memcpy(af.data + ch * plane_size, out_buf[ch], plane_size);
    }
    af.frames = static_cast<uint32_t>(out_samples);
    af.channels = out_ch_;
    af.sample_rate = out_rate_;
    af.pts_us = pts_us;
    last_pushed_pts_us_ = af.pts_us;
    last_pushed_frames_ = af.frames;
    queue.push(std::move(af));
    av_freep(&out_buf[0]);
    return;
  }

  // SoundTouch path: process audio through time-stretcher
  if (stretcher_initialized_ && !use_fallback_atempo_) {
    // Convert planar float to interleaved float for SoundTouch
    std::vector<float> interleaved(out_samples * out_ch_);
    float *left = reinterpret_cast<float *>(out_buf[0]);
    float *right = reinterpret_cast<float *>(out_buf[1]);
    
    for (int i = 0; i < out_samples; ++i) {
      interleaved[i * 2] = left[i];
      interleaved[i * 2 + 1] = right[i];
    }
    
    // Process through SoundTouch (output buffer large enough for worst case)
    std::vector<float> output(out_samples * 4 * out_ch_); // 4x for safety
    int received = stretcher_.process(interleaved.data(), out_samples, 
                                      output.data(), out_samples * 4);
    
    if (received > 0) {
      // Convert interleaved back to planar
      int bytes_per_sample = sizeof(float);
      int plane_size = received * bytes_per_sample;
      int total_bytes = received * out_ch_ * bytes_per_sample;

      DecodedAudioFrame af;
      af.data = static_cast<uint8_t *>(av_malloc(total_bytes));
      if (af.data) {
        float *out_left = reinterpret_cast<float *>(af.data);
        float *out_right = reinterpret_cast<float *>(af.data + plane_size);

        for (int i = 0; i < received; ++i) {
          out_left[i] = output[i * 2];
          out_right[i] = output[i * 2 + 1];
        }

        af.frames = static_cast<uint32_t>(received);
        af.channels = out_ch_;
        af.sample_rate = out_rate_;
        af.pts_us = pts_us;
        last_pushed_pts_us_ = af.pts_us;
        last_pushed_frames_ = af.frames;
        queue.push(std::move(af));
      }
    }
    
    av_freep(&out_buf[0]);
    return;
  }

  // Fallback: atempo filter path (if SoundTouch failed to initialize)
  if (use_fallback_atempo_ && filter_graph_) {
    // [Keep existing atempo filter code for fallback]
    // This is the same as the original implementation
    if (!filter_src_ || !filter_sink_) {
      lss_log_warn("Filter graph not initialized; falling back to passthrough");
      av_freep(&out_buf[0]);
      return;
    }

    AVFrame *in_frame = av_frame_alloc();
    if (!in_frame) {
      av_freep(&out_buf[0]);
      return;
    }
    in_frame->format = AV_SAMPLE_FMT_FLTP;
    in_frame->nb_samples = out_samples;
    in_frame->sample_rate = out_rate_;
    av_channel_layout_default(&in_frame->ch_layout, out_ch_);
    in_frame->pts = av_rescale_q(pts_us, {1, 1000000}, {1, out_rate_});

    ret = av_frame_get_buffer(in_frame, 0);
    if (ret < 0) {
      lss_log_warn("av_frame_get_buffer failed: %d", ret);
      av_frame_free(&in_frame);
      av_freep(&out_buf[0]);
      return;
    }

    for (int ch = 0; ch < out_ch_; ++ch) {
      if (!in_frame->data[ch]) {
        lss_log_warn("in_frame->data[%d] is null after get_buffer", ch);
        av_frame_free(&in_frame);
        av_freep(&out_buf[0]);
        return;
      }
    }

    int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_FLTP);
    int plane_size = out_samples * bytes_per_sample;
    for (int ch = 0; ch < out_ch_; ++ch) {
      memcpy(in_frame->data[ch], out_buf[ch], plane_size);
    }
    av_freep(&out_buf[0]);

    ret = av_buffersrc_add_frame_flags(filter_src_, in_frame,
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_free(&in_frame);
    if (ret < 0) {
      lss_log_warn("av_buffersrc_add_frame_flags failed: %d", ret);
      return;
    }

    AVFrame *out_frame = av_frame_alloc();
    if (!out_frame)
      return;
    while (true) {
      ret = av_buffersink_get_frame(filter_sink_, out_frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        break;
      if (ret < 0) {
        lss_log_warn("av_buffersink_get_frame failed: %d", ret);
        break;
      }

      int n = out_frame->nb_samples;
      int sink_ch = out_frame->ch_layout.nb_channels;
      int sink_fmt = out_frame->format;

      if (sink_fmt != AV_SAMPLE_FMT_FLTP || sink_ch != out_ch_ || n <= 0) {
        lss_log_warn("Filter output unexpected: fmt=%d ch=%d n=%d",
                     sink_fmt, sink_ch, n);
        av_frame_unref(out_frame);
        continue;
      }
      for (int ch = 0; ch < sink_ch; ++ch) {
        if (!out_frame->data[ch]) {
          lss_log_warn("Filter output data[%d] is null", ch);
          av_frame_unref(out_frame);
          goto next_iter;
        }
      }

      {
        int64_t out_pts_us = (out_frame->pts != AV_NOPTS_VALUE)
                                 ? av_rescale_q(out_frame->pts, {1, out_rate_},
                                                {1, 1000000})
                                 : pts_us;
        int bps = av_get_bytes_per_sample((AVSampleFormat)sink_fmt);
        int total = n * sink_ch * bps;

        DecodedAudioFrame af;
        af.data = static_cast<uint8_t *>(av_malloc(total));
        if (af.data) {
          int ps = n * bps;
          for (int ch = 0; ch < sink_ch; ++ch) {
            memcpy(af.data + ch * ps, out_frame->data[ch], ps);
          }
          af.frames = static_cast<uint32_t>(n);
          af.channels = sink_ch;
          af.sample_rate = out_rate_;
          af.pts_us = out_pts_us;
          last_pushed_pts_us_ = af.pts_us;
          last_pushed_frames_ = af.frames;
          queue.push(std::move(af));
        }
      }
      av_frame_unref(out_frame);
    next_iter:;
    }
    av_frame_free(&out_frame);
    return;
  }

  // Ultimate fallback: passthrough
  av_freep(&out_buf[0]);
}

void AudioDecoder::drain_stretcher(AudioFrameQueue &queue) {
  if (!stretcher_initialized_ || use_fallback_atempo_)
    return;

  // Flush SoundTouch's internal buffer
  std::vector<float> output(8192 * out_ch_);
  int received = stretcher_.flush(output.data(), 8192);

  if (received > 0) {
    // Convert interleaved back to planar
    int bytes_per_sample = sizeof(float);
    int plane_size = received * bytes_per_sample;
    int total_bytes = received * out_ch_ * bytes_per_sample;

    DecodedAudioFrame af;
    af.data = static_cast<uint8_t *>(av_malloc(total_bytes));
    if (af.data) {
      float *out_left = reinterpret_cast<float *>(af.data);
      float *out_right = reinterpret_cast<float *>(af.data + plane_size);

      for (int i = 0; i < received; ++i) {
        out_left[i] = output[i * 2];
        out_right[i] = output[i * 2 + 1];
      }

      af.frames = static_cast<uint32_t>(received);
      af.channels = out_ch_;
      af.sample_rate = out_rate_;
      // PTS continues right after the last pushed frame in stream-time
      // domain. Using now_us() (wall clock) instead would be interpreted
      // as a stream PTS downstream and produce a wildly wrong OBS timestamp.
      if (last_pushed_pts_us_ > 0 && out_rate_ > 0) {
        int64_t last_dur_us = (last_pushed_frames_ * 1000000LL) / out_rate_;
        af.pts_us = last_pushed_pts_us_ + last_dur_us;
      } else {
        af.pts_us = now_us();
      }
      last_pushed_pts_us_ = af.pts_us;
      last_pushed_frames_ = af.frames;
      queue.push(std::move(af));
    }
  }
}

void AudioDecoder::drain_filter(AudioFrameQueue &queue) {
  if (!filter_graph_ || !filter_src_ || !filter_sink_)
    return;

  // Send EOF to flush atempo's internal buffer.
  int ret = av_buffersrc_add_frame_flags(filter_src_, nullptr, 0);
  if (ret < 0)
    return;

  AVFrame *out_frame = av_frame_alloc();
  if (!out_frame)
    return;
  while (true) {
    ret = av_buffersink_get_frame(filter_sink_, out_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      break;
    if (ret < 0)
      break;

    int n = out_frame->nb_samples;
    int sink_ch = out_frame->ch_layout.nb_channels;
    int sink_fmt = out_frame->format;
    if (sink_fmt != AV_SAMPLE_FMT_FLTP || sink_ch != out_ch_ || n <= 0) {
      av_frame_unref(out_frame);
      continue;
    }
    int bps = av_get_bytes_per_sample((AVSampleFormat)sink_fmt);
    int total = n * sink_ch * bps;
    DecodedAudioFrame af;
    af.data = static_cast<uint8_t *>(av_malloc(total));
    if (af.data) {
      int ps = n * bps;
      for (int ch = 0; ch < sink_ch; ++ch) {
        memcpy(af.data + ch * ps, out_frame->data[ch], ps);
      }
      af.frames = static_cast<uint32_t>(n);
      af.channels = sink_ch;
      af.sample_rate = out_rate_;
      af.pts_us = (out_frame->pts != AV_NOPTS_VALUE)
                      ? av_rescale_q(out_frame->pts, {1, out_rate_},
                                     {1, 1000000})
                      : now_us();
      queue.push(std::move(af));
    }
    av_frame_unref(out_frame);
  }
  av_frame_free(&out_frame);
}

void AudioDecoder::set_tempo(double tempo) {
  // SoundTouch accepts 0.5–4.0 range
  // For tempo > 2.0, audio quality degrades but is still usable
  tempo = std::clamp(tempo, 0.5, 4.0);
  pending_tempo_.store(tempo);
}

bool AudioDecoder::apply_pending_tempo(AudioFrameQueue &queue) {
  double want = pending_tempo_.load();
  if (want == current_tempo_)
    return true;

  // SoundTouch path
  if (stretcher_initialized_ && !use_fallback_atempo_) {
    // When returning to 1.0x, drain SoundTouch's internal ~40ms of stretched
    // samples INTO the queue instead of clearing them. The old reset() path
    // called st_->clear() which discards those samples — that vanished chunk
    // is what the user heard as a tiny pıt at the catchup exit. drain_stretcher
    // flushes via st_->flush()+receiveSamples() so OBS receives every sample.
    if (want == 1.0 && current_tempo_ > 1.001) {
      drain_stretcher(queue);
    }
    stretcher_.set_tempo(want);
    current_tempo_ = want;
    lss_log_debug("Audio tempo: %.4fx (SoundTouch)", want);
    return true;
  }

  // Fallback: atempo filter path
  if (use_fallback_atempo_) {
    // For tempo > 2.0, mute audio (atempo can't handle it)
    if (want == 1.0 || want > 2.0) {
      teardown_filter_graph();
      current_tempo_ = want;
      if (want > 2.0) {
        lss_log_info("Audio tempo: %.2fx (muted - too fast for atempo)", want);
      } else {
        lss_log_debug("Audio tempo: passthrough (1.0x, filter torn down)");
      }
      return true;
    }

    // Live update on existing graph
    if (filter_graph_ && current_tempo_ != 1.0 && current_tempo_ <= 2.0) {
      char arg[32];
      snprintf(arg, sizeof(arg), "%.5f", want);
      int ret = avfilter_graph_send_command(filter_graph_, "atempo", "tempo", arg,
                                            nullptr, 0, 0);
      if (ret >= 0) {
        current_tempo_ = want;
        lss_log_debug("Audio tempo: %.4fx (atempo live update)", want);
        return true;
      }
      lss_log_warn("avfilter_graph_send_command failed (%d), rebuilding graph", ret);
      teardown_filter_graph();
    }

    // Build graph at requested tempo
    if (want <= 2.0 && build_filter_graph(want) == 0) {
      current_tempo_ = want;
      lss_log_info("Audio tempo: %.4fx (atempo filter graph built)", want);
      return true;
    }

    lss_log_warn("Failed to build atempo filter graph; falling back to 1.0x");
    current_tempo_ = 1.0;
    pending_tempo_.store(1.0);
    return false;
  }

  // No stretcher available, just update tempo
  current_tempo_ = want;
  return true;
}

int AudioDecoder::build_filter_graph(double tempo) {
  teardown_filter_graph();

  filter_graph_ = avfilter_graph_alloc();
  if (!filter_graph_)
    return AVERROR(ENOMEM);

  const AVFilter *abuffer = avfilter_get_by_name("abuffer");
  const AVFilter *atempo = avfilter_get_by_name("atempo");
  const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
  if (!abuffer || !atempo || !abuffersink) {
    lss_log_error("Missing required filters (abuffer/atempo/abuffersink)");
    teardown_filter_graph();
    return AVERROR_FILTER_NOT_FOUND;
  }

  char src_args[256];
  snprintf(src_args, sizeof(src_args),
           "sample_rate=%d:sample_fmt=%s:channel_layout=stereo:time_base=1/%d",
           out_rate_, av_get_sample_fmt_name(AV_SAMPLE_FMT_FLTP), out_rate_);

  int ret = avfilter_graph_create_filter(&filter_src_, abuffer, "in", src_args,
                                         nullptr, filter_graph_);
  if (ret < 0) {
    lss_log_error("abuffer create failed: %d", ret);
    teardown_filter_graph();
    return ret;
  }

  AVFilterContext *atempo_ctx = nullptr;
  char tempo_args[32];
  snprintf(tempo_args, sizeof(tempo_args), "tempo=%.5f", tempo);
  ret = avfilter_graph_create_filter(&atempo_ctx, atempo, "atempo", tempo_args,
                                     nullptr, filter_graph_);
  if (ret < 0) {
    lss_log_error("atempo create failed: %d", ret);
    teardown_filter_graph();
    return ret;
  }

  ret = avfilter_graph_create_filter(&filter_sink_, abuffersink, "out", nullptr,
                                     nullptr, filter_graph_);
  if (ret < 0) {
    lss_log_error("abuffersink create failed: %d", ret);
    teardown_filter_graph();
    return ret;
  }

  static const enum AVSampleFormat out_fmts[] = {AV_SAMPLE_FMT_FLTP,
                                                 AV_SAMPLE_FMT_NONE};
  ret = av_opt_set_int_list(filter_sink_, "sample_fmts", out_fmts,
                            AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
  if (ret < 0) {
    lss_log_warn("av_opt_set_int_list(sample_fmts) failed: %d", ret);
  }

  ret = avfilter_link(filter_src_, 0, atempo_ctx, 0);
  if (ret >= 0)
    ret = avfilter_link(atempo_ctx, 0, filter_sink_, 0);
  if (ret < 0) {
    lss_log_error("avfilter_link failed: %d", ret);
    teardown_filter_graph();
    return ret;
  }

  ret = avfilter_graph_config(filter_graph_, nullptr);
  if (ret < 0) {
    lss_log_error("avfilter_graph_config failed: %d", ret);
    teardown_filter_graph();
    return ret;
  }

  return 0;
}

void AudioDecoder::teardown_filter_graph() {
  if (filter_graph_) {
    avfilter_graph_free(&filter_graph_);
    filter_graph_ = nullptr;
  }
  filter_src_ = nullptr;
  filter_sink_ = nullptr;
}

void AudioDecoder::close() {
  teardown_filter_graph();
  if (stretcher_initialized_) {
    stretcher_.reset();
    stretcher_initialized_ = false;
  }
  if (swr_ctx_) {
    swr_free(&swr_ctx_);
    swr_ctx_ = nullptr;
  }
  if (codec_ctx_) {
    avcodec_free_context(&codec_ctx_);
    codec_ctx_ = nullptr;
  }
  consecutive_errors_ = 0;
  current_tempo_ = 1.0;
  pending_tempo_.store(1.0);
  use_fallback_atempo_ = false;
}

void AudioDecoder::reset_state() {
  if (!codec_ctx_)
    return;

  // Flush the codec to clear any corrupted internal state
  avcodec_flush_buffers(codec_ctx_);

  // Drain the resampler's internal buffered samples
  if (swr_ctx_) {
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

  // Reset AudioTimeStretcher
  if (stretcher_initialized_) {
    stretcher_.reset();
  }

  // Tear down filter graph (fallback)
  teardown_filter_graph();
  current_tempo_ = 1.0;
  pending_tempo_.store(1.0);

  consecutive_errors_ = 0;
  lss_log_debug("Audio decoder state reset");
}

} // namespace lss
