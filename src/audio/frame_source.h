#ifndef AUDIO_FRAME_SOURCE_H_
#define AUDIO_FRAME_SOURCE_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "audio/g722_encode.h"
#include "audio/sine_source.h"

// Where the 160-byte G.722 frames come from. Called once per 20 ms, also for
// frames that are dropped later: the source stays in step with the clock.
class FrameSource {
public:
  virtual ~FrameSource() = default;
  virtual bool next(std::span<uint8_t, G722_FRAME_BYTES> out) = 0;  // false: end of stream
  virtual std::string describe() const = 0;                         // for the log

  // ASHA resets its decoder on every Start. Sources which encode PCM must
  // reset the matching encoder state as well.
  virtual void restart_stream() {}

  // Live sources may ask ASHA to stop during a long idle period. While the
  // stream is stopped, resume_requested() is polled without consuming audio.
  virtual bool suspend_requested() const { return false; }
  virtual bool resume_requested() const { return false; }
};

// Raw G.722 file (frames back to back); a trailing partial frame is ignored
class G722FileSource : public FrameSource {
public:
  bool load(const char* path);
  bool next(std::span<uint8_t, G722_FRAME_BYTES> out) override;
  std::string describe() const override;

private:
  std::vector<uint8_t> data_;
  size_t frames_ = 0;
  size_t pos_ = 0;  // next frame
};

// Endless sine, encoded on the fly: the same PCM -> G.722 path PipeWire will use
class ToneSource : public FrameSource {
public:
  ToneSource(int freq_hz, double amplitude) : freq_hz_(freq_hz), sine_(freq_hz, amplitude) {
  }
  bool next(std::span<uint8_t, G722_FRAME_BYTES> out) override;
  std::string describe() const override;
  void restart_stream() override { encoder_.reset(); }

private:
  int freq_hz_;
  SineSource sine_;
  G722Encoder encoder_;  // one per stream: ADPCM state carries over
};

#endif  // AUDIO_FRAME_SOURCE_H_
