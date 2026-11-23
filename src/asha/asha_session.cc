#include <algorithm>
#include <cstdio>
#include <utility>

#include "asha/asha_session.h"

static const uint16_t ASHA_SERVICE = 0xFDF0;

// ROP     ReadOnlyProperties   read
// ACP     AudioControlPoint    write: Start / Stop
// STATUS  AudioStatus          notify: result of the last ACP command
// VOLUME  Volume               write without response
// PSM     LE_PSM_OUT           read: L2CAP PSM of the audio channel
static const char* UUID_ROP = "6333651e-c481-4a3e-9169-7c902aad37bb";
static const char* UUID_ACP = "f0d4de7e-4a88-476c-9d9f-1937b0996cc0";
static const char* UUID_STATUS = "38663f1a-e711-4cac-b641-326b56404837";
static const char* UUID_VOLUME = "00e4ca9e-ab14-41e4-8823-f9e70c7e91df";
static const char* UUID_PSM = "2d410339-82b6-42aa-b34e-e2e01df8cc1a";

static const uint16_t INTERVAL_20MS = 16;  // x 1.25 ms: one audio frame per connection event
static const uint16_t IDLE_INTERVAL = 80;  // 100 ms
static const uint16_t IDLE_LATENCY = 4;    // aid may wake only every 500 ms
static const uint32_t IDLE_POLL_MS = 250;
static const uint32_t IDLE_STOP_TIMEOUT_MS = 2000;
static const uint32_t STOP_TIMEOUT_MS = 2000;

// ReadOnlyProperties, 17 bytes, layout in CPP_GUIDE.md
static void print_rop(std::span<const uint8_t> v) {
  if (v.size() < 17) {
    printf("ROP too short: %zu bytes\n", v.size());
    return;
  }
  printf(
      "ROP: version %u, %s, binaural %u, hisync ", v[0], (v[1] & 1) ? "right" : "left",
      (v[1] >> 1) & 1);
  for (int i = 0; i < 8; i++)
    printf("%02x", v[2 + i]);
  printf(
      ", feature_map 0x%02x, delay %u ms, codecs 0x%04x\n", v[10], v[11] | (v[12] << 8),
      v[15] | (v[16] << 8));
}

AshaSession::AshaSession(
    BondSpec bond, FrameSource& source, int volume, std::function<void()> on_finished,
    std::function<void()> on_no_bond)
    : link_(std::move(bond), *this),
      stream_(
          source, link_, [this] { stop(); }, [this] { pause_stream(); },
          [this] { recover_stream(); }),
      on_finished_(std::move(on_finished)), on_no_bond_(std::move(on_no_bond)), stop_timer_([this] {
        printf("no answer to Stop\n");
        link_.shutdown();
      }),
      volume_((uint8_t)(int8_t)volume) {
}

void AshaSession::start() {
  link_.start();
}

// Streaming -> Stop first, the link goes down after it (or after the timeout);
// otherwise there is nothing to undo on the aid: shut the link down now
void AshaSession::stop() {
  if (stopping_)
    return;
  stopping_ = true;
  retry_timer_.cancel();  // Ctrl+C during the backoff
  idle_timer_.cancel();
  bool streaming = state_ == State::Streaming;
  state_ = State::Stopping;
  if (!streaming) {
    link_.shutdown();
    return;
  }
  stream_.stop();
  static const uint8_t acp_stop[] = {0x02};
  stop_timer_.start(STOP_TIMEOUT_MS);
  link_.write(acp_, acp_stop, [this](bool ok) {
    printf(ok ? "Stop written\n" : "Stop failed\n");
    stop_timer_.cancel();
    link_.shutdown();
  });
}

void AshaSession::fail_setup(const char* what) {
  printf("%s failed\n", what);
  stop();
}

// ============================================================================
// Setup chain
// ============================================================================

void AshaSession::on_link_ready() {
  if (stopping_)
    return;
  link_.discover_characteristics(
      ASHA_SERVICE, [this](bool ok, const std::vector<GattChar>& chars) { on_chars(ok, chars); });
}

// Handles differ per firmware, so the characteristics are found by UUID
void AshaSession::on_chars(bool ok, const std::vector<GattChar>& chars) {
  if (!ok)
    return fail_setup("ASHA service discovery");
  for (const auto& c : chars) {
    if (c.uuid == UUID_ROP)
      rop_ = c.value_handle;
    else if (c.uuid == UUID_ACP)
      acp_ = c.value_handle;
    else if (c.uuid == UUID_STATUS)
      status_ = c.value_handle;
    else if (c.uuid == UUID_VOLUME)
      volume_chr_ = c.value_handle;
    else if (c.uuid == UUID_PSM)
      psm_chr_ = c.value_handle;
  }
  if (!rop_ || !acp_ || !status_ || !psm_chr_)
    return fail_setup("ASHA characteristics lookup");
  read_rop();
}

void AshaSession::read_rop() {
  link_.read(rop_, [this](bool ok, std::span<const uint8_t> v) {
    if (!ok)
      return fail_setup("ReadOnlyProperties read");
    print_rop(v);
    read_psm();
  });
}

void AshaSession::read_psm() {
  link_.read(psm_chr_, [this](bool ok, std::span<const uint8_t> v) {
    if (!ok || v.size() < 2)
      return fail_setup("LE_PSM_OUT read");
    psm_ = v[0] | (v[1] << 8);
    printf("LE_PSM_OUT 0x%04x\n", psm_);
    subscribe_status();
  });
}

