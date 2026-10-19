/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/netplay_settings_dialog.h"
#include "xenia/app/discord/discord_presence.h"
#include "xenia/app/emulator_window.h"
#include "xenia/base/logging.h"
#include "xenia/base/system.h"

DECLARE_string(api_address);

DECLARE_bool(auto_check_updates);

DECLARE_bool(bind_interface);

DECLARE_bool(xhttp);

DECLARE_bool(upnp);

DECLARE_bool(logging);

DECLARE_bool(discord);

DECLARE_int32(discord_presence_user_index);

namespace xe {
namespace app {

namespace {
struct NetplayStatusText {
  const char* mode;
  const char* api;
  const char* services;
  const char* upnp;
  bool failed = false;
};

NetplayStatusText GetNetplayStatusText(Emulator* emulator) {
  NetplayStatusText status{};
  const auto api = emulator->GetXboxLiveAPI();
  const auto state = api->GetInitState();
  switch (cvars::network_mode) {
    case kernel::NETWORK_MODE::OFFLINE:
      status.mode = "Offline";
      break;
    case kernel::NETWORK_MODE::LAN:
      status.mode = "System Link";
      break;
    default:
      status.mode = "Xbox Live";
      break;
  }
  if (cvars::network_mode == kernel::NETWORK_MODE::OFFLINE) {
    status.services = "Disabled (offline mode)";
    status.api = "Not used in offline mode";
  } else if (!api->HasInitializationStarted()) {
    status.services = "Not started (launch a game)";
    status.api = "Not checked by game services";
  } else if (state == kernel::XLiveAPI::InitState::Pending) {
    status.services = "Initializing";
    status.api = "Checking connection";
  } else if (state == kernel::XLiveAPI::InitState::Success) {
    status.services = "Ready";
    status.api = "Last connection check succeeded";
  } else {
    status.services = "Initialization failed";
    status.api = "Last connection check failed";
    status.failed = true;
  }

  const auto upnp = emulator->GetUPnP();
  if (!cvars::upnp) {
    status.upnp = "Disabled in settings";
  } else if (!upnp) {
    status.upnp = "Unavailable";
  } else if (upnp->IsActive()) {
    status.upnp = "Router found";
  } else {
    switch (upnp->GetDiscoveryState()) {
      case kernel::UPnP::DiscoveryState::NotStarted:
        status.upnp = "Enabled; discovery not started";
        break;
      case kernel::UPnP::DiscoveryState::Searching:
        status.upnp = "Searching for a router";
        break;
      case kernel::UPnP::DiscoveryState::DeviceFound:
        status.upnp = "Router found (starts when a game launches)";
        break;
      case kernel::UPnP::DiscoveryState::Failed:
        status.upnp = "No compatible router found";
        break;
    }
  }
  return status;
}
}  // namespace

#if XE_PLATFORM_WINRT
namespace {
bool GuideActivationReleased() {
  return !ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) &&
         !ImGui::IsKeyDown(ImGuiKey_Enter) &&
         !ImGui::IsKeyDown(ImGuiKey_KeypadEnter) &&
         !ImGui::IsKeyDown(ImGuiKey_Space) &&
         !ImGui::IsMouseDown(ImGuiMouseButton_Left);
}
}  // namespace
#endif

void NetplaySettingsDialog::OnDraw(ImGuiIO& io) {
#if XE_PLATFORM_WINRT
  OnDrawGuide();
#else
  if (!dialog_opened_) {
    dialog_opened_ = true;
    ImGui::OpenPopup("Netplay Settings");
  }

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();

  float btn_height_padding = ImGui::GetStyle().FramePadding.x * 4.0f;

  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(350, -1), ImVec2(350, -1));
  if (ImGui::BeginPopupModal(
          "Netplay Settings", &dialog_opened_,
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    // Capture this before drawing nested popups so their Back action cannot
    // also dismiss the settings window in the same frame.
    const bool close_requested =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    ImGui::SetWindowFontScale(1.05f);

    const auto emulator = emulator_window_->emulator();
    const auto kernel_state = emulator->kernel_state();
    const auto xlive_api = kernel_state->GetXboxLiveAPI();

    const float window_width = ImGui::GetContentRegionAvail().x;

    float btn_height = 25;
    float btn_width =
        (window_width * 0.5f) - (ImGui::GetStyle().ItemSpacing.x * 0.5f);
    ImVec2 half_width_btn = ImVec2(btn_width, btn_height);

    const bool updatable =
        xlive_api->GetInitState() == xe::kernel::XLiveAPI::InitState::Pending;

    ImGui::Text("API Addresses");

    ImGui::SetNextItemWidth(window_width);

    ImGui::BeginDisabled(!updatable);
    if (ImGui::BeginCombo("##API Addresses", selected_api_address_item_)) {
      for (const auto& api_address : api_addresses_) {
        const bool is_selected = selected_api_address_item_ == api_address;

        if (ImGui::Selectable(api_address.c_str(), is_selected)) {
          selected_api_address_item_ = api_address.c_str();
          xlive_api->SetAPIAddress(selected_api_address_item_);
          xlive_api->StartWhoamiAsync();
        }

        if (is_selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !updatable) {
      ImGui::SetTooltip("Changing API address while playing isn't supported!");
    }

    const std::string remove_api_desc = "Remove";

    ImVec2 remove_api_lbl_size = ImGui::CalcTextSize(remove_api_desc.c_str());
    ImVec2 remove_api_btn_size =
        ImVec2(btn_width, remove_api_lbl_size.y + btn_height_padding);

    const bool is_default =
        selected_api_address_item_ == xlive_api->GetDefaultPublicServer();

    ImGui::BeginDisabled(!updatable || is_default);
    if (ImGui::Button(remove_api_desc.c_str(), remove_api_btn_size)) {
      ImGui::OpenPopup("Remove API Address");
    }
    ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !updatable) {
      ImGui::SetTooltip("Cannot remove API address while playing!");
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        updatable && is_default) {
      ImGui::SetTooltip("Cannot remove default API address!");
    }

    ImGui::SameLine();

    const std::string add_api_desc = "Add";

    ImVec2 add_api_lbl_size = ImGui::CalcTextSize(add_api_desc.c_str());
    ImVec2 add_api_btn_size =
        ImVec2(btn_width, add_api_lbl_size.y + btn_height_padding);

    if (ImGui::Button(add_api_desc.c_str(), add_api_btn_size)) {
      add_api_address_dialog_open_ = true;
      ImGui::OpenPopup("API Addresses");
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::Text("Network Modes");

    ImGui::SetNextItemWidth(window_width);

    if (ImGui::Combo("##Network Modes", &selected_network_mode_index_,
                     network_modes_, std::size(network_modes_))) {
      if (xlive_api->SelectNetworkMode(selected_network_mode_index_)) {
        xlive_api->SetNetworkMode(selected_network_mode_index_);
      } else {
        selected_network_mode_index_ = cvars::network_mode;
      }
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::Text("Network Interfaces");

    ImGui::SetNextItemWidth(window_width);

    // If we allow changing network interface while title is running then we
    // have to simulate disconnecting and reconnecting network to reinitialize
    // our own XNADDR?
    ImGui::BeginDisabled(!updatable);
    if (ImGui::BeginCombo("##Network Interfaces",
                          selected_network_interface_item_)) {
      for (uint32_t index = 0;
           const auto& interface_name : network_interfaces_) {
        const bool is_selected =
            selected_network_interface_item_ == interface_name;

        if (ImGui::Selectable(interface_name.c_str(), is_selected)) {
          selected_interface_index_ = index;
          selected_network_interface_item_ = interface_name.c_str();

          const auto guid_keys = std::views::keys(network_interface_guids_);
          std::vector<std::string> guids = {guid_keys.begin(), guid_keys.end()};

          network_adapter_manager_->SetSelectedAdapterGUID(
              guids.at(selected_interface_index_));
        }

        if (is_selected) {
          ImGui::SetItemDefaultFocus();
        }

        index++;
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !updatable) {
      ImGui::SetTooltip("Hot swapping network interface is not allowed!");
    }

    const std::string refresh_desc = "Refresh";

    ImVec2 refresh_desc_lbl_size = ImGui::CalcTextSize(refresh_desc.c_str());
    ImVec2 refresh_desc_btn_size =
        ImVec2(btn_width, refresh_desc_lbl_size.y + btn_height_padding);

    if (ImGui::Button(refresh_desc.c_str(), refresh_desc_btn_size)) {
      network_adapter_manager_->Initialize();
      RefreshInterfaces();
    }

    ImGui::SameLine();

    const std::string reset_desc = "Reset";

    ImVec2 reset_lbl_size = ImGui::CalcTextSize(reset_desc.c_str());
    ImVec2 reset_btn_size =
        ImVec2(btn_width, reset_lbl_size.y + btn_height_padding);

    ImGui::BeginDisabled(!updatable);
    if (ImGui::Button(reset_desc.c_str(), reset_btn_size)) {
      network_adapter_manager_->SelectBestInterface();
      RefreshInterfaces();
    }
    ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !updatable) {
      ImGui::SetTooltip("Hot swapping network interface is not allowed!");
    }

    ImGui::BeginDisabled(!updatable);
    if (ImGui::Checkbox("Bind Interface", &bind_interface_)) {
      xlive_api->SetBindInterface(bind_interface_);
    }
    ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Useful for network Tunnels/VPNs e.g. XLink Kai");
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Checkbox("Auto Check for Updates", &auto_check_for_updates_)) {
      emulator_window_->SetAutoCheckForUpdates(auto_check_for_updates_);
    }

    ImGui::SameLine();

    if (ImGui::Checkbox("NET Logging", &logging_)) {
      xlive_api->SetLogging(logging_);
    }

    ImGui::SameLine();

    if (ImGui::Checkbox("XHTTP", &xhttp_)) {
      xlive_api->SetXHttp(xhttp_);
    }

    if (ImGui::Checkbox("Discord Presence", &discord_)) {
      ActivateDiscordState(discord_);
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(225, 90), ImVec2(225, 90));
    if (ImGui::BeginPopupModal("Remove API Address", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_GamepadFaceRight, false)) {
        ImGui::CloseCurrentPopup();
      }

      float btn_width = (ImGui::GetContentRegionAvail().x * 0.5f) -
                        (ImGui::GetStyle().ItemSpacing.x * 0.5f);
      ImVec2 btn_size = ImVec2(btn_width, btn_height);

      const std::string desc = "Are you sure?";

      ImVec2 desc_size = ImGui::CalcTextSize(desc.c_str());

      ImGui::SetCursorPosX((ImGui::GetWindowWidth() - desc_size.x) * 0.5f);
      ImGui::Text(desc.c_str());

      ImGui::Separator();

      if (ImGui::Button("Yes", btn_size)) {
        const auto xlive_api =
            emulator_window_->emulator()->kernel_state()->GetXboxLiveAPI();

        xlive_api->RemoveAPIAddress(selected_api_address_item_);

        UpdateAPIAddress();
        UpdateSelectedAPIItem();

        ImGui::CloseCurrentPopup();
      }

      ImGui::SameLine();

      if (ImGui::Button("Cancel", btn_size)) {
        ImGui::CloseCurrentPopup();
      }

      ImGui::EndPopup();
    }

    ImGui::SetNextWindowContentSize(ImVec2(200, 0));
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("API Addresses", &add_api_address_dialog_open_,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      if (!add_address_context_open_ &&
          ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_GamepadFaceRight, false)) {
        ImGui::CloseCurrentPopup();
      }

      ImVec2 btn_size = ImVec2(ImGui::GetContentRegionAvail().x, btn_height);

      if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
      }

      ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
      const bool enter_pressed = ImGui::InputTextWithHint(
          "##AddAddress", "https://example.com", new_api_address_,
          sizeof(new_api_address_), ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::PopItemWidth();

      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Right Click/Gamepad A");
      }

      if (ImGui::IsItemFocused() &&
          ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_GamepadFaceDown, false)) {
        ImGui::OpenPopup("##AddAddressContexts");
      }

      if (ImGui::BeginPopupContextItem("##AddAddressContexts")) {
        add_address_context_open_ = true;

        if (ImGui::MenuItem("Paste")) {
          const char* clipboard = ImGui::GetClipboardText();

          if (clipboard) {
            xe::string_util::copy_truncating(new_api_address_, clipboard,
                                             sizeof(new_api_address_));
          }
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Clear")) {
          std::memset(new_api_address_, 0, sizeof(new_api_address_));
        }

        ImGui::EndPopup();
      } else {
        add_address_context_open_ = false;
      }

      if (ImGui::Button("Add", btn_size) || enter_pressed) {
        const std::string api_address(new_api_address_);

        if (!api_address.empty()) {
          xlive_api->AddAPIAddress(api_address);

          UpdateAPIAddress();
          UpdateSelectedAPIItem();

          ImGui::CloseCurrentPopup();
        }
      }

      ImGui::EndPopup();
    }

