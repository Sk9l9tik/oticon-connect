#include "app/wifi_avoidance.h"

#include <charconv>
#include <cstdlib>

bool make_wifi_avoidance_map(
    std::string_view channels, BleChannelMap& map, std::string& error) {
  map = {};
  if (channels.empty() || channels.back() == ',') {
    error = "channel list is empty or ends with a comma";
    return false;
  }

  size_t begin = 0;
  while (begin < channels.size()) {
    size_t end = channels.find(',', begin);
    if (end == std::string_view::npos)
      end = channels.size();
    std::string_view item = channels.substr(begin, end - begin);
    int wifi_channel = 0;
    auto parsed = std::from_chars(item.data(), item.data() + item.size(), wifi_channel);
    if (item.empty() || parsed.ec != std::errc{} || parsed.ptr != item.data() + item.size() ||
        wifi_channel < 1 || wifi_channel > 13) {
      error = "expected comma-separated Wi-Fi channels 1..13";
      return false;
    }

    const int wifi_center_mhz = 2412 + 5 * (wifi_channel - 1);
    for (unsigned ble_channel = 0; ble_channel < 37; ++ble_channel) {
      const int ble_center_mhz = 2404 + 2 * static_cast<int>(ble_channel);
      if (std::abs(ble_center_mhz - wifi_center_mhz) > 11)
        continue;
      if (ble_channel < 32)
        map.lower &= ~(uint32_t{1} << ble_channel);
      else
        map.upper &= static_cast<uint8_t>(~(uint8_t{1} << (ble_channel - 32)));
    }
    begin = end + 1;
  }

  map.usable = static_cast<unsigned>(__builtin_popcount(map.lower)) +
               static_cast<unsigned>(__builtin_popcount(map.upper));
  // The Bluetooth minimum is two, but an audio stream needs enough hopping
  // diversity to survive another interferer or a narrow fade.
  if (map.usable < 10) {
    error = "the requested Wi-Fi channels leave fewer than 10 of 37 BLE data channels";
    return false;
  }
  return true;
}
