// Entry point in place of port/libusb/main.c: options, signals, BTstack init.

#include <getopt.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <vector>

#include "btstack_chipset_realtek.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_posix.h"
#include "btstack_tlv.h"
#include "btstack_tlv_posix.h"
#include "hci.h"
#include "hci_dump.h"
#include "hci_dump_posix_fs.h"
#include "hci_transport.h"
#include "hci_transport_usb.h"

#include "app/asha_app.h"
#include "app/usb_adapters.h"

#include "ble/le_device_db_tlv.h"
#include "ble/timer.h"

#define USB_VENDOR_ID_REALTEK 0x0bda

// ---------------------------------------------------------------------------
// TLV: bonds (le_device_db) and SM keys, one file per adapter: a bond belongs
// to the pair adapter + aid, the aid knows us by the adapter's address
// ---------------------------------------------------------------------------

static btstack_tlv_posix_t tlv_context;  // static: BTstack keeps a pointer
static bool tlv_open = false;
static bd_addr_t tlv_adapter;  // whose TLV is open

// $XDG_STATE_HOME/oticon-connect/btstack_<adapter>.tlv, fallback ~/.local/state/...
static std::string tlv_path(const bd_addr_t adapter) {
  std::string dir;
  if (const char* x = getenv("XDG_STATE_HOME"); x && *x)
    dir = x;
  else
    dir = std::string(getenv("HOME")) + "/.local/state";
  dir += "/oticon-connect";
  mkdir(dir.c_str(), 0700);  // EEXIST is fine; ~/.local/state itself must exist
  return dir + "/btstack_" + bd_addr_to_str_with_delimiter(adapter, '-') + ".tlv";
}

static void open_tlv(const bd_addr_t adapter) {
  if (tlv_open) {
    if (memcmp(tlv_adapter, adapter, 6) == 0)  // read again, e.g. after a firmware download
      return;
    btstack_tlv_posix_deinit(&tlv_context);  // another adapter came up
  }
  memcpy(tlv_adapter, adapter, 6);
  std::string path = tlv_path(adapter);
  const btstack_tlv_t* tlv = btstack_tlv_posix_init_instance(&tlv_context, path.c_str());
  chmod(path.c_str(), 0600);  // LTKs inside
  btstack_tlv_set_instance(tlv, &tlv_context);
  le_device_db_tlv_configure(tlv, &tlv_context);
  tlv_open = true;
  std::printf("adapter %s, TLV: %s\n", bd_addr_to_str(adapter), path.c_str());
}

// ---------------------------------------------------------------------------
// Adapter selection: every Bluetooth controller on USB, external or built-in,
// external first. One is powered on at a time; it is dropped (and the next one
// tried) when it can't be opened, its init hangs, or it has no bond for the
// aid. None left: rescan every RESCAN_MS until one is plugged in.
// ---------------------------------------------------------------------------

static const uint32_t RESCAN_MS = 2000;
static const uint32_t INIT_TIMEOUT_MS = 10000;  // power on -> HCI_STATE_WORKING

static UsbAdapter usb_filter;  // --usb: only this one
static bool have_usb_filter = false;
static std::vector<UsbAdapter> rejected;  // dropped ones, forgotten once unplugged
static UsbAdapter current;                // powered on, or being powered on
static bool have_current = false;
static bool finishing = false;  // the app is done: power off and leave
static bool told_waiting = false;

static bool listed(const std::vector<UsbAdapter>& v, const UsbAdapter& a) {
  return std::any_of(v.begin(), v.end(), [&](const UsbAdapter& x) { return x.same_place(a); });
}

static void try_next_adapter();
static void drop_adapter(const char* why);
// Deferred through the run loop: these are started from inside BTstack's event
// dispatch, where powering on/off right away would re-enter it
static Timer next_timer([] { try_next_adapter(); });
static Timer init_timer([] { drop_adapter("init timed out"); });
static Timer no_bond_timer([] { drop_adapter("no bond for the aid"); });

