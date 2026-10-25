#ifndef BLE_TIMER_H_
#define BLE_TIMER_H_

#include <cstdint>
#include <functional>
#include <utility>

#include "btstack_run_loop.h"

// One-shot timer of the BTstack run loop with a C++ callback.
// The run loop links the timer into its list by address, so a Timer must not
// move while armed: it is non-copyable and lives as a class member.
class Timer {
public:
  explicit Timer(std::function<void()> fn) : fn_(std::move(fn)) {
    btstack_run_loop_set_timer_handler(&ts_, &Timer::trampoline);
    btstack_run_loop_set_timer_context(&ts_, this);
  }
  ~Timer() {
    cancel();
  }
  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;

  // (Re)arm: fires once after `ms`
  void start(uint32_t ms) {
    cancel();
    btstack_run_loop_set_timer(&ts_, ms);
    btstack_run_loop_add_timer(&ts_);
  }

  // Safe to call when not armed
  void cancel() {
    btstack_run_loop_remove_timer(&ts_);
  }

private:
  static void trampoline(btstack_timer_source_t* ts) {
    static_cast<Timer*>(btstack_run_loop_get_timer_context(ts))->fn_();
  }

  btstack_timer_source_t ts_{};
  std::function<void()> fn_;
};

#endif  // BLE_TIMER_H_
