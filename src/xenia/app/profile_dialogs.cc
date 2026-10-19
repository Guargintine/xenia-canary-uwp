/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/profile_dialogs.h"

#include <array>

#include "third_party/stb/stb_image.h"
#include "xenia/app/emulator_window.h"
#include "xenia/app/ui_text_effect_helpers.h"
#include "xenia/base/png_utils.h"
#include "xenia/base/system.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/guide_layout.h"
#include "xenia/ui/guide_input.h"
#include "xenia/ui/imgui_host_notification.h"

#include <xenia/vfs/devices/host_path_entry.h>
#include "xenia/kernel/xam/ui/create_profile_ui.h"
#include "xenia/kernel/xam/ui/friend_removal_confirmation.h"
#include "xenia/kernel/xam/ui/game_achievements_ui.h"
#include "xenia/kernel/xam/ui/gamercard_ui.h"
#include "xenia/kernel/xam/ui/signin_ui.h"
#include "xenia/kernel/xam/ui/title_info_ui.h"

namespace xe {
namespace app {

void NoProfileDialog::OnDraw(ImGuiIO& io) {
#if XE_PLATFORM_WINRT
  auto profile_manager = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager();

  if (profile_manager->GetAccountCount()) {
    Close();
    return;
  }

  const std::string message =
      "There is no profile available! You will not be able to save\n"
      "without one.\n\nWould you like to create one?";

  const auto content_files = xe::filesystem::ListDirectories(
      emulator_window_->emulator()->content_root());

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
  if (focus_requested_) {
    ImGui::SetNextWindowFocus();
  }

  if (!ImGui::Begin("##no_profile_overlay", nullptr, flags)) {
    ImGui::End();
    return;
  }

  ImDrawList* root_draw_list = ImGui::GetWindowDrawList();
  const ImVec2 screen_min = viewport->Pos;
  const ImVec2 screen_max(screen_min.x + viewport->Size.x,
                          screen_min.y + viewport->Size.y);
  root_draw_list->AddRectFilled(
      screen_min, screen_max,
      ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f)));

  static xe::app::GuideBackgroundTextureCache guide_background;
  const auto guide_bg_tex = guide_background.Get(imgui_drawer());
  static std::shared_ptr<xe::ui::ImmediateTexture> button_a_tex = nullptr;
  static std::shared_ptr<xe::ui::ImmediateTexture> button_b_tex = nullptr;
  if (!button_a_tex) {
    button_a_tex = xe::app::LoadButtonTexture(imgui_drawer(), 'A');
  }
  if (!button_b_tex) {
    button_b_tex = xe::app::LoadButtonTexture(imgui_drawer(), 'B');
  }

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

  const bool child_dialog_open = imgui_drawer()->GetDialogCount() > 2;
  bool should_close = false;
  bool should_open_create_profile = false;
  bool should_open_profile_menu = false;
  if (!child_dialog_open &&
      ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
    should_close = true;
  }

  const xe::app::OverlayHeaderLayout header_layout =
      xe::app::DrawOverlayHeader(root_draw_list, overlay_min, panel_size,
                                 panel_padding, ux, uy, "No Profiles Found");

  const float content_start_y =
      header_layout.position.y + header_layout.font_size + 55.0f;
  const float field_width = 320.0f * ux;
  ImGui::SetCursorScreenPos(
      ImVec2(overlay_min.x + panel_padding.x, content_start_y));
  ImGui::PushTextWrapPos(overlay_min.x + panel_padding.x + field_width);
  DrawConfiguredParagraph(message);
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0.0f, 14.0f * uy));

  const ImVec2 button_size(field_width, 0.0f);
  const char* create_label = content_files.empty()
                                 ? "Create Profile"
                                 : "Create profile & migrate data";
  if (focus_requested_) {
    ImGui::SetKeyboardFocusHere();
    focus_requested_ = false;
  }

  ImGui::BeginDisabled(child_dialog_open);
  ImGui::SetCursorScreenPos(
      ImVec2(overlay_min.x + panel_padding.x, ImGui::GetCursorScreenPos().y));
  if (DrawTextEffectButton(create_label, button_size)) {
    should_open_create_profile = true;
  }
  ImGui::Spacing();
  ImGui::SetCursorScreenPos(
      ImVec2(overlay_min.x + panel_padding.x, ImGui::GetCursorScreenPos().y));
  if (DrawTextEffectButton("Open profile menu", button_size)) {
    should_open_profile_menu = true;
  }
  ImGui::Spacing();
  ImGui::SetCursorScreenPos(
      ImVec2(overlay_min.x + panel_padding.x, ImGui::GetCursorScreenPos().y));
  if (DrawTextEffectButton("Close", button_size)) {
    should_close = true;
  }
  ImGui::EndDisabled();

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
  xe::app::DrawFooterPrompt(root_draw_list, button_b_tex, footer_text_size,
                            footer_icon_size, "Back", footer_back_y,
                            footer_back_text_x, footer_back_icon_offset);
  xe::app::DrawFooterPrompt(root_draw_list, button_a_tex, footer_text_size,
                            footer_icon_size, "Select", footer_select_y,
                            footer_select_text_x, footer_select_icon_offset);

  ImGui::End();

  if (should_open_create_profile) {
    if (content_files.empty()) {
      new kernel::xam::ui::CreateProfileUI(emulator_window_->imgui_drawer(),
                                           emulator_window_->emulator());
    } else {
      new kernel::xam::ui::CreateProfileUI(emulator_window_->imgui_drawer(),
                                           emulator_window_->emulator(), true);
    }
    return;
  }

  if (should_open_profile_menu) {
    emulator_window_->ToggleProfilesConfigDialog();
    return;
  }

  if (should_close) {
    emulator_window_->SetHotkeysState(true);
    Close();
    return;
  }

#else
  auto profile_manager = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager();

  if (profile_manager->GetAccountCount()) {
    Close();
    return;
  }

  const auto window_position =
      ImVec2(GetIO().DisplaySize.x * 0.35f, GetIO().DisplaySize.y * 0.4f);

  ImGui::SetNextWindowPos(window_position, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(1.0f);

  bool dialog_open = true;
  if (!ImGui::Begin("No Profiles Found", &dialog_open,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::End();
    Close();
    return;
  }

  const std::string message =
      "There is no profile available! You will not be able to save without "
      "one.\n\nWould you like to create one?";

  ImGui::TextUnformatted(message.c_str());

  ImGui::Separator();
  ImGui::NewLine();

  const auto content_files = xe::filesystem::ListDirectories(
      emulator_window_->emulator()->content_root());

  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }
  if (content_files.empty()) {
    if (ImGui::Button("Create Profile")) {
      new kernel::xam::ui::CreateProfileUI(emulator_window_->imgui_drawer(),
                                           emulator_window_->emulator());
    }
  } else {
    if (ImGui::Button("Create profile & migrate data")) {
      new kernel::xam::ui::CreateProfileUI(emulator_window_->imgui_drawer(),
                                           emulator_window_->emulator(), true);
    }
  }

  ImGui::SameLine();
  if (ImGui::Button("Open profile menu")) {
    emulator_window_->ToggleProfilesConfigDialog();
  }

  ImGui::SameLine();
  if (ImGui::Button("Close") || !dialog_open) {
    emulator_window_->SetHotkeysState(true);
    ImGui::End();
    Close();
    return;
  }
  ImGui::End();

#endif
}

