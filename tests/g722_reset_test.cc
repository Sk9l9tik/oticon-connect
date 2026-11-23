#include <array>
#include <cassert>
#include <cstdint>

#include "audio/g722_encode.h"

int main() {
  std::array<int16_t, G722_FRAME_SAMPLES> pcm{};
  for (size_t i = 0; i < pcm.size(); ++i)
    pcm[i] = static_cast<int16_t>((i * 997) % 20000 - 10000);

  std::array<uint8_t, G722_FRAME_BYTES> first{};
  std::array<uint8_t, G722_FRAME_BYTES> after_reset{};
  std::array<uint8_t, G722_FRAME_BYTES> scratch{};

  G722Encoder encoder;
  encoder.encode(pcm, first);
  encoder.encode(pcm, scratch);  // move the ADPCM predictor away from its initial state
  encoder.reset();
  encoder.encode(pcm, after_reset);

  assert(first == after_reset);
}
