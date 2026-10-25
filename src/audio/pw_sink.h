#ifndef AUDIO_PW_SINK_H_
#define AUDIO_PW_SINK_H_

#include "audio/sample_ring.h"
#include <string>

// PipeWire's types, so this header doesn't pull PipeWire in
struct pw_thread_loop;
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

  static void on_process(void* userdata);  // PipeWire realtime thread

 private:
  void cleanup();
  [[noreturn]] void fail(const std::string& what);

  pw_thread_loop* loop_ = nullptr;
  pw_stream* stream_ = nullptr;
  SampleRing ring_;
  std::atomic<size_t> overflows_{0};
};

#endif  // AUDIO_PW_SINK_H_
