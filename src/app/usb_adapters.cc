#include "app/usb_adapters.h"

#include <libusb.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

// Class E0 (wireless controller) / 01 (RF) / 01 (Bluetooth): on the device
// (most dongles, Intel) or only on interface 0 (device class 0 or EF, some
// Realtek / MediaTek)
static bool is_bt_class(uint8_t cls, uint8_t sub, uint8_t proto) {
  return cls == 0xE0 && sub == 0x01 && proto == 0x01;
}

static bool is_bt_device(libusb_device* dev, const libusb_device_descriptor& d) {
  if (is_bt_class(d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol))
    return true;
  libusb_config_descriptor* cfg;
  if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
    return false;
  bool bt = false;
  if (cfg->bNumInterfaces > 0 && cfg->interface[0].num_altsetting > 0) {
    const libusb_interface_descriptor& i = cfg->interface[0].altsetting[0];
    bt = is_bt_class(i.bInterfaceClass, i.bInterfaceSubClass, i.bInterfaceProtocol);
  }
  libusb_free_config_descriptor(cfg);
  return bt;
}

std::string UsbAdapter::path() const {
  std::string s = std::to_string(bus) + "-";
  for (size_t i = 0; i < ports.size(); i++)
    s += (i ? "." : "") + std::to_string(ports[i]);
  return s;
}

// "removable" / "fixed" / "unknown"; built-in adapters are "fixed" when the
// firmware (ACPI) says so, "unknown" on some machines: only "removable" counts
static bool is_removable(const UsbAdapter& a) {
  std::ifstream f("/sys/bus/usb/devices/" + a.path() + "/removable");
  std::string v;
  return f >> v && v == "removable";
}

std::vector<UsbAdapter> find_usb_adapters() {
  std::vector<UsbAdapter> out;
  libusb_context* ctx;  // own context: BTstack's transport uses the default one
  if (libusb_init(&ctx) != 0)
    return out;
  libusb_device** devs;
  ssize_t n = libusb_get_device_list(ctx, &devs);
  for (ssize_t i = 0; i < n; i++) {
    libusb_device_descriptor d;
    if (libusb_get_device_descriptor(devs[i], &d) != 0 || !is_bt_device(devs[i], d))
      continue;
    UsbAdapter a;
    a.bus = libusb_get_bus_number(devs[i]);
    uint8_t ports[7];  // USB allows at most 7 tiers
    int len = libusb_get_port_numbers(devs[i], ports, sizeof(ports));
    if (len <= 0)
      continue;  // a root hub, not a device
    a.ports.assign(ports, ports + len);
    a.vid = d.idVendor;
    a.pid = d.idProduct;
    a.removable = is_removable(a);
    out.push_back(std::move(a));
  }
  if (n >= 0)
    libusb_free_device_list(devs, 1);
  libusb_exit(ctx);
  // external first: a dongle is plugged in on purpose; stable among equals
  std::stable_sort(out.begin(), out.end(), [](const UsbAdapter& x, const UsbAdapter& y) {
    return x.removable > y.removable;
  });
  return out;
}

bool parse_usb_path(const std::string& s, UsbAdapter& out) {
  unsigned bus = 0;
  int n = 0;
  if (s.find('-') != std::string::npos &&
      (std::sscanf(s.c_str(), "%u-%n", &bus, &n) != 1 || n == 0 || bus == 0 || bus > 255))
    return false;
  std::vector<uint8_t> ports;
  const char* p = s.c_str() + n;
  while (*p) {
    char* end;
    unsigned long port = std::strtoul(p, &end, 10);
    if (end == p || port == 0 || port > 255 || ports.size() == 7)
      return false;
    ports.push_back(static_cast<uint8_t>(port));
    if (*end == '.' && end[1] != '\0')
      end++;
    else if (*end != '\0')
      return false;
    p = end;
  }
  if (ports.empty())
    return false;
  out.bus = static_cast<uint8_t>(bus);
  out.ports = std::move(ports);
  return true;
}

void reattach_kernel_driver(const UsbAdapter& a) {
  libusb_context* ctx;
  if (libusb_init(&ctx) != 0)
    return;
  libusb_device** devs;
  ssize_t n = libusb_get_device_list(ctx, &devs);
  for (ssize_t i = 0; i < n; i++) {
    UsbAdapter d;
    d.bus = libusb_get_bus_number(devs[i]);
    uint8_t ports[7];
    int len = libusb_get_port_numbers(devs[i], ports, sizeof(ports));
    if (len <= 0)
      continue;
    d.ports.assign(ports, ports + len);
    if (!d.same_place(a))
      continue;
    libusb_device_handle* h;
    if (libusb_open(devs[i], &h) == 0) {
      // interface 0 only: btusb's probe claims the isochronous interface 1 itself
      int r = libusb_attach_kernel_driver(h, 0);
      if (r == 0)
        std::printf("adapter %s given back to the kernel\n", a.path().c_str());
      else if (r != LIBUSB_ERROR_BUSY && r != LIBUSB_ERROR_NOT_FOUND)
        std::printf(
            "adapter %s: attach kernel driver: %s\n", a.path().c_str(), libusb_error_name(r));
      libusb_close(h);
    }
    break;
  }
  if (n >= 0)
    libusb_free_device_list(devs, 1);
  libusb_exit(ctx);
}
