#include "audio/frame_source.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

bool G722FileSource::load(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;
  data_.assign(std::istreambuf_iterator<char>(f), {});
  frames_ = data_.size() / G722_FRAME_BYTES;
  pos_ = 0;
  return frames_ > 0;
}

bool G722FileSource::next(std::span<uint8_t, G722_FRAME_BYTES> out) {
  if (pos_ >= frames_)
    return false;
  std::memcpy(out.data(), &data_[pos_ * G722_FRAME_BYTES], G722_FRAME_BYTES);
  pos_++;
  return true;
}

std::string G722FileSource::describe() const {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%zu frames (%.1f s)", frames_, frames_ * 0.020);
  return buf;
}

bool ToneSource::next(std::span<uint8_t, G722_FRAME_BYTES> out) {
  int16_t pcm[G722_FRAME_SAMPLES];
  sine_.fill(pcm);
  encoder_.encode(pcm, out);
  return true;
}

std::string ToneSource::describe() const {
  return "tone " + std::to_string(freq_hz_) + " Hz until Ctrl+C";
}
