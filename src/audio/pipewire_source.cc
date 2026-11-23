#include "audio/pipewire_source.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

bool PipeWireSource::next(std::span<uint8_t, G722_FRAME_BYTES> out) {
  SampleRing& ring = sink_.ring();
  size_t fill = ring.size();

  uint64_t signal_generation = sink_.signal_generation();
  if (signal_generation != last_signal_generation_) {
    last_signal_generation_ = signal_generation;
    silent_frames_ = 0;
  } else if (silent_frames_ < IDLE_FRAMES) {
    silent_frames_++;
  }

  if (fill > HIGH_WATER) {
    ring.skip(fill - TARGET);
    skipped_ += fill - TARGET;
    fill = TARGET;
  }
  if (!playing_ && fill >= TARGET)
    playing_ = true;

  int16_t pcm[G722_FRAME_SAMPLES];
  if (!(playing_ && ring.read(pcm, G722_FRAME_SAMPLES))) {
    // nothing playing, or ran dry: silence keeps the stream to the aid going
    if (playing_)
      underruns_++;
    playing_ = false;
    std::fill(std::begin(pcm), std::end(pcm), 0);
  }
  encoder_.encode(pcm, out);

  if (++frames_ % STATS_EVERY == 0)
    printf(
        "  pw: ring %zu, underruns %u, skipped %zu, overflows %zu\n", ring.size(), underruns_,
        skipped_, sink_.overflows());
  return true;
}

std::string PipeWireSource::describe() const {
  return "PipeWire sink 'Oticon ASHA' (oticon_asha) until Ctrl+C";
}
