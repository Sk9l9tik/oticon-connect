#ifndef BLE_BLE_LINK_H_
#define BLE_BLE_LINK_H_

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ble/bond_parser.h"
#include "ble/device_picker.h"
#include "ble/timer.h"

// Characteristic as the user of the link sees it; BleLink keeps BTstack's
// full struct (with the end handle, needed to find the CCCD) internally.
struct GattChar {
  std::string uuid;  // 128-bit, lowercase: "6333651e-c481-..."
  uint16_t value_handle;
  uint8_t properties;
};

// Which bond to use, decided on HCI_STATE_WORKING (the device DB needs the TLV)
struct BondSpec {
  std::optional<Bond> import;                  // from a BlueZ file: saved into the DB, then used
  std::optional<std::array<uint8_t, 6>> peer;  // stored bond of this peer; neither: the only one
  // import: only into this adapter (BlueZ keeps the bond under the adapter's address)
  std::optional<std::array<uint8_t, 6>> adapter;
  bool pair = false;  // --pair: new bonding with an aid in pairing mode
};

// BLE transport to one bonded peer over BTstack:
//   scan -> RPA resolves with the IRK -> connect -> re-encrypt with the LTK
//   -> Database Hash -> ready; then GATT client calls and one L2CAP CoC.
// All dongle / firmware quirks live here (see the .cc).
//
// Async like BTstack itself: every call returns at once and its result comes
// through the callback passed to it, or through Listener. One GATT operation
// at a time: start the next one from the callback of the previous.
//
// Single instance: BTstack's C callbacks have no context pointer and reach
// the object through a static pointer.
class BleLink {
public:
  class Listener {
  public:
    virtual ~Listener() = default;
    virtual void on_link_ready() = 0;  // connected, encrypted, GATT usable
    virtual void on_notification(uint16_t value_handle, std::span<const uint8_t> value) = 0;
    virtual void on_coc_can_send() = 0;  // one SDU may go out now
    // this adapter has no bond for the peer: power it off and try another one,
    // the next HCI_STATE_WORKING picks the bond again
    virtual void on_no_bond() = 0;
    // dropped (or connect failed) without shutdown(): reconnect() may follow
    virtual void on_link_lost(uint8_t reason) = 0;
    virtual void on_link_down() = 0;  // after shutdown() or no bond; final
  };

  using Done = std::function<void(bool ok)>;

  BleLink(BondSpec bond, Listener& listener);
  BleLink(const BleLink&) = delete;
  BleLink& operator=(const BleLink&) = delete;

  // Inits l2cap/sm/gatt; powering on is the caller's (it picks the adapter).
  // On every HCI_STATE_WORKING picks the bond and scans; ends in on_link_ready,
  // or on_no_bond when this adapter has none
  void start();

  // Unwinds from any state; on_link_down follows (at the latest after a
  // fallback timeout). Pending operation callbacks are not called any more.
  void shutdown();

  // Only after on_link_lost: scan again with the same bond
  void reconnect();

  // --- GATT (one at a time) ---
  void discover_characteristics(
      uint16_t service_uuid16, std::function<void(bool ok, const std::vector<GattChar>&)> done);
  void read(uint16_t value_handle, std::function<void(bool ok, std::span<const uint8_t>)> done);
  void write(uint16_t value_handle, std::span<const uint8_t> value, Done done);
  void subscribe(uint16_t value_handle, Done done);  // notifications -> Listener

  // interval in 1.25 ms units, latency 0, supervision timeout 2 s
  void set_connection_interval(uint16_t interval, Done done);

  // --- L2CAP CoC ---
  void open_coc(uint16_t psm, std::function<void(bool ok, uint16_t peer_mtu)> done);
  void request_can_send();
  // Only from on_coc_can_send. BTstack keeps the pointer until the next
  // can-send event: `sdu` must stay valid until then.
  void send(std::span<const uint8_t> sdu);

private:
  enum class State {
    Off,         // HCI not working yet, or down
    Scanning,    // looking for an advertisement that resolves with the IRK
    Connecting,  // LE Create Connection sent
    Connected,   // link up, encryption pending
    ReadHash,    // encrypted, reading Database Hash
    Ready,       // GATT and CoC usable
  };

  static void hci_trampoline(uint8_t type, uint16_t channel, uint8_t* packet, uint16_t size);
  static void gatt_trampoline(uint8_t type, uint16_t channel, uint8_t* packet, uint16_t size);
  static void coc_trampoline(uint8_t type, uint16_t channel, uint8_t* packet, uint16_t size);
  void on_hci_event(uint8_t* packet);
  void on_gatt_event(uint8_t* packet);
  void on_coc_event(uint8_t* packet);

  bool pick_bond();
  void start_scan();                         // passive for the bond; --pair: active + list
  void connect_picked(const ScanDevice& d);  // --pair: the user's choice
  void go_down();
  void lose(uint8_t reason);
  void fail(const char* what, uint8_t status);  // log + disconnect
  bool rpa_matches_irk(const uint8_t* addr) const;
  gatt_client_characteristic_t* find_char(uint16_t value_handle);

  static BleLink* self_;

  BondSpec spec_;
  Bond bond_;  // the one in use, set by pick_bond()
  Listener& listener_;
  bd_addr_t paired_identity_{};  // --pair: aid identity from SM_EVENT_IDENTITY_CREATED

  State state_ = State::Off;
  bool stopping_ = false;  // shutdown() called: no new steps, no callbacks
  bool down_ = false;      // on_link_down delivered
  uint16_t con_handle_;

  Timer enc_timer_;   // delay before encryption (quirk)
  Timer stop_timer_;  // shutdown fallback

  DevicePicker picker_;  // --pair: list of advertisers, choice from stdin

  // GATT: the running query and what its result events collected
  std::function<void(uint8_t att_status)> on_query_complete_;
  bool service_found_ = false;
  gatt_client_service_t service_;
  std::vector<gatt_client_characteristic_t> btstack_chars_;  // full structs, for subscribe()
  std::vector<GattChar> chars_;  // what discover_characteristics returns
  std::vector<uint8_t> read_value_;
  std::vector<uint8_t> write_buf_;           // GATT client sends from it asynchronously
  gatt_client_notification_t notification_;  // linked into BTstack's list while subscribed
  Done on_conn_update_;

  // CoC
  std::function<void(bool, uint16_t)> on_coc_open_;
  uint16_t coc_cid_ = 0;
  uint8_t coc_rx_[512];  // receive buffer, must outlive the channel
};

#endif  // BLE_BLE_LINK_H_
