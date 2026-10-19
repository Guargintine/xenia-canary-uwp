// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_SCREENSCRAPER_ARTWORK_H_
#define XENIA_APP_SCREENSCRAPER_ARTWORK_H_

#include <algorithm>
#include <cctype>
#include <utility>
#include "xenia/app/screenscraper_service.h"

namespace xe::app::screenscraper {
inline std::optional<MediaType> ArtworkType(std::string_view value) {
  for (unsigned i = 0; i < unsigned(MediaType::Count); ++i) {
    const auto type = MediaType(i);
    if (type != MediaType::Video && type != MediaType::Manual &&
        MediaDirectory(type) == value) {
      return type;
    }
  }
  return std::nullopt;
}

inline std::filesystem::path SelectedArtwork(
    const CachedGame& game, std::string_view value,
    std::span<const MediaType> fallback = {}) {
  if (value == "off") {
    return {};
  }
  const auto image = [&](MediaType type) -> std::filesystem::path {
    const auto found = game.media.find(type);
    if (found == game.media.end()) {
      return {};
    }
    auto extension = found->second.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return extension == ".webp" ? std::filesystem::path{} : found->second;
  };
  if (const auto type = ArtworkType(value)) {
    auto path = image(*type);
    if (!path.empty()) {
      return path;
    }
  }
  for (const auto type : fallback) {
    auto path = image(type);
    if (!path.empty()) {
      return path;
    }
  }
  return {};
}

inline std::filesystem::path ArtworkPreview(const Match& match,
                                            MediaType type) {
  const auto found = match.previews.find(type);
  return found == match.previews.end() ? std::filesystem::path{}
                                       : found->second;
}

inline std::pair<std::filesystem::path, std::filesystem::path> EditorArtwork(
    const Match& match, MediaType list, MediaType detail) {
  return {ArtworkPreview(match, list), ArtworkPreview(match, detail)};
}
}  // namespace xe::app::screenscraper
#endif
