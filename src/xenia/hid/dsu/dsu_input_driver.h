// Copyright 2026 Xenia Canary. Released under the BSD license.

#ifndef XENIA_HID_DSU_DSU_INPUT_DRIVER_H_
#define XENIA_HID_DSU_DSU_INPUT_DRIVER_H_

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "xenia/hid/dsu/dsu_protocol.h"
#include "xenia/hid/input_driver.h"

namespace xe::hid::dsu {

struct Configuration {
  bool enabled = false;
  std::string host = "127.0.0.1";
  uint32_t port = 26760;
  uint32_t remote_slot = 0;
  uint32_t local_player = 0;
  // Companion input uses alternate face-button masks and a registration offset.
  bool companion_extensions = true;
  bool motion_right_stick = false;
  float motion_sensitivity_x = 1.0f;
  float motion_sensitivity_y = 1.0f;
  float motion_deadzone = 0.5f;
  bool motion_invert_x = false;
  bool motion_invert_y = false;
  bool operator==(const Configuration&) const = default;
};

struct ConnectionStatus {
  bool enabled = false;
  bool connected = false;
  std::string host;
  uint32_t port = 0;
  uint32_t remote_slot = 0;
  uint32_t local_player = 0;
  uint64_t generation = 0;
  std::string error;
};

class DSUInputDriver final : public InputDriver {
 public:
  using ExtensionCallback =
      std::function<void(uint32_t type, const std::vector<uint8_t>& payload)>;

  explicit DSUInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~DSUInputDriver() override;

  static Configuration ReadConfiguration(uint32_t player);
  static void SaveConfigurations(const std::array<Configuration, 4>& players);
  X_STATUS Setup() override;
  // Call from the owner thread, never from an extension callback. Stops and
  // joins the old worker before replacing the endpoint, state and queues.
  X_STATUS Reconfigure(const Configuration& configuration);
  void Stop();

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  InputType GetInputType() const override { return InputType::Controller; }

  ConnectionStatus GetConnectionStatus() const;
  // Raw motion/touch metadata has no Xbox 360 XInput equivalent. Consumers
  // must account for the selected server's sensor units and touch range.
  bool GetControllerData(ControllerData* out_data) const;
  X_INPUT_GAMEPAD GetMotion(uint32_t user_index) const;

  // Callback runs on the DSU worker, without driver locks. It must not block or
  // destroy/reconfigure the driver. Post UI work and use weak lifetime tokens.
  // Stop() joins any in-flight callback before returning.
  void SetExtensionCallback(ExtensionCallback callback);
  static std::vector<uint8_t> BuildExtensionPacket(
      uint32_t type, std::span<const uint8_t> payload);
  // Accepts a complete packet from BuildExtensionPacket. The current client
  // ID and CRC are stamped at enqueue. Queue space and packet size are bounded.
  bool SendExtensionPacket(std::vector<uint8_t> packet, bool priority = false);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xe::hid::dsu

#endif  // XENIA_HID_DSU_DSU_INPUT_DRIVER_H_
