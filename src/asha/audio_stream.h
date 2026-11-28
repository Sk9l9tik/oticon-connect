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
  AudioStream(
      FrameSource& source, BleLink& link, std::function<void()> on_end,
      std::function<void()> on_idle, std::function<void()> on_stall);

  void start();  // first frame now, then every 20 ms
  void stop();   // stops the clock, prints the totals
  void on_can_send();
  bool resume_requested() const { return source_.resume_requested(); }

private:
  static const int FRAME_MS = 20;
  static const unsigned MAX_QUEUED = 2;  // 40 ms: + air retries stays under the render delay
  static const unsigned RING = 8;  // > MAX_QUEUED + 1 in flight, see on_can_send
  static const uint32_t STALL_TIMEOUT_MS = 10000;

  void on_tick();

  FrameSource& source_;
  BleLink& link_;
  std::function<void()> on_end_;  // source ended
  std::function<void()> on_idle_;
  std::function<void()> on_stall_;
  Timer timer_;
  uint32_t next_deadline_ms_ = 0;  // absolute time of the next frame
  uint32_t last_send_ms_ = 0;
  bool running_ = false;
  bool congested_ = false;
  uint32_t congestion_since_ms_ = 0;

  // Queued frames are copied to in_flight_ immediately before l2cap_send().
  uint8_t ring_[RING][1 + G722_FRAME_BYTES];  // SDU = [seq][160 bytes G.722]
  // BTstack owns the SDU pointer until the next can-send event. Keeping the
  // in-flight packet outside the queue prevents a long radio stall from
  // wrapping the queue and overwriting memory still used by L2CAP.
  uint8_t in_flight_[1 + G722_FRAME_BYTES];
  unsigned head_ = 0, tail_ = 0;              // pop at head, push at tail
  unsigned queued_ = 0;                       // frames in the ring, not yet sent
  size_t frames_ = 0;                         // frames taken from the source (statistics)
  unsigned sent_ = 0, dropped_ = 0;
  uint8_t sequence_ = 0;
};

#endif  // ASHA_AUDIO_STREAM_H_
