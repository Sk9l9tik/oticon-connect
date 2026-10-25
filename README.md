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

Works: connect with an existing bond, ASHA setup, streaming from all three
sources, clean shutdown on Ctrl+C. One aid only, no binaural streaming yet.
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

BTstack takes the dongle over from the kernel driver. To run without root,
give your user access to it with a udev rule (replace the IDs with your dongle's):

```sh
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0bda", ATTR{idProduct}=="a728", MODE="0660", TAG+="uaccess"' \
  | sudo tee /etc/udev/rules.d/70-oticon-asha-dongle.rules
sudo udevadm control --reload && sudo udevadm trigger
```

Only one process can hold the dongle.

## Bond (one-time setup)

`asha_probe` does not pair yet: it reuses a bond made by BlueZ **with the same
dongle**. The aid keys the bond to the dongle's address, so a bond made with
another adapter does not work.

1. Pair the aid with BlueZ on this dongle (`bluetoothctl`, then `pair <aid address>`).
   The aid must be in pairing mode, usually right after it is powered on.
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

The bond is now stored in `~/.local/state/oticon-connect/btstack_<adapter>.tlv`
(one file per adapter, mode 0600); later runs need no `--bond`.

`bond/info` and the TLV file contain the link keys: keep them private.

## Usage

```sh
./build/asha_probe --pipewire        # play into the "Oticon ASHA" output device
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
| `-b`, `--bond PATH` | import a BlueZ bond file |
| `-P`, `--peer ADDR` | aid identity address: picks a stored bond, or names the aid of a copied `--bond` file |
| `-u`, `--usb PATH` | use the adapter at these USB ports (`5`, or `1-2.3` as in `/sys/bus/usb/devices`); default: the first Bluetooth adapter. The ports are printed at start |
| `-l`, `--log PATH` | HCI trace, default `app.btsnoop` |

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
   re-encrypt with the stored LTK, read the GATT Database Hash.
2. **AshaSession:** find service `0xFDF0` and its characteristics, read
   ReadOnlyProperties and the audio PSM, subscribe to AudioStatus, set a 20 ms
   connection interval, open the L2CAP credit-based channel, write Start
   (G.722 16 kHz, media). AudioStatus 0 starts the stream.
3. **AudioStream:** every 20 ms takes a frame from the source and queues the SDU
   `[seq][160 bytes G.722]`; sends one per L2CAP can-send event. When the link
   falls behind, the oldest frame is dropped: the aid plays a frame about 89 ms
   after it is due, so an old frame is useless.
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
| `src/app/` | `main.cc`: options, signals (signalfd in the run loop), BTstack init, TLV; `asha_probe.cc`: glue |
| `src/ble/` | `BleLink`, BlueZ bond parser, bond store (BTstack device DB), run loop `Timer` |
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
- **`!!! new pairing started: bond not used`:** the stored keys don't match this
  aid or adapter; re-pair with BlueZ and import again.
- **Startup hangs after an unclean exit:** the controller was left scanning and
  ignores HCI Reset. Replug the dongle.
- **Dropouts, `dropped` grows in bursts:** frames are lost on air. The usual
  cause is 2.4 GHz Wi-Fi next to the dongle (check `nmcli dev wifi`); use 5 GHz
  or Ethernet, or move the dongle away on a USB extension cable.
- **Unplugging the dongle while running** aborts the program (`SIGABRT` in
  BTstack's libusb transport) or leaves it hanging; press Ctrl+C twice.

## Limitations

- One aid; no binaural (left + right) streaming.
- No pairing of its own: needs a bond made by BlueZ with the same dongle.
- Volume only at start (`--volume`).
- No reconnect after the link drops.
- Only Realtek dongles get their chipset driver; BTstack does not upload firmware
  for `0bda:a728`, it runs what the kernel loaded.

## License

MIT for the code in this repository, see [LICENSE](LICENSE).

BTstack is **free for non-commercial use only** (BlueKitchen license). Since
`asha_probe` links it, a build of this project may not be used commercially
without a commercial BTstack license. spandsp and libusb are LGPL-2.1,
PipeWire is MIT.
