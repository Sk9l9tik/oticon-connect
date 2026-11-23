#include <cstdint>
#include <stdexcept>

// include-order: keep
#include <spandsp/telephony.h>  // defines SPAN_DECLARE, needed by g722.h
#include <spandsp/g722.h>

#include "audio/g722_encode.h"


G722Encoder::G722Encoder() : state_(g722_encode_init(nullptr, 64000, 0)) {
  // nullptr: spandsp allocates the state; 64000: 16 kHz input, 64 kbit/s out;
  // options 0: not 8 kHz input, not packed (one byte per 2 samples)
  if (!state_)
    throw std::runtime_error("g722_encode_init failed");
}

G722Encoder::~G722Encoder() {
  g722_encode_free(state_);  // frees what init allocated
}

void G722Encoder::reset() {
  if (!g722_encode_init(state_, 64000, 0))
    throw std::runtime_error("g722_encode_init reset failed");
}

void G722Encoder::encode(
    std::span<const int16_t, G722_FRAME_SAMPLES> pcm, std::span<uint8_t, G722_FRAME_BYTES> out) {
  // returns the number of bytes written: 2 samples -> 1 byte
  int n = g722_encode(state_, out.data(), pcm.data(), (int)pcm.size());
  if (n != (int)out.size())
    throw std::runtime_error("g722_encode: unexpected output size");
}
