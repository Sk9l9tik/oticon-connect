#ifndef APP_USB_ADAPTERS_H_
#define APP_USB_ADAPTERS_H_

#include <cstdint>
#include <string>
#include <vector>

// A Bluetooth controller on USB: an external dongle or a laptop's built-in
// adapter (those sit on an internal USB port too). UART/SDIO controllers are
// not reachable through libusb.
struct UsbAdapter {
  uint8_t bus = 0;             // 0: any bus (only from parse_usb_path without "bus-")
  std::vector<uint8_t> ports;  // port numbers from the root hub
  uint16_t vid = 0, pid = 0;
  bool removable = false;  // sysfs "removable": an external port

  std::string path() const;  // "1-2.3", the form of --usb and /sys/bus/usb/devices
  bool same_place(const UsbAdapter& o) const {
    return (bus == 0 || o.bus == 0 || bus == o.bus) && ports == o.ports;
  }
};

// All Bluetooth controllers now on USB, external (removable) ones first.
// Only enumerates: nothing is opened, the kernel driver stays bound.
std::vector<UsbAdapter> find_usb_adapters();

// "1-2.3" -> bus 1, ports {2, 3}; "2.3" -> ports {2, 3} on any bus
bool parse_usb_path(const std::string& s, UsbAdapter& out);

// BTstack detaches btusb on open but does not attach it back on close: give the
// adapter back to the kernel (BlueZ), else a built-in one stays dead until reboot
void reattach_kernel_driver(const UsbAdapter& a);

#endif  // APP_USB_ADAPTERS_H_
