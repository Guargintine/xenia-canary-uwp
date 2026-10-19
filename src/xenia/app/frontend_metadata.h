// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_FRONTEND_METADATA_H_
#define XENIA_APP_FRONTEND_METADATA_H_

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>

namespace xe::app {

// Convert a five-star rating to a normalized fraction.
inline std::optional<double> NormalizeXboxMarketplaceRating(
    std::string_view rating) {
  if (rating.empty()) {
    return std::nullopt;
  }
  double value = 0;
  const auto parsed =
      std::from_chars(rating.data(), rating.data() + rating.size(), value);
  if (parsed.ec != std::errc() || parsed.ptr != rating.data() + rating.size() ||
      !std::isfinite(value)) {
    return std::nullopt;
  }
  return std::clamp(value / 5.0, 0.0, 1.0);
}

}  // namespace xe::app
#endif  // XENIA_APP_FRONTEND_METADATA_H_
