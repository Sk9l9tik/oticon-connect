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
    armed_ = true;
  }

  // Safe to call when not armed, also before the run loop exists (static
  // Timers are destroyed on exit after a bad option)
  void cancel() {
    if (!armed_)
      return;
    btstack_run_loop_remove_timer(&ts_);
    armed_ = false;
  }

private:
  static void trampoline(btstack_timer_source_t* ts) {
    Timer* t = static_cast<Timer*>(btstack_run_loop_get_timer_context(ts));
    t->armed_ = false;  // the run loop has already unlinked it
    t->fn_();           // may start() it again
  }

  btstack_timer_source_t ts_{};
  bool armed_ = false;
  std::function<void()> fn_;
};

#endif  // BLE_TIMER_H_
