#ifndef AUDIO_PW_SINK_H_
#define AUDIO_PW_SINK_H_

#include "audio/sample_ring.h"
#include <memory>

struct pw_stream;

// Virtual output device "Oticon ASHA" (S16LE, 16 kHz, mono). Apps play into it;
// PipeWire resamples/downmixes for us. Samples land in ring() from PipeWire's
// realtime thread; read them from any ONE other thread.
class PwSink {
 public:
  PwSink();   // starts the PipeWire thread; throws on failure
  ~PwSink();  // stops it before freeing anything
  PwSink(const PwSink&) = delete;
  PwSink& operator=(const PwSink&) = delete;

  SampleRing& ring() { return ring_; }
  size_t overflows() const { return overflows_; }  // samples lost: ring full
  uint64_t signal_generation() const {
    return signal_generation_.load(std::memory_order_relaxed);
  }

  static void on_process(void* userdata);  // PipeWire realtime thread

 private:
  struct Resources;

  SampleRing ring_;
  std::atomic<size_t> overflows_{0};
  // Incremented once per PipeWire quantum containing a real (non-silent)
  // sample. This lets the Bluetooth thread notice resumed playback while the
  // ASHA frame clock is stopped.
  std::atomic<uint64_t> signal_generation_{0};
  // Borrowed from Resources; Resources owns the stream and destruction waits for callbacks.
  pw_stream* stream_ = nullptr;
  std::unique_ptr<Resources> resources_;
};

#endif  // AUDIO_PW_SINK_H_
