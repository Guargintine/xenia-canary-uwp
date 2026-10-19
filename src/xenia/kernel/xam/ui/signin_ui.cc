/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <memory>

#include "xenia/kernel/xam/ui/signin_ui.h"

#include "xenia/emulator.h"

#include "xenia/app/ui_text_effect_helpers.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/ui/window.h"
#if XE_PLATFORM_WINRT
#include "xenia-canary-uwp/WinRTKeyboard.h"
#include "xenia/ui/guide_input.h"
#endif

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

SigninUI::SigninUI(xe::ui::ImGuiDrawer* imgui_drawer,
                   ProfileManager* profile_manager, uint32_t last_used_slot,
                   uint32_t users_needed, uint32_t flags)
    : XamDialog(imgui_drawer),
      profile_manager_(profile_manager),
      last_user_(last_used_slot),
      users_needed_(users_needed),
      flags_(flags),
      title_("Sign In") {
  if (flags_ & X_UI_FLAGS_ONLINEENABLED) {
    title_ = "Sign In - Xbox Live Profiles";
  }

  const auto gamerpic_key = CreateProfileUI::GetDefaultGamerPictureKey();

  if (gamerpic_key.has_value()) {
    create_profile_args_.gamerpic_key = gamerpic_key;
    create_profile_args_.downloaded_gamerpics =
        kernel_state()->GetXboxLiveAPI()->DownloadCompleteGamerpic(
            gamerpic_key.value());
  }
}

