// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_FRONTEND_THEME_H_
#define XENIA_APP_FRONTEND_THEME_H_

#include <array>
#include <string>
#include <string_view>

namespace xe::app {

struct ThemeColor {
  const char* value;
  const char* label;
};

inline constexpr std::array<ThemeColor, 7> kThemeColors = {{
    {"blue", "Blue"},
    {"green", "Green"},
    {"orange", "Orange"},
    {"purple", "Purple"},
    {"red", "Red"},
    {"silver", "Silver"},
    {"black", "Black"},
}};

inline std::string_view NormalizeThemeColor(std::string_view color) {
  if (color == "grey") {
    return "silver";
  }
  for (const auto& option : kThemeColors) {
    if (color == option.value) {
      return option.value;
    }
  }
  return "green";
}

enum class ThemeAsset { kOverlay, kGuide, kWideGuide, kBackground, kLogo };

inline std::string ThemeAssetPath(ThemeAsset asset, std::string_view color) {
  std::string folder;
  std::string filename;
  switch (asset) {
    case ThemeAsset::kOverlay:
      folder = "interface_overlays";
      filename = "Blades-2-left_2-right.png";
      break;
    case ThemeAsset::kGuide:
      folder = "guide_images";
      filename = "guide.png";
      break;
    case ThemeAsset::kWideGuide:
      folder = "guide_images";
      filename = "guide-wide-V2.png";
      break;
    case ThemeAsset::kBackground:
      folder = "backgrounds";
      filename = "xenia.png";
      break;
    case ThemeAsset::kLogo:
      folder = "interface_logos";
      filename = "xenia-logo.png";
      break;
  }
  return "Assets/" + folder + "/" + std::string(NormalizeThemeColor(color)) +
         "/" + filename;
}

template <typename SetColor>
void ApplyColorTheme(std::string_view color, SetColor set_color) {
  const std::string value(NormalizeThemeColor(color));
  for (const char* name :
       {"ui_color_theme", "ui_interface_overlay_color", "ui_guide_color",
        "ui_background_color", "ui_border_color"}) {
    set_color(name, value);
  }
}

}  // namespace xe::app
#endif  // XENIA_APP_FRONTEND_THEME_H_
