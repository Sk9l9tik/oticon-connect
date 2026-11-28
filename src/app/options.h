#ifndef APP_OPTIONS_H_
#define APP_OPTIONS_H_

#include <string>

struct Options {
  std::string bond;                           // BlueZ info file to import; empty: stored bond
  std::string peer;                           // identity address of the aid
  std::string usb;                            // adapter by USB path "bus-port.port"
  std::string audio = "assets/tone_1k.g722";  // raw G.722 frames, 160 bytes each
  std::string log = "app.btsnoop";            // HCI trace, readable by btmon -r
  std::string avoid_wifi;                     // 2.4 GHz Wi-Fi channels to avoid
  int tone_hz = 0;
  bool pipewire = false;
  bool pair = false;
  int volume = -50;
};

void usage(const char* name);
bool parse_options(int argc, char** argv, Options& opt);

#endif  // APP_OPTIONS_H_
