# oticon-connect

Streams audio from a Linux PC to an ASHA hearing aid (Audio Streaming for
Hearing Aids, Google's protocol over Bluetooth LE), without BlueZ.

`asha_probe` drives a USB Bluetooth dongle directly through libusb with
[BTstack](https://github.com/bluekitchen/btstack), connects to a bonded aid,
and sends G.722 audio from:

- a virtual PipeWire output device **"Oticon ASHA"**: anything apps play into it
  (browser, player, `pw-play`) is heard in the aid;
- a sine tone generated on the fly;
- a raw G.722 file.

Tested with an Oticon More 2 (right ear) and a Realtek RTL8761B dongle
(`0bda:a728`). BlueZ's own ASHA support does not work with this dongle:
its firmware hangs on scan-off while connected and has no LL Privacy.

## Status

Works: pairing (`--pair`) or importing a BlueZ bond, ASHA setup, streaming from
all three sources, reconnect after the link drops, clean shutdown on Ctrl+C.
One aid only, no binaural streaming yet.
See [Limitations](#limitations) and `.todo.md`.

## Requirements

- Linux with PipeWire
- CMake ≥ 3.20, pkg-config, a C++23 compiler (GCC or Clang)
- libusb-1.0, spandsp (G.722), libpipewire-0.3
- BTstack source in `third_partry/btstack`
- A USB Bluetooth LE dongle (Realtek tested), not used by BlueZ at the same time

Arch Linux:

```sh
pacman -S cmake pkgconf libusb spandsp pipewire
git clone https://github.com/bluekitchen/btstack third_partry/btstack
```

## Build

```sh
cmake -B build                       # or: CXX=clang++ CC=clang cmake -B build
cmake --build build -j
ctest --test-dir build               # ring buffer test under ThreadSanitizer
```

Binaries: `build/asha_probe`, `build/g722_gen`, `build/pw_capture`.

## USB access

BTstack takes the adapter over from the kernel driver. To run without root,
give your user access to it with a udev rule. It covers every USB Bluetooth
controller, dongles and laptops' built-in adapters alike. Install it once:

```sh
tools/install-udev-rules.sh
```

No logout or replug is needed. Without the rule, `asha_probe` reports
`can't open it (no access? ...)` and libusb logs `errno=13`.

The rule grants access to the user at the active local session only, so it does
not apply over SSH.

A few adapters report the Bluetooth class only on their interface (device class
`00`/`ef`). If yours is one of them, add a line by ID to
`tools/70-oticon-asha-adapters.rules`, next to the one for `0bda:a728`, and run
the script again.

Only one process can hold an adapter. While `asha_probe` uses it, BlueZ loses
it (other Bluetooth devices on it disconnect); on exit it is given back.

## Which adapter

Without `--usb`, every Bluetooth controller on USB is a candidate: external
(removable) ones first, then built-in. They are tried one at a time, and one
is skipped when it can't be opened, its init hangs, or it has no bond for the
aid. When none is usable, `asha_probe` waits and rescans every second, so a dongle
can be plugged in later. A skipped adapter is tried again after a replug.
With `--pair` no bond is needed, so the first adapter that comes up is used;
pick another one with `--usb`.
Controllers on UART/SDIO (some laptops, ARM boards) are not reachable.

## Bond (one-time setup)

The aid keys a bond to the adapter's address, so each adapter needs its own bond
(the aid keeps several). Make one per adapter you want to use, either way below.

### Pair with `asha_probe`

1. Put the aid into pairing mode, usually by restarting it (into the charger and
   out, or off and on).
2. Run with `--pair` and a source:

   ```sh
   ./build/asha_probe --pair --tone 1000
   ```

   It scans actively and lists connectable devices as they show up, numbered,
   with address, RSSI and name; ASHA devices are marked `<- ASHA`. Type the
   number + Enter. Pairing is LE Secure Connections, Just Works (the aid has no
   input). Once paired, it goes on to stream as usual.

If the picked aid is already bonded with this adapter, the stored bond is used
and no new pairing happens.

### Or import a BlueZ bond

A bond made by BlueZ works only **with the same adapter**.

1. Pair the aid with BlueZ on this adapter (`bluetoothctl`, then `pair <aid address>`).
   The aid must be in pairing mode.
2. Copy the bond file (readable by root only):

   ```sh
   mkdir -p bond
   sudo cat /var/lib/bluetooth/<adapter>/<aid>/info > bond/info
   ```

3. Import it once. `--peer` gives the aid's address, since a copied file no
   longer sits in a directory named after it:

   ```sh
   ./build/asha_probe --bond bond/info --peer <aid> --tone 1000
   ```

   A copied file no longer tells which adapter it belongs to: the bond goes into
   the first adapter that comes up. With several adapters, pick it with `--usb`.
   Read in place (`--bond /var/lib/bluetooth/<adapter>/<aid>/info`, as root),
   it is imported only into that adapter.

### Stored bonds

The bond is now stored in `~/.local/state/oticon-connect/btstack_<adapter>.tlv`
(one file per adapter, mode 0600); later runs need neither `--pair` nor `--bond`.

`bond/info` and the TLV file contain the link keys: keep them private.

## Usage

```sh
./build/asha_probe --pipewire        # play into the "Oticon ASHA" output device
./build/asha_probe --pipewire --avoid-wifi 6  # avoid a 2.4 GHz Wi-Fi channel
./build/asha_probe --tone 1000       # 1 kHz sine until Ctrl+C
./build/asha_probe -a assets/tone_1k.g722
```

With `--pipewire`, pick **Oticon ASHA** as the output in your desktop's sound
settings, pavucontrol, or the browser, or play to it directly:

```sh
pw-play --target oticon_asha music.wav
```

PipeWire converts whatever is played to 16 kHz mono.

| Option | Meaning |
|---|---|
| `-p`, `--pipewire` | audio from the PipeWire sink "Oticon ASHA" |
| `-t`, `--tone HZ` | endless sine, 1..7999 Hz |
| `-a`, `--audio PATH` | raw G.722 file, 160-byte frames (default `assets/tone_1k.g722`) |
| `-v`, `--volume N` | ASHA volume −128 (mute) .. 0 (max), default −50 |
| `-r`, `--pair` | pair with an aid in pairing mode, chosen from a list on stdin; not with `--bond` |
| `-b`, `--bond PATH` | import a BlueZ bond file |
| `-P`, `--peer ADDR` | aid identity address: picks a stored bond, or names the aid of a copied `--bond` file |
| `-u`, `--usb PATH` | use only the adapter at these USB ports (`5`, or `1-2.3` as in `/sys/bus/usb/devices`); default: any Bluetooth adapter on USB, see [Which adapter](#which-adapter). The ports are printed at start |
| `-l`, `--log PATH` | HCI trace, default `app.btsnoop` |
| `-W`, `--avoid-wifi N[,N...]` | mark BLE data frequencies overlapping these 2.4 GHz Wi-Fi channels (1–13) as bad; at least 10 of 37 BLE channels must remain |

`--avoid-wifi` changes the controller's LE channel classification. For example,
`--avoid-wifi 6` excludes BLE data channels whose centers overlap the 22 MHz
around Wi-Fi channel 6. Use the active 2.4 GHz channel, not every access point
visible in a scan. Wi-Fi on 5 GHz does not need this option. The controller may
reject the command; startup prints `BLE channel classification: status 0x00`
when it accepted it.

When the link drops (aid out of range, put into the charger), `asha_probe`
scans and reconnects on its own, retrying after 1, 2, 4 … up to 30 s; the delay
goes back to 1 s once streaming resumes. A channel which remains congested for
10 seconds is treated as wedged and reconnected instead of dropping audio
forever.

With `--pipewire`, 30 seconds without a non-silent sample sends ASHA Stop and
changes the BLE connection to a low-duty-cycle interval. The encrypted link and
the virtual sink stay available; playback automatically restores the 20 ms
interval, resets the G.722 stream and sends ASHA Start. This avoids continuously
transmitting 50 encoded-silence packets per second while nothing is playing.

Ctrl+C stops cleanly: ASHA Stop, disconnect, power off. A second Ctrl+C exits
at once; the dongle may then need replugging.

The output reports, every second, frames `sent` and `dropped` (the radio link
fell behind), and every 5 s the PipeWire ring state (`underruns` = nothing to
play while playing).

### HCI trace

Every run writes all HCI traffic to `app.btsnoop` (Linux monitor format), viewable
with `btmon -r app.btsnoop` or Wireshark. **It contains the link key** (LE Start
Encryption): don't share it.

## Tools

- `g722_gen out.g722 [freq_hz=1000] [seconds=15] [amplitude=0.15]`: sine to a raw
  G.722 file, for `-a`.
- `pw_capture out.raw [seconds=10]`: the PipeWire sink without Bluetooth, written
  to raw PCM; play it with `ffplay -f s16le -ar 16000 -ch_layout mono out.raw`.

## How it works

```
apps ─► PipeWire ─► PwSink ─► SampleRing ─► PipeWireSource ─┐   G.722, 160 B / 20 ms
tone / file ────────────────────────────────────────────────┤
                                                            ▼
        AudioStream (20 ms clock, SDU queue) ─► L2CAP CoC ─► aid
        AshaSession (ASHA: characteristics, Start/Stop, AudioStatus)
        BleLink (scan, RPA, connect, encryption, GATT, CoC)
        BTstack ─► libusb ─► dongle
```

1. **BleLink:** passive scan; an advertisement whose resolvable private address
   matches the bond's IRK is the aid. Stop scanning, connect, wait 300 ms,
   re-encrypt with the stored LTK, read the GATT Database Hash. With `--pair`
   the scan is active and `DevicePicker` lists advertisers; the picked one is
   connected and paired (SC Just Works), the new bond goes to the TLV.
   On a drop, `AshaSession` calls `reconnect()` with exponential backoff.
2. **AshaSession:** find service `0xFDF0` and its characteristics, read
   ReadOnlyProperties and the audio PSM, subscribe to AudioStatus, set a 20 ms
   connection interval, open the L2CAP credit-based channel, write Start
   (G.722 16 kHz, media). AudioStatus 0 starts the stream.
3. **AudioStream:** every 20 ms takes a frame from the source and queues the SDU
   `[seq][160 bytes G.722]`; sends one per L2CAP can-send event. When the link
   falls behind, the oldest frame is dropped: the aid plays a frame about 89 ms
   after it is due, so an old frame is useless. Sustained congestion reconnects
   the link; the in-flight SDU has dedicated storage so it cannot be overwritten
   while L2CAP still owns it.
4. **PipeWire:** `PwSink` is an `Audio/Sink` stream (S16LE, 16 kHz, mono) whose
   realtime callback fills a lock-free single-producer/single-consumer ring;
   `PipeWireSource` drains it on the 20 ms clock with a small prefill and drops
   backlog above 120 ms.

Dongle and aid quirks found on the way (scan off before connecting, delay before
encryption, Database Hash first) are documented at the top of
`src/ble/ble_link.cc`.

## Source layout

| Path | Contents |
|---|---|
| `src/app/` | `main.cc`: options, signals (signalfd in the run loop), BTstack init, TLV, adapter rotation; `usb_adapters.cc`: USB Bluetooth controller enumeration; `asha_probe.cc`: glue |
| `src/ble/` | `BleLink`, `DevicePicker` (`--pair` list, stdin), BlueZ bond parser, bond store (BTstack device DB), run loop `Timer` |
| `src/asha/` | `AshaSession` (protocol), `AudioStream` (frame clock and queue) |
| `src/audio/` | frame sources, `G722Encoder`, `SampleRing`, `PwSink`, `PipeWireSource` |
| `tools/`, `tests/` | helper programs, ring buffer test |
| `cmake/btstack.cmake` | the BTstack sources built (LE central, CoC, libusb, POSIX) |
| `config/btstack_config.h` | BTstack feature configuration |

## Troubleshooting

- **No "found peer" after "start scan":** the aid stops advertising some time
  after a disconnect. Restart it (into the charger and out). A phone connected
  to the aid may keep it busy.
- **`no stored bond, import one with --bond`:** do the [bond setup](#bond-one-time-setup).
- **The aid is not in the `--pair` list:** it is not in pairing mode; restart it.
- **`!!! new pairing started: bond not used`:** the stored keys don't match this
  aid or adapter; pair again with `--pair` (or BlueZ + `--bond`).
- **Startup hangs after an unclean exit:** the controller was left scanning and
  ignores HCI Reset. Replug the dongle.
- **Dropouts, `dropped` grows in bursts:** frames are lost on air. The usual
  cause is 2.4 GHz Wi-Fi next to the dongle (check `nmcli dev wifi`); use 5 GHz
  or Ethernet, or move the dongle away on a USB extension cable.
- **Unplugging the dongle while running** aborts the program (`SIGABRT` in
  BTstack's libusb transport) or leaves it hanging; press Ctrl+C twice.

## Limitations

- One aid; no binaural (left + right) streaming.
- Volume only at start (`--volume`).
- Only Realtek dongles get their chipset driver; BTstack does not upload firmware
  for `0bda:a728`, it runs what the kernel loaded.

## License

MIT for the code in this repository, see [LICENSE](LICENSE).

BTstack is **free for non-commercial use only** (BlueKitchen license). Since
`asha_probe` links it, a build of this project may not be used commercially
without a commercial BTstack license. spandsp and libusb are LGPL-2.1,
PipeWire is MIT.