void ProfileConfigDialog::LoadProfileIcon() {
  if (!emulator_window_) {
    return;
  }

  for (uint8_t user_index = 0; user_index < XUserMaxUserCount; user_index++) {
    const auto profile = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager()
                             ->GetProfile(user_index);

    if (!profile) {
      continue;
    }
    LoadProfileIcon(profile->xuid());
  }
}

void ProfileConfigDialog::LoadProfileIcon(const uint64_t xuid) {
  if (!emulator_window_) {
    return;
  }

  const auto xam_state =
      emulator_window_->emulator()->kernel_state()->xam_state();

  const auto profile_manager = xam_state->profile_manager();
  const auto user_tracker = xam_state->user_tracker();

  if (!profile_manager) {
    return;
  }

  if (!user_tracker) {
    return;
  }

  const auto profile = profile_manager->GetProfile(xuid);

  if (!profile) {
    if (profile_icon_.contains(xuid)) {
      profile_icon_[xuid].reset();
    }

    if (profile_gamerpic_key_.contains(xuid)) {
      profile_gamerpic_key_.erase(xuid);
    }
    return;
  }

  const auto gamerpic_key = user_tracker->GetUserGamerpicSetting(xuid);

  if (gamerpic_key.has_value()) {
    profile_gamerpic_key_[xuid] = gamerpic_key.value();
  }

  const auto profile_icon =
      profile->GetProfileIcon(kernel::xam::XTileType::kGamerTile);
  if (profile_icon.empty()) {
    return;
  }

  profile_icon_[xuid].reset();
  profile_icon_[xuid] = imgui_drawer()->LoadImGuiIcon(profile_icon);
}

void ProfileConfigDialog::LoadSignedInProfilesCount() {
  signed_in_profiles_count_ = emulator_window_->emulator()
                                  ->kernel_state()
                                  ->xam_state()
                                  ->profile_manager()
                                  ->SignedInProfilesCount();
}

bool ProfileConfigDialog::DrawProfileContextMenu(uint64_t xuid) {
  auto profile_manager = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager();
  auto profiles = profile_manager->GetAccounts();
  auto it = profiles->find(xuid);
  if (it == profiles->end()) {
    return true;
  }
  const auto& account = it->second;
  const uint8_t user_index =
      profile_manager->GetUserIndexAssignedToProfile(xuid);
  const ImVec2 next_window_position = ImGui::GetWindowPos();

  if (ImGui::BeginPopup("Profile Menu")) {
    //*selected_xuid = xuid;
    if (user_index == XUserIndexAny) {
      ImGui::BeginDisabled(!profile_manager->IsAnyProfileSlotFree());

      if (ImGui::MenuItem("Login")) {
        profile_manager->Login(xuid);
        if (!profile_manager->GetProfile(xuid)
                 ->GetProfileIcon(kernel::xam::XTileType::kGamerTile)
                 .empty()) {
          LoadProfileIcon(xuid);
        }
      }
      if (ImGui::BeginMenu("Login to slot:")) {
        for (uint8_t i = 1; i <= XUserMaxUserCount; i++) {
          if (ImGui::MenuItem(fmt::format("slot {}", i).c_str())) {
            uint64_t current_slot_xuid = 0;

            if (const auto current_profile =
                    profile_manager->GetProfile(static_cast<uint8_t>(i - 1));
                current_profile) {
              current_slot_xuid = current_profile->xuid();
            }

            profile_manager->Login(xuid, i - 1);
            LoadProfileIcon(xuid);

            // Release resources
            if (current_slot_xuid) {
              LoadProfileIcon(current_slot_xuid);
            }
          }
        }
        ImGui::EndMenu();
      }
      ImGui::EndDisabled();
    } else {
      if (ImGui::MenuItem("Logout")) {
        profile_manager->Logout(user_index);
        LoadProfileIcon(xuid);
      }
    }

    if (ImGui::MenuItem("Modify")) {
      new kernel::xam::ui::GamercardUI(
          emulator_window_->window(), emulator_window_->imgui_drawer(),
          emulator_window_->emulator()->kernel_state(), xuid);
    }

    if (ImGui::BeginMenu("Copy")) {
      if (ImGui::MenuItem("Gamertag")) {
        ImGui::SetClipboardText(account.GetGamertagString().c_str());
      }

      if (ImGui::MenuItem("XUID")) {
        ImGui::SetClipboardText(fmt::format("{:016X}", xuid).c_str());
      }

      if (account.IsLiveEnabled()) {
        if (ImGui::MenuItem("XUID Online")) {
          ImGui::SetClipboardText(
              fmt::format("{:016X}", account.xuid_online.get()).c_str());
        }
      }

      ImGui::EndMenu();
    }

    const bool is_signedin = profile_manager->GetProfile(xuid) != nullptr;
    ImGui::BeginDisabled(!is_signedin);
    if (ImGui::MenuItem("Show Played Titles")) {
      new TitleListUI(emulator_window_->imgui_drawer(), next_window_position,
                      profile_manager->GetProfile(user_index),
                      emulator_window_);
    }
    ImGui::EndDisabled();

    if (ImGui::MenuItem("Show Content Directory")) {
      const auto path = profile_manager->GetProfileContentPath(
          xuid, emulator_window_->emulator()->kernel_state()->title_id());

      if (!std::filesystem::exists(path)) {
        std::filesystem::create_directories(path);
      }

      std::thread path_open(LaunchFileExplorer, path);
      path_open.detach();
    }

    ImGui::BeginDisabled(!account.IsLiveEnabled());
    if (ImGui::BeginMenu("XStorage Content")) {
      const auto emulator = emulator_window_->emulator();
      const auto filesystem = emulator->kernel_state()->file_system();

      xe::vfs::HostPathEntry* entry = nullptr;
      std::string symlink_path;

      if (ImGui::MenuItem("Show Game Data")) {
        if (emulator->is_title_open()) {
          symlink_path =
              fmt::format("XSTORAGE:user\\{:016X}\\title\\{:08X}",
                          account.xuid_online.get(), emulator->title_id());

        } else {
          symlink_path =
              fmt::format("XSTORAGE:user\\{:016X}", account.xuid_online.get());
        }
      }

      ImGui::BeginDisabled(!emulator->is_title_open());
      if (ImGui::MenuItem("Show Game Clips")) {
        symlink_path =
            fmt::format("XSTORAGE:clips\\title\\{:08X}\\{:016X}",
                        emulator->title_id(), account.xuid_online.get());
      }
      ImGui::EndDisabled();

      if (!symlink_path.empty()) {
        entry = reinterpret_cast<xe::vfs::HostPathEntry*>(
            filesystem->ResolvePath(symlink_path));

        if (entry) {
          std::thread path_open(LaunchFileExplorer, entry->host_path());
          path_open.detach();
        }
      }

      ImGui::EndMenu();
    }
    ImGui::EndDisabled();

    if (!emulator_window_->emulator()->is_title_open()) {
#if !XE_PLATFORM_WINRT
      ImGui::Separator();
#endif

      if (account.IsLiveEnabled()) {
        ImGui::BeginDisabled(!is_signedin);
        if (ImGui::BeginMenu("Convert to Offline Profile")) {
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(
              fmt::format("You're about to convert profile: {} (XUID: {:016X}) "
                          "to an offline profile. Are you sure?",
                          account.GetGamertagString(), xuid)
                  .c_str());
          ImGui::EndTooltip();

          if (ImGui::MenuItem("Yes, convert it!")) {
            profile_manager->ConvertToOfflineProfile(xuid);
            ImGui::EndMenu();
            ImGui::EndDisabled();
            ImGui::EndPopup();
            return false;
          }

          ImGui::EndMenu();
        }
        ImGui::EndDisabled();
      } else {
        ImGui::BeginDisabled(!is_signedin);
        if (ImGui::BeginMenu("Convert to Xbox Live-Enabled Profile")) {
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(
              fmt::format("You're about to convert profile: {} (XUID: {:016X}) "
                          "to an Xbox Live-Enabled profile. Are you sure?",
                          account.GetGamertagString(), xuid)
                  .c_str());
          ImGui::EndTooltip();

          if (ImGui::MenuItem("Yes, convert it!")) {
            profile_manager->ConvertToXboxLiveEnabledProfile(xuid);
            ImGui::EndMenu();
            ImGui::EndDisabled();
            ImGui::EndPopup();
            return false;
          }

          ImGui::EndMenu();
        }
        ImGui::EndDisabled();
      }

      if (ImGui::BeginMenu("Delete Profile")) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            fmt::format("You're about to delete profile: {} (XUID: {:016X}). "
                        "This will remove all data assigned to this profile "
                        "including savefiles. Are you sure?",
                        account.GetGamertagString(), xuid)
                .c_str());
        ImGui::EndTooltip();

        if (ImGui::MenuItem("Yes, delete it!")) {
          profile_manager->DeleteProfile(xuid);
          ImGui::EndMenu();
          ImGui::EndPopup();
          return false;
        }

        ImGui::EndMenu();
      }
    }
    ImGui::EndPopup();
  }
  return true;
}

