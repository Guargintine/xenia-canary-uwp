// Copyright 2026 Xenia Canary. Released under the BSD license.

#ifndef XENIA_KERNEL_XAM_UI_FRIEND_REMOVAL_CONFIRMATION_H_
#define XENIA_KERNEL_XAM_UI_FRIEND_REMOVAL_CONFIRMATION_H_

#include <functional>

#include "third_party/imgui/imgui.h"

namespace xe::kernel::xam::ui {

struct FriendDialogPresentation {
  ImVec2 position;
  ImVec2 size;
  ImVec2 padding;
  std::function<void(const char*)> draw_guide;
  std::function<bool(const char*, bool*)> begin_guide;
  std::function<void()> end_guide;
  std::function<bool()> consume_back;
  std::function<bool(const char*, ImVec2)> button;
};

inline bool BeginFriendPopup(
    const char* name, bool* open, ImGuiWindowFlags flags,
    const FriendDialogPresentation* presentation = nullptr) {
  if (presentation && presentation->begin_guide) {
    return presentation->begin_guide(name, open);
  }
  if (presentation) {
    ImGui::SetNextWindowPos(presentation->position);
    ImGui::SetNextWindowSize(presentation->size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, presentation->padding);
    flags &= ~(ImGuiWindowFlags_AlwaysAutoResize |
               ImGuiWindowFlags_AlwaysVerticalScrollbar);
    flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
  }
  const bool visible = ImGui::BeginPopupModal(name, open, flags);
  if (presentation) {
    ImGui::PopStyleVar();
  }
  if (!visible) {
    return false;
  }
  if (ImGui::IsWindowAppearing()) {
    // Default focus does not make ImGui's navigation cursor visible. Without
    // this, A/Enter may be ignored until a directional key is pressed first.
    ImGui::SetNavCursorVisible(true);
  }
  if (presentation) {
    presentation->draw_guide(name);
  }
  return true;
}

inline bool FriendPopupButton(const char* label, const ImVec2& size,
                              const FriendDialogPresentation* presentation) {
  return presentation && presentation->button
             ? presentation->button(label, size)
             : ImGui::Button(label, size);
}

inline void EndFriendPopup(
    const FriendDialogPresentation* presentation = nullptr) {
  if (presentation && presentation->end_guide) {
    presentation->end_guide();
  } else {
    ImGui::EndPopup();
  }
}

inline bool ConsumeFriendPopupBackPress(
    const FriendDialogPresentation* presentation = nullptr) {
  if (presentation && presentation->consume_back) {
    return presentation->consume_back();
  }
  return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows |
                                ImGuiFocusedFlags_NoPopupHierarchy) &&
         !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
         (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
          ImGui::IsKeyPressed(ImGuiKey_Escape, false));
}

// A child dialog must not activate its first control with the press that
// opened it. Keep this independent from focus so the user can still navigate.
inline bool FriendPopupOpeningInputHeld() {
  const ImGuiID id = ImGui::GetID("##OpeningInputHeld");
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const bool waiting = ImGui::IsWindowAppearing() || storage->GetBool(id, true);
  const bool activating = ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) ||
                          ImGui::IsKeyDown(ImGuiKey_GamepadFaceUp) ||
                          ImGui::IsKeyDown(ImGuiKey_Enter) ||
                          ImGui::IsKeyDown(ImGuiKey_KeypadEnter) ||
                          ImGui::IsKeyDown(ImGuiKey_Space) ||
                          ImGui::IsMouseDown(ImGuiMouseButton_Left);
  storage->SetBool(id, waiting && activating);
  return waiting;
}

// Draw in the same ID scope as OpenPopup. A new dialog always selects Cancel,
// and cannot confirm until the input that opened it has been released.
inline bool DrawFriendRemovalConfirmation(
    const char* popup_name, const char* message,
    const FriendDialogPresentation* presentation = nullptr) {
  if (!presentation) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  }
  if (!BeginFriendPopup(popup_name, nullptr, ImGuiWindowFlags_AlwaysAutoResize,
                        presentation)) {
    return false;
  }

  const ImGuiID waiting_id = ImGui::GetID("##WaitingForOpeningRelease");
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const bool waiting_for_release =
      ImGui::IsWindowAppearing() || storage->GetBool(waiting_id, true);
  const bool activating = ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) ||
                          ImGui::IsKeyDown(ImGuiKey_GamepadFaceUp) ||
                          ImGui::IsKeyDown(ImGuiKey_Enter) ||
                          ImGui::IsKeyDown(ImGuiKey_KeypadEnter) ||
                          ImGui::IsKeyDown(ImGuiKey_Space) ||
                          ImGui::IsMouseDown(ImGuiMouseButton_Left);
  storage->SetBool(waiting_id, waiting_for_release && activating);

  ImGui::TextWrapped("%s", message);
  if (!presentation) {
    ImGui::Separator();
  }

  const ImVec2 button_size(presentation ? (ImGui::GetContentRegionAvail().x -
                                           ImGui::GetStyle().ItemSpacing.x) *
                                              0.5f
                                        : 120.0f,
                           presentation ? ImGui::GetFrameHeight() : 25.0f);
  const bool cancel = FriendPopupButton("Cancel", button_size, presentation);
  ImGui::SetItemDefaultFocus();
  ImGui::SameLine();
  ImGui::BeginDisabled(waiting_for_release);
  const bool confirmed = FriendPopupButton("Remove", button_size, presentation);
  ImGui::EndDisabled();

  const bool back = ConsumeFriendPopupBackPress(presentation);
  if (cancel || confirmed || back) {
    ImGui::CloseCurrentPopup();
  }
  EndFriendPopup(presentation);
  return confirmed;
}

}  // namespace xe::kernel::xam::ui

#endif  // XENIA_KERNEL_XAM_UI_FRIEND_REMOVAL_CONFIRMATION_H_
