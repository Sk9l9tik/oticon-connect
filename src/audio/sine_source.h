#ifndef AUDIO_SINE_SOURCE_H_
#define AUDIO_SINE_SOURCE_H_

#include <cstdint>
#include <cmath>
#include <numbers>
#include <span>

#include "audio/g722_encode.h"

class SineSource {
public:
  SineSource(double freq_hz, double amplitude) : step_(2 * std::numbers::pi * freq_hz / 16000), amp_(amplitude * 32767) {}

  void fill(std::span<int16_t, G722_FRAME_SAMPLES> pcm) {
    for(auto& s : pcm) {
      s = static_cast<int16_t>(std::lround(amp_ * std::sin(step_ * n_++)));
    }
  }

private:

  double step_;
  double amp_;
  int64_t n_ = 0;  // global sample index: phase continues across frames
};

#endif  // AUDIO_SINE_SOURCE_H_