void ProfileConfigDialog::OnClose() {
  XELOGI("ProfileConfigDialog: OnClose invoked");
  if (emulator_window_) {
    emulator_window_->DetachProfileConfigDialog(this);
  }
}

std::shared_ptr<ui::ImmediateTexture>
ProfileConfigDialog::GetOrCreateButtonTexture(char button) {
  std::shared_ptr<ui::ImmediateTexture>* dst = nullptr;
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

void ProfileConfigDialog::OnDraw(ImGuiIO& io) {
#if XE_PLATFORM_WINRT
  if (!emulator_window_->emulator() ||
      !emulator_window_->emulator()->kernel_state() ||
      !emulator_window_->emulator()->kernel_state()->xam_state()) {
    return;
  }

  auto profile_manager = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager();
  if (!profile_manager) {
    return;
  }

  auto profiles = profile_manager->GetAccounts();
  if (profiles->empty()) {
    selected_xuid_ = 0;
  } else if (profiles->find(selected_xuid_) == profiles->end()) {
    selected_xuid_ = profiles->begin()->first;
  }

  // Regular window like other overlay UIs
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGuiWindowFlags window_flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;

  if (!popup_opened_) {
    ImGui::SetNextWindowFocus();
    popup_opened_ = true;
  }

  if (!ImGui::Begin("##profiles_window", nullptr, window_flags)) {
    ImGui::End();
    return;
  }

  if (xe::app::ConsumeGuideBackPress()) {
    Close();
    ImGui::End();
    return;
  }

  // Background and styling like sign-in UI
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
                                 panel_padding, ux, uy, "Profiles Menu");

  const float content_start_y =
      header_layout.position.y + header_layout.font_size + 65.0f;
  const auto content_bounds =
      xe::ui::GetGuideContentBounds(panel_size.x, viewport->Size.x);
  const float shifted_x = overlay_min.x + panel_padding.x + (20.0f * ux);
  const float field_width =
      std::max(1.0f, overlay_min.x + content_bounds.right - shifted_x);
  ImGui::SetCursorScreenPos(ImVec2(shifted_x, content_start_y));
  bool should_open_selected_profile_menu = false;
  ImVec2 selected_profile_popup_anchor = ImVec2(shifted_x, content_start_y);
  const bool should_focus_selected_profile =
      (focus_target_ == FocusTarget::kAuto ||
       focus_target_ == FocusTarget::kProfiles) &&
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      !ImGui::IsAnyItemFocused() && !ImGui::IsAnyItemActive() &&
      !ImGui::IsMouseClicked(0);

  if (signed_in_profiles_count_ != profile_manager->SignedInProfilesCount()) {
    LoadProfileIcon();
    signed_in_profiles_count_ = profile_manager->SignedInProfilesCount();
  }

  // Profile list with proper rendering
  for (auto& [xuid, account] : *profiles) {
    ImGui::PushID(fmt::format("{:016X}", xuid).c_str());

    const uint8_t user_index =
        profile_manager->GetUserIndexAssignedToProfile(xuid);

    const auto current_gamerpic = emulator_window_->emulator()
                                      ->kernel_state()
                                      ->xam_state()
                                      ->user_tracker()
                                      ->GetUserGamerpicSetting(xuid);
    const auto cached_gamerpic = profile_gamerpic_key_.find(xuid);
    if (current_gamerpic.has_value() &&
        (cached_gamerpic == profile_gamerpic_key_.end() ||
         std::memcmp(&cached_gamerpic->second, &current_gamerpic.value(),
                     sizeof(kernel::xam::GamerPictureKey)) != 0)) {
      LoadProfileIcon(xuid);
    }
    const auto profile_icon = profile_icon_.find(xuid) != profile_icon_.cend()
                                  ? profile_icon_[xuid].get()
                                  : nullptr;

    bool selected = (selected_xuid_ == xuid);

    // Size rows to the wrapped text and avatar within the guide width.
    const float profile_shift_x = shifted_x + (4.0f * ux);
    const float selectable_width = std::max(1.0f, field_width - (8.0f * ux));
    const ImVec2 row_min(profile_shift_x, ImGui::GetCursorScreenPos().y);
    const ImVec2 row_padding(10.0f * io.DisplayFramebufferScale.x,
                              8.0f * io.DisplayFramebufferScale.y);
    const ImVec2 icon_size = xe::ui::default_image_icon_size;
    const float icon_text_gap = 10.0f * io.DisplayFramebufferScale.x;
    const float text_width =
        std::max(1.0f, selectable_width - row_padding.x * 2.0f -
                           icon_size.x - icon_text_gap);
    const std::array<std::string, 3> profile_lines = {
        fmt::format("User: {}", account.GetGamertagString()),
        fmt::format("XUID: {:016X}", xuid),
        user_index != XUserIndexAny
            ? fmt::format("Assigned to slot: {}", user_index + 1)
            : "Profile is not signed in"};
    float text_height = ImGui::GetStyle().ItemSpacing.y * 2.0f;
    for (const auto& line : profile_lines) {
      text_height +=
          ImGui::CalcTextSize(line.c_str(), nullptr, false, text_width).y;
    }
    const float content_height = std::max(icon_size.y, text_height);
    const float item_height = content_height + row_padding.y * 2.0f;
    ImGui::SetCursorScreenPos(row_min);

    if (selected && should_focus_selected_profile) {
      ImGui::SetKeyboardFocusHere();
      focus_target_ = FocusTarget::kNone;
    }

    // Make selectable transparent
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                          ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                          ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    // Selectable otherwise expands its hit rectangle by half ItemSpacing.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    bool activate_profile =
        ImGui::Selectable("##profile_selectable", selected, 0,
                          ImVec2(selectable_width, item_height));
    ImGui::PopStyleVar();
    if (!activate_profile && ImGui::IsItemFocused() &&
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
      activate_profile = true;
    }
    if (activate_profile) {
      selected_xuid_ = xuid;
      XELOGI(
          "ProfileConfigDialog: activated profile {:016X}, opening profile "
          "popup",
          xuid);
      should_open_selected_profile_menu = true;
    }

    ImGui::PopStyleColor(3);

    if (selected) {
      ImGui::SetItemDefaultFocus();
    }

    if (selected_xuid_ == xuid) {
      selected_profile_popup_anchor =
          ImVec2(ImGui::GetItemRectMin().x + selectable_width * 0.5f,
                 ImGui::GetItemRectMax().y);
    }

    const ImVec2 row_max = ImGui::GetItemRectMax();
    auto* row_draw_list = ImGui::GetWindowDrawList();
    row_draw_list->PushClipRect(row_min, row_max, true);
    // Keep the focus outline inside the row.
    if (ImGui::IsItemFocused()) {
      xe::app::DrawConfiguredComboHighlight(
          ImVec2(row_min.x + 1.0f, row_min.y + 1.0f),
          ImVec2(row_max.x - 1.0f, row_max.y - 1.0f),
          1.6f, 4.0f);
    }

    // Overlay the contents on the selectable and advance the row once.
    ImGui::SetCursorScreenPos(ImVec2(
        row_min.x + row_padding.x,
        row_min.y + row_padding.y + (content_height - icon_size.y) * 0.5f));
    if (profile_icon) {
      ImGui::Image(reinterpret_cast<ImTextureID>(profile_icon), icon_size);
    } else if (user_index < XUserMaxUserCount) {
      const auto icon = imgui_drawer()->GetNotificationIcon(user_index);
      ImGui::Image(reinterpret_cast<ImTextureID>(icon), icon_size);
    } else {
      ImGui::Dummy(icon_size);
    }
    ImGui::SetCursorScreenPos(ImVec2(
        row_min.x + row_padding.x + icon_size.x + icon_text_gap,
        row_min.y + row_padding.y + (content_height - text_height) * 0.5f));
    // Indentation keeps subsequent lines in the text column.
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_width);
    for (const auto& line : profile_lines) {
      ImGui::TextUnformatted(line.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    row_draw_list->PopClipRect();

    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_max.y));
    ImGui::Dummy(ImVec2(0.0f, 8.0f * uy));
  }

  if (should_open_selected_profile_menu) {
    XELOGI("ProfileConfigDialog: opening popup for {:016X}", selected_xuid_);
    ImGui::OpenPopup("Profile Menu");
  }

  if (!DrawProfileContextMenu(selected_xuid_)) {
    ImGui::End();
    return;
  }

  // Add spacing before buttons
  ImGui::Dummy(ImVec2(0.0f, 24.0f * uy));

  // Create Profile button
  const ImVec2 button_size(field_width, 0.0f);
  ImGui::SetCursorScreenPos(ImVec2(shifted_x, ImGui::GetCursorScreenPos().y));
  if (DrawTextEffectButton("Create Profile", button_size)) {
    XELOGD("ProfileConfigDialog: Create Profile button activated");
    new kernel::xam::ui::CreateProfileUI(emulator_window_->imgui_drawer(),
                                         emulator_window_->emulator());
    Close();
    ImGui::End();
    return;
  }

  // Close button
  ImGui::Dummy(ImVec2(0.0f, 4.0f * uy));
  ImGui::SetCursorScreenPos(ImVec2(shifted_x, ImGui::GetCursorScreenPos().y));
  if (DrawTextEffectButton("Close", button_size)) {
    XELOGD("ProfileConfigDialog: Close button activated");
    Close();
    ImGui::End();
    return;
  }

  // Draw footer prompts
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

  ImGui::End();

