#include <cstdio>
#include <cstring>

#include "asha/audio_stream.h"

AudioStream::AudioStream(
    FrameSource& source, BleLink& link, std::function<void()> on_end,
    std::function<void()> on_idle, std::function<void()> on_stall)
    : source_(source), link_(link), on_end_(std::move(on_end)), on_idle_(std::move(on_idle)),
      on_stall_(std::move(on_stall)), timer_([this] { on_tick(); }) {
}

void AudioStream::start() {
  std::printf("streaming %s\n", source_.describe().c_str());
  source_.restart_stream();
  next_deadline_ms_ = btstack_run_loop_get_time_ms();
  last_send_ms_ = next_deadline_ms_;
  sequence_ = 0;  // ASHA requires packet 0 after every Start
  running_ = true;
  congested_ = false;
  // after a reconnect: SDUs left from the old link must not go out
  head_ = 0;
  tail_ = 0;
  queued_ = 0;
  on_tick();  // first frame right away
}

void AudioStream::stop() {
  bool was_running = running_;
  running_ = false;
  timer_.cancel();
  if (!was_running)
    return;
  std::printf("done: %zu frames, sent %u, dropped %u\n", frames_, sent_, dropped_);
}

// Every 20 ms: put the next frame into the queue. It does not send: sending
// happens only on can-send (on_can_send).
void AudioStream::on_tick() {
  uint8_t frame[G722_FRAME_BYTES];
  if (!source_.next(frame)) {  // always taken: keeps the source in step with the clock
    on_end_();
    return;
  }
  if (source_.suspend_requested()) {
    on_idle_();
    return;
  }
  if (queued_ >= MAX_QUEUED) {
    // the link is behind: drop the oldest unsent frame (not the one in
    // flight: that one is at head_ - 1, already out of the queue)
    head_ = (head_ + 1) % RING;
    queued_--;
    dropped_++;
    uint32_t now = btstack_run_loop_get_time_ms();
    if (!congested_) {
      congested_ = true;
      congestion_since_ms_ = now;
    }
    if ((uint32_t)(now - last_send_ms_) >= STALL_TIMEOUT_MS ||
        (uint32_t)(now - congestion_since_ms_) >= STALL_TIMEOUT_MS) {
      std::printf("audio channel stalled for %u ms; reconnecting\n", STALL_TIMEOUT_MS);
      running_ = false;
      on_stall_();
      return;
    }
  }
  ring_[tail_][0] = sequence_++;
  std::memcpy(&ring_[tail_][1], frame, G722_FRAME_BYTES);
  tail_ = (tail_ + 1) % RING;
  queued_++;
  link_.request_can_send();
  frames_++;  // source/statistics advance even when an older queued frame was dropped
  if (frames_ % 50 == 0)
    std::printf(
        "  t=%4.1fs queued %u sent %u dropped %u\n", frames_ * FRAME_MS / 1000.0, queued_, sent_,
        dropped_);

  // absolute deadlines: timer jitter does not accumulate into drift
  next_deadline_ms_ += FRAME_MS;
  int32_t delay = (int32_t)(next_deadline_ms_ - btstack_run_loop_get_time_ms());
  timer_.start(delay > 0 ? delay : 0);
}

// The previous SDU went out and the aid gave credits: send one frame
void AudioStream::on_can_send() {
  if (!running_ || queued_ == 0)
    return;
  std::memcpy(in_flight_, ring_[head_], sizeof(in_flight_));
  if (!link_.send(in_flight_)) {
    running_ = false;
    on_stall_();
    return;
  }
  head_ = (head_ + 1) % RING;
  queued_--;
  sent_++;
  last_send_ms_ = btstack_run_loop_get_time_ms();
  if (queued_ == 0)
    congested_ = false;
  if (queued_ > 0)
    link_.request_can_send();
}
