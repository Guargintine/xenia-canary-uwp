// Copyright 2026 Xenia Canary. Released under the BSD license.

#include "xenia/hid/dsu/dsu_keystrokes.h"

#include <array>
#include <bit>

#include "xenia/ui/virtual_key.h"

namespace xe::hid::dsu {
namespace {
using VK = xe::ui::VirtualKey;
constexpr std::array<VK, 34> kKeys = {VK::kXInputPadDpadUp,
                                      VK::kXInputPadDpadDown,
                                      VK::kXInputPadDpadLeft,
                                      VK::kXInputPadDpadRight,
                                      VK::kXInputPadStart,
                                      VK::kXInputPadBack,
                                      VK::kXInputPadLThumbPress,
                                      VK::kXInputPadRThumbPress,
                                      VK::kXInputPadLShoulder,
                                      VK::kXInputPadRShoulder,
                                      VK::kXInputPadGuide,
                                      VK::kNone,
                                      VK::kXInputPadA,
                                      VK::kXInputPadB,
                                      VK::kXInputPadX,
                                      VK::kXInputPadY,
                                      VK::kXInputPadLTrigger,
                                      VK::kXInputPadRTrigger,
                                      VK::kXInputPadLThumbUp,
                                      VK::kXInputPadLThumbDown,
                                      VK::kXInputPadLThumbRight,
                                      VK::kXInputPadLThumbLeft,
                                      VK::kXInputPadLThumbUpLeft,
                                      VK::kXInputPadLThumbUpRight,
                                      VK::kXInputPadLThumbDownRight,
                                      VK::kXInputPadLThumbDownLeft,
                                      VK::kXInputPadRThumbUp,
                                      VK::kXInputPadRThumbDown,
                                      VK::kXInputPadRThumbRight,
                                      VK::kXInputPadRThumbLeft,
                                      VK::kXInputPadRThumbUpLeft,
                                      VK::kXInputPadRThumbUpRight,
                                      VK::kXInputPadRThumbDownRight,
                                      VK::kXInputPadRThumbDownLeft};

uint64_t KeyBits(const X_INPUT_GAMEPAD& pad) {
  uint64_t bits = uint16_t(pad.buttons) & ~(1u << 11);
  if (pad.left_trigger > X_INPUT_GAMEPAD_TRIGGER_THRESHOLD) {
    bits |= uint64_t{1} << 16;
  }
  if (pad.right_trigger > X_INPUT_GAMEPAD_TRIGGER_THRESHOLD) {
    bits |= uint64_t{1} << 17;
  }
  const auto stick = [&bits](int16_t x, int16_t y, unsigned offset,
                             int16_t deadzone) {
    const bool left = x<-deadzone, right = x> deadzone;
    const bool up = y > deadzone, down = y < -deadzone;
    int direction = -1;
    if (up) {
      direction = left ? 4 : right ? 5 : 0;
    } else if (down) {
      direction = right ? 6 : left ? 7 : 1;
    } else if (right) {
      direction = 2;
    } else if (left) {
      direction = 3;
    }
    if (direction >= 0) {
      bits |= uint64_t{1} << (offset + direction);
    }
  };
  stick(pad.thumb_lx, pad.thumb_ly, 18, X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
  stick(pad.thumb_rx, pad.thumb_ry, 26, X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
  return bits;
}
}  // namespace

void Keystrokes::EnqueueChanges(uint64_t before, uint64_t after) {
  // Key-up must precede key-down when changing stick direction.
  for (bool down : {false, true}) {
    for (uint8_t bit = 0; bit < kKeys.size(); ++bit) {
      const uint64_t mask = uint64_t{1} << bit;
      if (((before ^ after) & mask) && bool(after & mask) == down) {
        queue_.push_back({bit, down});
      }
    }
  }
}

void Keystrokes::Observe(const X_INPUT_GAMEPAD& gamepad, uint64_t now_ms) {
  const uint64_t next = KeyBits(gamepad);
  if (queue_.size() + std::popcount(observed_ ^ next) > kCapacity) {
    queue_.clear();
    EnqueueChanges(delivered_, next);
  } else {
    EnqueueChanges(observed_, next);
  }
  observed_ = next;
  if (repeat_bit_ >= 0 && !(observed_ & (uint64_t{1} << repeat_bit_))) {
    repeat_bit_ = -1;
  }
  (void)now_ms;
}

bool Keystrokes::Pop(uint8_t local_player, uint64_t now_ms,
                     X_INPUT_KEYSTROKE* output) {
  *output = {};
  uint8_t bit;
  uint16_t flags;
  if (!queue_.empty()) {
    const Event event = queue_.front();
    queue_.pop_front();
    bit = event.bit;
    const uint64_t mask = uint64_t{1} << bit;
    if (event.down) {
      delivered_ |= mask;
      repeat_bit_ = bit;
      repeat_at_ms_ = now_ms + 400;
      flags = X_INPUT_KEYSTROKE_KEYDOWN;
    } else {
      delivered_ &= ~mask;
      if (repeat_bit_ == bit) {
        repeat_bit_ = -1;
      }
      flags = X_INPUT_KEYSTROKE_KEYUP;
    }
  } else if (repeat_bit_ >= 0 && now_ms >= repeat_at_ms_ &&
             (observed_ & delivered_ & (uint64_t{1} << repeat_bit_))) {
    bit = uint8_t(repeat_bit_);
    flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
    repeat_at_ms_ = now_ms + 100;
  } else {
    return false;
  }
  output->virtual_key = uint16_t(kKeys[bit]);
  output->user_index = local_player;
  output->flags = flags;
  return true;
}

}  // namespace xe::hid::dsu