void SigninUI::OnDraw(ImGuiIO& io) {
#if XE_PLATFORM_WINRT
  constexpr const char* kSigninPopupName = "##signin_popup";

  if (!has_opened_) {
    has_opened_ = true;
    ReloadProfiles(true, flags_);
    ImGui::OpenPopup(kSigninPopupName);
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGuiWindowFlags popup_flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
  if (focus_requested_) {
    ImGui::SetNextWindowFocus();
    focus_requested_ = false;
  }

  if (!ImGui::BeginPopupModal(kSigninPopupName, nullptr, popup_flags)) {
    Close();
    return;
  }

  // Cancel only when this modal owned B before NewFrame. A combo or the
  // Create Profile child consumes its own back press without closing sign-in.
  if (xe::app::ConsumeGuideBackPress()) {
    ImGui::CloseCurrentPopup();
    Close();
    ImGui::EndPopup();
    return;
  }

  ImDrawList* root_draw_list = ImGui::GetWindowDrawList();
  const ImVec2 screen_min = viewport->Pos;
  const ImVec2 screen_max =
      ImVec2(screen_min.x + viewport->Size.x, screen_min.y + viewport->Size.y);
  root_draw_list->AddRectFilled(
      screen_min, screen_max,
      ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f)));

  static xe::app::GuideBackgroundTextureCache guide_background;
  const auto guide_bg_tex = guide_background.Get(imgui_drawer());

  const ImVec2 panel_size =
      xe::app::GetGuidePanelSize(guide_bg_tex, io.DisplaySize.y);
  const ImVec2 panel_padding = xe::app::GetGuidePanelPadding();
  const float ux = io.DisplaySize.x / 1024.0f;
  const float uy = io.DisplaySize.y / 576.0f;

  const ImVec2 overlay_min = viewport->Pos;
  const ImVec2 overlay_max(overlay_min.x + panel_size.x,
                           overlay_min.y + panel_size.y);

  xe::app::DrawGuidePanelBackground(root_draw_list, guide_bg_tex, overlay_min,
                                    overlay_max);

  const xe::app::OverlayHeaderLayout header_layout =
      xe::app::DrawOverlayHeader(root_draw_list, overlay_min, panel_size,
                                 panel_padding, ux, uy, title_.c_str());

  const float content_start_y =
      header_layout.position.y + header_layout.font_size + 65.0f;
  const float field_width = 320.0f * ux;
  ImGui::SetCursorScreenPos(
      ImVec2(overlay_min.x + panel_padding.x, content_start_y));
  ImGui::PushItemWidth(field_width);

  auto draw_signin_combo = [&](const char* id, std::vector<const char*>& items,
                               int& current_index) {
    if (items.empty()) {
      items.push_back("---");
      current_index = 0;
    }
    current_index = std::clamp(current_index, 0, (int)items.size() - 1);
    xe::app::ScopedAccentComboStyle combo_style;
    const char* preview = items[current_index];
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    bool open = ImGui::BeginCombo(id, preview);
    ImVec2 preview_min = ImGui::GetItemRectMin();
    ImVec2 preview_max = ImGui::GetItemRectMax();
    ImGui::PopStyleColor();
    xe::app::DrawConfiguredComboText(preview_min, preview_max, preview);
    bool changed = false;
    if (open) {
      for (int idx = 0; idx < static_cast<int>(items.size()); ++idx) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        bool is_selected = current_index == idx;
        if (ImGui::Selectable(items[idx], is_selected)) {
          current_index = idx;
          changed = true;
        }
        ImVec2 item_min = ImGui::GetItemRectMin();
        ImVec2 item_max = ImGui::GetItemRectMax();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
          xe::app::DrawConfiguredComboHighlight(item_min, item_max);
        }
        xe::app::DrawConfiguredComboText(item_min, item_max, items[idx]);
        if (is_selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    return changed;
  };

  for (uint32_t i = 0; i < users_needed_; i++) {
    ImGui::BeginGroup();

    std::vector<const char*> combo_items;
    int items_count = 0;
    int current_item = 0;

    std::vector<uint8_t> slots;
    slots.push_back(XUserIndexAny);
    combo_items.push_back("---");
    for (auto& elem : slot_data_) {
      bool already_taken = false;
      for (uint32_t j = 0; j < users_needed_; j++) {
        if (chosen_slots_[j] == elem.first) {
          if (i == j) {
            current_item = static_cast<int>(combo_items.size());
          } else {
            already_taken = true;
          }
          break;
        }
      }

      if (already_taken) {
        continue;
      }

      slots.push_back(elem.first);
      combo_items.push_back(elem.second.c_str());
    }
    items_count = static_cast<int>(combo_items.size());

    ImGui::BeginDisabled(users_needed_ == 1);
    draw_signin_combo(fmt::format("##Slot{:d}", i).c_str(), combo_items,
                      current_item);
    chosen_slots_[i] = slots[current_item];
    ImGui::EndDisabled();
    ImGui::Spacing();

    combo_items.clear();
    current_item = 0;

    std::vector<uint64_t> xuids;
    xuids.push_back(0);
    combo_items.push_back("---");
    if (chosen_slots_[i] != XUserIndexAny) {
      for (auto& elem : profile_data_) {
        bool already_taken = false;
        for (uint32_t j = 0; j < users_needed_; j++) {
          if (chosen_xuids_[j] == elem.first) {
            if (i == j) {
              current_item = static_cast<int>(combo_items.size());
            } else {
              already_taken = true;
            }
            break;
          }
        }

        if (already_taken) {
          continue;
        }

        xuids.push_back(elem.first);
        combo_items.push_back(elem.second.c_str());
      }
    }
    items_count = static_cast<int>(combo_items.size());

    ImGui::BeginDisabled(chosen_slots_[i] == XUserIndexAny);
    draw_signin_combo(fmt::format("##Profile{:d}", i).c_str(), combo_items,
                      current_item);
    chosen_xuids_[i] = xuids[current_item];
    ImGui::EndDisabled();
    ImGui::Spacing();

    uint8_t slot = chosen_slots_[i];
    uint64_t xuid = chosen_xuids_[i];
    const auto account = profile_manager_->GetAccount(xuid);

    if (slot != XUserIndexAny && account) {
      xeDrawProfileContent(imgui_drawer(), xuid, slot, account, nullptr, {}, {},
                           nullptr);
    }

    ImGui::EndGroup();
    if (i != (users_needed_ - 1) && (i == 0 || i == 2)) {
      ImGui::SameLine();
    }
  }
  ImGui::PopItemWidth();

  ImGui::Spacing();

  // Create Profile button with same width as combo boxes
  const ImVec2 create_button_size(field_width, 0.0f);
  ImGui::SetCursorScreenPos(
      ImVec2(overlay_min.x + panel_padding.x, ImGui::GetCursorScreenPos().y));
  if (xe::app::DrawTextEffectButton("Create Profile", create_button_size)) {
    creating_profile_ = true;
  }
  ImGui::Spacing();

  // OK and Cancel buttons side-by-side with same total width as combo boxes
  const float button_gap = 8.0f * ux;
  const ImVec2 ok_cancel_button_size((field_width - button_gap) / 2.0f, 0.0f);
  ImGui::SetCursorScreenPos(
      ImVec2(overlay_min.x + panel_padding.x, ImGui::GetCursorScreenPos().y));
  bool can_confirm = false;
  for (uint32_t i = 0; i < users_needed_; i++) {
    if (chosen_slots_[i] != XUserIndexAny && chosen_xuids_[i] != 0) {
      can_confirm = true;
      break;
    }
  }
  ImGui::BeginDisabled(!can_confirm);
  if (xe::app::DrawTextEffectButton("OK", ok_cancel_button_size)) {
    std::map<uint8_t, uint64_t> profile_map;
    for (uint32_t i = 0; i < users_needed_; i++) {
      uint8_t slot = chosen_slots_[i];
      uint64_t xuid = chosen_xuids_[i];
      if (slot != XUserIndexAny && xuid != 0) {
        profile_map[slot] = xuid;
      }
    }
    profile_manager_->LoginMultiple(profile_map);
    ImGui::CloseCurrentPopup();
    Close();
  }
  ImGui::EndDisabled();
  ImGui::SameLine(0.0f, button_gap);

  if (xe::app::DrawTextEffectButton("Cancel", ok_cancel_button_size)) {
    ImGui::CloseCurrentPopup();
    Close();
  }

  const float footer_text_size = 13.5f * uy;
  const float footer_icon_size = 15.6f * uy;
  const float footer_spacing_y = 17.0f * uy;
  const float footer_select_y = overlay_max.y - (32.0f * uy);
  const float footer_back_y = footer_select_y - footer_spacing_y;
  const float footer_base_x = overlay_min.x + panel_size.x * 0.9f;
  const float footer_back_text_x = footer_base_x - (70.0f * ux);
  const float footer_back_icon_offset = 35.0f * ux;
  const float footer_select_text_x = footer_base_x - (60.0f * ux);
  const float footer_select_icon_offset = 42.0f * ux;
  xe::app::DrawFooterPrompt(root_draw_list, GetOrCreateButtonTexture('B'),
                            footer_text_size, footer_icon_size, "Back",
                            footer_back_y, footer_back_text_x,
                            footer_back_icon_offset);
  xe::app::DrawFooterPrompt(root_draw_list, GetOrCreateButtonTexture('A'),
                            footer_text_size, footer_icon_size, "Select",
                            footer_select_y, footer_select_text_x,
                            footer_select_icon_offset);

  if (creating_profile_) {
    if (!create_profile_args_.dialog_open) {
      create_profile_args_.dialog_open = true;
      ImGui::OpenPopup("Create Profile");
    }
    if (!xeDrawCreateProfile(imgui_drawer(), kernel_state()->emulator(),
                             create_profile_args_)) {
      create_profile_args_.dialog_open = false;
      creating_profile_ = false;
      focus_requested_ = true;
      ReloadProfiles(false, flags_);
    }
  }
  ImGui::EndPopup();
#else

  if (!has_opened_) {
    ImGui::OpenPopup(title_.c_str());
    has_opened_ = true;
    ReloadProfiles(true, flags_);
  }
  if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    for (uint32_t i = 0; i < users_needed_; i++) {
      ImGui::BeginGroup();

      std::vector<const char*> combo_items;
      int items_count = 0;
      int current_item = 0;

      // Fill slot list.
      std::vector<uint8_t> slots;
      slots.push_back(XUserIndexAny);
      combo_items.push_back("---");
      for (auto& elem : slot_data_) {
        // Select the slot or skip it if it's already used.
        bool already_taken = false;
        for (uint32_t j = 0; j < users_needed_; j++) {
          if (chosen_slots_[j] == elem.first) {
            if (i == j) {
              current_item = static_cast<int>(combo_items.size());
            } else {
              already_taken = true;
            }
            break;
          }
        }

        if (already_taken) {
          continue;
        }

        slots.push_back(elem.first);
        combo_items.push_back(elem.second.c_str());
      }
      items_count = static_cast<int>(combo_items.size());

      ImGui::BeginDisabled(users_needed_ == 1);
      ImGui::Combo(fmt::format("##Slot{:d}", i).c_str(), &current_item,
                   combo_items.data(), items_count);
      chosen_slots_[i] = slots[current_item];
      ImGui::EndDisabled();
      ImGui::Spacing();

      combo_items.clear();
      current_item = 0;

      // Fill profile list.
      std::vector<uint64_t> xuids;
      xuids.push_back(0);
      combo_items.push_back("---");
      if (chosen_slots_[i] != XUserIndexAny) {
        for (auto& elem : profile_data_) {
          // Select the profile or skip it if it's already used.
          bool already_taken = false;
          for (uint32_t j = 0; j < users_needed_; j++) {
            if (chosen_xuids_[j] == elem.first) {
              if (i == j) {
                current_item = static_cast<int>(combo_items.size());
              } else {
                already_taken = true;
              }
              break;
            }
          }

          if (already_taken) {
            continue;
          }

          xuids.push_back(elem.first);
          combo_items.push_back(elem.second.c_str());
        }
      }
      items_count = static_cast<int>(combo_items.size());

      ImGui::BeginDisabled(chosen_slots_[i] == XUserIndexAny);
      ImGui::Combo(fmt::format("##Profile{:d}", i).c_str(), &current_item,
                   combo_items.data(), items_count);
      chosen_xuids_[i] = xuids[current_item];
      ImGui::EndDisabled();
      ImGui::Spacing();

      // Draw profile badge.
      uint8_t slot = chosen_slots_[i];
      uint64_t xuid = chosen_xuids_[i];
      const auto account = profile_manager_->GetAccount(xuid);

      if (slot != XUserIndexAny && account) {
        xeDrawProfileContent(imgui_drawer(), xuid, slot, account, nullptr, {},
                             {}, nullptr);
      }

      ImGui::EndGroup();
      if (i != (users_needed_ - 1) && (i == 0 || i == 2)) {
        ImGui::SameLine();
      }
    }

    ImGui::Spacing();

    if (ImGui::Button("Create Profile")) {
      create_profile_args_.dialog_open = true;
      ImGui::OpenPopup("Create Profile");
    }
    ImGui::Spacing();

    if (create_profile_args_.dialog_open) {
      if (!xeDrawCreateProfile(imgui_drawer(), kernel_state()->emulator(),
                               create_profile_args_)) {
        ReloadProfiles(false, flags_);
      }
    }

    if (ImGui::Button("OK")) {
      std::map<uint8_t, uint64_t> profile_map;
      for (uint32_t i = 0; i < users_needed_; i++) {
        uint8_t slot = chosen_slots_[i];
        uint64_t xuid = chosen_xuids_[i];
        if (slot != XUserIndexAny && xuid != 0) {
          profile_map[slot] = xuid;
        }
      }
      profile_manager_->LoginMultiple(profile_map);

      ImGui::CloseCurrentPopup();
      Close();
    }
    ImGui::SameLine();

    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
      Close();
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::EndPopup();
  } else {
    Close();
  }
