// audio-time-stretcher.h - SoundTouch wrapper for professional audio time-stretching
#pragma once

#include "core/common.h"

// Forward declare SoundTouch to avoid exposing the header
namespace soundtouch {
class SoundTouch;
}

namespace lss {

// AudioTimeStretcher wraps the SoundTouch library to provide professional-grade
// audio time-stretching with minimal latency and artifacts. Used for catch-up
// playback where audio must be sped up while maintaining pitch.
//
// Key features:
// - Tempo range: 1.0x - 2.0x (smoothly), up to 4.0x (acceptable quality)
// - Low latency: ~50ms internal buffering
// - High quality: TDHS algorithm (better than WSOLA)
// - No clicks/pops: smooth tempo transitions
//
// Usage:
//   AudioTimeStretcher stretcher;
//   stretcher.initialize(48000, 2);
//   stretcher.set_tempo(1.5);
//   int output_count = stretcher.process(input, input_samples, output, output_capacity);
//
class AudioTimeStretcher {
public:
  AudioTimeStretcher();
  ~AudioTimeStretcher();

  AudioTimeStretcher(const AudioTimeStretcher &) = delete;
  AudioTimeStretcher &operator=(const AudioTimeStretcher &) = delete;

  // Initialize with audio format parameters.
  // Must be called before any processing.
  // sample_rate: Audio sample rate in Hz (e.g., 48000)
  // channels: Number of audio channels (1 = mono, 2 = stereo)
  void initialize(int sample_rate, int channels);

  // Set playback tempo (speed without pitch change).
  // tempo: Playback speed multiplier
  //   1.0 = normal speed (passthrough)
  //   1.5 = 50% faster
  //   2.0 = double speed
  // Valid range: 0.5 - 4.0 (clamped internally)
  // Optimal range: 1.0 - 2.0 (minimal artifacts)
  void set_tempo(double tempo);

  // Process audio samples through the time-stretcher.
  // input: Input audio samples (interleaved if stereo)
  // input_samples: Number of input samples (per channel)
  // output: Output buffer for processed samples
  // output_capacity: Maximum number of output samples (per channel)
  // Returns: Number of output samples available (per channel)
  //
  // Note: Output count may be less than input count due to internal buffering.
  // Call flush() at end of stream to retrieve remaining samples.
  int process(const float *input, int input_samples, float *output,
              int output_capacity);

  // Flush remaining samples from internal buffer.
  // Call at end of stream or before reset to avoid losing audio.
  // output: Output buffer for remaining samples
  // output_capacity: Maximum number of output samples (per channel)
  // Returns: Number of output samples flushed (per channel)
  int flush(float *output, int output_capacity);

  // Reset internal state and clear buffers.
  // Call when seeking or starting a new stream.
  void reset();

private:
  soundtouch::SoundTouch *st_;
  int sample_rate_;
  int channels_;
  double current_tempo_;
};

} // namespace lss