static void try_next_adapter() {
  if (finishing)
    return;
  std::vector<UsbAdapter> found = find_usb_adapters();
  // a dropped adapter that was unplugged gets another chance when it is back
  std::erase_if(rejected, [&](const UsbAdapter& r) { return !listed(found, r); });
  for (const UsbAdapter& a : found) {
    if ((have_usb_filter && !a.same_place(usb_filter)) || listed(rejected, a))
      continue;
    current = a;
    have_current = true;
    told_waiting = false;
    std::printf(
        "adapter %s (%04x:%04x, %s)\n", a.path().c_str(), a.vid, a.pid,
        a.removable ? "external" : "built-in");
    uint8_t ports[7];
    std::copy(a.ports.begin(), a.ports.end(), ports);
    hci_transport_usb_set_bus_and_path(a.bus, (int)a.ports.size(), ports);
    init_timer.start(INIT_TIMEOUT_MS);
    hci_power_control(HCI_POWER_ON);  // failure: BTSTACK_EVENT_POWERON_FAILED
    return;
  }
  if (!told_waiting) {
    if (have_usb_filter)
      std::printf("waiting for the adapter at %s\n", usb_filter.path().c_str());
    else if (found.empty())
      std::printf("waiting for a Bluetooth adapter\n");
    else
      std::printf("no usable adapter (bond missing? import it with --bond), waiting for another\n");
    told_waiting = true;
  }
  next_timer.start(RESCAN_MS);
}

// Power the current adapter off; HCI_STATE_OFF then tries the next one
static void drop_adapter(const char* why) {
  if (!have_current || finishing)
    return;
  std::printf("adapter %s: %s, trying the next one\n", current.path().c_str(), why);
  rejected.push_back(current);
  init_timer.cancel();
  hci_power_control(HCI_POWER_OFF);
}

// The app is done (Ctrl+C, end of file, ...): power off, then leave the run loop
static void finish() {
  finishing = true;
  next_timer.cancel();
  init_timer.cancel();
  no_bond_timer.cancel();
  if (hci_get_state() == HCI_STATE_OFF)
    btstack_run_loop_trigger_exit();  // between adapters: nothing to power off
  else
    hci_power_control(HCI_POWER_OFF);
}

// ---------------------------------------------------------------------------
// HCI events
// ---------------------------------------------------------------------------

static btstack_packet_callback_registration_t main_hci_reg;  // static: kept in BTstack's list

static void main_hci_handler(
    uint8_t type, [[maybe_unused]] uint16_t channel, uint8_t* packet,
    [[maybe_unused]] uint16_t size) {
  if (type != HCI_EVENT_PACKET)
    return;
  switch (hci_event_packet_get_type(packet)) {
    // Comes from the transport while powering on, before HCI Reset:
    // the only moment to pick the chipset driver
    case HCI_EVENT_TRANSPORT_USB_INFO: {
      uint16_t vid = hci_event_transport_usb_info_get_vendor_id(packet);
      uint16_t pid = hci_event_transport_usb_info_get_product_id(packet);
      // ports in the --usb form (the bus field is only set when --usb gives one)
      printf("USB device %04x:%04x, ports ", vid, pid);
      const uint8_t* path = hci_event_transport_usb_info_get_path(packet);
      for (int i = 0; i < hci_event_transport_usb_info_get_path_len(packet); i++)
        printf(i ? ".%u" : "%u", path[i]);
      printf("\n");
      if (vid == USB_VENDOR_ID_REALTEK) {
        btstack_chipset_realtek_set_product_id(pid);
        hci_set_chipset(btstack_chipset_realtek_instance());
        hci_enable_custom_pre_init();
      } else {
        // the previous adapter may have been a Realtek one. Others run the
        // firmware the kernel loaded (pre-init without a chipset is a no-op)
        hci_set_chipset(nullptr);
      }
      break;
    }
    // Adapter address is known: open its TLV. Comes during init, before
    // HCI_STATE_WORKING, where SM loads its keys from the TLV (SM's handler
    // runs before ours, so opening it on WORKING would be too late).
    case HCI_EVENT_COMMAND_COMPLETE: {
      if (hci_event_command_complete_get_command_opcode(packet) != HCI_OPCODE_HCI_READ_BD_ADDR)
        break;
      const uint8_t* ret = hci_event_command_complete_get_return_parameters(packet);
      if (ret[0] != ERROR_CODE_SUCCESS)
        break;
      bd_addr_t addr;
      reverse_bd_addr(&ret[1], addr);
      open_tlv(addr);
      break;
    }
    case BTSTACK_EVENT_STATE:
      switch (btstack_event_state_get_state(packet)) {
        case HCI_STATE_WORKING:
          init_timer.cancel();
          break;
        // Power off done: dropped adapter, our shutdown or a fatal error.
        // Give the adapter back to the kernel, then the next one or leave
        // (main() then closes TLV and the trace)
        case HCI_STATE_OFF:
          if (have_current)
            reattach_kernel_driver(current);
          have_current = false;
          if (finishing)
            btstack_run_loop_trigger_exit();
          else
            next_timer.start(0);
          break;
        default:
          break;
      }
      break;
    // Transport did not open (no access, device gone): the state stays OFF
    case BTSTACK_EVENT_POWERON_FAILED:
      init_timer.cancel();
      if (!have_current)
        break;
      std::printf(
          "adapter %s: can't open it (no access? needs a udev rule, see README)\n",
          current.path().c_str());
      rejected.push_back(current);
      reattach_kernel_driver(current);
      have_current = false;
      if (finishing)
        btstack_run_loop_trigger_exit();
      else
        next_timer.start(0);
      break;
    default:
      break;
  }
}

