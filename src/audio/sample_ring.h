#ifndef AUDIO_SAMPLE_RING_H_
#define AUDIO_SAMPLE_RING_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <algorithm>

class SampleRing {
public:
  static constexpr size_t CAP = 1 << 14;  // 16384 samples ≈ 1 s @ 16 kHz, power of two

  size_t size() const {  // filled samples, approximate across threads
    size_t tail = tail_.load(std::memory_order_acquire);
    size_t head = head_.load(std::memory_order_acquire);
    return head - tail;
  }

  // producer (PipeWire): returns how many fit; the rest is dropped
  size_t write(const int16_t* src, size_t n) {
    size_t head = head_.load(std::memory_order_relaxed);
    size_t tail = tail_.load(std::memory_order_acquire);
    n = std::min(n, CAP - (head - tail));
    for (size_t i = 0; i < n; ++i)
      buf_[(head + i) & (CAP - 1)] = src[i];
    head_.store(head + n, std::memory_order_release);  // publish the samples
    return n;
  }

  // consumer (BTstack): false if fewer than n are there
  bool read(int16_t* dst, size_t n) {
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t head = head_.load(std::memory_order_acquire);

    if (head - tail < n)
      return false;
    for (size_t i = 0; i < n; ++i)
      dst[i] = buf_[(tail + i) & (CAP - 1)];
    tail_.store(tail + n, std::memory_order_release);
    return true;
  }

  // consumer: drop the n oldest samples (fewer if fewer are there)
  void skip(size_t n) {
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t head = head_.load(std::memory_order_acquire);
    n = std::min(n, head - tail);  // never move tail past head
    tail_.store(tail + n, std::memory_order_release);
  }

private:
  int16_t buf_[CAP];
  std::atomic<size_t> head_{0};  // written by producer only
  std::atomic<size_t> tail_{0};  // written by consumer only
};

#endif  // AUDIO_SAMPLE_RING_H_
