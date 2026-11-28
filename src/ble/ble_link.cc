// BLE transport over BTstack, see ble_link.h. Dongle / firmware quirks:
//   - scan must be stopped BEFORE gap_connect (RTL8761B, else the connect hangs)
//   - LE Start Encryption right after connecting makes the aid drop the link
//     (0x13): wait ENC_DELAY_MS
//   - Robust Caching is on in the bond: read Database Hash (0x2B2A) before any
//     other GATT read, else reads fail with DATABASE_OUT_OF_SYNC. Peers
//     without it answer ATTRIBUTE_NOT_FOUND: fine, go on
//   - BTstack does not resolve advertisements itself: RPA matched by hand

#include <strings.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "ble/ble_link.h"

#include "ble/bond_store.h"

extern "C" {
#include "rijndael.h"
}

static const uint32_t ENC_DELAY_MS = 300;
static const uint32_t STOP_TIMEOUT_MS = 2000;

BleLink* BleLink::self_ = nullptr;

// Event subscriptions: BTstack links them into its lists, so they must live
// for the whole program
static btstack_packet_callback_registration_t hci_cb;  // HCI / GAP events
static btstack_packet_callback_registration_t sm_cb;   // Security Manager events

BleLink::BleLink(BondSpec bond, Listener& listener)
    : spec_(std::move(bond)), listener_(listener), con_handle_(HCI_CON_HANDLE_INVALID),
      enc_timer_([this] {
        // bonded: SM finds the LTK in le_device_db and re-encrypts, no pairing
        std::printf("request encryption\n");
        sm_request_pairing(con_handle_);
      }),
      stop_timer_([this] {
        std::printf("stop timeout, link down anyway\n");
        go_down();
      }),
      picker_([this](const ScanDevice& d) { connect_picked(d); }) {
  self_ = this;
}

void BleLink::start() {
  l2cap_init();
  sm_init();
  gatt_client_init();
  // public address 8C:68:8B:81:C0:E7: the aid knows the bond under it
  gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_OFF);

  hci_cb.callback = &hci_trampoline;
  hci_add_event_handler(&hci_cb);
  // SM events go only to SM subscribers, not to HCI handlers
  sm_cb.callback = &hci_trampoline;
  sm_add_event_handler(&sm_cb);

  sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
  sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);
  // HCI_STATE_WORKING arrives in on_hci_event once the caller powers an adapter on
}

// Off, Scanning -> down now (power off stops the scan itself)
// Connecting    -> cancel; LE_CONNECTION_COMPLETE with an error -> down
// connected     -> disconnect -> DISCONNECTION_COMPLETE -> down
void BleLink::shutdown() {
  if (stopping_ || down_)
    return;
  stopping_ = true;
  on_query_complete_ = nullptr;
  on_conn_update_ = nullptr;
  on_coc_open_ = nullptr;
  switch (state_) {
    case State::Off:
    case State::Scanning:
      go_down();
      return;
    case State::Connecting:
      gap_connect_cancel();
      break;
    default:
      enc_timer_.cancel();  // don't encrypt a link we are dropping
      gap_disconnect(con_handle_);
      break;
  }
  stop_timer_.start(STOP_TIMEOUT_MS);
}

void BleLink::go_down() {
  if (down_)
    return;
  down_ = true;
  state_ = State::Off;  // late events (adv reports, timers) are ignored from now on
  enc_timer_.cancel();
  stop_timer_.cancel();
  picker_.stop();
  listener_.on_link_down();
}

void BleLink::lose(uint8_t reason) {
  state_ = State::Off;
  enc_timer_.cancel();
  on_query_complete_ = nullptr;
  on_conn_update_ = nullptr;
  on_coc_open_ = nullptr;
  coc_cid_ = 0;
  // notification_ is still linked in BTstack's list; subscribe() would link it a second time
  gatt_client_stop_listening_for_characteristic_value_updates(&notification_);
  listener_.on_link_lost(reason);
}

void BleLink::reconnect() {
  if (stopping_ || down_ || state_ != State::Off)
    return;
  start_scan();  // bond_ already picked, no pick_bond(); --pair: pick again
}

void BleLink::recover() {
  if (stopping_ || down_ || con_handle_ == HCI_CON_HANDLE_INVALID)
    return;
  std::printf("recovering stalled audio link\n");
  gap_disconnect(con_handle_);
}

