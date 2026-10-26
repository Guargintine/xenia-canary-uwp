/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/create_profile_ui.h"
#include "xenia/emulator.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/xam/user_settings.h"
#include "xenia/ui/resources.h"
#include "xenia/ui/window.h"
#if XE_PLATFORM_WINRT
#include "xenia-canary-uwp/WinRTKeyboard.h"
#include "xenia/app/ui_text_effect_helpers.h"
#endif

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

void CreateProfileUI::OnDraw(ImGuiIO& io) {
  if (!create_profile_args_.dialog_open) {
    ImGui::OpenPopup("Create Profile");
    create_profile_args_.dialog_open = true;
  }

  if (!xeDrawCreateProfile(imgui_drawer(), emulator_, create_profile_args_)) {
    Close();
  }
}

void CreateProfileUI::OnClose() {
#if XE_PLATFORM_WINRT
  // ImGui's current draw commands still reference the guide and gamerpic.
  // Release them after this frame has been submitted, not when Draw deletes us.
  auto frame_resources =
      std::make_shared<CreateProfileUIArgs>(std::move(create_profile_args_));
  emulator_->display_window()->app_context().CallInUIThreadDeferred(
      [frame_resources = std::move(frame_resources)]() {});
#endif
  XamDialog::OnClose();
}

void CreateProfileUI::Initialize() {
  const auto gamerpic_key = GetDefaultGamerPictureKey();

  if (gamerpic_key.has_value()) {
    create_profile_args_.gamerpic_key = gamerpic_key;
    create_profile_args_.downloaded_gamerpics =
        kernel_state()->GetXboxLiveAPI()->DownloadCompleteGamerpic(
            gamerpic_key.value());
  }
}

std::optional<GamerPictureKey> CreateProfileUI::GetDefaultGamerPictureKey() {
  const auto default_picture_key = std::find_if(
      default_setting_values.cbegin(), default_setting_values.cend(),
      [](const UserSetting& setting) {
        return setting.get_setting_id() ==
               static_cast<uint32_t>(
                   UserSettingId::XPROFILE_GAMERCARD_PICTURE_KEY);
      });

  if (default_picture_key == default_setting_values.cend()) {
    return std::nullopt;
  }

  const UserSetting setting = *default_picture_key;

  const auto gamerpic_key = *reinterpret_cast<const GamerPictureKey*>(
      xe::to_utf8(std::get<std::u16string>(setting.get_host_data())).c_str());

  return gamerpic_key;
}

