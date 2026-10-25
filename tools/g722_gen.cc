// g722_gen: writes a sine tone as raw G.722 frames, the format asha_probe -a reads.
// Usage: g722_gen out.g722 [freq_hz=1000] [seconds=15] [amplitude=0.15]

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <string>

#include "audio/g722_encode.h"
#include "audio/sine_source.h"

static constexpr int SAMPLE_RATE = 16000;
static constexpr int FRAMES_PER_SECOND = SAMPLE_RATE / G722_FRAME_SAMPLES;  // 50

int main(int argc, char** argv) {
  if (argc < 2 || argc > 5) {
    std::fprintf(
        stderr, "usage: %s out.g722 [freq_hz=1000] [seconds=15] [amplitude=0.15]\n", argv[0]);
    return EXIT_FAILURE;
  }

  double freq = 1000, amplitude = 0.15;
  int seconds = 15;
  try {
    if (argc > 2)
      freq = std::stod(argv[2]);
    if (argc > 3)
      seconds = std::stoi(argv[3]);
    if (argc > 4)
      amplitude = std::stod(argv[4]);
  } catch (const std::exception&) {
    std::fprintf(stderr, "bad number\n");
    return EXIT_FAILURE;
  }
  if (freq <= 0 || freq >= SAMPLE_RATE / 2 || seconds <= 0 || amplitude <= 0 || amplitude > 1) {
    std::fprintf(stderr, "out of range: 0 < freq < 8000, seconds > 0, 0 < amplitude <= 1\n");
    return EXIT_FAILURE;
  }

  std::ofstream out(argv[1], std::ios::binary);
  if (!out) {
    std::perror(argv[1]);
    return EXIT_FAILURE;
  }

  G722Encoder enc;  // one encoder for the whole stream: ADPCM state carries over
  int16_t pcm[G722_FRAME_SAMPLES];
  uint8_t frame[G722_FRAME_BYTES];
  const int frames = seconds * FRAMES_PER_SECOND;

  SineSource src(freq, amplitude);


  for (int f = 0; f < frames; f++) {
    src.fill(pcm);
    enc.encode(pcm, frame);
    out.write(reinterpret_cast<const char*>(frame), sizeof(frame));
  }

  std::printf(
      "%s: %d frames, %.0f Hz, %d s, amplitude %.2f\n", argv[1], frames, freq, seconds, amplitude);
  return out ? EXIT_SUCCESS : EXIT_FAILURE;
}
