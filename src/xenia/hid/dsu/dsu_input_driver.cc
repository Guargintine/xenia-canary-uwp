// Copyright 2026 Xenia Canary. Released under the BSD license.

#include "xenia/hid/dsu/dsu_input_driver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#include "xenia/base/platform_win.h"
#include "xenia/hid/dsu/dsu_input_merge.h"
#include "xenia/hid/dsu/dsu_keystrokes.h"

#include <winsock2.h>  // NOLINT(build/include_order)
#include <ws2tcpip.h>  // NOLINT(build/include_order)

namespace xe::hid::dsu {
namespace {
uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
constexpr size_t kQueueLimit = 64;
constexpr uint64_t kRegistrationIntervalMs = 1000;
constexpr uint64_t kRumbleIntervalMs = 200;
}  // namespace

class DSUInputDriver::Impl {
 public:
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> running{false};
  std::thread worker;
  Configuration config;
  uint64_t generation = 0;
  std::string error;
  SOCKET socket = INVALID_SOCKET;
  sockaddr_in endpoint{};
  bool winsock_started = false;
  uint32_t client_id = 0;
  SessionState session;
  Keystrokes keys;
  bool had_input = false;
  uint64_t motion_timestamp = 0, motion_received_at = 0;
  std::array<uint8_t, 2> motors{};
  std::deque<std::vector<uint8_t>> priority_queue, queue;
  ExtensionCallback callback;

  void Expire(uint64_t now) {
    if (had_input && !session.Connected(now)) {
      keys.Reset();
      motors = {};
      had_input = false;
      motion_received_at = 0;
    }
  }

  bool Enqueue(std::vector<uint8_t> packet, bool priority) {
    if (!running.load() || packet.empty()) {
      return false;
    }
    if (priority_queue.size() + queue.size() >= kQueueLimit) {
      if (!priority || queue.empty()) {
        return false;
      }
      queue.pop_front();
    }
    (priority ? priority_queue : queue).push_back(std::move(packet));
    wake.notify_one();
    return true;
  }

  void Send(std::span<const uint8_t> packet) {
    // Socket is nonblocking and is owned by this worker until it is joined.
    sendto(socket, reinterpret_cast<const char*>(packet.data()),
           int(packet.size()), 0, reinterpret_cast<const sockaddr*>(&endpoint),
           sizeof(endpoint));
  }

  void RequestState() {
    Send(BuildPacket(kVersionMessage, {}, client_id));
    std::array<uint8_t, 5> info{};
    WriteU32(info, 0, 1);
    info[4] = uint8_t(config.remote_slot);
    Send(BuildPacket(kControllerMessage, info, client_id));
    const auto registration = BuildRegistration(uint8_t(config.remote_slot),
                                                config.companion_extensions);
    Send(BuildPacket(kDataMessage, registration, client_id));
    Send(BuildPacket(kMotorInfoMessage, registration, client_id));
    if (config.companion_extensions) {
    }
  }

  void SendRumble(bool stopping = false) {
    std::array<uint8_t, 2> values{};
    uint8_t count;
    {
      std::lock_guard lock(mutex);
      count = session.motor_count();
      if (!stopping && session.Connected(NowMs())) {
        values = motors;
      }
    }
    for (uint8_t motor = 0; motor < count; ++motor) {
      // Rumble uses the standard slot offset, unlike registration.
      std::array<uint8_t, 10> payload{};
      payload[0] = 1;
      payload[1] = uint8_t(config.remote_slot);
      payload[8] = motor;
      payload[9] = values[motor];
      Send(BuildPacket(kRumbleMessage, payload, client_id));
    }
  }

  void Receive() {
    std::array<uint8_t, kMaxPacketSize> buffer{};
    // Bound work per tick so floods cannot starve shutdown or registration.
    for (size_t received = 0; received < 64 && running.load(); ++received) {
      sockaddr_in source{};
      int source_size = sizeof(source);
      const int length = recvfrom(
          socket, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
          reinterpret_cast<sockaddr*>(&source), &source_size);
      if (length == SOCKET_ERROR) {
        break;
      }
      if (source.sin_family != AF_INET ||
          source.sin_addr.s_addr != endpoint.sin_addr.s_addr ||
          source.sin_port != endpoint.sin_port) {
        continue;
      }
      const auto packet = ParsePacket(std::span(buffer.data(), size_t(length)));
      if (!packet) {
        continue;
      }
      ExtensionCallback notify;
      {
        std::lock_guard lock(mutex);
        const uint64_t now = NowMs();
        Expire(now);
        if (!session.Accept(*packet, uint8_t(config.remote_slot), now)) {
          continue;
        }
        if (packet->type == kDataMessage) {
          if (session.Connected(now)) {
            const auto timestamp = session.controller().motion_timestamp_us;
            if (!had_input || timestamp != motion_timestamp) {
              motion_timestamp = timestamp;
              motion_received_at = now;
            }
            keys.Observe(
                MapGamepad(session.controller(), config.companion_extensions),
                now);
            had_input = true;
            error.clear();
          } else {
            keys.Reset();
            motors = {};
            had_input = false;
          }
        } else if (IsExtensionMessage(packet->type)) {
          notify = callback;
        }
      }
      if (notify && running.load()) {
        // Keep packet storage alive during callback; never expose recv buffer.
        const std::vector<uint8_t> payload(packet->payload.begin(),
                                           packet->payload.end());
        try {
          notify(packet->type, payload);
        } catch (...) {
          std::lock_guard lock(mutex);
          error = "Remote command handler failed";
        }
      }
    }
  }

