// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UI_FRONTEND_SELECTION_H_
#define XENIA_UI_FRONTEND_SELECTION_H_

#include "third_party/imgui/imgui_internal.h"

namespace xe::ui {
// Move focus immediately so a later carousel row cannot restore the old
// selection. Do not activate the item or steal popup focus.
inline bool FocusFrontendSelection() {
  auto& g = *ImGui::GetCurrentContext();
  if (!g.LastItemData.ID ||
      (g.LastItemData.ItemFlags & ImGuiItemFlags_Disabled) ||
      ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                     ImGuiPopupFlags_AnyPopupLevel)) {
    return false;
  }
  auto* window = ImGui::GetCurrentWindow();
  ImGui::FocusWindow(window, ImGuiFocusRequestFlags_UnlessBelowModal);
  if (g.NavWindow != window) {
    return false;
  }
  ImGui::SetNavWindow(window);
  g.NavInitResult = ImGuiNavItemData();
  ImGui::SetFocusID(g.LastItemData.ID, window);
  ImGui::SetNavCursorVisible(true);
  return true;
}
}  // namespace xe::ui

#endif  // XENIA_UI_FRONTEND_SELECTION_H_