// Bonded: passive (0), interval = window = 0x30 x 0.625 ms: listen all the time.
// --pair: active (1), the name and the ASHA data may be only in the scan response
void BleLink::start_scan() {
  gap_set_scan_params(spec_.pair ? 1 : 0, 0x30, 0x30, 0);
  gap_start_scan();
  state_ = State::Scanning;
  if (spec_.pair)
    picker_.start();
}

void BleLink::connect_picked(const ScanDevice& d) {
  if (stopping_ || state_ != State::Scanning)
    return;
  gap_stop_scan();  // quirk: scan must be off BEFORE connecting
  gap_connect(d.addr.data(), d.type);
  state_ = State::Connecting;
}

void BleLink::fail(const char* what, uint8_t status) {
  std::printf("%s failed 0x%02x\n", what, status);
  if (con_handle_ != HCI_CON_HANDLE_INVALID)
    gap_disconnect(con_handle_);
}

// The bond must be in BTstack's device DB: SM resolves the RPA after
// connecting and takes the LTK from there. A BlueZ bond is saved into it
// first (same identity -> same entry), else a stored one is looked up.
bool BleLink::pick_bond() {
  if (spec_.import) {
    bd_addr_t local;
    gap_local_bd_addr(local);
    if (spec_.adapter && std::memcmp(local, spec_.adapter->data(), 6) != 0) {
      std::printf("bond file is for adapter %s, ", bd_addr_to_str(spec_.adapter->data()));
      std::printf("this is %s\n", bd_addr_to_str(local));
      return false;
    }
    bond_store_save(*spec_.import);
    bond_ = *spec_.import;
    return true;
  }
  return bond_store_find(spec_.peer ? spec_.peer->data() : nullptr, bond_);
}

// RPA = prand (3 bytes, top bits 01) | hash (3 bytes),
// hash = low 24 bits of AES-128(IRK, 0...0 | prand), spec function ah()
bool BleLink::rpa_matches_irk(const uint8_t* addr) const {
  if ((addr[0] & 0xc0) != 0x40)
    return false;
  uint32_t rk[RKLENGTH(128)];
  uint8_t plain[16] = {0};
  uint8_t cipher[16];
  std::memcpy(&plain[13], &addr[0], 3);
  int nrounds = rijndaelSetupEncrypt(rk, bond_.irk, 128);
  rijndaelEncrypt(rk, nrounds, plain, cipher);
  return std::memcmp(&cipher[13], &addr[3], 3) == 0;
}

gatt_client_characteristic_t* BleLink::find_char(uint16_t value_handle) {
  for (auto& c : btstack_chars_)
    if (c.value_handle == value_handle)
      return &c;
  return nullptr;
}

// ============================================================================
// GATT
// ============================================================================

// Two queries: the primary service, then its characteristics
void BleLink::discover_characteristics(
    uint16_t service_uuid16, std::function<void(bool, const std::vector<GattChar>&)> done) {
  service_found_ = false;
  btstack_chars_.clear();
  chars_.clear();
  on_query_complete_ = [this, done](uint8_t status) {
    if (status != ATT_ERROR_SUCCESS || !service_found_) {
      done(false, chars_);
      return;
    }
    on_query_complete_ = [this, done](uint8_t status) {
      done(status == ATT_ERROR_SUCCESS, chars_);
    };
    gatt_client_discover_characteristics_for_service(&gatt_trampoline, con_handle_, &service_);
  };
  gatt_client_discover_primary_services_by_uuid16(&gatt_trampoline, con_handle_, service_uuid16);
}

void BleLink::read(
    uint16_t value_handle, std::function<void(bool, std::span<const uint8_t>)> done) {
  read_value_.clear();
  on_query_complete_ = [this, done](uint8_t status) {
    done(status == ATT_ERROR_SUCCESS, read_value_);
  };
  gatt_client_read_value_of_characteristic_using_value_handle(
      &gatt_trampoline, con_handle_, value_handle);
}

void BleLink::write(uint16_t value_handle, std::span<const uint8_t> value, Done done) {
  write_buf_.assign(value.begin(), value.end());
  on_query_complete_ = [done](uint8_t status) { done(status == ATT_ERROR_SUCCESS); };
  gatt_client_write_value_of_characteristic(
      &gatt_trampoline, con_handle_, value_handle, (uint16_t)write_buf_.size(), write_buf_.data());
}