    ImGui::SetWindowFontScale(1.0f);

    ImGui::Separator();
    if (ImGui::Button("Close") || close_requested) {
      dialog_opened_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (!dialog_opened_) {
    Close();
    emulator_window_->ToggleNetplaySettingsDialog();
  }
#endif
}

#if XE_PLATFORM_WINRT
void NetplaySettingsDialog::OpenGuidePage(GuidePage page) {
  guide_page_ = page;
  guide_focus_requested_ = true;
  guide_activation_armed_ = false;
}

void NetplaySettingsDialog::GoBackGuidePage() {
  OpenGuidePage(guide_page_ == GuidePage::kAddressActions
                    ? GuidePage::kAddAPIAddress
                    : GuidePage::kSettings);
}

void NetplaySettingsDialog::OnDrawGuide() {
  if (!dialog_opened_) {
    dialog_opened_ = true;
    ImGui::OpenPopup("Netplay Settings");
  }

  if (BeginGuidePopup("Netplay Settings", &dialog_opened_, guide_dialog_state_,
                      imgui_drawer())) {
    // Handle Back before controls so each press returns only one page.
    const bool back_requested = ConsumeGuideBackPress();
    bool close_requested = false;
    if (back_requested) {
      if (guide_page_ == GuidePage::kSettings) {
        close_requested = true;
      } else {
        GoBackGuidePage();
      }
    }

    if (!guide_activation_armed_ && GuideActivationReleased()) {
      guide_activation_armed_ = true;
    }
    const bool focus_requested =
        guide_focus_requested_ && guide_activation_armed_;
    if (focus_requested) {
      guide_focus_requested_ = false;
    }
    const auto xlive_api =
        emulator_window_->emulator()->kernel_state()->GetXboxLiveAPI();
    const bool updatable =
        xlive_api->GetInitState() == kernel::XLiveAPI::InitState::Pending;
    const bool is_default =
        selected_api_address_item_ == xlive_api->GetDefaultPublicServer();

    // Reset scroll position when returning to a page.
    if (focus_requested) {
      ImGui::SetScrollY(0.0f);
    }

    ImGui::BeginDisabled(!guide_activation_armed_);
    switch (guide_page_) {
      case GuidePage::kSettings: {
        ImGui::TextUnformatted("API Address");
        ImGui::TextWrapped("%s", selected_api_address_item_
                                     ? selected_api_address_item_
                                     : "Unspecified API Address");
        ImGui::BeginDisabled(!updatable);
        if (focus_requested && updatable) {
          ImGui::SetKeyboardFocusHere();
        }
        if (GuideButton("Change API Address")) {
          OpenGuidePage(GuidePage::kAPIAddresses);
        }
        ImGui::EndDisabled();
        if (!updatable) {
          ImGui::TextWrapped(
              "The API address cannot be changed while playing.");
        }
        if (focus_requested && !updatable) {
          ImGui::SetKeyboardFocusHere();
        }
        if (GuideButton("Add API Address")) {
          OpenGuidePage(GuidePage::kAddAPIAddress);
        }
        ImGui::BeginDisabled(!updatable || is_default);
        if (GuideButton("Remove API Address")) {
          OpenGuidePage(GuidePage::kRemoveAPIAddress);
        }
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Text("Network Mode: %s",
                    selected_network_mode_index_ >= 0 &&
                            selected_network_mode_index_ <
                                static_cast<int>(std::size(network_modes_))
                        ? network_modes_[selected_network_mode_index_]
                        : "Unspecified");
        if (GuideButton("Change Network Mode")) {
          OpenGuidePage(GuidePage::kNetworkModes);
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Network Interface");
        ImGui::TextWrapped("%s", selected_network_interface_item_
                                     ? selected_network_interface_item_
                                     : "Unspecified Network");
        ImGui::BeginDisabled(!updatable);
        if (GuideButton("Change Network Interface")) {
          OpenGuidePage(GuidePage::kNetworkInterfaces);
        }
        ImGui::EndDisabled();
        if (!updatable) {
          ImGui::TextWrapped(
              "The network interface cannot be changed while playing.");
        }
        if (GuideButton("Refresh Interfaces")) {
          network_adapter_manager_->Initialize();
          RefreshInterfaces();
        }
        ImGui::BeginDisabled(!updatable);
        if (GuideButton("Reset Network Interface")) {
          network_adapter_manager_->SelectBestInterface();
          RefreshInterfaces();
        }
        if (ImGui::Checkbox("Bind Interface", &bind_interface_)) {
          xlive_api->SetBindInterface(bind_interface_);
        }
        ImGui::EndDisabled();

        ImGui::Spacing();
        if (ImGui::Checkbox("Auto Check for Updates",
                            &auto_check_for_updates_)) {
          emulator_window_->SetAutoCheckForUpdates(auto_check_for_updates_);
        }
        if (ImGui::Checkbox("NET Logging", &logging_)) {
          xlive_api->SetLogging(logging_);
        }
        if (ImGui::Checkbox("XHTTP", &xhttp_)) {
          xlive_api->SetXHttp(xhttp_);
        }
        break;
      }
      case GuidePage::kAPIAddresses: {
        ImGui::TextUnformatted("Choose API Address");
        const bool selected_address_present =
            selected_api_address_item_ &&
            std::ranges::find(api_addresses_, selected_api_address_item_) !=
                api_addresses_.end();
        ImGui::BeginDisabled(!updatable);
        for (size_t index = 0; index < api_addresses_.size(); ++index) {
          const auto& address = api_addresses_[index];
          const bool selected = selected_api_address_item_ &&
                                selected_api_address_item_ == address;
          ImGui::PushID(static_cast<int>(index));
          ImGui::TextWrapped("%s", address.c_str());
          if (focus_requested && updatable &&
              (selected || (!selected_address_present && index == 0))) {
            ImGui::SetKeyboardFocusHere();
          }
          if (GuideButton(selected ? "Selected Address" : "Use Address")) {
            selected_api_address_item_ = address.c_str();
            xlive_api->SetAPIAddress(selected_api_address_item_);
            xlive_api->StartWhoamiAsync();
            OpenGuidePage(GuidePage::kSettings);
          }
          ImGui::PopID();
          ImGui::Spacing();
        }
        ImGui::EndDisabled();
        if (!updatable) {
          ImGui::TextWrapped(
              "The API address cannot be changed while playing.");
        }
        if (focus_requested && (!updatable || api_addresses_.empty())) {
          ImGui::SetKeyboardFocusHere();
        }
        if (GuideButton("Back")) {
          GoBackGuidePage();
        }
        break;
      }
      case GuidePage::kAddAPIAddress: {
        ImGui::TextUnformatted("Add API Address");
        ImGui::TextWrapped("Enter the address of the Netplay service.");
        if (focus_requested) {
          ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(GuideContentWidth());
        const bool enter_pressed = ImGui::InputTextWithHint(
            "##AddAddress", "https://example.com", new_api_address_,
            sizeof(new_api_address_), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
          OpenGuidePage(GuidePage::kAddressActions);
        }
        if (GuideButton("Paste or Clear Address")) {
          OpenGuidePage(GuidePage::kAddressActions);
        }
        if (GuideButton("Add") || enter_pressed) {
          const std::string api_address(new_api_address_);
          if (!api_address.empty()) {
            xlive_api->AddAPIAddress(api_address);
            UpdateAPIAddress();
            UpdateSelectedAPIItem();
            OpenGuidePage(GuidePage::kSettings);
          }
        }
        if (GuideButton("Cancel")) {
          GoBackGuidePage();
        }
        break;
      }
      case GuidePage::kAddressActions: {
        ImGui::TextUnformatted("Edit API Address");
        if (focus_requested) {
          ImGui::SetKeyboardFocusHere();
        }
        if (GuideButton("Paste")) {
          if (const char* clipboard = ImGui::GetClipboardText()) {
            xe::string_util::copy_truncating(new_api_address_, clipboard,
                                             sizeof(new_api_address_));
          }
          GoBackGuidePage();
        }
        if (GuideButton("Clear")) {
          std::memset(new_api_address_, 0, sizeof(new_api_address_));
          GoBackGuidePage();
        }
        if (GuideButton("Cancel")) {
          GoBackGuidePage();
        }
        break;
      }
      case GuidePage::kRemoveAPIAddress: {
        ImGui::TextUnformatted("Remove API Address");
        ImGui::TextWrapped("Remove this address from the saved list?");
        ImGui::TextWrapped("%s", selected_api_address_item_);
        ImGui::Spacing();
        // Default to Cancel to avoid accidental removal on activation.
        if (focus_requested) {
          ImGui::SetKeyboardFocusHere();
        }
        if (GuideButton("Cancel")) {
          GoBackGuidePage();
        }
        ImGui::BeginDisabled(!updatable || is_default);
        if (GuideButton("Remove")) {
          xlive_api->RemoveAPIAddress(selected_api_address_item_);
          UpdateAPIAddress();
          UpdateSelectedAPIItem();
          OpenGuidePage(GuidePage::kSettings);
        }
        ImGui::EndDisabled();
        break;
      }
      case GuidePage::kNetworkModes: {
        ImGui::TextUnformatted("Choose Network Mode");
        const bool valid_selection =
            selected_network_mode_index_ >= 0 &&
            selected_network_mode_index_ <
                static_cast<int>(std::size(network_modes_));
        for (int index = 0; index < static_cast<int>(std::size(network_modes_));
             ++index) {
          if (focus_requested && (index == selected_network_mode_index_ ||
                                  (!valid_selection && index == 0))) {
            ImGui::SetKeyboardFocusHere();
          }
          if (GuideButton(network_modes_[index])) {
            selected_network_mode_index_ = index;
            if (xlive_api->SelectNetworkMode(index)) {
              xlive_api->SetNetworkMode(index);
            } else {
              selected_network_mode_index_ = cvars::network_mode;
            }
            OpenGuidePage(GuidePage::kSettings);
          }
        }
        if (GuideButton("Back")) {
          GoBackGuidePage();
        }
        break;
      }
      case GuidePage::kNetworkInterfaces: {
        ImGui::TextUnformatted("Choose Network Interface");
        const bool selected_interface_present =
            selected_network_interface_item_ &&
            std::ranges::find(network_interfaces_,
                              selected_network_interface_item_) !=
                network_interfaces_.end();
        ImGui::BeginDisabled(!updatable);
        for (size_t index = 0; index < network_interfaces_.size(); ++index) {
          const auto& interface_name = network_interfaces_[index];
          const bool selected =
              selected_network_interface_item_ &&
              selected_network_interface_item_ == interface_name;
          ImGui::PushID(static_cast<int>(index));
          ImGui::TextWrapped("%s", interface_name.c_str());
          if (focus_requested && updatable &&
              (selected || (!selected_interface_present && index == 0))) {
            ImGui::SetKeyboardFocusHere();
          }
          if (GuideButton(selected ? "Selected Interface" : "Use Interface")) {
            selected_interface_index_ = index;
            selected_network_interface_item_ = interface_name.c_str();
            const auto guid_keys = std::views::keys(network_interface_guids_);
            const std::vector<std::string> guids = {guid_keys.begin(),
                                                    guid_keys.end()};
            network_adapter_manager_->SetSelectedAdapterGUID(
                guids.at(selected_interface_index_));
            OpenGuidePage(GuidePage::kSettings);
          }
          ImGui::PopID();
          ImGui::Spacing();
        }
        ImGui::EndDisabled();
        if (!updatable) {
          ImGui::TextWrapped(
              "The network interface cannot be changed while playing.");
        }
        if (focus_requested && (!updatable || network_interfaces_.empty())) {
          ImGui::SetKeyboardFocusHere();
        }
        if (GuideButton("Back")) {
          GoBackGuidePage();
        }
        break;
      }
    }
    ImGui::EndDisabled();

    if (close_requested) {
      dialog_opened_ = false;
      ImGui::CloseCurrentPopup();
    }
    EndGuidePopup();
  }

  if (!dialog_opened_) {
    Close();
    emulator_window_->ToggleNetplaySettingsDialog();
  }
}
#endif

void NetplaySettingsDialog::UpdateInterfaceGUIDs() {
  for (const auto& adapter : network_adapter_manager_->GetAdapters()) {
    const std::string guid = adapter.AdapterName;
    const std::string interface_name =
        network_adapter_manager_->GetAdapterFriendlyName(adapter);

    network_interface_guids_[guid] = interface_name;
  }

  const auto network_interfaces = std::views::values(network_interface_guids_);
  network_interfaces_ = {network_interfaces.begin(), network_interfaces.end()};
}

void NetplaySettingsDialog::UpdateSelectedInterfaceItemAndIndex() {
  const auto guid_keys = std::views::keys(network_interface_guids_);

  const auto it = std::ranges::find(
      guid_keys, network_adapter_manager_->GetSelectedAdapaterGUID());

  if (it != guid_keys.end()) {
    selected_interface_index_ =
        static_cast<size_t>(std::distance(guid_keys.begin(), it));
    selected_network_interface_item_ = network_interface_guids_.at(*it).c_str();
  } else {
    selected_interface_index_ = 0;
    selected_network_interface_item_ = "Unspecified Network";
  }
}

void NetplaySettingsDialog::RefreshInterfaces() {
  network_interfaces_.clear();
  network_interface_guids_.clear();

  UpdateInterfaceGUIDs();
  UpdateSelectedInterfaceItemAndIndex();
}

void xe::app::NetplaySettingsDialog::UpdateAPIAddress() {
  api_addresses_ =
      emulator_window_->emulator()->GetXboxLiveAPI()->ParseAPIList();
}

void xe::app::NetplaySettingsDialog::UpdateSelectedAPIItem() {
  const std::string current_api_address =
      emulator_window_->emulator()->GetXboxLiveAPI()->GetApiAddress();

  const auto it = std::ranges::find(api_addresses_, current_api_address);

  if (it != api_addresses_.end()) {
    selected_api_address_item_ = it->c_str();
  } else {
    selected_api_address_item_ = "Unspecified API Address";
  }
}

void xe::app::NetplaySettingsDialog::UpdateSelectedNetworkModeIndex() {
  selected_network_mode_index_ = cvars::network_mode;
}

void xe::app::NetplaySettingsDialog::InitializeCheckboxSettings() {
  xhttp_ = cvars::xhttp;
  auto_check_for_updates_ = cvars::auto_check_updates;
  logging_ = cvars::logging;
  discord_ = cvars::discord;
  bind_interface_ = cvars::bind_interface;
}

void NetplaySettingsDialog::ActivateDiscordState(bool state) {
  const auto emulator = emulator_window_->emulator();
  const auto kernel_state = emulator->kernel_state();

  discord::DiscordPresence::SetDiscordState(state);

  if (cvars::discord) {
    discord::DiscordPresence::Initialize();

    if (emulator->is_title_open()) {
      const auto user = kernel_state->xam_state()->GetUserProfile(
          uint32_t(cvars::discord_presence_user_index));

      if (user) {
        discord::DiscordPresence::PlayingTitle(
            emulator->title_name(), xe::to_utf8(user->GetPresenceString()));
      }
    } else {
      discord::DiscordPresence::NotPlaying();
    }

    discord::DiscordPresence::Update();
  } else {
    discord::DiscordPresence::Shutdown();
  }
}

void NetplayStatusDialog::OnDraw(ImGuiIO& io) {
#if XE_PLATFORM_WINRT
  OnDrawGuide();
#else
  if (!dialog_opened_) {
    dialog_opened_ = true;
    ImGui::OpenPopup("Netplay Status");
  }

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();

  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(450, -1), ImVec2(450, -1));
  if (ImGui::BeginPopupModal(
          "Netplay Status", &dialog_opened_,
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    const bool close_requested =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Escape, false));

    ImGui::SetWindowFontScale(1.10f);

    const auto emulator = emulator_window_->emulator();
    const auto kernel_state = emulator->kernel_state();
    const auto xlive_api = kernel_state->GetXboxLiveAPI();

    const auto status = GetNetplayStatusText(emulator);
    ImGui::Text("Network mode: %s", status.mode);
    ImGui::Spacing();
    ImGui::Text("Netplay services: %s", status.services);
    ImGui::Spacing();
    ImGui::Text("UPnP: %s", status.upnp);
    ImGui::Spacing();
    if (status.failed) {
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 50, 50, 255));
    }
    ImGui::Text("API Server: %s", status.api);
    if (status.failed) {
      ImGui::PopStyleColor();
    }
    if (ImGui::TextLink(cvars::api_address.c_str())) {
      std::jthread open_link(LaunchWebBrowser, cvars::api_address);
      open_link.detach();
    }
    ImGui::Spacing();