#else
  if (!emulator_window_->emulator() ||
      !emulator_window_->emulator()->kernel_state() ||
      !emulator_window_->emulator()->kernel_state()->xam_state()) {
    return;
  }

  auto profile_manager = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager();
  if (!profile_manager) {
    return;
  }

  auto profiles = profile_manager->GetAccounts();

  ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.8f);

  bool dialog_open = true;
  if (!ImGui::Begin("Profiles Menu", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::End();
    return;
  }

  // For whatever reason dialog wasn't opened. It's probably in closing state.
  // We need to handle it here before it will make icons allocation.
  if (!dialog_open) {
    ImGui::CloseCurrentPopup();
    Close();
    ImGui::End();
    emulator_window_->ToggleProfilesConfigDialog();
    return;
  }

  if (profiles->empty()) {
    ImGui::TextUnformatted("No profiles found!");
    ImGui::Spacing();
    ImGui::Separator();
  }

  const ImVec2 next_window_position =
      ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x + 20.f,
             ImGui::GetWindowPos().y);

  // Load profile icon for profiles created while dialog is rendering.
  if (signed_in_profiles_count_ != profile_manager->SignedInProfilesCount()) {
    LoadProfileIcon();
    signed_in_profiles_count_ = profile_manager->SignedInProfilesCount();
  }

  for (auto& [xuid, account] : *profiles) {
    ImGui::PushID(fmt::format("{:016X}", xuid).c_str());

    const uint8_t user_index =
        profile_manager->GetUserIndexAssignedToProfile(xuid);

    const auto profile_icon = profile_icon_.find(xuid) != profile_icon_.cend()
                                  ? profile_icon_[xuid].get()
                                  : nullptr;

    auto context_menu_fun = [=, this]() -> bool {
      if (ImGui::BeginPopupContextItem("Profile Menu")) {
        //*selected_xuid = xuid;
        if (user_index == XUserIndexAny) {
          ImGui::BeginDisabled(!profile_manager->IsAnyProfileSlotFree());

          if (ImGui::MenuItem("Login")) {
            profile_manager->Login(xuid);
            if (!profile_manager->GetProfile(xuid)
                     ->GetProfileIcon(kernel::xam::XTileType::kGamerTile)
                     .empty()) {
              LoadProfileIcon(xuid);
            }
          }
          if (ImGui::BeginMenu("Login to slot:")) {
            for (uint8_t i = 1; i <= XUserMaxUserCount; i++) {
              if (ImGui::MenuItem(fmt::format("slot {}", i).c_str())) {
                uint64_t current_slot_xuid = 0;

                if (const auto current_profile = profile_manager->GetProfile(
                        static_cast<uint8_t>(i - 1));
                    current_profile) {
                  current_slot_xuid = current_profile->xuid();
                }

                profile_manager->Login(xuid, i - 1);
                LoadProfileIcon(xuid);

                // Release resources
                if (current_slot_xuid) {
                  LoadProfileIcon(current_slot_xuid);
                }
              }
            }
            ImGui::EndMenu();
          }
          ImGui::EndDisabled();
        } else {
          if (ImGui::MenuItem("Logout")) {
            profile_manager->Logout(user_index);
            LoadProfileIcon(xuid);
          }
        }

        if (ImGui::MenuItem("Modify")) {
          new kernel::xam::ui::GamercardUI(
              emulator_window_->window(), emulator_window_->imgui_drawer(),
              emulator_window_->emulator()->kernel_state(), xuid);
        }

        if (ImGui::BeginMenu("Copy")) {
          if (ImGui::MenuItem("Gamertag")) {
            ImGui::SetClipboardText(account.GetGamertagString().c_str());
          }

          if (ImGui::MenuItem("XUID")) {
            ImGui::SetClipboardText(fmt::format("{:016X}", xuid).c_str());
          }

          if (account.IsLiveEnabled()) {
            if (ImGui::MenuItem("XUID Online")) {
              ImGui::SetClipboardText(
                  fmt::format("{:016X}", account.xuid_online.get()).c_str());
            }
          }

          ImGui::EndMenu();
        }

        const bool is_signedin = profile_manager->GetProfile(xuid) != nullptr;
        ImGui::BeginDisabled(!is_signedin);
        if (ImGui::MenuItem("Show Played Titles")) {
          new kernel::xam::ui::TitleListUI(
              emulator_window_->imgui_drawer(), next_window_position,
              profile_manager->GetProfile(user_index));
        }
        ImGui::EndDisabled();

        if (ImGui::MenuItem("Show Content Directory")) {
          const auto path = profile_manager->GetProfileContentPath(
              xuid, emulator_window_->emulator()->kernel_state()->title_id());

          if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(path);
          }

          std::thread path_open(LaunchFileExplorer, path);
          path_open.detach();
        }

        ImGui::BeginDisabled(!account.IsLiveEnabled());
        if (ImGui::BeginMenu("XStorage Content")) {
          const auto emulator = emulator_window_->emulator();
          const auto filesystem = emulator->kernel_state()->file_system();

          xe::vfs::HostPathEntry* entry = nullptr;
          std::string symlink_path;

          if (ImGui::MenuItem("Show Game Data")) {
            if (emulator->is_title_open()) {
              symlink_path =
                  fmt::format("XSTORAGE:user\\{:016X}\\title\\{:08X}",
                              account.xuid_online.get(), emulator->title_id());

            } else {
              symlink_path = fmt::format("XSTORAGE:user\\{:016X}",
                                         account.xuid_online.get());
            }
          }

          ImGui::BeginDisabled(!emulator->is_title_open());
          if (ImGui::MenuItem("Show Game Clips")) {
            symlink_path =
                fmt::format("XSTORAGE:clips\\title\\{:08X}\\{:016X}",
                            emulator->title_id(), account.xuid_online.get());
          }
          ImGui::EndDisabled();

          if (!symlink_path.empty()) {
            entry = reinterpret_cast<xe::vfs::HostPathEntry*>(
                filesystem->ResolvePath(symlink_path));

            if (entry) {
              std::thread path_open(LaunchFileExplorer, entry->host_path());
              path_open.detach();
            }
          }

          ImGui::EndMenu();
        }
        ImGui::EndDisabled();

        if (!emulator_window_->emulator()->is_title_open()) {
          ImGui::Separator();

          if (account.IsLiveEnabled()) {
            ImGui::BeginDisabled(!is_signedin);
            if (ImGui::BeginMenu("Convert to Offline Profile")) {
              ImGui::BeginTooltip();
              ImGui::TextUnformatted(
                  fmt::format(
                      "You're about to convert profile: {} (XUID: {:016X}) "
                      "to an offline profile. Are you sure?",
                      account.GetGamertagString(), xuid)
                      .c_str());
              ImGui::EndTooltip();

              if (ImGui::MenuItem("Yes, convert it!")) {
                profile_manager->ConvertToOfflineProfile(xuid);
                ImGui::EndMenu();
                ImGui::EndDisabled();
                ImGui::EndPopup();
                return false;
              }

              ImGui::EndMenu();
            }
            ImGui::EndDisabled();
          } else {
            ImGui::BeginDisabled(!is_signedin);
            if (ImGui::BeginMenu("Convert to Xbox Live-Enabled Profile")) {
              ImGui::BeginTooltip();
              ImGui::TextUnformatted(
                  fmt::format(
                      "You're about to convert profile: {} (XUID: {:016X}) "
                      "to an Xbox Live-Enabled profile. Are you sure?",
                      account.GetGamertagString(), xuid)
                      .c_str());
              ImGui::EndTooltip();

              if (ImGui::MenuItem("Yes, convert it!")) {
                profile_manager->ConvertToXboxLiveEnabledProfile(xuid);
                ImGui::EndMenu();
                ImGui::EndDisabled();
                ImGui::EndPopup();
                return false;
              }

              ImGui::EndMenu();
            }
            ImGui::EndDisabled();
          }

          if (ImGui::BeginMenu("Delete Profile")) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(
                fmt::format(
                    "You're about to delete profile: {} (XUID: {:016X}). "
                    "This will remove all data assigned to this profile "
                    "including savefiles. Are you sure?",
                    account.GetGamertagString(), xuid)
                    .c_str());
            ImGui::EndTooltip();

            if (ImGui::MenuItem("Yes, delete it!")) {
              profile_manager->DeleteProfile(xuid);
              ImGui::EndMenu();
              ImGui::EndPopup();
              return false;
            }

            ImGui::EndMenu();
          }
        }
        ImGui::EndPopup();
      }
      return true;
    };

    if (!kernel::xam::xeDrawProfileContent(
            imgui_drawer(), xuid, user_index, &account, profile_icon,
            context_menu_fun, [=, this]() { LoadProfileIcon(xuid); },
            &selected_xuid_)) {
      ImGui::PopID();
      ImGui::End();
      return;
    }

    ImGui::PopID();
    ImGui::Separator();
  }

  ImGui::Spacing();

  if (ImGui::Button("Create Profile")) {
    new kernel::xam::ui::CreateProfileUI(emulator_window_->imgui_drawer(),
                                         emulator_window_->emulator());
  }

  ImGui::End();

