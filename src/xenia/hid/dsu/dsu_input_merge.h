// Copyright 2026 Xenia Canary. Released under the BSD license.

#ifndef XENIA_HID_DSU_DSU_INPUT_MERGE_H_
#define XENIA_HID_DSU_DSU_INPUT_MERGE_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "xenia/hid/input.h"

namespace xe::hid::dsu {

inline X_INPUT_GAMEPAD MapRightStickMotion(const std::array<float, 3>& gyro,
                                           float sensitivity_x = 1.0f,
                                           float sensitivity_y = 1.0f,
                                           float deadzone = 0.5f,
                                           bool invert_x = false,
                                           bool invert_y = false) {
  deadzone = std::isfinite(deadzone) ? std::clamp(deadzone, 0.0f, 10.0f) : 0.5f;
  const auto axis = [deadzone](float rate, float sensitivity,
                               bool inverted) -> int16_t {
    if (!std::isfinite(rate) || std::abs(rate) <= deadzone) {
      return 0;
    }
    sensitivity =
        std::isfinite(sensitivity) ? std::clamp(sensitivity, 0.1f, 5.0f) : 1.0f;
    const float value = rate / 180.0f * sensitivity * (inverted ? -1.0f : 1.0f);
    return int16_t(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767));
  };
  X_INPUT_GAMEPAD motion{};
  motion.thumb_rx = axis(-gyro[1], sensitivity_x, invert_x);
  motion.thumb_ry = axis(gyro[0], sensitivity_y, invert_y);
  return motion;
}

inline void AddRightStickMotion(X_INPUT_GAMEPAD& gamepad,
                                const X_INPUT_GAMEPAD& motion) {
  gamepad.thumb_rx = int16_t(
      std::clamp(int(int16_t(gamepad.thumb_rx)) + int(int16_t(motion.thumb_rx)),
                 -32768, 32767));
  gamepad.thumb_ry = int16_t(
      std::clamp(int(int16_t(gamepad.thumb_ry)) + int(int16_t(motion.thumb_ry)),
                 -32768, 32767));
}

inline void MergeGamepads(X_INPUT_GAMEPAD& primary,
                          const X_INPUT_GAMEPAD& remote) {
  primary.buttons = uint16_t(primary.buttons) | uint16_t(remote.buttons);
  primary.left_trigger = std::max(primary.left_trigger, remote.left_trigger);
  primary.right_trigger = std::max(primary.right_trigger, remote.right_trigger);
  const auto axis = [](int16_t local, int16_t network) {
    return std::abs(int(network)) > std::abs(int(local)) ? network : local;
  };
  primary.thumb_lx = axis(primary.thumb_lx, remote.thumb_lx);
  primary.thumb_ly = axis(primary.thumb_ly, remote.thumb_ly);
  primary.thumb_rx = axis(primary.thumb_rx, remote.thumb_rx);
  primary.thumb_ry = axis(primary.thumb_ry, remote.thumb_ry);
}

// Multiple sources have independent counters. Expose one counter that changes
// only with the final gamepad state or a reconnection, including remote expiry.
class MergedInputState {
 public:
  void Update(bool connected, X_INPUT_STATE& state) {
    if (connected && (!connected_ || std::memcmp(&gamepad_, &state.gamepad,
                                                 sizeof(gamepad_)))) {
      ++packet_number_;
    }
    connected_ = connected;
    gamepad_ = connected ? state.gamepad : X_INPUT_GAMEPAD{};
    state.packet_number = packet_number_;
  }

 private:
  X_INPUT_GAMEPAD gamepad_{};
  uint32_t packet_number_ = 0;
  bool connected_ = false;
};

}  // namespace xe::hid::dsu

#endif  // XENIA_HID_DSU_DSU_INPUT_MERGE_H_
