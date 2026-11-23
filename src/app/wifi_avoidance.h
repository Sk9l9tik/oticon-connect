#ifndef APP_WIFI_AVOIDANCE_H_
#define APP_WIFI_AVOIDANCE_H_

#include <cstdint>
#include <string>
#include <string_view>

struct BleChannelMap {
  uint32_t lower = 0xffffffffu;  // LE data channels 0..31
  uint8_t upper = 0x1f;          // LE data channels 32..36
  unsigned usable = 37;
};

// Build an LE data-channel map which avoids the occupied 22 MHz around the
// listed 2.4 GHz Wi-Fi channels (1..13, comma-separated).
bool make_wifi_avoidance_map(
    std::string_view channels, BleChannelMap& map, std::string& error);

#endif  // APP_WIFI_AVOIDANCE_H_
