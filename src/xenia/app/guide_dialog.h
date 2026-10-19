// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_GUIDE_DIALOG_H_
#define XENIA_APP_GUIDE_DIALOG_H_

#include "xenia/app/ui_text_effect_helpers.h"
#include "xenia/ui/guide_input.h"
#include "xenia/ui/guide_layout.h"

namespace xe::app {

struct GuideDialogState {
  GuideBackgroundTextureCache background;
  std::shared_ptr<ui::ImmediateTexture> button_a;
  std::shared_ptr<ui::ImmediateTexture> button_b;
  ui::ImGuiDrawer* drawer = nullptr;
};

// Call after OpenPopup, in the same ID scope. A successful begin opens both the
// modal and its scrolling content child; always pair it with EndGuidePopup.
inline bool BeginGuidePopup(const char* name, bool* open,
                            GuideDialogState& state,
                            ui::ImGuiDrawer* drawer = nullptr,
                            bool prefer_wide = false) {
  if (drawer) {
    state.drawer = drawer;
  }
  if (state.drawer && !state.button_a) {
    state.button_a = LoadButtonTexture(state.drawer, 'A');
  }
  if (state.drawer && !state.button_b) {
    state.button_b = LoadButtonTexture(state.drawer, 'B');
  }
  const auto background = state.background.Get(state.drawer, prefer_wide);
  auto* viewport = ImGui::GetMainViewport();
  ImVec2 size = GetGuidePanelSize(background, viewport->Size.y);
  size.x = std::min(size.x, viewport->Size.x);
  auto bounds = ui::GetGuideContentBounds(size.x, viewport->Size.x);
  if (prefer_wide) {
    bounds.left = 32.0f * viewport->Size.x / 1024.0f;
    bounds.right = std::max(bounds.left + 1.0f, size.x * 0.90f);
  }
  const ImVec2 pos = viewport->Pos;
  const float ux = viewport->Size.x / 1024.0f;
  const float uy = viewport->Size.y / 576.0f;
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const bool visible = ImGui::BeginPopupModal(
      name, open,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar(3);
  if (!visible) {
    return false;
  }
  if (state.drawer) {
    state.drawer->RetainTextureForFrame(state.button_a);
    state.drawer->RetainTextureForFrame(state.button_b);
  }
  // Track release of the opening input separately for each modal.
  bool* activation_ready = ImGui::GetStateStorage()->GetBoolRef(
      ImGui::GetID("##guide_activation_ready"), false);
  if (ImGui::IsWindowAppearing()) {
    *activation_ready = false;
    ImGui::SetNavCursorVisible(true);
  }
  const bool was_activation_ready = *activation_ready;
  if (!ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) &&
      !ImGui::IsKeyDown(ImGuiKey_Enter) &&
      !ImGui::IsKeyDown(ImGuiKey_KeypadEnter) &&
      !ImGui::IsKeyDown(ImGuiKey_Space) && !ImGui::IsAnyMouseDown()) {
    *activation_ready = true;
  }
  const bool enable_content = *activation_ready;
  const bool activate_focus = enable_content && !was_activation_ready;
  auto* draw_list = ImGui::GetWindowDrawList();
  const ImVec2 panel_max(pos.x + size.x, pos.y + size.y);
  DrawGuidePanelBackground(draw_list, background, pos, panel_max);
  const std::string title =
      std::string(name).substr(0, std::string(name).find("##"));
  draw_list->PushClipRect(pos, ImVec2(pos.x + bounds.right, panel_max.y), true);
  DrawOverlayHeader(draw_list, pos, size, GetGuidePanelPadding(), ux, uy,
                    title.c_str(), false);
  draw_list->PopClipRect();
  DrawFooterPrompt(draw_list, state.button_b, 13.5f * uy, 15.6f * uy, "Back",
                   panel_max.y - 42.0f * uy, pos.x + bounds.left + 20.0f * ux,
                   -20.0f * ux);
  DrawFooterPrompt(draw_list, state.button_a, 13.5f * uy, 15.6f * uy, "Select",
                   panel_max.y - 25.0f * uy, pos.x + bounds.left + 20.0f * ux,
                   -20.0f * ux);
  const auto body = ui::GetGuideBodyBounds(size.y);
  const float content_y = pos.y + body.top;
  ImGui::SetCursorScreenPos(ImVec2(pos.x + bounds.left, content_y));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild(
      "##guide_content",
      ImVec2(bounds.width(), std::max(1.0f, body.height())),
      ImGuiChildFlags_None,
      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  ImGui::BeginDisabled(!enable_content);
  if (activate_focus) {
    InitializeGuideNavigation();
  }
  ImGui::PushItemWidth(-1.0f);
  ImGui::PushTextWrapPos(0.0f);
  return true;
}

inline void EndGuidePopup() {
  ImGui::PopTextWrapPos();
  ImGui::PopItemWidth();
  ImGui::EndDisabled();
  ImGui::EndChild();
  ImGui::EndPopup();
}

inline float GuideContentWidth() {
  return std::max(1.0f, ImGui::GetContentRegionAvail().x);
}

inline bool GuideButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
  const float remaining = GuideContentWidth();
  size.x = size.x <= 0.0f ? remaining : std::min(size.x, remaining);
  return DrawTextEffectButton(label, size);
}

}  // namespace xe::app
#endif  // XENIA_APP_GUIDE_DIALOG_H_
