// Copyright 2026 Xenia Canary. Released under the BSD license.

#ifndef XENIA_HID_DSU_DSU_KEYSTROKES_H_
#define XENIA_HID_DSU_DSU_KEYSTROKES_H_

#include <cstdint>
#include <deque>

#include "xenia/hid/input.h"

namespace xe::hid::dsu {

// Preserve short remote presses between guest polls, with a bounded queue.
// Overflow rebuilds the transitions from what the guest has actually received.
class Keystrokes {
 public:
  static constexpr size_t kCapacity = 64;
  void Observe(const X_INPUT_GAMEPAD& gamepad, uint64_t now_ms);
  bool Pop(uint8_t local_player, uint64_t now_ms, X_INPUT_KEYSTROKE* output);
  void Reset() { *this = {}; }
  size_t pending() const { return queue_.size(); }

 private:
  struct Event {
    uint8_t bit;
    bool down;
  };
  void EnqueueChanges(uint64_t before, uint64_t after);
  std::deque<Event> queue_;
  uint64_t observed_ = 0;
  uint64_t delivered_ = 0;
  int repeat_bit_ = -1;
  uint64_t repeat_at_ms_ = 0;
};

}  // namespace xe::hid::dsu

#endif  // XENIA_HID_DSU_DSU_KEYSTROKES_H_