    if (xlive_api->IsXUIDMismatched()) {
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 50, 50, 255));
      ImGui::Text("XUID mismatch expect unstable netplay!");
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(50, 240, 50, 255));
      ImGui::Text(
          "Go to Netplay->Manager->Delete Netplay Profiles to fix this issue.");
      ImGui::PopStyleColor(2);
    }

    ImGui::SetWindowFontScale(1.0f);

    ImGui::Separator();
    if (ImGui::Button("Close") || close_requested) {
      dialog_opened_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (!dialog_opened_) {
    Close();
    emulator_window_->ToggleNetplayStatusDialog();
  }
#endif
}

#if XE_PLATFORM_WINRT
void NetplayStatusDialog::OnDrawGuide() {
  if (!dialog_opened_) {
    dialog_opened_ = true;
    ImGui::OpenPopup("Netplay Status");
  }

  if (BeginGuidePopup("Netplay Status", &dialog_opened_, guide_dialog_state_,
                      imgui_drawer())) {
    const bool close_requested = ConsumeGuideBackPress();
    const bool focus_requested =
        !guide_activation_armed_ && GuideActivationReleased();
    if (focus_requested) {
      guide_activation_armed_ = true;
    }
    ImGui::BeginDisabled(!guide_activation_armed_);
    const auto emulator = emulator_window_->emulator();
    const auto xlive_api = emulator->kernel_state()->GetXboxLiveAPI();
    const auto status = GetNetplayStatusText(emulator);
    ImGui::TextWrapped("Network mode: %s", status.mode);
    ImGui::Spacing();
    ImGui::TextWrapped("Netplay services: %s", status.services);
    ImGui::Spacing();
    ImGui::TextWrapped("UPnP: %s", status.upnp);
    ImGui::Spacing();
    if (status.failed) {
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 50, 50, 255));
    }
    ImGui::TextWrapped("API Server: %s", status.api);
    if (status.failed) {
      ImGui::PopStyleColor();
    }
    ImGui::TextWrapped("%s", cvars::api_address.c_str());
    if (GuideButton("Open API Address")) {
      std::jthread open_link(LaunchWebBrowser, cvars::api_address);
      open_link.detach();
    }

    if (xlive_api->IsXUIDMismatched()) {
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 50, 50, 255));
      ImGui::TextWrapped("XUID mismatch expect unstable netplay!");
      ImGui::PopStyleColor();
      ImGui::TextWrapped(
          "Go to Netplay > Manager > Delete Netplay Profiles to fix this "
          "issue.");
    }

    ImGui::Spacing();
    if (focus_requested) {
      ImGui::SetKeyboardFocusHere();
    }
    if (GuideButton("Close") || close_requested) {
      dialog_opened_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    EndGuidePopup();
  }

  if (!dialog_opened_) {
    Close();
    emulator_window_->ToggleNetplayStatusDialog();
  }
}
#endif

}  // namespace app
}  // namespace xe
