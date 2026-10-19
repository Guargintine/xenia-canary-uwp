/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_PROFILE_DIALOGS_H_
#define XENIA_APP_PROFILE_DIALOGS_H_

#include <future>

#include "xenia/kernel/json/session_object_json.h"
#include "xenia/kernel/xam/ui/friends_ui.h"
#include "xenia/kernel/xam/ui/netplay_manager_util.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/xbox.h"

namespace xe {
namespace app {

class EmulatorWindow;

class NoProfileDialog final : public ui::ImGuiDialog {
 public:
  NoProfileDialog(ui::ImGuiDrawer* imgui_drawer,
                  EmulatorWindow* emulator_window)
      : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

  EmulatorWindow* emulator_window_;
  bool focus_requested_ = true;
};

class ProfileConfigDialog final : public ui::ImGuiDialog {
 public:
  ProfileConfigDialog(ui::ImGuiDrawer* imgui_drawer,
                      EmulatorWindow* emulator_window)
      : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {
    LoadProfileIcon();
    LoadSignedInProfilesCount();
  }

 protected:
  void OnClose() override;
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class FocusTarget { kAuto, kProfiles, kCreateProfile, kNone };
  FocusTarget focus_target_ = FocusTarget::kAuto;
  bool popup_opened_ = false;
  std::shared_ptr<ui::ImmediateTexture> button_a_tex_;
  std::shared_ptr<ui::ImmediateTexture> button_b_tex_;
  std::shared_ptr<ui::ImmediateTexture> GetOrCreateButtonTexture(char button);
  bool DrawProfileContextMenu(uint64_t xuid);
  void LoadProfileIcon();
  void LoadProfileIcon(const uint64_t xuid);

  void LoadSignedInProfilesCount();

  std::map<uint64_t, std::unique_ptr<ui::ImmediateTexture>> profile_icon_;
  std::map<uint64_t, kernel::xam::GamerPictureKey> profile_gamerpic_key_;

  uint64_t selected_xuid_ = 0;
  uint8_t signed_in_profiles_count_ = 0;
  EmulatorWindow* emulator_window_;
};

class ManagerDialog final : public ui::ImGuiDialog {
 public:
  ManagerDialog(ui::ImGuiDrawer* imgui_drawer, EmulatorWindow* emulator_window,
                bool guide_style = false)
      : ui::ImGuiDialog(imgui_drawer),
        direct_friends_(guide_style),
        emulator_window_(emulator_window) {
    friends_ui_args_.content_args.guide_style = guide_style;
    Initialize(imgui_drawer, 0);
  }

 protected:
  void OnDraw(ImGuiIO& io) override;
  void Initialize(ui::ImGuiDrawer* imgui_drawer, uint32_t user_index);
  void OnDrawGuide(ImGuiIO& io);

 private:
  bool direct_friends_ = false;
  bool manager_opened_ = false;
  uint64_t selected_xuid_ = 0;
  uint64_t removed_xuid_ = 0;
  Emulator* emulator_;
  xe::kernel::xam::ui::FriendsUIArgs friends_ui_args_;
  xe::kernel::xam::ui::SessionsContentArgs sessions_args = {};
  xe::kernel::xam::ui::MyDeletedProfilesArgs deletion_args = {};
  xe::kernel::xam::ui::UPnPAndPortsArgs upnp_and_ports_args = {};
  std::vector<std::unique_ptr<xe::kernel::SessionObjectJSON>> sessions;
  std::map<uint64_t, std::string> deleted_profiles;
  EmulatorWindow* emulator_window_;
};

class TitleListUI final : public ui::ImGuiDialog {
 public:
  TitleListUI(ui::ImGuiDrawer* imgui_drawer, const ImVec2 drawing_position,
              const kernel::xam::UserProfile* profile,
              EmulatorWindow* emulator_window);

  ~TitleListUI();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void LoadProfileTitleList(ui::ImGuiDrawer* imgui_drawer,
                            const kernel::xam::UserProfile* profile);

  static constexpr uint8_t title_name_filter_size = 15;

  std::string dialog_name_ = "";
  char title_name_filter_[title_name_filter_size] = "";
  uint32_t selected_title_ = 0;
  const ImVec2 drawing_position_ = {};
  bool has_opened_ = false;
  bool focus_requested_ = false;
#if XE_PLATFORM_WINRT
  bool guide_close_pending_ = false;
#endif

  const kernel::xam::UserProfile* profile_;
  const kernel::xam::ProfileManager* profile_manager_;
  EmulatorWindow* emulator_window_;

  std::map<uint32_t, std::unique_ptr<ui::ImmediateTexture>> title_icon;
  std::vector<kernel::xam::TitleInfo> info_;
};

}  // namespace app
}  // namespace xe

#endif