// Locally: route notifications of this characteristic to us; on the peer:
// CCCD = 0x0001, otherwise it sends none
void BleLink::subscribe(uint16_t value_handle, Done done) {
  gatt_client_characteristic_t* c = find_char(value_handle);
  if (!c) {
    done(false);
    return;
  }
  gatt_client_listen_for_characteristic_value_updates(
      &notification_, &gatt_trampoline, con_handle_, c);
  on_query_complete_ = [done](uint8_t status) { done(status == ATT_ERROR_SUCCESS); };
  gatt_client_write_client_characteristic_configuration(
      &gatt_trampoline, con_handle_, c, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
}

// Result comes as HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE
void BleLink::set_connection_parameters(uint16_t interval, uint16_t latency, Done done) {
  on_conn_update_ = std::move(done);
  gap_update_connection_parameters(con_handle_, interval, interval, latency, 200);
}

void BleLink::gatt_trampoline(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
  if (type == HCI_EVENT_PACKET)
    self_->on_gatt_event(packet);
}

void BleLink::on_gatt_event(uint8_t* packet) {
  switch (hci_event_packet_get_type(packet)) {
    case GATT_EVENT_SERVICE_QUERY_RESULT:
      gatt_event_service_query_result_get_service(packet, &service_);
      service_found_ = true;
      std::printf("service 0x%04x-0x%04x\n", service_.start_group_handle, service_.end_group_handle);
      break;
    case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
      gatt_client_characteristic_t c;
      gatt_event_characteristic_query_result_get_characteristic(packet, &c);
      btstack_chars_.push_back(c);
      std::string uuid = uuid128_to_str(c.uuid128);
      for (auto& ch : uuid)
        ch = (char)std::tolower((unsigned char)ch);
      chars_.push_back({uuid, c.value_handle, (uint8_t)c.properties});
      std::printf(
          "  chr %s value_handle 0x%04x props 0x%02x\n", uuid.c_str(), c.value_handle,
          c.properties);
      break;
    }
    case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
      const uint8_t* v = gatt_event_characteristic_value_query_result_get_value(packet);
      uint16_t len = gatt_event_characteristic_value_query_result_get_value_length(packet);
      read_value_.assign(v, v + len);
      break;
    }
    case GATT_EVENT_QUERY_COMPLETE: {
      uint8_t status = gatt_event_query_complete_get_att_status(packet);
      if (state_ == State::ReadHash) {
        // our own setup query, see on_hci_event
        if (status == ATT_ERROR_ATTRIBUTE_NOT_FOUND) {
          std::printf("no Database Hash (no Robust Caching)\n");
        } else if (status != ATT_ERROR_SUCCESS) {
          fail("Database Hash read", status);
          break;
        }
        if (stopping_)
          break;
        state_ = State::Ready;
        listener_.on_link_ready();
        break;
      }
      if (status != ATT_ERROR_SUCCESS)
        std::printf("GATT error 0x%02x\n", status);
      // moved out first: the callback usually starts the next query
      auto done = std::move(on_query_complete_);
      on_query_complete_ = nullptr;
      if (done)
        done(status);
      break;
    }
    case GATT_EVENT_NOTIFICATION: {
      uint16_t handle = gatt_event_notification_get_value_handle(packet);
      const uint8_t* v = gatt_event_notification_get_value(packet);
      uint16_t len = gatt_event_notification_get_value_length(packet);
      listener_.on_notification(handle, std::span<const uint8_t>(v, len));
      break;
    }
    default:
      break;
  }
}

// ============================================================================
// L2CAP CoC
// ============================================================================

void BleLink::open_coc(uint16_t psm, std::function<void(bool, uint16_t)> done) {
  on_coc_open_ = std::move(done);
  // MTU 512, 8 initial credits for the peer; MPS is taken from the ACL size
  uint8_t status = l2cap_cbm_create_channel(
      &coc_trampoline, con_handle_, psm, coc_rx_, sizeof(coc_rx_), 8, LEVEL_2, &coc_cid_);
  if (status != ERROR_CODE_SUCCESS) {
    auto cb = std::move(on_coc_open_);
    on_coc_open_ = nullptr;
    cb(false, 0);
  }
}

void BleLink::request_can_send() {
  l2cap_request_can_send_now_event(coc_cid_);
}

bool BleLink::send(std::span<const uint8_t> sdu) {
  uint8_t status = l2cap_send(coc_cid_, sdu.data(), (uint16_t)sdu.size());
  if (status != ERROR_CODE_SUCCESS)
    std::printf("L2CAP send failed 0x%02x\n", status);
  return status == ERROR_CODE_SUCCESS;
}

