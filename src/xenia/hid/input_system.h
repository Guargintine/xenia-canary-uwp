/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_INPUT_SYSTEM_H_
#define XENIA_HID_INPUT_SYSTEM_H_

#include <bitset>
#include <memory>
#include <vector>
#include "xenia/base/mutex.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/portal/portal.h"
#include "xenia/xbox.h"

#if XE_PLATFORM_WIN32
#include "xenia/hid/dsu/dsu_input_driver.h"
#include "xenia/hid/dsu/dsu_input_merge.h"
#endif

#if XE_PLATFORM_WINRT
#include "xenia-canary-uwp/guest_input_capture.h"
#endif

namespace xe {
namespace ui {
class Window;
}  // namespace ui
}  // namespace xe

namespace xe {
namespace hid {

class InputSystem {
 public:
  explicit InputSystem(xe::ui::Window* window);
  ~InputSystem();

  xe::ui::Window* window() const { return window_; }

  X_STATUS Setup();

  void AddDriver(std::unique_ptr<InputDriver> driver);

#if XE_PLATFORM_WIN32
  dsu::DSUInputDriver* dsu_driver(uint32_t player) const {
    return player < dsu_drivers_.size() ? dsu_drivers_[player].get() : nullptr;
  }
#endif

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, uint32_t flags,
                    X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke);

  bool GetVibrationCvar();
  void ToggleVibration();

  const std::bitset<XUserMaxUserCount> GetConnectedSlots() const {
    return connected_slots;
  }

  uint32_t GetLastUsedSlot() const { return last_used_slot; }

  Portal* GetPortal() { return portal_.get(); }

  std::unique_lock<xe_unlikely_mutex> lock();

#if XE_PLATFORM_WINRT
  // Guest exports only; callers must hold lock(). Raw host input is unchanged.
  UWP::GuestInputCapture& guest_input_capture() { return guest_input_capture_; }
#endif

 private:
  typedef std::pair<uint16_t, uint16_t> joystick_value;

  const std::string controller_slot_state_change_message[2] = {
      "Controller disconnected from slot {}.",
      "New controller connected to slot {}."};

  void UpdateUsedSlot(InputDriver* driver, uint8_t slot, bool connected);
  void AdjustDeadzoneLevels(const uint8_t slot, X_INPUT_GAMEPAD* gamepad);
  X_INPUT_VIBRATION ModifyVibrationLevel(X_INPUT_VIBRATION* vibration);

  std::vector<InputDriver*> FilterDrivers(uint32_t flags);

  xe::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

#if XE_PLATFORM_WIN32
  std::array<std::unique_ptr<dsu::DSUInputDriver>, XUserMaxUserCount>
      dsu_drivers_;
  std::array<dsu::MergedInputState, XUserMaxUserCount> merged_input_;
#endif

  std::unique_ptr<Portal> portal_;

  std::bitset<XUserMaxUserCount> connected_slots = {};
  std::array<std::pair<joystick_value, joystick_value>, XUserMaxUserCount>
      controllers_max_joystick_value = {};
  uint32_t last_used_slot = 0;

  xe_unlikely_mutex lock_;
#if XE_PLATFORM_WINRT
  UWP::GuestInputCapture guest_input_capture_;
#endif
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_INPUT_SYSTEM_H_
