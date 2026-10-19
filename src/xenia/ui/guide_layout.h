// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UI_GUIDE_LAYOUT_H_
#define XENIA_UI_GUIDE_LAYOUT_H_

#include <algorithm>

namespace xe::ui {

struct GuideContentBounds {
  float left;
  float right;
  float width() const { return right - left; }
};

struct GuideBodyBounds {
  float top;
  float bottom;
  float height() const { return bottom - top; }
};

// The blades artwork's grey body ends at y=464 at 576 pixels high. Keep
// content and the slightly expanded navigation outline inside that body.
inline GuideBodyBounds GetGuideBodyBounds(float panel_height) {
  const float scale = std::max(1.0f, panel_height) / 576.0f;
  return {82.0f * scale, 456.0f * scale};
}

// Match the small sign-in guide's 320-pixel fields at 1024x576. The artwork
// includes a decorative right edge; its full width is not a button width.
inline GuideContentBounds GetGuideContentBounds(float panel_width,
                                                float display_width,
                                                float padding = 32.0f) {
  panel_width = std::max(1.0f, panel_width);
  const float left = std::min(padding, panel_width * 0.25f);
  const float right =
      std::min(panel_width - left, left + 320.0f * display_width / 1024.0f);
  return {left, std::max(left + 1.0f, right)};
}

}  // namespace xe::ui
#endif  // XENIA_UI_GUIDE_LAYOUT_H_