#endif
}

void SigninUI::OnClose() {
#if XE_PLATFORM_WINRT
  // Keep image handles used by this closing frame alive until it is submitted.
  auto frame_resources =
      std::make_shared<CreateProfileUIArgs>(std::move(create_profile_args_));
  kernel_state()
      ->emulator()
      ->display_window()
      ->app_context()
      .CallInUIThreadDeferred([frame_resources = std::move(frame_resources),
                               button_a = std::move(button_a_tex_),
                               button_b = std::move(button_b_tex_)]() {});
#endif
  XamDialog::OnClose();
}

std::shared_ptr<xe::ui::ImmediateTexture> SigninUI::GetOrCreateButtonTexture(
    char button) {
  std::shared_ptr<xe::ui::ImmediateTexture>* dst = nullptr;
  switch (button) {
    case 'A':
      dst = &button_a_tex_;
      break;
    case 'B':
      dst = &button_b_tex_;
      break;
    default:
      return nullptr;
  }

  if (*dst) {
    return *dst;
  }

  *dst = xe::app::LoadButtonTexture(imgui_drawer(), button);
  return *dst;
}

void SigninUI::ReloadProfiles(bool first_draw, uint32_t flags) {
  auto profile_manager = kernel_state()->xam_state()->profile_manager();
  auto profiles = profile_manager->GetAccounts();

  profile_data_.clear();
  for (auto& [xuid, account] : *profiles) {
    if (flags_ & X_UI_FLAGS_ONLINEENABLED) {
      if (account.IsLiveEnabled()) {
        profile_data_.push_back({xuid, account.GetGamertagString()});
      }
    } else {
      profile_data_.push_back({xuid, account.GetGamertagString()});
    }
  }

  if (first_draw) {
    // If only one user is requested, request last used controller to sign in.
    if (users_needed_ == 1) {
      chosen_slots_[0] = last_user_;
    } else {
      for (uint32_t i = 0; i < users_needed_; i++) {
        // TODO: Not sure about this, needs testing on real hardware.
        chosen_slots_[i] = i;
      }
    }

    // Default profile selection to profile that is already signed in.
    for (auto& elem : profile_data_) {
      uint64_t xuid = elem.first;
      uint8_t slot = profile_manager->GetUserIndexAssignedToProfile(xuid);
      for (uint32_t j = 0; j < users_needed_; j++) {
        if (chosen_slots_[j] != XUserIndexAny && slot == chosen_slots_[j]) {
          chosen_xuids_[j] = xuid;
        }
      }
    }
  }
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
