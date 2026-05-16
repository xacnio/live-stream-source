// audio-time-stretcher.cpp - SoundTouch wrapper implementation
#include "media/audio-time-stretcher.h"

#include <SoundTouch.h>
#include <algorithm>

namespace lss {

AudioTimeStretcher::AudioTimeStretcher()
    : st_(nullptr), sample_rate_(0), channels_(0), current_tempo_(1.0) {}

AudioTimeStretcher::~AudioTimeStretcher() {
  if (st_) {
    lss_log_debug("AudioTimeStretcher destructor: cleaning up SoundTouch instance");
    delete st_;
    st_ = nullptr;
  }
}

void AudioTimeStretcher::initialize(int sample_rate, int channels) {
  lss_log_debug("AudioTimeStretcher::initialize called: %d Hz, %d ch", 
                sample_rate, channels);

  // Clean up existing instance if any
  if (st_) {
    lss_log_debug("Cleaning up existing SoundTouch instance");
    delete st_;
    st_ = nullptr;
  }

  // Create new SoundTouch instance
  st_ = new soundtouch::SoundTouch();

  // Store audio format parameters
  sample_rate_ = sample_rate;
  channels_ = channels;
  current_tempo_ = 1.0;

  // Configure SoundTouch settings
  st_->setSampleRate(sample_rate);
  st_->setChannels(channels);

  // Use TDHS algorithm (default, better than WSOLA)
  // This is the default setting, no explicit call needed

  // Enable anti-aliasing filter for better quality
  st_->setSetting(SETTING_USE_AA_FILTER, 1);

  // Low latency configuration (from design.md):
  // - Sequence length: 40ms
  // - Seek window: 15ms
  // - Overlap: 8ms
  st_->setSetting(SETTING_SEQUENCE_MS, 40);
  st_->setSetting(SETTING_SEEKWINDOW_MS, 15);
  st_->setSetting(SETTING_OVERLAP_MS, 8);

  // Set initial tempo to 1.0 (passthrough)
  st_->setTempo(1.0);

  lss_log_info("AudioTimeStretcher initialized: %d Hz, %d ch, latency config: seq=40ms, seek=15ms, overlap=8ms", 
               sample_rate, channels);
}

void AudioTimeStretcher::set_tempo(double tempo) {
  if (!st_) {
    lss_log_warn("AudioTimeStretcher::set_tempo called before initialize");
    return;
  }

  // Store original tempo for logging
  double original_tempo = tempo;

  // Clamp tempo to valid range (0.5 - 4.0)
  // Optimal range is 1.0 - 2.0 for minimal artifacts
  tempo = std::clamp(tempo, 0.5, 4.0);

  // Log if tempo was clamped
  if (tempo != original_tempo) {
    lss_log_warn("AudioTimeStretcher tempo clamped: %.4fx → %.4fx (valid range: 0.5-4.0)", 
                 original_tempo, tempo);
  }

  if (tempo != current_tempo_) {
    st_->setTempo(tempo);
    double old_tempo = current_tempo_;
    current_tempo_ = tempo;
    
    // Log tempo change with quality indicator
    const char* quality = "optimal";
    if (tempo < 1.0 || tempo > 2.0) {
      quality = "acceptable";
    } else if (tempo > 1.8) {
      quality = "good";
    }
    
    lss_log_debug("AudioTimeStretcher tempo: %.4fx → %.4fx (quality: %s)", 
                  old_tempo, tempo, quality);
  }
}

int AudioTimeStretcher::process(const float *input, int input_samples,
                                float *output, int output_capacity) {
  if (!st_) {
    lss_log_warn("AudioTimeStretcher::process called before initialize");
    return 0;
  }

  if (!input || input_samples <= 0 || !output || output_capacity <= 0) {
    lss_log_debug("AudioTimeStretcher::process invalid parameters: input=%p, input_samples=%d, output=%p, output_capacity=%d",
                  input, input_samples, output, output_capacity);
    return 0;
  }

  // Feed input samples to SoundTouch
  // SoundTouch expects interleaved samples for stereo
  st_->putSamples(input, input_samples);

  // Retrieve processed samples
  // receiveSamples returns the number of samples available per channel
  int received = st_->receiveSamples(output, output_capacity);

  // Log processing details at debug level (only if tempo != 1.0 to reduce spam)
  if (current_tempo_ != 1.0) {
    int latency = st_->numSamples();
    lss_log_debug("AudioTimeStretcher::process: in=%d, out=%d, tempo=%.4fx, latency=%d samples (%.1fms)",
                  input_samples, received, current_tempo_, latency,
                  (latency * 1000.0) / sample_rate_);
  }

  return received;
}

int AudioTimeStretcher::flush(float *output, int output_capacity) {
  if (!st_) {
    lss_log_warn("AudioTimeStretcher::flush called before initialize");
    return 0;
  }

  if (!output || output_capacity <= 0) {
    lss_log_debug("AudioTimeStretcher::flush invalid parameters: output=%p, output_capacity=%d",
                  output, output_capacity);
    return 0;
  }

  // Get latency before flush for logging
  int latency_before = st_->numSamples();

  // Flush remaining samples from internal buffer
  st_->flush();

  // Retrieve any remaining samples
  int received = st_->receiveSamples(output, output_capacity);

  lss_log_debug("AudioTimeStretcher::flush: flushed %d samples (latency was %d samples, %.1fms)",
                received, latency_before, (latency_before * 1000.0) / sample_rate_);

  return received;
}

void AudioTimeStretcher::reset() {
  if (!st_) {
    lss_log_debug("AudioTimeStretcher::reset called before initialize");
    return;
  }

  // Get state before reset for logging
  int latency_before = st_->numSamples();
  double tempo_before = current_tempo_;

  // Clear internal buffers and reset state
  st_->clear();

  // Reset tempo to 1.0
  st_->setTempo(1.0);
  current_tempo_ = 1.0;

  lss_log_debug("AudioTimeStretcher reset: cleared %d buffered samples, tempo %.4fx → 1.0x",
                latency_before, tempo_before);
}

} // namespace lss
