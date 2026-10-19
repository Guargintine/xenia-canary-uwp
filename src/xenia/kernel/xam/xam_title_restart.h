// Copyright 2026 Xenia Canary. Released under the BSD license.

#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>

#include "xenia/kernel/xam/xam_loader_data.h"

namespace xe::kernel::xam {

inline std::optional<LoaderData> PrepareTitleRestartData(
    const LoaderData& source) {
  LoaderData result = source;
  auto& module = result.launch_path;
  std::replace(module.begin(), module.end(), '/', '\\');
  const auto lower_ascii = [](std::string text) {
    for (char& value : text) {
      if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
    }
    return text;
  };
  for (const auto prefix : {"game:\\", "d:\\"}) {
    if (lower_ascii(module).starts_with(prefix)) {
      module.erase(0, std::char_traits<char>::length(prefix));
      break;
    }
  }
  module.erase(0, module.find_first_not_of('\\'));

  try {
    auto host = std::filesystem::path(
        std::u8string(source.host_path.begin(), source.host_path.end()));
    const auto extension = host.extension().u8string();
    if (lower_ascii(std::string(extension.begin(), extension.end())) ==
        ".xex") {
      if (!module.empty()) {
        const auto module_path =
            std::filesystem::path(std::u8string(module.begin(), module.end()));
        host = (host.parent_path() / module_path).lexically_normal();
      }
      const auto path = host.u8string();
      result.host_path.assign(path.begin(), path.end());
      module.clear();
    }
  } catch (const std::filesystem::filesystem_error&) {
    return std::nullopt;
  }
  return result;
}

}  // namespace xe::kernel::xam
