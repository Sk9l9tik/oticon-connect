#ifndef AUDIO_G722_ENCODE_H_
#define AUDIO_G722_ENCODE_H_

#include <cstdint>
#include <cstddef>
#include <span>

inline constexpr size_t G722_FRAME_SAMPLES = 320;  // 20 ms @ 16 kHz, s16 mono
inline constexpr size_t G722_FRAME_BYTES = 160;    // 64 kbit/s


class G722Encoder {
public:
  G722Encoder();

  G722Encoder(const G722Encoder& enc) = delete;
  G722Encoder& operator=(const G722Encoder& enc) const = delete;

  void encode(std::span<const int16_t, G722_FRAME_SAMPLES> pcm, std::span<uint8_t, G722_FRAME_BYTES> out);

  ~G722Encoder();

private:
  using g722_encode_state_t = struct g722_encode_state_s;

  g722_encode_state_t* state_;
};



#endif  // AUDIO_G722_ENCODE_H_