#endif
}

void ManagerDialog::Initialize(ui::ImGuiDrawer* imgui_drawer,
                               uint32_t user_index) {
  emulator_ = emulator_window_->emulator();

  const auto profile =
      emulator_->kernel_state()->xam_state()->GetUserProfile(user_index);

  if (!profile) {
    return;
  }

  // If title is open we can assume periodic maintenance is running, therefore
  // presence sync isn't needed.
  if (!emulator_->is_title_open()) {
    friends_ui_args_.friends_presence_sync =
        emulator_->kernel_state()->presence_manager()->SyncPresenceAsync(
            profile->xuid());
  } else {
    friends_ui_args_.friends_presence =
        emulator_->kernel_state()
            ->xam_state()
            ->presence_manager()
            ->GetFriendsPresenceSorted(profile->xuid());
  }

  friends_ui_args_.immediate_gamerpics =
      emulator_->GetXboxLiveAPI()->GetFriendsGamerpicsAsync(profile->xuid(),
                                                            imgui_drawer);
}

void ManagerDialog::OnDraw(ImGuiIO& io) {
  if (direct_friends_) {
    OnDrawGuide(io);
    return;
  }
  if (!manager_opened_) {
    manager_opened_ = true;
    ImGui::OpenPopup("Manager");

    sessions_args.filter_own = true;
  }

  // Add profile dropdown selector?
  const uint32_t user_index = 0;

  auto profile =
      emulator_->kernel_state()->xam_state()->GetUserProfile(user_index);

  const bool is_profile_signed_in = profile == nullptr;

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();

  kernel::xam::ui::FriendDialogPresentation guide;
  const kernel::xam::ui::FriendDialogPresentation* presentation = nullptr;
#if XE_PLATFORM_WINRT
  guide = kernel::xam::xeMakeNetplayGuidePresentation(imgui_drawer());
  presentation = &guide;
#endif
  if (!presentation) {
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  }
  if (kernel::xam::ui::BeginFriendPopup("Manager", &manager_opened_,
                                        ImGuiWindowFlags_AlwaysAutoResize,
                                        presentation)) {
    const bool close_requested =
        kernel::xam::ui::ConsumeFriendPopupBackPress(presentation);
    ImVec2 btn_size = presentation ? ImVec2(ImGui::GetContentRegionAvail().x,
                                            ImGui::GetFrameHeight())
                                   : ImVec2(200, 40);

    if (is_profile_signed_in) {
      ImGui::Text("You're not logged into a profile!");
      if (!presentation) {
        ImGui::Separator();
      }
    }

    if (!presentation) {
      ImGui::SetWindowFontScale(1.2f);
    }

    ImGui::BeginDisabled(is_profile_signed_in);
    if (kernel::xam::ui::FriendPopupButton("Friends", btn_size, presentation)) {
      friends_ui_args_.content_args.friends_open = true;
      friends_ui_args_.content_args.first_draw = true;

      if (emulator_->kernel_state()->GetXboxLiveAPI()->IsConnectedToServer()) {
        friends_ui_args_.content_args.filter_offline = true;
      }

      ImGui::OpenPopup("Friends");
    }
    ImGui::EndDisabled();

    if (!presentation) {
      ImGui::SameLine();
    }

    ImGui::BeginDisabled(is_profile_signed_in ||
                         !emulator_->GetXboxLiveAPI()->IsConnectedToServer());
    if (kernel::xam::ui::FriendPopupButton("Sessions", btn_size,
                                           presentation)) {
      sessions_args.sessions_open = true;
      ImGui::OpenPopup("Sessions");
    }
    ImGui::EndDisabled();

    if (emulator_->GetXboxLiveAPI()->IsXUIDMismatched()) {
      ImVec2 button_pos = ImGui::GetCursorScreenPos();
      ImVec2 button_end =
          ImVec2(button_pos.x + btn_size.x, button_pos.y + btn_size.y);

      ImDrawList* draw_list = ImGui::GetWindowDrawList();

      draw_list->AddRect(button_pos, button_end, IM_COL32(255, 0, 0, 255), 0.0f,
                         0, 3.0f);
    }

    if (kernel::xam::ui::FriendPopupButton("Delete Netplay Profiles", btn_size,
                                           presentation)) {
      ImGui::OpenPopup("Delete Profiles");
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Delete profiles to fix XUID mismatch error.");
    }

    if (!presentation) {
      ImGui::SameLine();
    }

    if (kernel::xam::ui::FriendPopupButton("UPnP and Ports", btn_size,
                                           presentation)) {
      upnp_and_ports_args.first_draw = true;
      upnp_and_ports_args.dialog_open = true;
      ImGui::OpenPopup("UPnP and Ports");
    }

    if (!presentation) {
      ImGui::SetWindowFontScale(1.0f);
    }

    if (!sessions_args.sessions_open) {
      sessions_args.first_draw = false;
      sessions_args.refresh_sessions_sync = true;
      sessions.clear();
    }

    xeDrawFriendsUI(imgui_drawer(), profile, friends_ui_args_);

    xeDrawSessionsContent(imgui_drawer(), profile, sessions_args, &sessions);

    if (!deletion_args.deleted_profiles_open) {
      deletion_args.first_draw = false;
      deleted_profiles = {};
    }

    bool open_deleted_profiles = false;

    float btn_height = 25;
    if (!presentation) {
      ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowSizeConstraints(ImVec2(225, -1), ImVec2(225, -1));
    }
    if (kernel::xam::ui::BeginFriendPopup("Delete Profiles", nullptr,
                                          ImGuiWindowFlags_AlwaysAutoResize,
                                          presentation)) {
      if (kernel::xam::ui::ConsumeFriendPopupBackPress(presentation)) {
        ImGui::CloseCurrentPopup();
      }

      float btn_width = (ImGui::GetContentRegionAvail().x * 0.5f) -
                        (ImGui::GetStyle().ItemSpacing.x * 0.5f);
      ImVec2 btn_size = ImVec2(
          btn_width, presentation ? ImGui::GetFrameHeight() : btn_height);

      const std::string desc = "Are you sure?";
      const std::string desc2 = "You will be signed out.";

      ImVec2 desc_size = ImGui::CalcTextSize(desc.c_str());
      ImVec2 desc2_size = ImGui::CalcTextSize(desc2.c_str());

      ImGui::SetCursorPosX((ImGui::GetWindowWidth() - desc_size.x) * 0.5f);
      ImGui::Text(desc.c_str());

      if (!is_profile_signed_in) {
        ImGui::Spacing();

        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - desc2_size.x) * 0.5f);
        ImGui::Text(desc2.c_str());
      }

      if (!presentation) {
        ImGui::Separator();
      }

      const bool opening_input = kernel::xam::ui::FriendPopupOpeningInputHeld();
      if (kernel::xam::ui::FriendPopupButton("Cancel", btn_size,
                                             presentation)) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::SetItemDefaultFocus();
      ImGui::SameLine();
      ImGui::BeginDisabled(opening_input);
      if (kernel::xam::ui::FriendPopupButton("Yes", btn_size, presentation)) {
        if (!is_profile_signed_in) {
          std::map<uint8_t, uint64_t> xuids;

          kernel::xam::XamState* xam_state =
              emulator_window_->emulator()->kernel_state()->xam_state();

          for (uint32_t i = 0; i < XUserMaxUserCount; i++) {
            if (xam_state->IsUserSignedIn(i)) {
              xuids[i] = xam_state->GetUserProfile(i)->xuid();
            }
          }

          xam_state->profile_manager()->LogoutMultiple(xuids);
        }

        deleted_profiles = emulator_->GetXboxLiveAPI()->DeleteMyProfiles();

        open_deleted_profiles = true;

        ImGui::CloseCurrentPopup();
      }

      ImGui::EndDisabled();

      kernel::xam::ui::EndFriendPopup(presentation);
    }

    if (open_deleted_profiles) {
      emulator_->GetXboxLiveAPI()->SetXUIDMismatch(false);

      deletion_args.deleted_profiles_open = true;
      ImGui::OpenPopup("Deleted Profiles");
    }

    xe::kernel::xam::xeDrawMyDeletedProfiles(imgui_drawer(), deletion_args,
                                             &deleted_profiles);

    if (!upnp_and_ports_args.dialog_open) {
      upnp_and_ports_args.first_draw = false;
    }

    xe::kernel::xam::xeDrawUPnPAndPorts(imgui_drawer(), upnp_and_ports_args,
                                        emulator_->GetUPnP());

    if (!presentation) {
      ImGui::Separator();
    }
    if (kernel::xam::ui::FriendPopupButton("Close", ImVec2(0, 0),
                                           presentation) ||
        close_requested) {
      manager_opened_ = false;
      ImGui::CloseCurrentPopup();
    }
    kernel::xam::ui::EndFriendPopup(presentation);
  }

  if (!manager_opened_) {
    Close();
    emulator_window_->ToggleFriendsDialog();
  }
}