struct Options {
  std::string bond;                           // BlueZ info file to import; empty: stored bond
  std::string peer;                           // identity address of the aid
  std::string usb;                            // adapter by USB path "bus-port.port"
  std::string audio = "assets/tone_1k.g722";  // raw G.722 frames, 160 bytes each
  std::string log = "app.btsnoop";            // HCI trace, readable by btmon -r
  int tone_hz = 0;        // > 0: endless sine encoded on the fly instead of the file
  bool pipewire = false;  // audio from the PipeWire sink instead of the file
  int volume = -50;       // ASHA volume, -128 (mute) .. 0 (max)
};

static void usage(const char* name) {
  std::printf(
      "usage: %s [options]\n"
      "  -b, --bond PATH     import a BlueZ bond (/var/lib/bluetooth/<adapter>/<aid>/info);\n"
      "                      without it the bond saved by an earlier import is used\n"
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
      "  -h, --help          this help\n",
      name);
}

// Returns false on a bad option; the caller prints usage and exits
static bool parse_options(int argc, char** argv, Options& opt) {
  static const option long_options[] = {
      {"bond", required_argument, nullptr, 'b'},   {"peer", required_argument, nullptr, 'P'},
      {"usb", required_argument, nullptr, 'u'},    {"audio", required_argument, nullptr, 'a'},
      {"volume", required_argument, nullptr, 'v'}, {"log", required_argument, nullptr, 'l'},
      {"help", no_argument, nullptr, 'h'},         {"tone", required_argument, nullptr, 't'},
      {"pipewire", no_argument, nullptr, 'p'},     {nullptr, 0, nullptr, 0}};

  int c;
  while ((c = getopt_long(argc, argv, "b:P:u:a:v:l:t:ph", long_options, nullptr)) != -1) {
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
        char* end;
        long v = std::strtol(optarg, &end, 10);
        if (*end != '\0' || v < -128 || v > 0) {
          fprintf(stderr, "bad volume '%s', expected -128..0\n", optarg);
          return false;
        }
        opt.volume = static_cast<int>(v);
        break;
      }
      case 'l':
        opt.log = optarg;
        break;
      case 't': {
        char* end;
        long t = std::strtol(optarg, &end, 10);
        if (*end != '\0' || t < 1 || t > 7999) {
          fprintf(stderr, "bad tone '%s', expected 1..7999\n", optarg);
          return false;
        }
        opt.tone_hz = static_cast<int>(t);
        break;
      }
      case 'p':
        opt.pipewire = true;
        break;
      default:  // 'h' and '?' (getopt already printed the error)
        return false;
    }
  }
  if (opt.pipewire && opt.tone_hz > 0) {
    std::fprintf(stderr, "--tone and --pipewire: pick one\n");
    return false;
  }
  if (optind < argc) {
    std::fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------------
// SIGINT/SIGTERM are blocked and read from a signalfd registered in the run
// loop, so the handler runs on the BTstack thread like any other event and may
// call BTstack freely. The mask must be set before libusb starts its threads:
// they inherit it, otherwise the kernel may deliver the signal to one of them.

static btstack_data_source_t signal_ds;  // static: the run loop keeps a pointer
static bool shutdown_requested = false;

// The app unwinds (Stop, disconnect), then calls on_finished -> power off ->
// HCI_STATE_OFF -> trigger_exit in main_hci_handler
static void request_shutdown() {
  asha_request_stop();
}

static void on_signal(
    btstack_data_source_t* ds, [[maybe_unused]] btstack_data_source_callback_type_t type) {
  signalfd_siginfo si;
  if (read(ds->source.fd, &si, sizeof(si)) != (ssize_t)sizeof(si))
    return;
  if (shutdown_requested) {
    // Second Ctrl+C: shutdown is stuck, leave now (the dongle may need a replug)
    std::fprintf(stderr, "signal %u again, exiting\n", si.ssi_signo);
    std::exit(1);
  }
  std::printf("signal %u, shutting down\n", si.ssi_signo);
  shutdown_requested = true;
  request_shutdown();
}

// Step 1, before any thread exists: block the signals, return a signalfd
static int block_signals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &set, nullptr) != 0) {
    perror("sigprocmask");
    return -1;
  }
  int fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
  if (fd < 0)
    std::perror("signalfd");
  return fd;
}

