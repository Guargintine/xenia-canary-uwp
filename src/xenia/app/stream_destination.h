// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_STREAM_DESTINATION_H_
#define XENIA_APP_STREAM_DESTINATION_H_

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xe::app {

struct StreamDestination {
  std::string host;
  uint16_t port;
};

inline std::optional<StreamDestination> ParseStreamDestination(
    std::string_view url) {
  if (url.size() > 255 || !url.starts_with("udp://")) {
    return std::nullopt;
  }
  const auto colon = url.find(':', 6);
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  const auto host = url.substr(6, colon - 6);
  uint32_t address = 0;
  size_t offset = 0;
  for (size_t i = 0; i < 4; ++i) {
    const auto dot = host.find('.', offset);
    if ((i < 3 && dot == std::string_view::npos) ||
        (i == 3 && dot != std::string_view::npos)) {
      return std::nullopt;
    }
    const auto part =
        host.substr(offset, dot == std::string_view::npos ? host.size() - offset
                                                          : dot - offset);
    if (part.empty() || part.size() > 3 ||
        (part.size() > 1 && part.front() == '0')) {
      return std::nullopt;
    }
    uint32_t octet = 0;
    const auto parsed =
        std::from_chars(part.data(), part.data() + part.size(), octet);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() ||
        octet > 255) {
      return std::nullopt;
    }
    address = (address << 8) | octet;
    offset = dot == std::string_view::npos ? host.size() : dot + 1;
  }
  if (!address || address == UINT32_MAX) {
    return std::nullopt;
  }
  uint32_t port = 0;
  const char* begin = url.data() + colon + 1;
  const char* end = url.data() + url.size();
  const auto parsed = std::from_chars(begin, end, port);
  if (parsed.ec != std::errc{} || parsed.ptr != end || !port || port > 65535) {
    return std::nullopt;
  }
  return StreamDestination{std::string(host), static_cast<uint16_t>(port)};
}

}  // namespace xe::app

#endif  // XENIA_APP_STREAM_DESTINATION_H_