  void Run() {
    uint64_t next_registration = 0, next_rumble = 0;
    while (running.load()) {
      const uint64_t now = NowMs();
      if (now >= next_registration) {
        RequestState();
        next_registration = now + kRegistrationIntervalMs;
      }
      if (now >= next_rumble) {
        SendRumble();
        next_rumble = now + kRumbleIntervalMs;
      }
      Receive();
      for (size_t sent = 0; sent < 32 && running.load(); ++sent) {
        std::vector<uint8_t> packet;
        {
          std::lock_guard lock(mutex);
          auto& pending = !priority_queue.empty() ? priority_queue : queue;
          if (pending.empty()) {
            break;
          }
          packet = std::move(pending.front());
          pending.pop_front();
        }
        Send(packet);
      }
      std::unique_lock lock(mutex);
      Expire(NowMs());
      wake.wait_for(lock, std::chrono::milliseconds(10), [this] {
        return !running.load() || !priority_queue.empty() || !queue.empty();
      });
    }
    SendRumble(true);
  }
};

DSUInputDriver::DSUInputDriver(xe::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order), impl_(std::make_unique<Impl>()) {}

DSUInputDriver::~DSUInputDriver() { Stop(); }

X_STATUS DSUInputDriver::Setup() { return Reconfigure(ReadConfiguration(0)); }

void DSUInputDriver::Stop() {
  impl_->running.store(false);
  impl_->wake.notify_all();
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->socket != INVALID_SOCKET) {
    closesocket(impl_->socket);
    impl_->socket = INVALID_SOCKET;
  }
  if (impl_->winsock_started) {
    WSACleanup();
    impl_->winsock_started = false;
  }
  impl_->session.Reset();
  impl_->keys.Reset();
  impl_->motors = {};
  impl_->had_input = false;
  impl_->motion_timestamp = impl_->motion_received_at = 0;
  impl_->queue.clear();
  impl_->priority_queue.clear();
  ++impl_->generation;
}

