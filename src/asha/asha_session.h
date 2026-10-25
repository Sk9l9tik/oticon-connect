#ifndef ASHA_ASHA_SESSION_H_
#define ASHA_ASHA_SESSION_H_

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "asha/audio_stream.h"
#include "ble/ble_link.h"
#include "ble/timer.h"

// ASHA protocol on top of BleLink (port of python-impl/asha_test.py):
//   link ready -> discover service 0xFDF0 and its 5 characteristics
//   -> ReadOnlyProperties -> LE_PSM_OUT -> subscribe AudioStatus
//   -> connection interval 20 ms -> open CoC on the PSM -> write Start
//   -> AudioStatus 0 -> stream
// stop() (Ctrl+C or end of source): Stop -> link shutdown -> on_finished.
class AshaSession : private BleLink::Listener {
public:
  AshaSession(
      BondSpec bond, FrameSource& source, int volume, std::function<void()> on_finished);

  void start();  // power on; the rest follows from events
  void stop();   // from any state; on_finished comes at the end

private:
  enum class State {
    Setup,      // link not ready yet, or the GATT / CoC chain is running
    Starting,   // Start written, waiting for AudioStatus
    Streaming,  // frame clock running
    Stopping,   // Stop written or link shutting down
  };

  // BleLink::Listener
  void on_link_ready() override;
  void on_notification(uint16_t value_handle, std::span<const uint8_t> value) override;
  void on_coc_can_send() override;
  void on_link_down() override;

  // the setup chain, in order: each step starts the next from its callback
  void on_chars(bool ok, const std::vector<GattChar>& chars);
  void read_rop();
  void read_psm();
  void subscribe_status();
  void update_interval();
  void open_coc();
  void write_start();

  void fail_setup(const char* what);  // setup step failed: shut the link down

  BleLink link_;
  AudioStream stream_;
  std::function<void()> on_finished_;
  Timer stop_timer_;  // the aid does not confirm Stop: shut down anyway

  State state_ = State::Setup;
  bool stopping_ = false;

  uint8_t volume_;  // -128..0 as a raw byte
  uint16_t psm_ = 0;
  // value handles of the ASHA characteristics, 0 = not found
  uint16_t rop_ = 0, acp_ = 0, status_ = 0, volume_chr_ = 0, psm_chr_ = 0;
};

#endif  // ASHA_ASHA_SESSION_H_