void AshaSession::subscribe_status() {
  link_.subscribe(status_, [this](bool ok) {
    if (!ok)
      return fail_setup("AudioStatus subscribe");
    printf("subscribed to AudioStatus\n");
    update_interval();
  });
}

void AshaSession::update_interval() {
  link_.set_connection_parameters(INTERVAL_20MS, 0, [this](bool ok) {
    // a rejected update is not fatal: the stream may still fit the old interval
    if (!ok)
      printf("warning: connection interval not changed\n");
    open_coc();
  });
}

// A PipeWire source has been silent for long enough to stop the radio stream.
// Keep the encrypted GATT/CoC connection: ASHA explicitly permits a slower,
// higher-latency connection after Stop and requires a fresh Start on resume.
void AshaSession::pause_stream() {
  if (stopping_ || state_ != State::Streaming)
    return;
  stream_.stop();
  state_ = State::Pausing;
  idle_timer_.start(IDLE_STOP_TIMEOUT_MS);
  static const uint8_t acp_stop[] = {0x02};
  link_.write(acp_, acp_stop, [this](bool ok) {
    if (!ok && !stopping_)
      recover_stream();
  });
}

void AshaSession::enter_idle() {
  if (stopping_ || state_ != State::Pausing)
    return;
  idle_timer_.cancel();
  state_ = State::Paused;
  link_.set_connection_parameters(IDLE_INTERVAL, IDLE_LATENCY, [this](bool ok) {
    if (!ok)
      printf("warning: low-power connection parameters rejected\n");
    if (!stopping_ && state_ == State::Paused) {
      printf("stream idle; waiting for PipeWire audio\n");
      idle_timer_.start(IDLE_POLL_MS);
    }
  });
}

void AshaSession::on_idle_timer() {
  if (state_ == State::Pausing) {
    printf("no answer to idle Stop; reconnecting\n");
    recover_stream();
  } else {
    poll_resume();
  }
}

void AshaSession::poll_resume() {
  if (stopping_ || state_ != State::Paused)
    return;
  if (!stream_.resume_requested()) {
    idle_timer_.start(IDLE_POLL_MS);
    return;
  }
  printf("PipeWire audio resumed\n");
  state_ = State::Setup;
  link_.set_connection_parameters(INTERVAL_20MS, 0, [this](bool ok) {
    if (!ok)
      printf("warning: streaming connection interval not restored\n");
    if (!stopping_ && state_ == State::Setup)
      write_start();
  });
}

void AshaSession::recover_stream() {
  if (stopping_)
    return;
  idle_timer_.cancel();
  if (state_ == State::Streaming)
    stream_.stop();
  link_.recover();
}

void AshaSession::open_coc() {
  link_.open_coc(psm_, [this](bool ok, uint16_t) {
    if (!ok)
      return fail_setup("CoC open");
    write_start();
  });
}

// Start: codec G.722 16 kHz, audio type media, volume, other side not connected
void AshaSession::write_start() {
  const uint8_t acp_start[] = {0x01 /*Start*/, 0x01 /*G.722*/, 0x03 /*media*/, volume_, 0x00};
  state_ = State::Starting;
  link_.write(acp_, acp_start, [this](bool ok) {
    if (!ok)
      return fail_setup("Start write");
    // AudioStatus may arrive before this response: on_notification handles it
    printf("Start written, waiting for AudioStatus\n");
  });
}

// ============================================================================
// Events from the link
// ============================================================================

// AudioStatus: result of the last ACP command, 0 = OK
void AshaSession::on_notification(uint16_t value_handle, std::span<const uint8_t> value) {
  if (value_handle != status_ || value.empty())
    return;
  int8_t audio_status = (int8_t)value[0];
  printf("AudioStatus %d\n", audio_status);
  if (stopping_)
    return;
  if (state_ == State::Starting) {
    if (audio_status != 0)
      return fail_setup("Start (aid rejected it)");
    state_ = State::Streaming;
    backoff_ms_ = 1000;
    stream_.start();
  } else if (state_ == State::Pausing) {
    if (audio_status != 0) {
      printf("idle Stop rejected; reconnecting\n");
      recover_stream();
      return;
    }
    enter_idle();
  }
}

void AshaSession::on_coc_can_send() {
  stream_.on_can_send();
}

// Final: whatever the state, the link is gone
// Nothing set up yet on this adapter: the app tries the next one
void AshaSession::on_no_bond() {
  if (!stopping_ && on_no_bond_)
    on_no_bond_();
}

void AshaSession::on_link_down() {
  if (state_ == State::Streaming)
    stream_.stop();  // the aid dropped the link mid-stream
  state_ = State::Stopping;
  stopping_ = true;
  stop_timer_.cancel();
  idle_timer_.cancel();
  retry_timer_.cancel();
  if (on_finished_)
    on_finished_();
}

void AshaSession::on_link_lost(uint8_t reason) {
  if (state_ == State::Streaming)
    stream_.stop();  // the aid dropped the link mid-stream

  if (stopping_) {     // stop() already running: finish instead of retrying
    link_.shutdown();  // state Off -> go_down -> on_link_down -> on_finished
    return;
  }

  stop_timer_.cancel();
  idle_timer_.cancel();
  state_ = State::Setup;
  psm_ = rop_ = acp_ = status_ = volume_chr_ = psm_chr_ = 0;
  printf("link lost 0x%02x, retry in %u ms\n", reason, backoff_ms_);
  retry_timer_.start(backoff_ms_);
  backoff_ms_ = std::min(backoff_ms_ * 2, 30000u);
}