bool xeDrawCreateProfile(xe::ui::ImGuiDrawer* imgui_drawer, Emulator* emulator,
                         CreateProfileUIArgs& args) {
  auto profile_manager =
      emulator->kernel_state()->xam_state()->profile_manager();

#if XE_PLATFORM_WINRT
  ImGuiIO& io = ImGui::GetIO();
  if (!args.guide_background) {
    args.guide_background =
        xe::app::LoadConfiguredGuideBackgroundTexture(imgui_drawer);
  }
  const ImVec2 panel_size =
      xe::app::GetGuidePanelSize(args.guide_background, io.DisplaySize.y);
  const ImVec2 panel_padding = xe::app::GetGuidePanelPadding();
  const ImVec2 panel_position = ImGui::GetMainViewport()->Pos;
  ImGui::SetNextWindowPos(panel_position, ImGuiCond_Always);
  ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const bool visible = ImGui::BeginPopupModal(
      "Create Profile", &args.dialog_open,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
          ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar(3);
  if (!visible) {
    return false;
  }
  auto* draw_list = ImGui::GetWindowDrawList();
  xe::app::DrawGuidePanelBackground(
      draw_list, args.guide_background, panel_position,
      ImVec2(panel_position.x + panel_size.x, panel_position.y + panel_size.y));
  const auto header = xe::app::DrawOverlayHeader(
      draw_list, panel_position, panel_size, panel_padding,
      io.DisplaySize.x / 1024.0f, io.DisplaySize.y / 576.0f, "Create Profile");
  ImGui::SetCursorScreenPos(
      ImVec2(panel_position.x + panel_padding.x,
             header.position.y + header.font_size + 55.0f));
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
    args.dialog_open = false;
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return false;
  }
#else
  if (!ImGui::BeginPopupModal("Create Profile", &args.dialog_open,
                              ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
    return false;
  }
#endif

  const float window_width = ImGui::GetContentRegionAvail().x;

#if XE_PLATFORM_WINRT
  const float btn_height = ImGui::GetFrameHeight();
#else
  const float btn_height = 25;
#endif
  float btn_width =
      (window_width * 0.5f) - (ImGui::GetStyle().ItemSpacing.x * 0.5f);
  ImVec2 half_width_btn = ImVec2(btn_width, btn_height);

  if (args.downloaded_gamerpics.valid() &&
      args.downloaded_gamerpics.wait_for(0s) == std::future_status::ready) {
    const std::pair<std::vector<uint8_t>, std::vector<uint8_t>> gamerpics =
        args.downloaded_gamerpics.get();

    args.big_gamerpic_texture = imgui_drawer->LoadImGuiIcon(gamerpics.first);
  }

  ImTextureID gamerpic =
      reinterpret_cast<ImTextureID>(imgui_drawer->GetLoadingTileIcon());

  if (args.big_gamerpic_texture) {
    gamerpic = reinterpret_cast<ImTextureID>(args.big_gamerpic_texture.get());
  }

  ImGui::Image(gamerpic, xe::ui::default_image_icon_size);

  ImGui::SameLine();

  ImGui::BeginGroup();

  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
#if XE_PLATFORM_WINRT
    UWP::ShowKeyboard();
#endif
  }

  ImGui::TextUnformatted("Gamertag:");

  const bool enter_pressed =
      ImGui::InputText("##Gamertag", args.gamertag, sizeof(args.gamertag),
                       ImGuiInputTextFlags_EnterReturnsTrue);

#if XE_PLATFORM_WINRT
  if (ImGui::IsItemActivated()) {
    UWP::ShowKeyboard();
  }
#endif

  if (ImGui::IsItemEdited() || enter_pressed) {
    args.valid_gamertag =
        profile_manager->IsGamertagValid(std::string(args.gamertag));
  }

  ImGui::Checkbox("Xbox Live Enabled", &args.live_enabled);

  ImGui::EndGroup();

  ImGui::BeginDisabled(!args.valid_gamertag);
  if (ImGui::Button("Create", half_width_btn) ||
      (enter_pressed && args.valid_gamertag)) {
    bool autologin = (profile_manager->GetAccountCount() == 0);

    uint32_t reserved_flags = 0;

    if (args.live_enabled) {
      reserved_flags |= X_XAMACCOUNTINFO::AccountReservedFlags::kLiveEnabled;
    }

    uint64_t created_profile_xuid = 0;

    bool created = profile_manager->CreateProfile(
        std::string(args.gamertag), autologin, args.migration, reserved_flags,
        &created_profile_xuid);

    if (created && args.migration) {
      emulator->DataMigration(0xB13EBABEBABEBABE);
    }

    // Update gamerpic on profile creation.
    // If 4 profiles are signed in then we must sign out last profile in order
    // to update newly created profile gamerpic.
    if (created) {
      uint64_t last_user_profile_xuid = 0;
      const uint8_t last_user_index = XUserMaxUserCount - 1;

      if (profile_manager->SignedInProfilesCount() == XUserMaxUserCount) {
        auto last_user_profile = profile_manager->GetProfile(last_user_index);

        last_user_profile_xuid = last_user_profile->xuid();

        profile_manager->Logout(last_user_index);
        last_user_profile = nullptr;
      }

      profile_manager->Login(created_profile_xuid);

      const auto created_user_profile =
          profile_manager->GetProfile(created_profile_xuid);

      if (args.live_enabled) {
        kernel_state()
            ->xam_state()
            ->user_tracker()
            ->SetupDefaultProfileSettings(created_profile_xuid);
      }

      if (args.downloaded_gamerpics.valid() && args.gamerpic_key.has_value()) {
        const gamerpics_pair gamerpics = args.downloaded_gamerpics.get();
        const GamerPictureKey key = args.gamerpic_key.value();

        const auto user_tracker = kernel_state()->xam_state()->user_tracker();

        user_tracker->UpdateUserGamerpic(
            created_user_profile->xuid(), key.GetTitleId(), key.GetBigTileId(),
            key.GetSmallTileId(), gamerpics.first, gamerpics.second);

        if (!autologin) {
          const uint8_t created_user_index =
              emulator->kernel_state()
                  ->xam_state()
                  ->GetUserIndexAssignedToProfileFromXUID(created_profile_xuid);

          profile_manager->Logout(created_user_index);
        }
      }

      if (last_user_profile_xuid) {
        profile_manager->Login(last_user_profile_xuid);
      }
    }
    std::ranges::fill(args.gamertag, '\0');
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();

  if (ImGui::Button("Cancel", half_width_btn)) {
    std::ranges::fill(args.gamertag, '\0');
    ImGui::CloseCurrentPopup();
  }

  ImGui::EndPopup();

  return true;
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