void ManagerDialog::OnDrawGuide(ImGuiIO& io) {
  auto* profile =
      emulator_->kernel_state()->xam_state()->GetUserProfile(uint32_t{0});
  if (!profile) {
    Close();
    emulator_window_->ToggleFriendsDialog();
    return;
  }
  if (!manager_opened_) {
    manager_opened_ = true;
    auto& content = friends_ui_args_.content_args;
    content.friends_open = true;
    content.first_draw = true;
    content.filter_offline = emulator_->GetXboxLiveAPI()->IsConnectedToServer();
    ImGui::OpenPopup("Friends");
  }
  if (!xeDrawFriendsUI(imgui_drawer(), profile, friends_ui_args_)) {
    Close();
    emulator_window_->ToggleFriendsDialog();
  }
}

TitleListUI::TitleListUI(ui::ImGuiDrawer* imgui_drawer,
                         const ImVec2 drawing_position,
                         const kernel::xam::UserProfile* profile,
                         EmulatorWindow* emulator_window)
    : ui::ImGuiDialog(imgui_drawer),
      drawing_position_(drawing_position),
      profile_(profile),
      emulator_window_(emulator_window),
      profile_manager_(emulator_window->emulator()
                           ->kernel_state()
                           ->xam_state()
                           ->profile_manager()),
      dialog_name_(fmt::format("{}'s Games List", profile->name())) {
  LoadProfileTitleList(imgui_drawer, profile);
}

