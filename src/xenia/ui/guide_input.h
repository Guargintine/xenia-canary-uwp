// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UI_GUIDE_INPUT_H_
#define XENIA_UI_GUIDE_INPUT_H_

#include "third_party/imgui/imgui_internal.h"

namespace xe::app {

struct GuideBackTarget {
  ImGuiContext* context = nullptr;
  ImGuiID window_id = 0;
  int frame = -1;
  bool active_item = false;
  bool consumed = false;
};

inline GuideBackTarget guide_back_target;

inline void PrepareGuideTextInput(const char* label) {
  auto& g = *ImGui::GetCurrentContext();
  const auto id = ImGui::GetID(label);
  // B leaves the field; the page's Cancel action discards the edits.
  if (g.ActiveId == id &&
      ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
    ImGui::ClearActiveID();
  }
  if (g.NavActivateId == id &&
      g.NavInputSource == ImGuiInputSource_Gamepad) {
    g.NavActivateFlags |= ImGuiActivateFlags_PreferInput;
  }
  // While the field is active, the D-pad and left stick send arrow key presses
  // (applied in the next frame).
  if (g.ActiveId == id) {
    ImGuiIO& io = ImGui::GetIO();
    const struct {
      ImGuiKey pad;
      ImGuiKey stick;
      ImGuiKey arrow;
    } caret_keys[] = {
        {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft,
         ImGuiKey_LeftArrow},
        {ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight,
         ImGuiKey_RightArrow},
        {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp, ImGuiKey_UpArrow},
        {ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown,
         ImGuiKey_DownArrow},
    };
    for (const auto& keys : caret_keys) {
      if (ImGui::IsKeyPressed(keys.pad, true) ||
          ImGui::IsKeyPressed(keys.stick, true)) {
        io.AddKeyEvent(keys.arrow, true);
        io.AddKeyEvent(keys.arrow, false);
      }
    }
  }
}

inline void LinkGuideNavigationFromWindow(ImGuiID source_window,
                                          ImGuiDir direction) {
  auto& g = *ImGui::GetCurrentContext();
  if (!source_window || !g.NavWindow || g.NavWindow->ID != source_window ||
      g.ActiveId || !g.NavMoveSubmitted || g.NavMoveDir != direction ||
      g.OpenPopupStack.Size != g.BeginPopupStack.Size) {
    return;
  }
  g.NavMoveResultOther.Clear();
  g.NavMoveResultLocalVisible.Clear();
  ImGui::NavMoveRequestResolveWithLastItem(&g.NavMoveResultLocal);
}

// Select the first control after the opening press releases. Keyboard focus
// requests activate text fields; navigation initialization only highlights
// them.
inline bool InitializeGuideNavigation() {
  auto& g = *ImGui::GetCurrentContext();
  if (g.OpenPopupStack.Size != g.BeginPopupStack.Size) {
    return false;
  }
  auto* window = ImGui::GetCurrentWindow()->RootWindowForNav;
  ImGui::FocusWindow(window, ImGuiFocusRequestFlags_UnlessBelowModal);
  if (g.NavWindow != window) {
    return false;
  }
  ImGui::NavInitWindow(window, true);
  ImGui::SetNavCursorVisible(true);
  return true;
}

// ImGui's NewFrame handles B/Escape before dialogs draw. Remember which popup
// owned navigation first, so dismissing a combo cannot also dismiss its parent.
inline void SnapshotGuideBackTarget() {
  auto& g = *ImGui::GetCurrentContext();
  ImGuiWindow* target = g.NavWindow;
  if (!g.OpenPopupStack.empty()) {
    target = g.OpenPopupStack.back().Window;
  }
  guide_back_target = {&g, target ? target->RootWindow->ID : 0,
                       g.FrameCount + 1, g.ActiveId != 0, false};
}

inline bool ConsumeGuideBackPress() {
  auto& g = *ImGui::GetCurrentContext();
  auto* window = ImGui::GetCurrentWindow();
  auto& target = guide_back_target;
  if (target.context != &g || target.frame != g.FrameCount || target.consumed ||
      target.active_item || !window ||
      target.window_id != window->RootWindow->ID ||
      ImGui::IsWindowAppearing()) {
    return false;
  }
  if (!ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) &&
      !ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    return false;
  }
  target.consumed = true;
  return true;
}

}  // namespace xe::app
#endif  // XENIA_UI_GUIDE_INPUT_H_
