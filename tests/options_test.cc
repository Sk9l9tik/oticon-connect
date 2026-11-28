#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "app/options.h"

static bool parses(std::initializer_list<std::string> arguments, Options& options) {
  std::vector<std::vector<char>> storage;
  for (const auto& argument : arguments) {
    storage.emplace_back(argument.begin(), argument.end());
    storage.back().push_back('\0');
  }
  std::vector<char*> argv;
  for (auto& argument : storage)
    argv.push_back(argument.data());
  const auto argc = static_cast<int>(argv.size());
  argv.push_back(nullptr);
  options = Options{};
  return parse_options(argc, argv.data(), options);
}

int main() {
  Options options;
  if (!parses({"oticon-connect"}, options) || options.volume != -50 || options.tone_hz != 0 ||
      options.audio != "assets/tone_1k.g722" || options.log != "app.btsnoop" || options.pipewire ||
      options.pair || !options.bond.empty() || !options.peer.empty() || !options.usb.empty() ||
      !options.avoid_wifi.empty())
    return 1;

  if (!parses(
          {"oticon-connect", "--bond", "bond", "--peer", "addr", "--usb", "1-2.3", "--audio",
           "audio", "--log", "trace", "--avoid-wifi", "6", "--volume", "+0", "--tone", " 7999"},
          options) ||
      options.bond != "bond" || options.peer != "addr" || options.usb != "1-2.3" ||
      options.audio != "audio" || options.log != "trace" || options.avoid_wifi != "6" ||
      options.volume != 0 || options.tone_hz != 7999 || options.pair)
    return 2;

  if (!parses({"oticon-connect", "-v", "-128", "-t", "1"}, options) || options.volume != -128 ||
      options.tone_hz != 1)
    return 3;

  for (const auto& invalid :
       {"", " ", "+", "+-1", "-129", "1", "1x", "0 ", "999999999999999999999999",
        "-999999999999999999999999"}) {
    if (parses({"oticon-connect", "--volume", invalid}, options))
      return 4;
  }
  for (const auto& invalid : {"", "0", "8000", "-1", "1x", "999999999999999999999999"}) {
    if (parses({"oticon-connect", "--tone", invalid}, options))
      return 5;
  }
  if (parses({"oticon-connect", "--tone", "1", "--pipewire"}, options) ||
      parses({"oticon-connect", "--pair", "--bond", "bond"}, options))
    return 6;
  if (parses({"oticon-connect", "unexpected"}, options) ||
      parses({"oticon-connect", "--help"}, options) ||
      parses({"oticon-connect", "--volume"}, options) ||
      parses({"oticon-connect", "--unknown"}, options))
    return 7;

  if (!parses({"oticon-connect", "-pr"}, options) || !options.pipewire || !options.pair)
    return 8;
  if (!parses({"oticon-connect", "--tone", "+1000", "--volume", " -50"}, options) ||
      options.tone_hz != 1000 || options.volume != -50)
    return 9;
  if (parses({"oticon-connect", "-xp"}, options) || !parses({"oticon-connect"}, options) ||
      options.pipewire)
    return 10;

  std::puts("OK");
  return 0;
}
