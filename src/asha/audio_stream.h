#ifndef ASHA_AUDIO_STREAM_H_
#define ASHA_AUDIO_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include "audio/frame_source.h"
#include "audio/g722_encode.h"
#include "ble/ble_link.h"
#include "ble/timer.h"

// ASHA audio over the CoC: every 20 ms take a frame from the source and queue
// the SDU [seq][160 bytes]; the link sends one SDU per can-send event.
// If the link falls behind (queue full), the OLDEST frame is dropped: the aid
// renders a frame ~89 ms (ROP delay) after it is due, a frame queued longer is
// useless there and only pushes the fresh ones past their deadline too.
class AudioStream {
public:
  AudioStream(FrameSource& source, BleLink& link, std::function<void()> on_end);

  void start();  // first frame now, then every 20 ms
  void stop();   // stops the clock, prints the totals
  void on_can_send();

private:
  static const int FRAME_MS = 20;
  static const unsigned MAX_QUEUED = 2;  // 40 ms: + air retries stays under the render delay
  static const unsigned RING = 8;  // > MAX_QUEUED + 1 in flight, see on_can_send

  void on_tick();

  FrameSource& source_;
  BleLink& link_;
  std::function<void()> on_end_;  // source ended
  Timer timer_;
  uint32_t next_deadline_ms_ = 0;  // absolute time of the next frame

  // l2cap_send() keeps a pointer to the SDU until the next can-send event, so
  // a slot is reused only after it went out
  uint8_t ring_[RING][1 + G722_FRAME_BYTES];  // SDU = [seq][160 bytes G.722]
  unsigned head_ = 0, tail_ = 0;              // pop at head, push at tail
  unsigned queued_ = 0;                       // frames in the ring, not yet sent
  size_t frames_ = 0;                         // frames taken from the source, also seq
  unsigned sent_ = 0, dropped_ = 0;
};

#endif  // ASHA_AUDIO_STREAM_H_
