#include <pipewire/pipewire.h>

#include <cstdio>
#include <stdexcept>
#include <string>

#include "audio/pw_sink.h"

// Replace the PipeWire C boundary so every acquisition failure is deterministic
// and no server, virtual sink, or realtime thread is needed.
struct pw_thread_loop {};
struct pw_stream {};

namespace {
enum class Failure { None, Loop, Stream, Connect, Start };
Failure failure;
std::string events;
pw_thread_loop loop;
pw_stream stream;
pw_properties properties{};
const pw_stream_events* callbacks;
void* callback_data;
bool valid;
}  // namespace

extern "C" {
void pw_init(int*, char**[]) {
  events += 'I';
}
void pw_deinit() {
  events += 'D';
}

pw_thread_loop* pw_thread_loop_new(const char*, const spa_dict*) {
  events += 'L';
  return failure == Failure::Loop ? nullptr : &loop;
}
pw_loop* pw_thread_loop_get_loop(pw_thread_loop*) {
  return nullptr;
}
void pw_thread_loop_destroy(pw_thread_loop*) {
  events += 'l';
}

pw_properties* pw_properties_new(const char*, ...) {
  return &properties;
}
pw_stream* pw_stream_new_simple(
    pw_loop*, const char*, pw_properties*, const pw_stream_events* listener, void* data) {
  events += 'S';
  callbacks = listener;
  callback_data = data;
  return failure == Failure::Stream ? nullptr : &stream;
}
void pw_stream_destroy(pw_stream*) {
  events += 's';
  if (failure == Failure::None)
    callbacks->process(callback_data);  // RT processing can outlive the main loop
}
int pw_stream_connect(
    pw_stream*, spa_direction, uint32_t, pw_stream_flags, const spa_pod**, uint32_t) {
  events += 'C';
  return failure == Failure::Connect ? -1 : 0;
}
int pw_thread_loop_start(pw_thread_loop*) {
  events += 'T';
  if (failure == Failure::Start)
    return -1;
  callbacks->process(callback_data);
  return 0;
}
void pw_thread_loop_stop(pw_thread_loop*) {
  events += 't';
  callbacks->process(callback_data);  // a final callback before the thread joins
}
pw_buffer* pw_stream_dequeue_buffer(pw_stream* value) {
  valid = valid && value == &stream;
  return nullptr;
}
int pw_stream_queue_buffer(pw_stream*, pw_buffer*) {
  return 0;
}
}  // extern "C"

int main() {
  const struct {
    Failure failure;
    const char* expected;
  } cases[] = {
      {Failure::Loop, "ILD"},       {Failure::Stream, "ILSlD"},   {Failure::Connect, "ILSCslD"},
      {Failure::Start, "ILSCTslD"}, {Failure::None, "ILSCTtslD"},
  };
  for (const auto& test : cases) {
    failure = test.failure;
    events.clear();
    valid = true;
    bool threw = false;
    try {
      PwSink sink;
    } catch (const std::runtime_error&) {
      threw = true;
    }
    if (events != test.expected || !valid || threw != (failure != Failure::None)) {
      std::fprintf(stderr, "lifetime: got %s, expected %s\n", events.c_str(), test.expected);
      return 1;
    }
  }
  std::puts("OK");
}
