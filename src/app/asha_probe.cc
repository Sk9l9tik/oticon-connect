// asha_probe: glue between main.cc and the ASHA client (asha_app.h).
//
// Layers, each in its own file:
//   ble_link      BTstack: scan, RPA, connect, encryption, GATT, CoC; dongle quirks
//   asha_session  ASHA protocol: characteristics, ROP, PSM, Start/Stop, AudioStatus
//   audio_stream  20 ms frame clock, SDU ring, drop; FrameSource = file or tone
//   pipewire_source, pw_sink  FrameSource from the PipeWire sink "Oticon ASHA"
//
// BTstack is single-threaded and event-driven: every call returns at once and
// its result arrives later as an event. Everything here runs on that thread.

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <utility>

#include "app/asha_app.h"

#include "asha/asha_session.h"

#include "audio/frame_source.h"
#include "audio/pipewire_source.h"

#include "ble/bond_parser.h"

static const double TONE_AMPLITUDE = 0.15;

// Live for the whole program: BTstack keeps pointers into them
static std::unique_ptr<FrameSource> source;
static std::unique_ptr<AshaSession> session;

// --bond: the file gives the keys; the identity address comes from --peer or
// the BlueZ directory name. No --bond: a stored bond, of --peer if given.
static bool make_bond_spec(const AppConfig& cfg, BondSpec& spec) {
  if (cfg.pair) {
    spec.pair = true;
    return true;
  }
  bd_addr_t peer;
  bool have_peer = false;
  if (cfg.peer) {
    if (sscanf_bd_addr(cfg.peer, peer) != 1) {
      fprintf(stderr, "bad peer address '%s'\n", cfg.peer);
      return false;
    }
    have_peer = true;
  }

  if (!cfg.bond) {
    if (have_peer)
      spec.peer = std::to_array(peer);
    return true;
  }

  Bond bond;
  bool loaded = false;
  try {
    loaded = load_bond(cfg.bond, bond);
  } catch (const std::exception& e) {
    fprintf(stderr, "bad bond file %s: %s\n", cfg.bond, e.what());
  }
  if (!loaded) {
    fprintf(stderr, "can't load bond from %s\n", cfg.bond);
    return false;
  }
  if (have_peer) {
    memcpy(bond.identity, peer, 6);
  } else if (!identity_from_path(cfg.bond, bond.identity)) {
    fprintf(stderr, "%s: no address in the path, give it with --peer\n", cfg.bond);
    return false;
  }
  bd_addr_t adapter;
  if (adapter_from_path(cfg.bond, adapter))
    spec.adapter = std::to_array(adapter);
  spec.import = bond;
  return true;
}

bool asha_start(const AppConfig& cfg) {
  BondSpec bond;
  if (!make_bond_spec(cfg, bond))
    return false;

  if (cfg.pipewire) {
    try {
      source = std::make_unique<PipeWireSource>();
    } catch (const std::exception& e) {
      fprintf(stderr, "PipeWire: %s\n", e.what());
      return false;
    }
  } else if (cfg.tone_hz > 0) {
    source = std::make_unique<ToneSource>(cfg.tone_hz, TONE_AMPLITUDE);
  } else {
    auto file = std::make_unique<G722FileSource>();
    if (!file->load(cfg.audio)) {
      fprintf(stderr, "can't load G.722 audio from %s\n", cfg.audio);
      return false;
    }
    source = std::move(file);
  }

  session = std::make_unique<AshaSession>(
      std::move(bond), *source, cfg.volume, cfg.on_finished, cfg.on_no_bond);
  session->start();
  return true;
}

// Signals are read inside the run loop, which main.cc starts only after a
// successful asha_start, so the session always exists here
void asha_request_stop() {
  if (session)
    session->stop();
}
