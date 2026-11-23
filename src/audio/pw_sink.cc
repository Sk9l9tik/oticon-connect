#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <stdexcept>

#include "audio/pw_sink.h"

// Only process() is needed; the other callbacks stay nullptr (not called)
static pw_stream_events make_stream_events() {
  pw_stream_events ev{};
  ev.version = PW_VERSION_STREAM_EVENTS;
  ev.process = &PwSink::on_process;
  return ev;
}

static const pw_stream_events stream_events = make_stream_events();

PwSink::PwSink() {
  pw_init(nullptr, nullptr);  // safe to call more than once, paired with pw_deinit

  // 1. own thread running PipeWire's main loop: events and process() run there
  loop_ = pw_thread_loop_new("asha-pw", nullptr);
  if (!loop_)
    fail("pw_thread_loop_new");

  // 2. the stream = our node in the PipeWire graph
  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_CLASS,
      "Audio/Sink",                            // shows up as an output device
      PW_KEY_NODE_NAME, "oticon_asha",         // id for pw-play --target, wpctl
      PW_KEY_NODE_DESCRIPTION, "Oticon ASHA",  // name in pavucontrol / browser
      PW_KEY_NODE_LATENCY, "320/16000",        // ask for a 20 ms quantum = one ASHA frame
      nullptr);
  // takes ownership of props, also on failure
  stream_ = pw_stream_new_simple(
      pw_thread_loop_get_loop(loop_), "Oticon ASHA", props, &stream_events, this);
  if (!stream_)
    fail("pw_stream_new_simple");

  // 3. the only format we accept; PipeWire converts whatever apps play into it
  uint8_t pod_buf[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_S16_LE;
  info.rate = 16000;
  info.channels = 1;
  info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

  // 4. INPUT: the stream consumes audio. Flags:
  //    AUTOCONNECT  - join the graph by itself
  //    MAP_BUFFERS  - buffer memory is mmapped, d.data is ready to read
  //    RT_PROCESS   - process() runs directly in the realtime thread
  int res = pw_stream_connect(
      stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
      static_cast<pw_stream_flags>(
          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
      params, 1);
  if (res < 0)
    fail("pw_stream_connect");

  // 5. go: from here on process() may run at any time
  if (pw_thread_loop_start(loop_) < 0)
    fail("pw_thread_loop_start");
}

void PwSink::on_process(void* userdata) {
  auto* self = static_cast<PwSink*>(userdata);
  pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
  if (!b)
    return;
  spa_data& d = b->buffer->datas[0];
  if (d.data && d.chunk) {
    auto* s = static_cast<const int16_t*>(SPA_PTROFF(d.data, d.chunk->offset, void));
    size_t n = d.chunk->size / sizeof(int16_t);
    constexpr int16_t SIGNAL_THRESHOLD = 8;
    for (size_t i = 0; i < n; ++i) {
      if (s[i] > SIGNAL_THRESHOLD || s[i] < -SIGNAL_THRESHOLD) {
        self->signal_generation_.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
    size_t w = self->ring_.write(s, n);
    if (w < n)
      self->overflows_.fetch_add(n - w, std::memory_order_relaxed);
  }
  pw_stream_queue_buffer(self->stream_, b);
}

PwSink::~PwSink() {
  cleanup();
}

void PwSink::cleanup() {
  if (loop_)
    pw_thread_loop_stop(loop_);  // first: on_process can't run after this
  if (stream_)
    pw_stream_destroy(stream_);
  if (loop_)
    pw_thread_loop_destroy(loop_);
  pw_deinit();
}

// Constructor failed: the destructor won't run, free what exists and throw
void PwSink::fail(const std::string& what) {
  cleanup();
  throw std::runtime_error(what + " failed");
}