X_STATUS DSUInputDriver::Reconfigure(const Configuration& configuration) {
  Stop();
  std::lock_guard lock(impl_->mutex);
  impl_->config = configuration;
  impl_->error.clear();
  if (!configuration.enabled) {
    return X_STATUS_SUCCESS;
  }
  impl_->endpoint = {};
  impl_->endpoint.sin_family = AF_INET;
  if (!configuration.port || configuration.port > 65535 ||
      configuration.local_player >= XUserMaxUserCount ||
      configuration.remote_slot >=
          (configuration.companion_extensions ? 16u : 4u) ||
      InetPtonA(AF_INET, configuration.host.c_str(),
                &impl_->endpoint.sin_addr) != 1 ||
      impl_->endpoint.sin_addr.s_addr == INADDR_ANY ||
      impl_->endpoint.sin_addr.s_addr == INADDR_BROADCAST ||
      IN_MULTICAST(ntohl(impl_->endpoint.sin_addr.s_addr))) {
    impl_->error = "Invalid DSU IPv4 address, port or controller slot";
    return X_STATUS_INVALID_PARAMETER;
  }
  impl_->endpoint.sin_port = htons(uint16_t(configuration.port));
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    impl_->error = "Could not initialize DSU networking";
    return X_STATUS_UNSUCCESSFUL;
  }
  impl_->winsock_started = true;
  impl_->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in local{};
  local.sin_family = AF_INET;
  u_long nonblocking = 1;
  if (impl_->socket == INVALID_SOCKET ||
      bind(impl_->socket, reinterpret_cast<const sockaddr*>(&local),
           sizeof(local)) == SOCKET_ERROR ||
      ioctlsocket(impl_->socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
    impl_->error = "Could not open the DSU UDP socket";
    if (impl_->socket != INVALID_SOCKET) {
      closesocket(impl_->socket);
      impl_->socket = INVALID_SOCKET;
    }
    WSACleanup();
    impl_->winsock_started = false;
    return X_STATUS_UNSUCCESSFUL;
  }
  try {
    impl_->client_id = std::random_device{}();
    impl_->running.store(true);
    impl_->worker = std::thread([this] { impl_->Run(); });
  } catch (...) {
    impl_->running.store(false);
    closesocket(impl_->socket);
    impl_->socket = INVALID_SOCKET;
    WSACleanup();
    impl_->winsock_started = false;
    impl_->error = "Could not start the DSU input worker";
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_RESULT DSUInputDriver::GetState(uint32_t user_index,
                                  X_INPUT_STATE* out_state) {
  if (!out_state || user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }
  *out_state = {};
  std::lock_guard lock(impl_->mutex);
  if (user_index != impl_->config.local_player || !impl_->running.load() ||
      !impl_->session.Connected(NowMs())) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  out_state->packet_number = impl_->session.controller().packet_number;
  out_state->gamepad = MapGamepad(impl_->session.controller(),
                                  impl_->config.companion_extensions);
  return X_ERROR_SUCCESS;
}

X_RESULT DSUInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (!out_caps || user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }
  *out_caps = {};
  std::lock_guard lock(impl_->mutex);
  if (user_index != impl_->config.local_player || !impl_->running.load() ||
      !impl_->session.Connected(NowMs())) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
  out_caps->flags =
      impl_->session.motor_count() ? X_INPUT_CAPS_FFB_SUPPORTED : 0;
  out_caps->gamepad.buttons = 0xF7FF;
  out_caps->gamepad.left_trigger = out_caps->gamepad.right_trigger = 255;
  out_caps->gamepad.thumb_lx = out_caps->gamepad.thumb_ly = 32767;
  out_caps->gamepad.thumb_rx = out_caps->gamepad.thumb_ry = 32767;
  if (impl_->session.motor_count()) {
    out_caps->vibration.left_motor_speed = 65535;
    out_caps->vibration.right_motor_speed = 65535;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT DSUInputDriver::SetState(uint32_t user_index,
                                  X_INPUT_VIBRATION* vibration) {
  if (!vibration || user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::lock_guard lock(impl_->mutex);
  if (user_index != impl_->config.local_player || !impl_->running.load() ||
      !impl_->session.Connected(NowMs())) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  impl_->motors = {uint8_t(uint16_t(vibration->left_motor_speed) / 257),
                   uint8_t(uint16_t(vibration->right_motor_speed) / 257)};
  impl_->wake.notify_one();
  return X_ERROR_SUCCESS;
}

X_RESULT DSUInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  if (!out_keystroke ||
      (user_index != XUserIndexAny && user_index >= XUserMaxUserCount)) {
    return X_ERROR_INVALID_PARAMETER;
  }
  *out_keystroke = {};
  std::lock_guard lock(impl_->mutex);
  const uint64_t now = NowMs();
  impl_->Expire(now);
  if ((user_index != XUserIndexAny &&
       user_index != impl_->config.local_player) ||
      !impl_->running.load() || !impl_->session.Connected(now)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return impl_->keys.Pop(uint8_t(impl_->config.local_player), now,
                         out_keystroke)
             ? X_ERROR_SUCCESS
             : X_ERROR_EMPTY;
}

ConnectionStatus DSUInputDriver::GetConnectionStatus() const {
  std::lock_guard lock(impl_->mutex);
  return {impl_->config.enabled,
          impl_->running.load() && impl_->session.Connected(NowMs()),
          impl_->config.host,
          impl_->config.port,
          impl_->config.remote_slot,
          impl_->config.local_player,
          impl_->generation,
          impl_->error};
}

bool DSUInputDriver::GetControllerData(ControllerData* out_data) const {
  if (!out_data) {
    return false;
  }
  *out_data = {};
  std::lock_guard lock(impl_->mutex);
  if (!impl_->running.load() || !impl_->session.Connected(NowMs())) {
    return false;
  }
  *out_data = impl_->session.controller();
  return true;
}

void DSUInputDriver::SetExtensionCallback(ExtensionCallback callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->callback = std::move(callback);
}

X_INPUT_GAMEPAD DSUInputDriver::GetMotion(uint32_t user_index) const {
  std::lock_guard lock(impl_->mutex);
  const auto now = NowMs();
  if (user_index != impl_->config.local_player ||
      !impl_->config.motion_right_stick || !impl_->running.load() ||
      !impl_->session.Connected(now) || now - impl_->motion_received_at > 250) {
    return {};
  }
  return MapRightStickMotion(
      impl_->session.controller().gyro, impl_->config.motion_sensitivity_x,
      impl_->config.motion_sensitivity_y, impl_->config.motion_deadzone,
      impl_->config.motion_invert_x, impl_->config.motion_invert_y);
}

std::vector<uint8_t> DSUInputDriver::BuildExtensionPacket(
    uint32_t type, std::span<const uint8_t> payload) {
  return IsExtensionMessage(type) ? BuildPacket(type, payload, 0)
                                  : std::vector<uint8_t>{};
}

bool DSUInputDriver::SendExtensionPacket(std::vector<uint8_t> packet,
                                         bool priority) {
  const auto parsed = ParsePacket(packet, false);
  if (!parsed || !IsExtensionMessage(parsed->type)) {
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  WriteU32(packet, 12, impl_->client_id);
  WriteU32(packet, 8, PacketCrc32(packet));
  return impl_->Enqueue(std::move(packet), priority);
}

}  // namespace xe::hid::dsu
