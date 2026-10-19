// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_SCREENSCRAPER_TARGET_H_
#define XENIA_APP_SCREENSCRAPER_TARGET_H_

#include <algorithm>
#include <span>
#include "xenia-canary-uwp/game_storage.h"
#include "xenia/app/screenscraper_service.h"

namespace xe::app {
// Compare Windows roots without case, preserving relative filename spelling.
// LexicallyRelative alone can produce ../ for a drive-letter case mismatch.
inline screenscraper::Target MakeScreenScraperTarget(
    const std::filesystem::path& path, const std::string& name,
    std::span<const std::string> game_roots) {
  screenscraper::Target target;
  target.content_path = path.lexically_normal();
  target.name = name;
  if (path.empty()) {
    return target;
  }
  const auto utf8_path = target.content_path.generic_u8string();
  const std::string original(utf8_path.begin(), utf8_path.end());
  const auto normalized = UWP::ComparableStoragePath(original);
  uint64_t id = 14695981039346656037ull;
  for (unsigned char c : normalized) {
    if (c == '\\') {
      c = '/';
    }
    id = (id ^ c) * 1099511628211ull;
  }
  target.id = id ? id : 1;
  size_t root_length = 0;
  size_t relative_offset = 0;
  for (const auto& root : game_roots) {
    const auto comparable = UWP::ComparableStoragePath(root);
    if (comparable.size() <= root_length ||
        normalized.size() <= comparable.size() ||
        normalized.compare(0, comparable.size(), comparable) ||
        (comparable.back() != '\\' && normalized[comparable.size()] != '\\')) {
      continue;
    }
    root_length = comparable.size();
    relative_offset = root_length + (comparable.back() == '\\' ? 0 : 1);
    target.content_root = std::filesystem::u8path(root).lexically_normal();
  }
  if (root_length) {
    target.relative_game_path =
        std::filesystem::u8path(original.substr(relative_offset));
  }
  auto filename = target.content_path.filename().u8string();
  std::transform(filename.begin(), filename.end(), filename.begin(),
                 [](char8_t c) {
                   return char8_t(std::tolower(static_cast<unsigned char>(c)));
                 });
  std::filesystem::path legacy_relative;
  if (filename == u8"default.xex" &&
      (target.relative_game_path.empty() ||
       target.relative_game_path.has_parent_path())) {
    legacy_relative = target.relative_game_path.empty()
                          ? target.content_path.parent_path().filename()
                          : target.relative_game_path.parent_path();
  }
  // Folders with supported game extensions represent single library entries.
  std::filesystem::path directory;
  for (const auto& part : target.relative_game_path.parent_path()) {
    directory /= part;
    const auto extension = part.extension().u8string();
    if (extension == u8".iso" || extension == u8".ISO" ||
        extension == u8".lnk" || extension == u8".LNK" ||
        extension == u8".xex" || extension == u8".XEX" ||
        extension == u8".zar" || extension == u8".ZAR") {
      target.directory_as_file = true;
      target.relative_game_path = directory;
      break;
    }
  }
  if (!legacy_relative.empty() &&
      (!target.directory_as_file ||
       legacy_relative != target.relative_game_path)) {
    target.legacy_relative_game_path = std::move(legacy_relative);
  }
  return target;
}
// Directory entries retain their complete names; file entries omit the extension.
inline std::filesystem::path ScreenScraperRelativeGameName(
    const screenscraper::Target& target) {
  auto relative = target.relative_game_path.empty()
                      ? target.content_path.filename()
                      : target.relative_game_path;
  if (!target.directory_as_file) {
    relative.replace_extension();
  }
  return relative;
}
}  // namespace xe::app
#endif
