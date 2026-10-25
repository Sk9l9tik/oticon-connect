#include "asha/audio_stream.h"

#include <cstdio>
#include <cstring>

AudioStream::AudioStream(FrameSource& source, BleLink& link, std::function<void()> on_end)
    : source_(source), link_(link), on_end_(std::move(on_end)), timer_([this] { on_tick(); }) {
}

void AudioStream::start() {
  printf("streaming %s\n", source_.describe().c_str());
  next_deadline_ms_ = btstack_run_loop_get_time_ms();
  on_tick();  // first frame right away
}

void AudioStream::stop() {
  timer_.cancel();
  printf("done: %zu frames, sent %u, dropped %u\n", frames_, sent_, dropped_);
}

// Every 20 ms: put the next frame into the queue. It does not send: sending
// happens only on can-send (on_can_send).
void AudioStream::on_tick() {
  uint8_t frame[G722_FRAME_BYTES];
  if (!source_.next(frame)) {  // always taken: keeps the source in step with the clock
    on_end_();
    return;
  }
  if (queued_ >= MAX_QUEUED) {
    // the link is behind: drop the oldest unsent frame (not the one in
    // flight: that one is at head_ - 1, already out of the queue)
    head_ = (head_ + 1) % RING;
    queued_--;
    dropped_++;
  }
  ring_[tail_][0] = (uint8_t)frames_;  // seq wraps after 255
  memcpy(&ring_[tail_][1], frame, G722_FRAME_BYTES);
  tail_ = (tail_ + 1) % RING;
  queued_++;
  link_.request_can_send();
  frames_++;  // seq advances for dropped frames too, as in asha_test.py
  if (frames_ % 50 == 0)
    printf(
        "  t=%4.1fs queued %u sent %u dropped %u\n", frames_ * FRAME_MS / 1000.0, queued_, sent_,
        dropped_);

  // absolute deadlines: timer jitter does not accumulate into drift
  next_deadline_ms_ += FRAME_MS;
  int32_t delay = (int32_t)(next_deadline_ms_ - btstack_run_loop_get_time_ms());
  timer_.start(delay > 0 ? delay : 0);
}

// The previous SDU went out and the aid gave credits: send one frame
void AudioStream::on_can_send() {
  if (queued_ == 0)
    return;
  link_.send(ring_[head_]);
  head_ = (head_ + 1) % RING;
  queued_--;
  sent_++;
  if (queued_ > 0)
    link_.request_can_send();
}