void BleLink::coc_trampoline(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
  if (type == HCI_EVENT_PACKET)  // L2CAP_DATA_PACKET: the aid sends nothing here
    self_->on_coc_event(packet);
}

void BleLink::on_coc_event(uint8_t* packet) {
  switch (hci_event_packet_get_type(packet)) {
    case L2CAP_EVENT_CBM_CHANNEL_OPENED: {
      uint8_t status = l2cap_event_cbm_channel_opened_get_status(packet);
      uint16_t mtu = l2cap_event_cbm_channel_opened_get_remote_mtu(packet);
      if (status == ERROR_CODE_SUCCESS)
        std::printf("CoC open, cid 0x%04x, peer MTU %u\n", coc_cid_, mtu);
      else
        std::printf("CoC open failed 0x%02x\n", status);
      auto cb = std::move(on_coc_open_);
      on_coc_open_ = nullptr;
      if (cb)
        cb(status == ERROR_CODE_SUCCESS, mtu);
      break;
    }
    case L2CAP_EVENT_CAN_SEND_NOW:
      if (!stopping_)
        listener_.on_coc_can_send();
      break;
    case L2CAP_EVENT_CHANNEL_CLOSED:
      std::printf("CoC closed\n");
      break;
    default:
      break;
  }
}

// ============================================================================
// HCI / GAP / SM
// ============================================================================

void BleLink::hci_trampoline(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
  if (type == HCI_EVENT_PACKET)
    self_->on_hci_event(packet);
}

