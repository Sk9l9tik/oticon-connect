#include "app/options.h"

#include <getopt.h>

#include <cctype>
#include <charconv>
#include <cstdio>
#include <string_view>

namespace {

bool parse_integer(const char* text, int& value) {
  std::string_view token(text);
  while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
    token.remove_prefix(1);
  if (token.empty())
    return false;
  if (token.front() == '+') {
    token.remove_prefix(1);
    if (token.empty() || token.front() < '0' || token.front() > '9')
      return false;
  }
  int parsed;
  const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), parsed, 10);
  if (error != std::errc{} || end != token.data() + token.size())
    return false;
  value = parsed;
  return true;
}

}  // namespace

void usage(const char* name) {
  std::printf(
      "usage: %s [options]\n"
      "  -b, --bond PATH     import a BlueZ bond (/var/lib/bluetooth/<adapter>/<aid>/info);\n"
      "                      without it the bond saved by an earlier import is used\n"
      "  -r, --pair          pair with an aid in pairing mode (restart it first), the bond\n"
      "                      is stored for this adapter; not with --bond\n"
      "  -P, --peer ADDR     aid identity address: picks a stored bond, or names the aid\n"
      "                      of a --bond file copied out of its BlueZ directory\n"
      "  -u, --usb PATH      only the adapter at USB ports [bus-]port[.port...], as printed\n"
      "                      at start or in /sys/bus/usb/devices (e.g. 1-2.3); default: every\n"
      "                      Bluetooth adapter on USB, external first, the first with a bond\n"
      "  -a, --audio PATH    G.722 file to stream (default assets/tone_1k.g722)\n"
      "  -v, --volume N      volume -128..0 (default -50)\n"
      "  -t, --tone HZ       endless sine 1..7999 Hz instead of the file\n"
      "  -p, --pipewire      play what apps send to the PipeWire sink \"Oticon ASHA\"\n"
      "  -l, --log PATH      HCI trace, btsnoop format (default app.btsnoop)\n"
      "  -W, --avoid-wifi N  avoid 2.4 GHz Wi-Fi channels 1..13 (comma-separated)\n"
      "  -h, --help          this help\n",
      name);
}

bool parse_options(int argc, char** argv, Options& opt) {
  static const option long_options[] = {
      {"bond", required_argument, nullptr, 'b'},
      {"peer", required_argument, nullptr, 'P'},
      {"usb", required_argument, nullptr, 'u'},
      {"audio", required_argument, nullptr, 'a'},
      {"volume", required_argument, nullptr, 'v'},
      {"log", required_argument, nullptr, 'l'},
      {"avoid-wifi", required_argument, nullptr, 'W'},
      {"help", no_argument, nullptr, 'h'},
      {"tone", required_argument, nullptr, 't'},
      {"pipewire", no_argument, nullptr, 'p'},
      {"pair", no_argument, nullptr, 'r'},
      {nullptr, 0, nullptr, 0}};

  optind = 0;
  int c;
  while ((c = getopt_long(argc, argv, "b:P:u:a:v:l:W:t:prh", long_options, nullptr)) != -1) {
    switch (c) {
      case 'b':
        opt.bond = optarg;
        break;
      case 'P':
        opt.peer = optarg;
        break;
      case 'u':
        opt.usb = optarg;
        break;
      case 'a':
        opt.audio = optarg;
        break;
      case 'v': {
        int v;
        if (!parse_integer(optarg, v) || v < -128 || v > 0) {
          std::fprintf(stderr, "bad volume '%s', expected -128..0\n", optarg);
          return false;
        }
        opt.volume = v;
        break;
      }
      case 'l':
        opt.log = optarg;
        break;
      case 'W':
        opt.avoid_wifi = optarg;
        break;
      case 't': {
        int t;
        if (!parse_integer(optarg, t) || t < 1 || t > 7999) {
          std::fprintf(stderr, "bad tone '%s', expected 1..7999\n", optarg);
          return false;
        }
        opt.tone_hz = t;
        break;
      }
      case 'p':
        opt.pipewire = true;
        break;
      case 'r':
        opt.pair = true;
        break;
      default:
        return false;
    }
  }
  if (opt.pipewire && opt.tone_hz > 0) {
    std::fprintf(stderr, "--tone and --pipewire: pick one\n");
    return false;
  }
  if (opt.pair && !opt.bond.empty()) {
    std::fprintf(stderr, "--pair and --bond: pick one\n");
    return false;
  }
  if (optind < argc) {
    std::fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
    return false;
  }
  return true;
}