// Step 2, after btstack_run_loop_init: watch the signalfd in the run loop
static void watch_signals(int fd) {
  btstack_run_loop_set_data_source_fd(&signal_ds, fd);
  btstack_run_loop_set_data_source_handler(&signal_ds, &on_signal);
  btstack_run_loop_enable_data_source_callbacks(&signal_ds, DATA_SOURCE_CALLBACK_READ);
  btstack_run_loop_add_data_source(&signal_ds);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  int sig_fd = block_signals();
  if (sig_fd < 0)
    return EXIT_FAILURE;

  Options opt;
  if (!parse_options(argc, argv, opt)) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  have_usb_filter = !opt.usb.empty();
  if (have_usb_filter && !parse_usb_path(opt.usb, usb_filter)) {
    std::fprintf(
        stderr, "bad USB path '%s', expected [bus-]port[.port...], e.g. 5 or 1-2.3\n",
        opt.usb.c_str());
    return EXIT_FAILURE;
  }

  btstack_memory_init();
  btstack_run_loop_init(btstack_run_loop_posix_get_instance());
  watch_signals(sig_fd);

  hci_dump_posix_fs_open(opt.log.c_str(), HCI_DUMP_BTSNOOP);
  hci_dump_init(hci_dump_posix_fs_get_instance());
  std::printf("HCI trace: %s\n", opt.log.c_str());

  // TLV is opened later, in main_hci_handler, once the adapter address is read
  hci_init(hci_transport_usb_instance(), nullptr);

  main_hci_reg.callback = &main_hci_handler;
  hci_add_event_handler(&main_hci_reg);

  AppConfig cfg{
      .bond = opt.bond.empty() ? nullptr : opt.bond.c_str(),
      .peer = opt.peer.empty() ? nullptr : opt.peer.c_str(),
      .audio = opt.audio.c_str(),
      .tone_hz = opt.tone_hz,
      .pipewire = opt.pipewire,
      .volume = opt.volume,
      .on_finished = [] { finish(); },
      .on_no_bond = [] { no_bond_timer.start(0); },
  };
  if (!asha_start(cfg))
    return EXIT_FAILURE;
  next_timer.start(0);  // pick an adapter and power it on, from the run loop

  btstack_run_loop_execute();

  if (tlv_open)
    btstack_tlv_posix_deinit(&tlv_context);

  hci_dump_posix_fs_close();
  close(sig_fd);
  return 0;
}