TitleListUI::~TitleListUI() {
  for (auto& entry : title_icon) {
    entry.second.reset();
  }
}

void TitleListUI::LoadProfileTitleList(
    ui::ImGuiDrawer* imgui_drawer, const kernel::xam::UserProfile* profile) {
  info_.clear();

  xe::ui::IconsData data;

  info_ = emulator_window_->emulator()
              ->kernel_state()
              ->xam_state()
              ->user_tracker()
              ->GetPlayedTitles(profile->xuid());
  for (const auto& title_info : info_) {
    if (!title_info.icon.empty()) {
      data[title_info.id] = title_info.icon;
    }
  }

  title_icon = imgui_drawer->LoadIcons(data);
}

void TitleListUI::OnDraw(ImGuiIO& io) {
#if XE_PLATFORM_WINRT
  // The prior frame has now consumed the raw texture IDs owned by title_icon.
  if (guide_close_pending_) {
    Close();
    return;
  }
#endif
  constexpr const char* kPlayedTitlesWindowName = "##played_titles_window";

  if (!has_opened_) {
    has_opened_ = true;
    focus_requested_ = true;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;

  if (focus_requested_) {
    ImGui::SetNextWindowFocus();
    focus_requested_ = false;
  }

  if (!ImGui::Begin(kPlayedTitlesWindowName, nullptr, flags)) {
    ImGui::End();
    return;
  }

  ImDrawList* root_draw_list = ImGui::GetWindowDrawList();
  const ImVec2 screen_min = viewport->Pos;
  const ImVec2 screen_max(screen_min.x + viewport->Size.x,
                          screen_min.y + viewport->Size.y);
  root_draw_list->AddRectFilled(
      screen_min, screen_max,
      ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f)));

  static xe::app::GuideBackgroundTextureCache guide_background;
  const auto guide_bg_tex = guide_background.Get(imgui_drawer());
  static std::shared_ptr<xe::ui::ImmediateTexture> button_a_tex = nullptr;
  static std::shared_ptr<xe::ui::ImmediateTexture> button_b_tex = nullptr;
  if (!button_a_tex) {
    button_a_tex = xe::app::LoadButtonTexture(imgui_drawer(), 'A');
  }
  if (!button_b_tex) {
    button_b_tex = xe::app::LoadButtonTexture(imgui_drawer(), 'B');
  }

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

  bool should_close = false;
