#include <cstdint>
#include <stdexcept>

// clang-format off
#include <spandsp/telephony.h>  // defines SPAN_DECLARE, needed by g722.h
#include <spandsp/g722.h>
// clang-format on

#include "audio/g722_encode.h"

void G722Encoder::StateDeleter::operator()(g722_encode_state_t* state) const noexcept {
  g722_encode_free(state);
}

G722Encoder::G722Encoder() : state_(g722_encode_init(nullptr, 64000, 0)) {
  // nullptr: spandsp allocates the state; 64000: 16 kHz input, 64 kbit/s out;
  // options 0: not 8 kHz input, not packed (one byte per 2 samples)
  if (!state_)
    throw std::runtime_error("g722_encode_init failed");
}

G722Encoder::~G722Encoder() = default;

void G722Encoder::reset() {
  if (!g722_encode_init(state_.get(), 64000, 0))
    throw std::runtime_error("g722_encode_init reset failed");
}

void G722Encoder::encode(
    std::span<const int16_t, G722_FRAME_SAMPLES> pcm, std::span<uint8_t, G722_FRAME_BYTES> out) {
  // returns the number of bytes written: 2 samples -> 1 byte
  int n = g722_encode(state_.get(), out.data(), pcm.data(), static_cast<int>(pcm.size()));
  if (n != static_cast<int>(out.size()))
    throw std::runtime_error("g722_encode: unexpected output size");
}
