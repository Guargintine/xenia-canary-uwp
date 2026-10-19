// Copyright 2026 Xenia Canary. Released under the BSD license.
#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/app/stream_output.h"

#include <algorithm>
#include <array>
#include <mutex>

#include <winsock2.h>
#include <ws2tcpip.h>
#include "xenia/app/stream_destination.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"

namespace xe::app {
struct StreamOutput::Impl {
  mutable std::mutex mutex;
  SOCKET socket = INVALID_SOCKET;
  bool winsock = false;
  uint64_t dropped_packets = 0;
  uint64_t last_error_log = 0;
  struct Destination {
    std::string url;
    sockaddr_in address{};
  };
  std::array<Destination, 5> destinations;
};

StreamOutput::StreamOutput() : impl_(std::make_unique<Impl>()) {
  WSADATA data{};
  impl_->winsock = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  if (!impl_->winsock) {
    return;
  }
  impl_->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (impl_->socket == INVALID_SOCKET) {
    return;
  }
  const int buffer_size = 1024 * 1024;
  setsockopt(impl_->socket, SOL_SOCKET, SO_SNDBUF,
             reinterpret_cast<const char*>(&buffer_size), sizeof(buffer_size));
  u_long nonblocking = 1;
  if (ioctlsocket(impl_->socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
    closesocket(impl_->socket);
    impl_->socket = INVALID_SOCKET;
  }
}

StreamOutput::~StreamOutput() {
  if (impl_->socket != INVALID_SOCKET) {
    closesocket(impl_->socket);
  }
  if (impl_->winsock) {
    WSACleanup();
  }
}

bool StreamOutput::SetDestination(uint32_t receiver, const std::string& url) {
  const auto parsed = ParseStreamDestination(url);
  if (!parsed || receiver >= impl_->destinations.size() ||
      impl_->socket == INVALID_SOCKET) {
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(parsed->port);
  if (InetPtonA(AF_INET, parsed->host.c_str(), &address.sin_addr) != 1) {
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  impl_->destinations[receiver] = {url, address};
  return true;
}

void StreamOutput::RemoveDestination(uint32_t receiver) {
  std::lock_guard lock(impl_->mutex);
  if (receiver < impl_->destinations.size()) {
    impl_->destinations[receiver] = {};
  }
}

void StreamOutput::Clear() {
  std::lock_guard lock(impl_->mutex);
  impl_->destinations = {};
}

bool StreamOutput::Empty() const {
  std::lock_guard lock(impl_->mutex);
  return std::all_of(
      impl_->destinations.begin(), impl_->destinations.end(),
      [](const auto& destination) { return destination.url.empty(); });
}

std::string StreamOutput::GetDestination(uint32_t receiver) const {
  std::lock_guard lock(impl_->mutex);
  return receiver < impl_->destinations.size()
             ? impl_->destinations[receiver].url
             : std::string{};
}

int StreamOutput::Write(std::span<const uint8_t> bytes) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->socket == INVALID_SOCKET) {
    return -1;
  }
  std::array<std::pair<uint32_t, uint16_t>, 5> sent{};
  size_t sent_count = 0;
  for (const auto& destination : impl_->destinations) {
    const auto key = std::make_pair(destination.address.sin_addr.s_addr,
                                    destination.address.sin_port);
    if (destination.url.empty() ||
        std::find(sent.begin(), sent.begin() + sent_count, key) !=
            sent.begin() + sent_count) {
      continue;
    }
    sent[sent_count++] = key;
    for (size_t offset = 0; offset < bytes.size(); offset += 1316) {
      const auto size = std::min(size_t(1316), bytes.size() - offset);
      const auto send = [&] {
        return sendto(impl_->socket,
                      reinterpret_cast<const char*>(bytes.data() + offset),
                      int(size), 0,
                      reinterpret_cast<const sockaddr*>(&destination.address),
                      sizeof(destination.address));
      };
      int result = send();
      if (result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(impl_->socket, &writable);
        timeval timeout{0, 2000};
        if (select(0, nullptr, &writable, nullptr, &timeout) > 0) {
          result = send();
        }
      }
      if (result == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        ++impl_->dropped_packets;
        const auto now = GetTickCount64();
        if (now - impl_->last_error_log >= 5000) {
          XELOGW("Streaming UDP send failed: error={} dropped_packets={}",
                 error, impl_->dropped_packets);
          impl_->last_error_log = now;
        }
      }
    }
  }
  return int(bytes.size());
}
}  // namespace xe::app
#endif