#if XE_PLATFORM_WINRT
  if (xe::app::ConsumeGuideBackPress()) {
#else
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
       ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
#endif
    should_close = true;
  }

  const xe::app::OverlayHeaderLayout header_layout =
      xe::app::DrawOverlayHeader(root_draw_list, overlay_min, panel_size,
                                 panel_padding, ux, uy, "Played Titles");

  const float content_start_y =
      header_layout.position.y + header_layout.font_size + 55.0f;
  const float results_height = overlay_max.y - (102.0f * uy) - content_start_y;
  const auto content_bounds =
      xe::ui::GetGuideContentBounds(panel_size.x, viewport->Size.x);
  const float content_left = overlay_min.x + content_bounds.left;
  ImGui::SetCursorScreenPos(ImVec2(content_left, content_start_y));

  if (!info_.empty()) {
    if (ImGui::BeginChild("##played_titles_results",
                          ImVec2(content_bounds.width(), results_height), false,
                          ImGuiWindowFlags_NoScrollbar)) {
      const float carousel_spacing = 16.0f * ux;
      const float carousel_card_height = 126.0f * uy;
      size_t selected_index = 0;
      for (size_t i = 0; i < info_.size(); ++i) {
        if (info_[i].id == selected_title_) {
          selected_index = i;
          break;
        }
      }
      std::vector<size_t> ordered_indices;
      ordered_indices.reserve(info_.size());
      for (size_t i = 0; i < info_.size(); ++i) {
        ordered_indices.push_back((selected_index + i) % info_.size());
      }
      ImGui::Dummy(ImVec2(0.0f, 6.0f * uy));
      for (size_t ordered_index = 0; ordered_index < ordered_indices.size();
           ++ordered_index) {
        const auto& entry = info_[ordered_indices[ordered_index]];
        ImGui::PushID(entry.id);
        const bool is_selected = selected_title_ == entry.id;
        const float card_width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        const ImVec2 card_min = ImGui::GetCursorScreenPos();
        const float card_padding = 10.0f * ux;
        const ImVec2 icon_size = xe::ui::default_image_icon_size;
        const float text_x = card_min.x + card_padding + icon_size.x + 10.0f * ux;
        const float text_width =
            std::max(1.0f, card_min.x + card_width - card_padding - text_x);
        const std::string title = xe::to_utf8(entry.title_name);
        const std::string progress =
            fmt::format("{}/{} Achievements unlocked ({} Gamerscore)",
                        entry.unlocked_achievements_count,
                        entry.achievements_count, entry.title_earned_gamerscore);
        std::string last_played = "Last played: Unknown";
        if (entry.WasTitlePlayed()) {
          const auto time_date = std::chrono::system_clock::to_time_t(
              std::chrono::system_clock::time_point(
                  entry.last_played.time_since_epoch()));
          last_played = fmt::format("Last played: {:%Y-%m-%d %H:%M}",
                                     *std::localtime(&time_date));
        }
        ImGui::PushFont(imgui_drawer()->GetTitleFont());
        const float title_height =
            ImGui::CalcTextSize(title.c_str(), nullptr, false, text_width).y;
        ImGui::PopFont();
        const float text_height = title_height +
            ImGui::CalcTextSize(progress.c_str(), nullptr, false, text_width).y +
            ImGui::CalcTextSize(last_played.c_str(), nullptr, false, text_width).y +
            ImGui::GetStyle().ItemSpacing.y * 2.0f;
        const float card_height = std::max(
            carousel_card_height,
            std::max(icon_size.y, text_height) + card_padding * 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * ux);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        // Selectable normally adds half ItemSpacing to its hit rectangle.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        if (ImGui::Selectable("##played_title_card", is_selected,
                              ImGuiSelectableFlags_None,
                              ImVec2(card_width, card_height))) {
          selected_title_ = entry.id;
          const ImVec2 next_window_position(
              content_left, overlay_min.y + 72.0f * uy);
          new kernel::xam::ui::GameAchievementsUI(
              imgui_drawer(), next_window_position, &entry, profile_);
        }
        ImGui::PopStyleVar(3);
        const ImVec2 card_max = ImGui::GetItemRectMax();
        auto* card_draw_list = ImGui::GetWindowDrawList();
        card_draw_list->PushClipRect(card_min, card_max, true);
        if (is_selected) {
          xe::app::DrawConfiguredComboHighlight(
              ImVec2(card_min.x + 1.0f, card_min.y + 1.0f),
              ImVec2(card_max.x - 1.0f, card_max.y - 1.0f), 1.6f, 6.0f);
        }
        // Render inside the selectable rather than after its full-width row.
        ImGui::SetCursorScreenPos(
            ImVec2(card_min.x + card_padding, card_min.y + card_padding));
        if (title_icon.count(entry.id)) {
          ImGui::Image(
              reinterpret_cast<ImTextureID>(title_icon.at(entry.id).get()),
              icon_size);
        } else {
          ImGui::Dummy(icon_size);
        }
        ImGui::SetCursorScreenPos(ImVec2(text_x, card_min.y + card_padding));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_width);
        ImGui::PushFont(imgui_drawer()->GetTitleFont());
        ImGui::TextUnformatted(title.c_str());
        ImGui::PopFont();
        ImGui::TextUnformatted(progress.c_str());
        ImGui::TextUnformatted(last_played.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        card_draw_list->PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(card_min.x, card_max.y));
        ImGui::Dummy(ImVec2(0.0f, carousel_spacing));
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
  } else {
    ImGui::SetCursorScreenPos(ImVec2(content_left, content_start_y));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_bounds.width());
    ImGui::PushFont(imgui_drawer()->GetTitleFont());
    ImGui::TextUnformatted("There are no titles, so far.");
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
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
  xe::app::DrawFooterPrompt(root_draw_list, button_b_tex, footer_text_size,
                            footer_icon_size, "Back", footer_back_y,
                            footer_back_text_x, footer_back_icon_offset);
  xe::app::DrawFooterPrompt(root_draw_list, button_a_tex, footer_text_size,
                            footer_icon_size, "Select", footer_select_y,
                            footer_select_text_x, footer_select_icon_offset);

  ImGui::End();

  if (should_close) {
#if XE_PLATFORM_WINRT
    // ImGuiDialog deletes after OnDraw, before draw-list submission. Retire on
    // the next draw so the closing frame can still use this dialog's icons.
    guide_close_pending_ = true;
#else
    Close();
#endif
  }
  return;
}

}  // namespace app
}  // namespace xe