void BleLink::on_hci_event(uint8_t* packet) {
  switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
      // stack is up (firmware, HCI Reset done): load the bond, start scanning
      if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING || stopping_)
        break;
      if (!spec_.pair && !pick_bond()) {
        listener_.on_no_bond();  // state stays Off: another adapter may come up
        break;
      }
      if (!spec_.pair)
        std::printf("peer %s, start scan\n", bd_addr_to_str(bond_.identity));
      start_scan();
      break;

    case GAP_EVENT_ADVERTISING_REPORT: {
      // reports keep coming until the controller processes the stop
      if (state_ != State::Scanning)
        break;
      if (spec_.pair) {
        picker_.on_adv_report(packet);  // the user picks, connect_picked() connects
        break;
      }
      // bonded: only RPAs that resolve with the IRK
      if (gap_event_advertising_report_get_address_type(packet) != BD_ADDR_TYPE_LE_RANDOM)
        break;
      bd_addr_t addr;
      gap_event_advertising_report_get_address(packet, addr);
      if (!rpa_matches_irk(addr))
        break;
      std::printf(
          "found peer %s type %u\n", bd_addr_to_str(addr),
          gap_event_advertising_report_get_advertising_event_type(packet));
      gap_stop_scan();  // quirk: scan must be off BEFORE connecting
      gap_connect(addr, BD_ADDR_TYPE_LE_RANDOM);
      state_ = State::Connecting;
      break;
    }

    case HCI_EVENT_META_GAP: {
      // result of gap_connect()
      if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE)
        break;
      uint8_t status = gap_subevent_le_connection_complete_get_status(packet);
      if (status != ERROR_CODE_SUCCESS) {
        // also the result of gap_connect_cancel() on shutdown (0x02)
        std::printf("connect failed 0x%02x\n", status);
        if (stopping_)
          go_down();
        else
          lose(status);  // e.g. 0x3E at the edge of range: retry
        break;
      }
      con_handle_ = gap_subevent_le_connection_complete_get_connection_handle(packet);
      if (stopping_) {
        // shutdown came while connecting and the link won the race with the cancel
        gap_disconnect(con_handle_);
        break;
      }
      std::printf(
          "connected, handle 0x%04x, interval %u x 1.25 ms\n", con_handle_,
          gap_subevent_le_connection_complete_get_conn_interval(packet));
      state_ = State::Connected;
      enc_timer_.start(ENC_DELAY_MS);  // quirk: immediate encryption -> drop
      break;
    }

    case HCI_EVENT_LE_META: {
      // result of gap_update_connection_parameters()
      if (hci_event_le_meta_get_subevent_code(packet) != HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE)
        break;
      uint8_t status = hci_subevent_le_connection_update_complete_get_status(packet);
      std::printf(
          "conn update: status 0x%02x, interval %u x 1.25 ms, latency %u, "
          "timeout %u x 10 ms\n",
          status, hci_subevent_le_connection_update_complete_get_conn_interval(packet),
          hci_subevent_le_connection_update_complete_get_conn_latency(packet),
          hci_subevent_le_connection_update_complete_get_supervision_timeout(packet));
      auto cb = std::move(on_conn_update_);
      on_conn_update_ = nullptr;
      if (cb)
        cb(status == ERROR_CODE_SUCCESS);
      break;
    }

    case HCI_EVENT_DISCONNECTION_COMPLETE: {
      // 0x16 = we disconnected, 0x13 = the peer did, 0x08 = supervision timeout
      uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
      std::printf("disconnected, reason 0x%02x\n", reason);
      con_handle_ = HCI_CON_HANDLE_INVALID;
      if (stopping_) {
        go_down();
        break;
      }
      lose(reason);
      break;
    }

    case HCI_EVENT_ENCRYPTION_CHANGE:
    case HCI_EVENT_ENCRYPTION_CHANGE_V2: {
      // controller confirms the link is encrypted: GATT reads are allowed now.
      // BTstack enables v2, controllers that support it (Intel 8087:0033) send
      // only v2. Same layout up to encryption_enabled, v1 getters read both
      uint8_t status = hci_event_encryption_change_get_status(packet);
      uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
      std::printf("encryption change: status 0x%02x, enabled %u\n", status, enabled);
      if (status != ERROR_CODE_SUCCESS || !enabled) {
        fail("encryption", status);
        break;
      }
      if (stopping_ || state_ != State::Connected || spec_.pair)
        break;                   // pairing: wait for SM_EVENT_PAIRING_COMPLETE
      state_ = State::ReadHash;  // quirk: Robust Caching, see the top of the file
      gatt_client_read_value_of_characteristics_by_uuid16(
          &gatt_trampoline, con_handle_, 0x0001, 0xffff, 0x2B2A);
      break;
    }

    // --- SM events (diagnostics: show whether the bond is used) ---
    case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED: {
      bd_addr_t id;
      sm_event_identity_resolving_succeeded_get_identity_address(packet, id);
      std::printf("SM resolved RPA -> %s\n", bd_addr_to_str(id));
      std::memcpy(paired_identity_, id, 6);  // --pair: the picked device is already bonded
      break;
    }
    case SM_EVENT_IDENTITY_RESOLVING_FAILED:
      std::printf("SM: RPA not resolved (bond not in db?)\n");
      break;
    case SM_EVENT_PAIRING_STARTED:
      // bad sign: SM did not find the LTK and pairs from scratch
      std::printf("!!! new pairing started: bond not used\n");
      break;
    case SM_EVENT_REENCRYPTION_COMPLETE: {
      uint8_t st = sm_event_reencryption_complete_get_status(packet);
      std::printf("re-encryption status 0x%02x\n", st);
      // --pair with a device that is already bonded: SM re-encrypts with the
      // stored LTK, no PAIRING_COMPLETE follows. Use that bond; comes before
      // HCI_EVENT_ENCRYPTION_CHANGE (SM's handler runs first), which then
      // goes on to ReadHash as for a bonded start
      if (st != ERROR_CODE_SUCCESS || !spec_.pair || stopping_)
        break;
      if (!bond_store_find(paired_identity_, bond_)) {
        fail("bond after re-encryption", 0);
        break;
      }
      std::printf("already bonded with %s, using the stored bond\n", bd_addr_to_str(bond_.identity));
      spec_.pair = false;
      break;
    }
    // --- pairing (--pair): SC Just Works, the aid has no IO ---
    case SM_EVENT_JUST_WORKS_REQUEST:
      sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
      break;
    case SM_EVENT_IDENTITY_CREATED:
      sm_event_identity_created_get_identity_address(packet, paired_identity_);
      break;
    case SM_EVENT_PAIRING_COMPLETE: {
      uint8_t st = sm_event_pairing_complete_get_status(packet);
      if (st != ERROR_CODE_SUCCESS) {
        std::printf("pairing reason 0x%02x\n", sm_event_pairing_complete_get_reason(packet));
        fail("pairing", st);
        break;
      }
      if (!spec_.pair || stopping_ || state_ != State::Connected)
        break;
      if (!bond_store_find(paired_identity_, bond_)) {
        fail("bond after pairing", 0);
        break;
      }
      spec_.pair = false;  // reconnect() goes the IRK path from now on
      state_ = State::ReadHash;
      gatt_client_read_value_of_characteristics_by_uuid16(
          &gatt_trampoline, con_handle_, 0x0001, 0xffff, 0x2B2A);
      break;
    }
    default:
      break;
  }
}
