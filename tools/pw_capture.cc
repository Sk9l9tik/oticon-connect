// pw_capture: PipeWire sink "Oticon ASHA" -> raw file, no Bluetooth.
// Reads the ring the way PipeWireSource::next() will: 320 samples every 20 ms
// on its own clock, silence when there is not enough. The stats show how the
// PipeWire clock and ours drift apart (ring size) and how often we run dry.
//
// Usage: pw_capture out.raw [seconds=10]
// Play:  ffplay -f s16le -ar 16000 -ch_layout mono out.raw

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

#include "audio/g722_encode.h"
#include "audio/pw_sink.h"

static constexpr int FRAMES_PER_SECOND = 50;  // 20 ms frames

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr, "usage: %s out.raw [seconds=10]\n", argv[0]);
    return EXIT_FAILURE;
  }
  int seconds = 10;
  try {
    if (argc > 2)
      seconds = std::stoi(argv[2]);
  } catch (const std::exception&) {
    seconds = 0;
  }
  if (seconds <= 0) {
    std::fprintf(stderr, "bad seconds '%s'\n", argv[2]);
    return EXIT_FAILURE;
  }

  std::FILE* out = std::fopen(argv[1], "wb");
  if (!out) {
    std::perror(argv[1]);
    return EXIT_FAILURE;
  }

  try {
    PwSink sink;
    std::printf("sink 'Oticon ASHA' (oticon_asha) ready, play something into it\n");

    int16_t pcm[G722_FRAME_SAMPLES];
    const int16_t silence[G722_FRAME_SAMPLES] = {};
    unsigned got = 0, underruns = 0;
    size_t min_fill = SIZE_MAX, max_fill = 0;  // ring size seen at tick time, per second

    // absolute deadlines, like AudioStream: sleep jitter does not add up
    auto next = std::chrono::steady_clock::now();
    for (int f = 1; f <= seconds * FRAMES_PER_SECOND; f++) {
      next += std::chrono::milliseconds(20);
      std::this_thread::sleep_until(next);

      size_t fill = sink.ring().size();
      min_fill = std::min(min_fill, fill);
      max_fill = std::max(max_fill, fill);

      if (sink.ring().read(pcm, G722_FRAME_SAMPLES)) {
        std::fwrite(pcm, sizeof(pcm), 1, out);
        got++;
      } else {
        std::fwrite(silence, sizeof(silence), 1, out);
        underruns++;
      }

      if (f % FRAMES_PER_SECOND == 0) {
        std::printf(
            "  t=%3ds frames %u underruns %u overflows %zu ring %zu..%zu samples\n",
            f / FRAMES_PER_SECOND, got, underruns, sink.overflows(), min_fill, max_fill);
        std::fflush(stdout);
        min_fill = SIZE_MAX;
        max_fill = 0;
      }
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "PipeWire: %s\n", e.what());
    std::fclose(out);
    return EXIT_FAILURE;
  }

  std::fclose(out);
  std::printf("written %s\n", argv[1]);
  return EXIT_SUCCESS;
}
