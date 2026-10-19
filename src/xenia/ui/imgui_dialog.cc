/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_dialog.h"

#include "third_party/imgui/imgui.h"
#include "xenia/base/assert.h"
#include "xenia/ui/imgui_drawer.h"
#if XE_PLATFORM_WINRT
#include "xenia/app/guide_dialog.h"
#endif

namespace xe {
namespace ui {

std::atomic<uint64_t> ImGuiDialog::next_window_id_ = 0;

ImGuiDialog::ImGuiDialog(ImGuiDrawer* imgui_drawer)
    : imgui_drawer_(imgui_drawer) {
  imgui_drawer_->AddDialog(this);
  next_window_id_++;
}

ImGuiDialog::~ImGuiDialog() {
  imgui_drawer_->RemoveDialog(this);
  for (auto fence : waiting_fences_) {
    fence->Signal();
  }
  for (auto& callback : close_callbacks_) {
    callback();
  }
}

void ImGuiDialog::Then(xe::threading::Fence* fence) {
  waiting_fences_.push_back(fence);
}

void ImGuiDialog::Then(std::function<void()> callback) {
  close_callbacks_.push_back(std::move(callback));
}

void ImGuiDialog::Close() { has_close_pending_ = true; }

ImGuiIO& ImGuiDialog::GetIO() { return imgui_drawer()->GetIO(); }

void ImGuiDialog::Draw() {
  // Draw UI.
  if (!has_close_pending_) {
    OnDraw(GetIO());
  }

  // Check to see if the UI closed itself and needs to be deleted.
  if (has_close_pending_) {
    OnClose();
    delete this;
  }
}

class MessageBoxDialog final : public ImGuiDialog {
 public:
  MessageBoxDialog(ImGuiDrawer* imgui_drawer, std::string title,
                   std::string body)
      : ImGuiDialog(imgui_drawer),
        title_(std::move(title)),
        body_(std::move(body)) {}

  void OnDraw(ImGuiIO& io) override {
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
    }
#if XE_PLATFORM_WINRT
    if (xe::app::BeginGuidePopup(title_.c_str(), nullptr, guide_state_,
                                  imgui_drawer())) {
      ImGui::TextWrapped("%s", body_.c_str());
      if (xe::app::GuideButton("OK") || xe::app::ConsumeGuideBackPress()) {
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::SetItemDefaultFocus();
      xe::app::EndGuidePopup();
    } else {
      Close();
    }
#else
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      char* text = const_cast<char*>(body_.c_str());
      ImGui::InputTextMultiline(
          "##body", text, body_.size() + 1, ImVec2(600, 0),
          ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_ReadOnly);
      if (ImGui::Button("OK")) {
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::EndPopup();
    } else {
      Close();
    }
#endif
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string body_;
#if XE_PLATFORM_WINRT
  xe::app::GuideDialogState guide_state_;
#endif
};

ImGuiDialog* ImGuiDialog::ShowMessageBox(ImGuiDrawer* imgui_drawer,
                                         std::string title, std::string body) {
  return new MessageBoxDialog(imgui_drawer, std::move(title), std::move(body));
}

}  // namespace ui
}  // namespace xe
