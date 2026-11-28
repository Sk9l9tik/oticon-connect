#include <cstdio>
#include <random>
#include <thread>

#include "audio/sample_ring.h"

static constexpr size_t TOTAL = 50'000'000;  // samples
static SampleRing ring;  // 32 KB: static, not on the stack

int main() {
  std::thread producer([] {
    std::mt19937 rng(1);
    std::uniform_int_distribution<size_t> chunk(1, 2000);  // uneven, crosses the array edge anywhere
    int16_t buf[2000];
    size_t next = 0;
    while (next < TOTAL) {
      size_t n = std::min(chunk(rng), TOTAL - next);
      for (size_t i = 0; i < n; i++)
        buf[i] = (int16_t)(next + i);  // sequence, wraps every 65536
      size_t done = 0;
      while (done < n)  // ring full: retry the rest, nothing may be lost here
        done += ring.write(buf + done, n - done);
      next += n;
    }
  });

  // consumer: frames of 320 like PipeWireSource::next()
  int16_t frame[320];
  size_t expect = 0;
  while (expect + 320 <= TOTAL) {
    if (!ring.read(frame, 320))
      continue;  // not enough yet
    for (int16_t s : frame) {
      if (s != (int16_t)expect) {
        std::printf("FAIL at sample %zu: got %d, expected %d\n", expect, s, (int16_t)expect);
        producer.detach();
        return 1;
      }
      expect++;
    }
  }
  producer.join();
  std::printf("OK: %zu samples\n", expect);
  return 0;
}
