#include "ble/device_picker.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

// ASHA service: Google member UUID, BTstack has no name for it
static const uint16_t ASHA_SERVICE = 0xFDF0;

// Legacy advertising event types in GAP_EVENT_ADVERTISING_REPORT
static const uint8_t ADV_IND = 0;         // connectable, scannable
static const uint8_t ADV_DIRECT_IND = 1;  // connectable, directed

DevicePicker* DevicePicker::self_ = nullptr;

DevicePicker::DevicePicker(Picked picked) : picked_(std::move(picked)) {
  self_ = this;
}

DevicePicker::~DevicePicker() {
  stop();
}

void DevicePicker::start() {
  devices_.clear();
  line_.clear();
  if (!active_) {
    btstack_run_loop_set_data_source_fd(&ds_, STDIN_FILENO);
    btstack_run_loop_set_data_source_handler(&ds_, &stdin_trampoline);
    btstack_run_loop_enable_data_source_callbacks(&ds_, DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_add_data_source(&ds_);
    active_ = true;
  }
  printf("scanning; type a device number + Enter to pair (ASHA ones are marked)\n");
}

void DevicePicker::stop() {
  if (!active_)
    return;
  btstack_run_loop_remove_data_source(&ds_);
  active_ = false;
}

void DevicePicker::on_adv_report(const uint8_t* packet) {
  if (!active_)
    return;
  bd_addr_t addr;
  gap_event_advertising_report_get_address(packet, addr);
  uint8_t event_type = gap_event_advertising_report_get_advertising_event_type(packet);

  std::string name;
  bool asha = false;
  const uint8_t* data = gap_event_advertising_report_get_data(packet);
  uint8_t len = gap_event_advertising_report_get_data_length(packet);
  ad_context_t ctx;
  for (ad_iterator_init(&ctx, len, data); ad_iterator_has_more(&ctx); ad_iterator_next(&ctx)) {
    const uint8_t* d = ad_iterator_get_data(&ctx);
    uint8_t n = ad_iterator_get_data_len(&ctx);
    switch (ad_iterator_get_data_type(&ctx)) {
      case BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME:
      case BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME:
        name.assign(reinterpret_cast<const char*>(d), n);
        break;
      case BLUETOOTH_DATA_TYPE_SERVICE_DATA_16_BIT_UUID:
        if (n >= 2 && little_endian_read_16(d, 0) == ASHA_SERVICE)
          asha = true;
        break;
      case BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS:
      case BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS:
        for (uint8_t i = 0; i + 1 < n; i += 2)
          if (little_endian_read_16(d, i) == ASHA_SERVICE)
            asha = true;
        break;
      default:
        break;
    }
  }

  for (size_t i = 0; i < devices_.size(); i++) {
    ScanDevice& dev = devices_[i];
    if (memcmp(dev.addr.data(), addr, 6) != 0)
      continue;
    // known one: the scan response may bring the name or the ASHA data
    dev.rssi = gap_event_advertising_report_get_rssi(packet);
    bool changed = false;
    if (!name.empty() && dev.name != name) {
      dev.name = name;
      changed = true;
    }
    if (asha && !dev.asha) {
      dev.asha = true;
      changed = true;
    }
    if (changed)
      print(i, true);
    return;
  }

  // new ones only from connectable advertising (scan responses follow ADV_IND)
  if (event_type != ADV_IND && event_type != ADV_DIRECT_IND)
    return;
  ScanDevice dev;
  memcpy(dev.addr.data(), addr, 6);
  dev.type = static_cast<bd_addr_type_t>(gap_event_advertising_report_get_address_type(packet));
  dev.name = name;
  dev.rssi = gap_event_advertising_report_get_rssi(packet);
  dev.asha = asha;
  devices_.push_back(std::move(dev));
  print(devices_.size() - 1, false);
}

void DevicePicker::print(size_t index, bool update) const {
  const ScanDevice& d = devices_[index];
  printf(
      "%s[%zu] %s %-6s %4d dBm  %s%s\n", update ? "  " : "", index, bd_addr_to_str(d.addr.data()),
      d.type == BD_ADDR_TYPE_LE_PUBLIC ? "public" : "random", d.rssi,
      d.name.empty() ? "(no name)" : d.name.c_str(), d.asha ? "  <- ASHA" : "");
}

void DevicePicker::stdin_trampoline(btstack_data_source_t*, btstack_data_source_callback_type_t) {
  self_->on_stdin();
}

void DevicePicker::on_stdin() {
  char buf[64];
  ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
  if (n <= 0) {  // EOF (stdin not a terminal?) or error: nothing more to read
    printf("stdin closed, can't pick a device\n");
    stop();
    return;
  }
  for (ssize_t i = 0; i < n && active_; i++) {
    if (buf[i] != '\n') {
      line_ += buf[i];
      continue;
    }
    std::string line = std::move(line_);
    line_.clear();
    on_line(line);  // may stop() us
  }
}

void DevicePicker::on_line(const std::string& line) {
  if (line.empty())
    return;
  char* end;
  unsigned long i = std::strtoul(line.c_str(), &end, 10);
  if (*end != '\0' || i >= devices_.size()) {
    printf("no device '%s'\n", line.c_str());
    return;
  }
  ScanDevice dev = devices_[i];  // copy: start() may clear the list later
  printf("pairing with [%lu] %s\n", i, bd_addr_to_str(dev.addr.data()));
  stop();
  picked_(dev);
}
