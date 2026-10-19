// Copyright 2026 Xenia Canary. Released under the BSD license.

#ifndef XENIA_HID_DSU_DSU_PROTOCOL_H_
#define XENIA_HID_DSU_DSU_PROTOCOL_H_

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "xenia/hid/input.h"

namespace xe::hid::dsu {

constexpr uint16_t kProtocolVersion = 1001;
constexpr size_t kHeaderSize = 20;
constexpr size_t kMaxPacketSize = 8192;
constexpr uint32_t kVersionMessage = 0x100000;
constexpr uint32_t kControllerMessage = 0x100001;
constexpr uint32_t kDataMessage = 0x100002;
constexpr uint32_t kMotorInfoMessage = 0x110001;
constexpr uint32_t kRumbleMessage = 0x110002;
constexpr uint64_t kInputTimeoutMs = 1000;

struct PacketView {
  uint32_t sender_id;
  uint32_t type;
  std::span<const uint8_t> payload;
};

struct Touch {
  bool active = false;
  uint8_t id = 0;
  uint16_t x = 0;
  uint16_t y = 0;
};

struct ControllerData {
  uint8_t slot = 0;
  bool connected = false;
  std::array<uint8_t, 6> mac{};
  uint32_t packet_number = 0;
  uint16_t buttons = 0;
  bool home = false;
  bool touch_button = false;
  uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
  uint8_t left_trigger = 0, right_trigger = 0;
  std::array<Touch, 2> touch{};
  uint64_t motion_timestamp_us = 0;
  std::array<float, 3> accel{}, gyro{};
};

uint16_t ReadU16(std::span<const uint8_t> bytes, size_t offset);
uint32_t ReadU32(std::span<const uint8_t> bytes, size_t offset);
void WriteU16(std::span<uint8_t> bytes, size_t offset, uint16_t value);
void WriteU32(std::span<uint8_t> bytes, size_t offset, uint32_t value);
uint32_t PacketCrc32(std::span<const uint8_t> bytes);
std::optional<PacketView> ParsePacket(std::span<const uint8_t> bytes,
                                      bool server = true);
std::vector<uint8_t> BuildPacket(uint32_t type,
                                 std::span<const uint8_t> payload,
                                 uint32_t sender_id, bool server = false);
std::array<uint8_t, 8> BuildRegistration(uint8_t remote_slot,
                                         bool companion_extensions);
std::optional<ControllerData> DecodeController(
    std::span<const uint8_t> payload);
X_INPUT_GAMEPAD MapGamepad(const ControllerData& data,
                           bool companion_extensions);
bool IsExtensionMessage(uint32_t type);

// The source address/port is checked by the transport before this gate. DSU
// identifies a server but has no authentication or echoed client challenge.
// Retire this object whenever the socket/endpoint changes.
class SessionState {
 public:
  bool Accept(const PacketView& packet, uint8_t remote_slot, uint64_t now_ms);
  bool Connected(uint64_t now_ms) const;
  void Reset();
  const ControllerData& controller() const { return controller_; }
  uint8_t motor_count() const { return motor_count_; }
  bool has_identity() const { return server_id_.has_value(); }

 private:
  std::optional<uint32_t> server_id_;
  ControllerData controller_{};
  uint64_t last_input_ms_ = 0;
  bool has_input_ = false;
  uint8_t motor_count_ = 0;
};

}  // namespace xe::hid::dsu

#endif  // XENIA_HID_DSU_DSU_PROTOCOL_H_
