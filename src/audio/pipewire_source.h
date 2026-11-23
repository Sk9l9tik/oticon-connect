#ifndef AUDIO_PIPEWIRE_SOURCE_H_
#define AUDIO_PIPEWIRE_SOURCE_H_

#include <cstddef>
#include <string>

#include "audio/frame_source.h"
#include "audio/g722_encode.h"
#include "audio/pw_sink.h"

// Frames from the PipeWire sink "Oticon ASHA": whatever apps play into it,
// encoded to G.722 on the fly. Never ends; silence while nothing plays.
//
// The sink fills its ring on PipeWire's clock, next() drains it on ours (the
// 20 ms BTstack timer). The two clocks are not in phase and drift slowly:
//   - prefill: after start or an underrun, play only once TARGET samples are
//     there, so a late quantum doesn't cut every frame (pw_capture: ring at
//     tick time swings by ~150 samples)
//   - drift / backlog: above HIGH_WATER drop the oldest down to TARGET instead
//     of letting the latency grow (also clears what queued up before streaming)
class PipeWireSource : public FrameSource {
public:
  PipeWireSource() = default;  // starts the sink; throws if PipeWire is not available

  bool next(std::span<uint8_t, G722_FRAME_BYTES> out) override;
  std::string describe() const override;
  void restart_stream() override { encoder_.reset(); }
  bool suspend_requested() const override { return silent_frames_ >= IDLE_FRAMES; }
  bool resume_requested() const override {
    return sink_.signal_generation() != last_signal_generation_;
  }

private:
  static constexpr size_t TARGET = 2 * G722_FRAME_SAMPLES;      // 40 ms buffered
  static constexpr size_t HIGH_WATER = 6 * G722_FRAME_SAMPLES;  // 120 ms: cut back
  static constexpr unsigned STATS_EVERY = 250;                  // frames: 5 s
  static constexpr unsigned IDLE_FRAMES = 30 * 1000 / 20;       // 30 s

  PwSink sink_;
  G722Encoder encoder_;   // one per stream: ADPCM state carries over
  bool playing_ = false;  // prefill reached, reading the ring
  uint64_t last_signal_generation_ = 0;
  unsigned silent_frames_ = 0;

  // stats, printed every STATS_EVERY frames
  unsigned frames_ = 0;
  unsigned underruns_ = 0;  // ran dry while playing: an audible gap
  size_t skipped_ = 0;      // samples dropped above HIGH_WATER
};

#endif  // AUDIO_PIPEWIRE_SOURCE_H_
