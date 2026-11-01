#ifndef APP_ASHA_APP_H_
#define APP_ASHA_APP_H_

#include <functional>

struct AppConfig {
  const char* bond;                   // BlueZ bond file to import, nullptr: use a stored bond
  const char* peer;                   // identity address of the aid, nullptr: from the bond
                                      // file path, or the only stored bond
  bool pair;                          // pair anew with an aid in pairing mode (no bond)
  const char* audio;                  // G.722 file; ignored when tone_hz > 0
  int tone_hz;                        // > 0: endless sine encoded on the fly instead of the file
  bool pipewire;                      // audio from the PipeWire sink "Oticon ASHA"
  int volume;                         // -128 (mute) .. 0 (max)
  std::function<void()> on_finished;  // link is down (or never came up): safe to power off
  std::function<void()> on_no_bond;   // the powered adapter has no bond for the aid
};

// Loads bond file (if any) + audio, inits l2cap/sm/gatt. The caller powers an
// adapter on; the rest runs in the run loop. false: bad bond, peer or audio
// file, no PipeWire. An adapter without the bond shows up as on_no_bond.
bool asha_start(const AppConfig& cfg);

// Ctrl+C / SIGTERM: wind down from any state, then call cfg.on_finished
void asha_request_stop();

#endif  // APP_ASHA_APP_H_
