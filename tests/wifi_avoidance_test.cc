#include <cstdio>
#include <string>

#include "app/wifi_avoidance.h"

static bool enabled(const BleChannelMap& map, unsigned channel) {
  return channel < 32 ? (map.lower & (uint32_t{1} << channel)) != 0
                      : (map.upper & (uint8_t{1} << (channel - 32))) != 0;
}

int main() {
  BleChannelMap map;
  std::string error;
  if (!make_wifi_avoidance_map("6", map, error))
    return 1;
  if (enabled(map, 16) || !enabled(map, 4) || !enabled(map, 28) || map.usable != 25)
    return 2;
  if (!make_wifi_avoidance_map("1,11", map, error) || map.usable != 16)
    return 3;
  if (make_wifi_avoidance_map("1,6,11", map, error))
    return 4;
  if (make_wifi_avoidance_map("0", map, error) ||
      make_wifi_avoidance_map("6,", map, error) ||
      make_wifi_avoidance_map("x", map, error))
    return 5;
  std::puts("OK");
  return 0;
}
