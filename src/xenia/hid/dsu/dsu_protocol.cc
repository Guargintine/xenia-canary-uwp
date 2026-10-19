// Copyright 2026 Xenia Canary. Released under the BSD license.

#include "xenia/hid/dsu/dsu_protocol.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace xe::hid::dsu {

uint16_t ReadU16(std::span<const uint8_t> bytes, size_t offset) {
  return uint16_t(bytes[offset]) | uint16_t(bytes[offset + 1]) << 8;
}
uint32_t ReadU32(std::span<const uint8_t> bytes, size_t offset) {
  return uint32_t(ReadU16(bytes, offset)) | uint32_t(ReadU16(bytes, offset + 2))
                                                << 16;
}
void WriteU16(std::span<uint8_t> bytes, size_t offset, uint16_t value) {
  bytes[offset] = uint8_t(value);
  bytes[offset + 1] = uint8_t(value >> 8);
}
void WriteU32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
  WriteU16(bytes, offset, uint16_t(value));
  WriteU16(bytes, offset + 2, uint16_t(value >> 16));
}

uint32_t PacketCrc32(std::span<const uint8_t> bytes) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t index = 0; index < bytes.size(); ++index) {
    // CRC is evaluated with its own field zeroed, without mutating input.
    crc ^= index >= 8 && index < 12 ? 0 : bytes[index];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

std::optional<PacketView> ParsePacket(std::span<const uint8_t> bytes,
                                      bool server) {
  if (bytes.size() < kHeaderSize || bytes.size() > kMaxPacketSize ||
      std::memcmp(bytes.data(), server ? "DSUS" : "DSUC", 4) != 0 ||
      ReadU16(bytes, 4) != kProtocolVersion ||
      ReadU16(bytes, 6) != bytes.size() - 16 ||
      ReadU32(bytes, 8) != PacketCrc32(bytes)) {
    return std::nullopt;
  }
  return PacketView{ReadU32(bytes, 12), ReadU32(bytes, 16),
                    bytes.subspan(kHeaderSize)};
}

std::vector<uint8_t> BuildPacket(uint32_t type,
                                 std::span<const uint8_t> payload,
                                 uint32_t sender_id, bool server) {
  if (payload.size() > kMaxPacketSize - kHeaderSize) {
    return {};
  }
  std::vector<uint8_t> result(kHeaderSize + payload.size());
  std::memcpy(result.data(), server ? "DSUS" : "DSUC", 4);
  WriteU16(result, 4, kProtocolVersion);
  WriteU16(result, 6, uint16_t(result.size() - 16));
  WriteU32(result, 12, sender_id);
  WriteU32(result, 16, type);
  std::copy(payload.begin(), payload.end(), result.begin() + kHeaderSize);
  WriteU32(result, 8, PacketCrc32(result));
  return result;
}

std::array<uint8_t, 8> BuildRegistration(uint8_t remote_slot,
                                         bool companion_extensions) {
  std::array<uint8_t, 8> result{};
  result[0] = 1;  // Slot-based, never all controllers.
  result[1] = remote_slot;
  // Extended registration repeats the slot in the first unused MAC byte.
  result[2] = companion_extensions ? remote_slot : 0;
  return result;
}

std::optional<ControllerData> DecodeController(
    std::span<const uint8_t> payload) {
  if (payload.size() != 80 || payload[0] >= 16 || payload[1] > 2 ||
      payload[11] > 1 || payload[36] > 1 || payload[42] > 1) {
    return std::nullopt;
  }
  ControllerData data;
  data.slot = payload[0];
  data.connected = payload[1] == 2 && payload[11] == 1;
  std::copy_n(payload.begin() + 4, 6, data.mac.begin());
  data.packet_number = ReadU32(payload, 12);
  data.buttons = ReadU16(payload, 16);
  data.home = payload[18] != 0;
  data.touch_button = payload[19] != 0;
  data.lx = payload[20];
  data.ly = payload[21];
  data.rx = payload[22];
  data.ry = payload[23];
  data.right_trigger = payload[34];
  data.left_trigger = payload[35];
  for (size_t i = 0; i < data.touch.size(); ++i) {
    const size_t offset = 36 + i * 6;
    data.touch[i] = {payload[offset] != 0, payload[offset + 1],
                     ReadU16(payload, offset + 2),
                     ReadU16(payload, offset + 4)};
  }
  data.motion_timestamp_us =
      uint64_t(ReadU32(payload, 48)) | uint64_t(ReadU32(payload, 52)) << 32;
  for (size_t i = 0; i < 3; ++i) {
    data.accel[i] = std::bit_cast<float>(ReadU32(payload, 56 + i * 4));
    data.gyro[i] = std::bit_cast<float>(ReadU32(payload, 68 + i * 4));
    if (!std::isfinite(data.accel[i]) || !std::isfinite(data.gyro[i])) {
      return std::nullopt;
    }
  }
  return data;
}

X_INPUT_GAMEPAD MapGamepad(const ControllerData& data,
                           bool companion_extensions) {
  X_INPUT_GAMEPAD result{};
  if (!data.connected) {
    return result;
  }
  // DSU low byte uses PlayStation order, not the Xbox XInput bit order.
  constexpr std::array<uint16_t, 12> low_buttons = {
      X_INPUT_GAMEPAD_BACK,
      X_INPUT_GAMEPAD_LEFT_THUMB,
      X_INPUT_GAMEPAD_RIGHT_THUMB,
      X_INPUT_GAMEPAD_START,
      X_INPUT_GAMEPAD_DPAD_UP,
      X_INPUT_GAMEPAD_DPAD_RIGHT,
      X_INPUT_GAMEPAD_DPAD_DOWN,
      X_INPUT_GAMEPAD_DPAD_LEFT,
      0,
      0,
      X_INPUT_GAMEPAD_LEFT_SHOULDER,
      X_INPUT_GAMEPAD_RIGHT_SHOULDER};
  uint16_t buttons = 0;
  for (size_t bit = 0; bit < low_buttons.size(); ++bit) {
    if (data.buttons & (1u << bit)) {
      buttons |= low_buttons[bit];
    }
  }
  const std::array<uint16_t, 4> face_buttons =
      companion_extensions
          ? std::array<uint16_t, 4>{X_INPUT_GAMEPAD_Y, X_INPUT_GAMEPAD_B,
                                    X_INPUT_GAMEPAD_A, X_INPUT_GAMEPAD_X}
          : std::array<uint16_t, 4>{X_INPUT_GAMEPAD_X, X_INPUT_GAMEPAD_A,
                                    X_INPUT_GAMEPAD_B, X_INPUT_GAMEPAD_Y};
  for (size_t bit = 0; bit < face_buttons.size(); ++bit) {
    if (data.buttons & (1u << (12 + bit))) {
      buttons |= face_buttons[bit];
    }
  }
  if (data.home) {
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  }
  result.buttons = buttons;
  result.left_trigger = data.left_trigger;
  result.right_trigger = data.right_trigger;
  // Use digital triggers when pressure bytes are absent. Preserve nonzero
  // analog values even when the corresponding digital bit is set.
  if (companion_extensions && !result.left_trigger && (data.buttons & 0x0100)) {
    result.left_trigger = 255;
  }
  if (companion_extensions && !result.right_trigger &&
      (data.buttons & 0x0200)) {
    result.right_trigger = 255;
  }
  const auto axis = [](uint8_t value) -> int16_t {
    // Exactly neutral at 128, with both signed endpoints attainable.
    return value < 128 ? int16_t((int(value) - 128) * 256)
                       : int16_t((int(value) - 128) * 32767 / 127);
  };
  result.thumb_lx = axis(data.lx);
  result.thumb_ly = axis(data.ly);  // DSU and Xbox are both positive upward.
  result.thumb_rx = axis(data.rx);
  result.thumb_ry = axis(data.ry);
  return result;
}

bool IsExtensionMessage(uint32_t type) {
  switch (type) {
    case 0x120000:  // Keyboard.
    case 0x120001:  // Mouse.
    case 0x130000:  // Legacy command.
    case 0x140000:  // State.
    case 0x150000:  // Main stream start / stop / status.
    case 0x150001:
    case 0x150002:
    case 0x150100:  // Auxiliary stream start / stop.
    case 0x150101:
    case 0x160000:  // Playlists.
    case 0x160001:
    case 0x160002:
    case 0x160003:
    case 0x170000:  // Contentless cores.
    case 0x170001:
    case 0x180004:  // Catalogue launch / status / selection.
    case 0x180005:
    case 0x180006:
      return true;
    default:
      return false;
  }
}

void SessionState::Reset() { *this = {}; }

bool SessionState::Connected(uint64_t now_ms) const {
  return has_input_ && controller_.connected && now_ms >= last_input_ms_ &&
         now_ms - last_input_ms_ < kInputTimeoutMs;
}

bool SessionState::Accept(const PacketView& packet, uint8_t remote_slot,
                          uint64_t now_ms) {
  const auto payload = packet.payload;
  const bool version = packet.type == kVersionMessage && payload.size() == 2 &&
                       ReadU16(payload, 0) == kProtocolVersion;
  const bool info = packet.type == kControllerMessage && payload.size() == 12 &&
                    payload[0] == remote_slot && payload[1] <= 2;
  // Only a valid handshake can establish/change the remote server identity.
  if (!server_id_ || *server_id_ != packet.sender_id) {
    if ((!version && !info) || Connected(now_ms)) {
      return false;
    }
    Reset();
    server_id_ = packet.sender_id;
  }
  if (packet.type == kVersionMessage) {
    return version;
  }
  if (packet.type == kControllerMessage) {
    if (info && payload[1] != 2) {
      has_input_ = false;
      controller_ = {};
    }
    return info;
  }
  if (packet.type == kMotorInfoMessage) {
    if (payload.size() != 12 || payload[0] != remote_slot || payload[11] > 2) {
      return false;
    }
    motor_count_ = payload[11];
    return true;
  }
  if (packet.type != kDataMessage) {
    return IsExtensionMessage(packet.type);
  }
  const auto next = DecodeController(payload);
  if (!next || next->slot != remote_slot) {
    return false;
  }
  // Modular comparison admits UINT32_MAX -> 0 but rejects duplicates and
  // reordered input. A timed-out server may restart its counter at zero.
  if (has_input_ && now_ms >= last_input_ms_ &&
      now_ms - last_input_ms_ < kInputTimeoutMs &&
      int32_t(next->packet_number - controller_.packet_number) <= 0) {
    return false;
  }
  controller_ = *next;
  last_input_ms_ = now_ms;
  has_input_ = true;
  return true;
}

}  // namespace xe::hid::dsu
