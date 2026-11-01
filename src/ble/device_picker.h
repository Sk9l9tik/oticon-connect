#ifndef BLE_DEVICE_PICKER_H_
#define BLE_DEVICE_PICKER_H_

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "btstack.h"

struct ScanDevice {
  std::array<uint8_t, 6> addr;
  bd_addr_type_t type;
  std::string name;  // from ADV or the scan response; empty: none seen yet
  int8_t rssi;
  bool asha;  // 0xFDF0 in Service Data or in the 16-bit UUID list
};

// --pair: lists connectable advertisers as they show up, numbered; the user
// types a number + Enter, `picked` gets that device. stdin is read through a
// BTstack data source, so everything runs on the run loop thread.
// Single instance (the data source has no context pointer).
class DevicePicker {
public:
  using Picked = std::function<void(const ScanDevice&)>;

  explicit DevicePicker(Picked picked);
  ~DevicePicker();
  DevicePicker(const DevicePicker&) = delete;
  DevicePicker& operator=(const DevicePicker&) = delete;

  void start();  // empty list, listen on stdin
  void stop();   // stop listening; safe when not started

  // GAP_EVENT_ADVERTISING_REPORT while started (active scan: scan responses too)
  void on_adv_report(const uint8_t* packet);

private:
  static void stdin_trampoline(btstack_data_source_t* ds, btstack_data_source_callback_type_t type);
  void on_stdin();
  void on_line(const std::string& line);
  void print(size_t index, bool update) const;

  static DevicePicker* self_;

  Picked picked_;
  std::vector<ScanDevice> devices_;
  btstack_data_source_t ds_{};
  bool active_ = false;
  std::string line_;  // stdin input up to the next '\n'
};

#endif  // BLE_DEVICE_PICKER_H_
