/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <regex>
#include <tuple>

#include "xenia/app/emulator_window.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/png_utils.h"
#include "xenia/base/string_util.h"
#include "xenia/base/system.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/kernel/xam/xcontent/xcontent_package_directory.h"

#include "xenia/app/content_list_dialog.h"

#include "third_party/fmt/include/fmt/format.h"

#if XE_PLATFORM_WINRT
#include "xenia-canary-uwp/UWPUtil.h"
#endif

namespace xe {
namespace app {

bool isBootableType(XContentType type) {
  return type == XContentType::kArcadeTitle ||
         type == XContentType::kGameDemo ||
         type == XContentType::kInstalledGame ||
         type == XContentType::kXbox360Title;
}

namespace {

std::string FormatSize(size_t bytes) {
  constexpr const char* units[] = {"B", "KiB", "MiB", "GiB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < static_cast<int>(xe::countof(units)) - 1) {
    value /= 1024.0;
    ++unit;
  }
  return unit == 0 ? fmt::format("{} B", bytes)
                   : fmt::format("{:.1f} {}", value, units[unit]);
}

}  // namespace

void ContentListDialog::Initialize() {
  size_t next_index = 0;
  const auto content_root = emulator_window_.emulator()->content_root();
  std::error_code ec;
  if (!std::filesystem::is_directory(content_root, ec)) {
    return;
  }

  // Lists content_root/<xuid>/<title id>/<content type>/*.
  const std::regex xuid_pattern("[A-Fa-f0-9]{16}");
  const std::regex title_pattern("[A-Fa-f0-9]{8}");
  const auto xuid_dirs = xe::filesystem::FilterByName(
      xe::filesystem::ListDirectories(content_root), xuid_pattern);
  for (const auto& xuid_dir : xuid_dirs) {
    const auto xuid_path = xuid_dir.path / xuid_dir.name;
    const uint64_t xuid = xe::string_util::from_string<uint64_t>(
        xe::path_to_utf8(xuid_dir.name), true);
    const auto title_dirs = xe::filesystem::FilterByName(
        xe::filesystem::ListDirectories(xuid_path), title_pattern);
    for (const auto& title_dir : title_dirs) {
      const uint32_t title_id = xe::string_util::from_string<uint32_t>(
          xe::path_to_utf8(title_dir.name), true);
      if (title_filter_ && title_id != title_filter_) {
        continue;
      }
      const auto title_path = title_dir.path / title_dir.name;
      for (const auto& [_, types] : typesToEnumerate) {
        for (const auto type : types) {
          const auto type_path =
              title_path / fmt::format("{:08X}", static_cast<uint32_t>(type));
          for (const auto& file_info : xe::filesystem::ListFiles(type_path)) {
            AddEntry(type, xuid, title_id, file_info.path / file_info.name,
                     next_index);
          }
        }
      }
    }
  }

  for (auto& [type, entries] : enumerated_entries_) {
    std::ranges::sort(
        entries, [](const ContentListEntry& a, const ContentListEntry& b) {
          return std::tie(a.title_id, a.name) < std::tie(b.title_id, b.name);
        });
  }

#if XE_PLATFORM_WINRT
  RefreshStorageText();
#endif
}

void ContentListDialog::AddEntry(XContentType type, uint64_t xuid,
                                 uint32_t title_id,
                                 const std::filesystem::path& path,
                                 size_t& next_index) {
  const auto package = content_manager_->OpenPackage(path, true);
  if (!package || !package->IsValidPackage()) {
    return;
  }

  const auto metadata = package->GetContentMetadata();
  ContentListEntry entry{
      .index = next_index++,
      .xuid = xuid,
      .title_id = title_id,
      .size = package->GetPackageSize(),
      .is_package =
          package->GetPackageType() == kernel::xam::ContentPackageType::Package,
      .name = metadata.display_name(),
      .title_name = package->GetContainerMetadata()->title_name(),
      .path = path,
  };
  if (entry.name.empty()) {
    entry.name = xe::to_utf16(xe::path_to_utf8(path.filename()));
  }

  if (xuid) {
    entry.owner = fmt::format("{:016X}", xuid);
    auto* profile_manager = emulator_window_.emulator()
                                ->kernel_state()
                                ->xam_state()
                                ->profile_manager();
    if (profile_manager) {
      if (const auto* account = profile_manager->GetAccount(xuid)) {
        entry.owner = account->GetGamertagString();
      }
    }
  }

  std::vector<uint8_t> buffer{};
  if (package->GetThumbnail(buffer) == X_ERROR_SUCCESS && !buffer.empty()) {
    entry.icon = imgui_drawer()->LoadImGuiIcon(buffer);
  }

  enumerated_entries_[type].push_back(std::move(entry));
}

std::vector<ContentListDialog::CategoryEntry>
ContentListDialog::CollectCategory(size_t category) const {
  std::vector<CategoryEntry> entries;
  if (category >= typesToEnumerate.size()) {
    return entries;
  }
  for (const auto type : typesToEnumerate[category].second) {
    const auto it = enumerated_entries_.find(type);
    if (it == enumerated_entries_.end()) {
      continue;
    }
    for (const auto& entry : it->second) {
      entries.emplace_back(type, &entry);
    }
  }
  return entries;
}

void ContentListDialog::OnCategoryDraw(
    ImGuiIO& io, const XContentType type,
    const std::vector<ContentListEntry>& entries) {
  // Add optional search?
  if (ImGui::BeginTable("", 2, ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableNextRow(0, xe::ui::default_image_icon_size.y);

    for (const auto& entry : entries) {
      OnEntryDraw(io, type, entry);
    }

    ImGui::EndTable();
  }
}

void ContentListDialog::OnEntryDraw(ImGuiIO& io, const XContentType type,
                                    const ContentListEntry& entry) {
  const auto start_position = ImGui::GetCursorPos();
  // First Column
  ImGui::TableSetColumnIndex(0);
  if (entry.icon) {
    ImGui::Image(reinterpret_cast<ImTextureID>(entry.icon.get()),
                 ui::default_image_icon_size);
  } else {
    ImGui::Dummy(ui::default_image_icon_size);
  }

  // Second Column
  ImGui::TableNextColumn();
  if (entry.is_package) {
    ImGui::Text("Name: %s (Filename: %s)", xe::to_utf8(entry.name).c_str(),
                xe::path_to_utf8(entry.path.filename()).c_str());
  } else {
    ImGui::Text("Name: %s", xe::to_utf8(entry.name).c_str());
  }

  if (!entry.title_name.empty()) {
    ImGui::Text("Title Name: %s (Title ID: %s)",
                xe::to_utf8(entry.title_name).c_str(),
                fmt::format("{:08X}", entry.title_id).c_str());
  } else {
    ImGui::Text("Title ID: %s", fmt::format("{:08X}", entry.title_id).c_str());
  }

  if (!entry.owner.empty()) {
    ImGui::Text("Profile: %s", entry.owner.c_str());
  }
  ImGui::Text("Content Installation Type: %s",
              entry.is_package ? "Package" : "Directory");
  ImGui::Text("Installation Size: %s", FormatSize(entry.size).c_str());

  // Third Column
  ImGui::TableNextColumn();
  const ImVec2 end_draw_position =
      ImVec2(ImGui::GetCursorPos().x - start_position.x,
             ImGui::GetCursorPos().y - start_position.y);

  ImGui::SetCursorPos(start_position);
  if (ImGui::Selectable(fmt::format("##{:08X}Selectable", entry.index).c_str(),
                        selected_entry_ == entry.index,
                        ImGuiSelectableFlags_SpanAllColumns,
                        end_draw_position)) {
    selected_entry_ = entry.index;
    ImGui::OpenPopup(fmt::format("Popup-{}", entry.index).c_str());
  }

  if (ImGui::BeginPopupContextItem(
          fmt::format("Popup-{}", entry.index).c_str())) {
    selected_entry_ = entry.index;

    if (isBootableType(type)) {
      ImGui::BeginDisabled(emulator_window_.emulator()->is_title_open());
      if (ImGui::MenuItem("Start title", nullptr, nullptr, true)) {
        pending_boot_ = {&entry};
      }

      ImGui::EndDisabled();
    }

    if (ImGui::MenuItem("Open location", nullptr, nullptr, true)) {
      if (entry.is_package) {
        std::thread path_open(LaunchFileExplorer, entry.path.parent_path());
        path_open.detach();
      } else {
        std::thread path_open(LaunchFileExplorer, entry.path);
        path_open.detach();
      }
    }
    if (entry.is_package) {
      if (ImGui::MenuItem("Extract", nullptr, nullptr, true)) {
        const auto path = entry.path;
        emulator_window_.window()->app_context().CallInUIThread(
            [emulator_window = &emulator_window_, path]() {
              emulator_window->ExtractContent(path);
            });
      }
    }

    ImGui::BeginDisabled(emulator_window_.emulator()->is_title_open());
    if (ImGui::BeginMenu("Delete")) {
      if (ImGui::MenuItem("Yes, delete it!")) {
        pending_deletion_ = {type, &entry};
      }
      ImGui::EndMenu();
    }
    ImGui::EndDisabled();

    ImGui::EndPopup();
  }
}

void ContentListDialog::OnDraw(ImGuiIO& io) {
#if XE_PLATFORM_WINRT
  OnDrawGuide(io);
#else
  ExecutePendingStart();
  ExecutePendingDeletion();

  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  const auto viewport_size = ImGui::GetMainViewport()->Size;
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(viewport_size.x * 0.05f, viewport_size.y * 0.05f),
      ImVec2(viewport_size.x * 0.75f, viewport_size.y * 0.85f));

  ImGui::SetNextWindowBgAlpha(0.90f);
  bool dialog_open = true;
  if (!ImGui::Begin(
          "Content List", &dialog_open,
          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  if (ImGui::BeginTabBar("mainTab")) {
    for (const auto& [name, types] : typesToEnumerate) {
      if (ImGui::BeginTabItem(fmt::format("{}", name).c_str())) {
        bool is_everything_empty = true;
        for (const auto& type : types) {
          if (enumerated_entries_.contains(type)) {
            is_everything_empty = false;
            OnCategoryDraw(io, type, enumerated_entries_.at(type));
          }
        }
        if (is_everything_empty) {
          ImGui::Text("No %s available", name.c_str());
        }
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }

  if (!dialog_open) {
    Close();
    ImGui::End();
    emulator_window_.ToggleContentListDialog();
    return;
  }

  ImGui::End();
#endif
}

#if XE_PLATFORM_WINRT
bool ContentListDialog::OnGuideEntryDraw(const ContentListEntry& entry,
                                         float row_height, float icon_size) {
  const ImVec2 row_min = ImGui::GetCursorScreenPos();
  const float row_width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  const ImVec2 row_max(row_min.x + row_width, row_min.y + row_height);
  const bool was_selected = selected_entry_ == entry.index;

  ImGui::PushID(static_cast<int>(entry.index));
  const bool clicked = ImGui::Selectable("##content_entry", was_selected,
                                         ImGuiSelectableFlags_None,
                                         ImVec2(row_width, row_height));
  const bool focused = ImGui::IsItemFocused();
  ImGui::PopID();
  // Focusing a row also selects it.
  if (clicked || focused) {
    selected_entry_ = entry.index;
    if (!was_selected) {
      guide_confirm_delete_ = false;
    }
  }

  auto* draw_list = ImGui::GetWindowDrawList();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float padding = style.FramePadding.x;
  const ImVec2 icon_min(row_min.x + padding,
                        row_min.y + (row_height - icon_size) * 0.5f);
  const ImVec2 icon_max(icon_min.x + icon_size, icon_min.y + icon_size);
  if (entry.icon) {
    draw_list->AddImage(reinterpret_cast<ImTextureID>(entry.icon.get()),
                        icon_min, icon_max);
  } else {
    draw_list->AddRect(icon_min, icon_max,
                       ImGui::GetColorU32(ImGuiCol_TextDisabled));
  }

  const std::string primary = xe::to_utf8(entry.name);
  std::string details = entry.title_name.empty()
                            ? fmt::format("{:08X}", entry.title_id)
                            : xe::to_utf8(entry.title_name);
  details += fmt::format(" | {} | {}", FormatSize(entry.size),
                         entry.is_package ? "Package" : "Directory");
  if (!entry.owner.empty()) {
    details += fmt::format(" | {}", entry.owner);
  }

  const float text_x = icon_max.x + padding * 2.0f;
  const float line_height = ImGui::GetTextLineHeight();
  const float line_gap = style.ItemInnerSpacing.y;
  const float text_y =
      row_min.y + (row_height - line_height * 2.0f - line_gap) * 0.5f;
  draw_list->PushClipRect(ImVec2(text_x, row_min.y), row_max, true);
  draw_list->AddText(ImVec2(text_x, text_y), ImGui::GetColorU32(ImGuiCol_Text),
                     primary.c_str());
  draw_list->AddText(ImVec2(text_x, text_y + line_height + line_gap),
                     ImGui::GetColorU32(ImGuiCol_TextDisabled),
                     details.c_str());
  draw_list->PopClipRect();

  return selected_entry_ == entry.index;
}

void ContentListDialog::RequestExtraction(
    const std::filesystem::path& package_path) {
  // This dialog closes before the picker returns, so only the window is
  // captured.
  auto* emulator_window = &emulator_window_;
  emulator_window->imgui_drawer()->SetIgnoreInput(true);
  UWP::SelectFolder([emulator_window, package_path](std::string folder) {
    emulator_window->imgui_drawer()->SetIgnoreInput(false);
    if (folder.empty()) {
      return;
    }
    emulator_window->ExtractContentTo({package_path},
                                      std::filesystem::u8path(folder));
  });
}

void ContentListDialog::RefreshStorageText() {
  guide_storage_.clear();
  std::error_code ec;
  const auto space =
      std::filesystem::space(emulator_window_.emulator()->content_root(), ec);
  if (!ec) {
    guide_storage_ =
        fmt::format("Storage: {} free of {}", FormatSize(space.available),
                    FormatSize(space.capacity));
  }
}

void ContentListDialog::OnDrawGuide(ImGuiIO& io) {
  ExecutePendingDeletion();

  constexpr char kPopup[] = "Installed Content##content_list";
  if (!guide_popup_opened_) {
    guide_popup_opened_ = true;
    ImGui::OpenPopup(kPopup);
  }

  bool open = true;
  if (!BeginGuidePopup(kPopup, &open, guide_state_, imgui_drawer(), true)) {
    Close();
    emulator_window_.ToggleContentListDialog();
    return;
  }

  const float uy = io.DisplaySize.y / 576.0f;
  const ImGuiStyle& style = ImGui::GetStyle();
  auto* emulator = emulator_window_.emulator();
  const bool title_open = emulator->is_title_open();

  if (title_filter_) {
    ImGui::Text("Title: %08X", title_filter_);
  }
  if (!guide_storage_.empty()) {
    ImGui::TextUnformatted(guide_storage_.c_str());
  }

  const auto category_label = [&](size_t category) {
    return fmt::format("{} ({})", typesToEnumerate[category].first,
                       CollectCategory(category).size());
  };
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                      ImVec2(GuideContentWidth(), FLT_MAX));
  if (ImGui::BeginCombo("##content_category",
                        category_label(guide_category_).c_str())) {
    for (size_t i = 0; i < typesToEnumerate.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(category_label(i).c_str(), guide_category_ == i)) {
        guide_category_ = i;
        selected_entry_.reset();
        guide_confirm_delete_ = false;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }

  const auto entries = CollectCategory(guide_category_);
  const ContentListEntry* selected = nullptr;
  XContentType selected_type = XContentType::kInvalid;

  const float row_height = 44.0f * uy;
  const float icon_size = 34.0f * uy;
  // Space for the confirmation text and up to three action buttons.
  const float actions_height = ImGui::GetFrameHeightWithSpacing() * 3.0f +
                               ImGui::GetTextLineHeightWithSpacing() * 3.0f;
  const float list_height = std::max(
      row_height * 2.0f, ImGui::GetContentRegionAvail().y - actions_height);
  ImGui::BeginChild("##content_entries",
                    ImVec2(GuideContentWidth(), list_height),
                    ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  if (entries.empty()) {
    ImGui::TextWrapped("No %s installed.",
                       typesToEnumerate[guide_category_].first.c_str());
  }
  for (const auto& [type, entry] : entries) {
    if (OnGuideEntryDraw(*entry, row_height, icon_size)) {
      selected = entry;
      selected_type = type;
    }
  }
  ImGui::EndChild();
  ImGui::Spacing();

  if (selected) {
    if (guide_confirm_delete_) {
      ImGui::TextWrapped("Delete \"%s\"? This cannot be undone.",
                         xe::to_utf8(selected->name).c_str());
      const float half_width =
          std::max(1.0f, (GuideContentWidth() - style.ItemSpacing.x) * 0.5f);
      if (GuideButton("Yes, delete it##confirm_delete",
                      ImVec2(half_width, 0.0f))) {
        pending_deletion_ = {selected_type, selected};
        guide_confirm_delete_ = false;
      }
      ImGui::SameLine();
      if (guide_confirm_focus_requested_) {
        // Focus Cancel when the confirmation opens.
        ImGui::SetKeyboardFocusHere();
        guide_confirm_focus_requested_ = false;
      }
      if (GuideButton("Cancel##confirm_delete", ImVec2(half_width, 0.0f))) {
        guide_confirm_delete_ = false;
      }
    } else {
      if (title_open) {
        ImGui::TextWrapped(
            "Close the running game to start or delete content.");
      }
      if (isBootableType(selected_type)) {
        ImGui::BeginDisabled(title_open);
        if (GuideButton("Start title")) {
          // Deferred, as RunTitle clears the dialogs, including this one.
          const auto path = selected->path;
          auto* emulator_window = &emulator_window_;
          emulator_window_.app_context().CallInUIThreadDeferred(
              [emulator_window, path]() { emulator_window->RunTitle(path); });
          open = false;
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
      }
      if (selected->is_package && GuideButton("Extract...")) {
        RequestExtraction(selected->path);
        open = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::BeginDisabled(title_open);
      if (GuideButton("Delete")) {
        guide_confirm_delete_ = true;
        guide_confirm_focus_requested_ = true;
      }
      ImGui::EndDisabled();
    }
  } else if (!entries.empty()) {
    ImGui::TextWrapped("Select an item to start, extract or delete it.");
  }
  if (!guide_status_.empty()) {
    ImGui::TextWrapped("%s", guide_status_.c_str());
  }

  if (ConsumeGuideBackPress()) {
    if (guide_confirm_delete_) {
      guide_confirm_delete_ = false;
    } else {
      open = false;
      ImGui::CloseCurrentPopup();
    }
  }
  EndGuidePopup();

  if (!open) {
    Close();
    emulator_window_.ToggleContentListDialog();
  }
}
#endif  // XE_PLATFORM_WINRT

void ContentListDialog::ExecutePendingStart() {
  if (pending_boot_ && !emulator_window_.emulator()->is_title_open()) {
    // Path must be provided as a copy, not reference.
    const auto path = pending_boot_.value()->path;
    pending_boot_.reset();

    std::jthread(
        [path](EmulatorWindow* emulator_window) {
          emulator_window->RunTitle(path);
        },
        &emulator_window_)
        .detach();
  }
}

void ContentListDialog::ExecutePendingDeletion() {
  if (!pending_deletion_) {
    return;
  }

  const auto [type, entry] = pending_deletion_.value();
  pending_deletion_.reset();

  const auto file_to_delete = entry->path;
  const bool is_directory_package = !entry->is_package;
  const std::string deleted_name = xe::to_utf8(entry->name);

  if (std::erase_if(enumerated_entries_[type], [&](const ContentListEntry& e) {
        return &e == entry;
      }) == 0) {
    return;
  }

  if (enumerated_entries_[type].empty()) {
    enumerated_entries_.erase(type);
  }
  selected_entry_.reset();

  // Before removing anything ensure that path is a package entry
  // (xuid/title/type/name) within content. We REALLY don't want to yeet
  // everything!
  // Strip a trailing separator from the root before comparing paths.
  std::filesystem::path content_root =
      emulator_window_.emulator()->content_root().lexically_normal();
  if (content_root.filename().empty() && content_root.has_parent_path()) {
    content_root = content_root.parent_path();
  }
  const auto relative = file_to_delete.lexically_relative(content_root);
  const auto depth =
      static_cast<size_t>(std::distance(relative.begin(), relative.end()));
  if (!xe::path_to_utf8(file_to_delete)
           .starts_with(xe::path_to_utf8(content_root)) ||
      relative.empty() || *relative.begin() == std::filesystem::path("..") ||
      depth < 4) {
    XELOGE("{}: Refusing to delete {} outside of content root {}", __func__,
           file_to_delete, content_root);
    return;
  }

  std::error_code ec;
  std::filesystem::remove_all(file_to_delete, ec);
  if (ec) {
    XELOGE("{}: Cannot delete {}: {} ({:08X})", __func__, file_to_delete,
           ec.message(), ec.value());
#if XE_PLATFORM_WINRT
    guide_status_ =
        fmt::format("Could not delete {}: {}", deleted_name, ec.message());
#endif
    return;
  }

  // Remove header file for "directory" type packages
  if (is_directory_package) {
    const auto header_path =
        file_to_delete.parent_path().parent_path() /
        kernel::xam::kGameContentHeaderDirName /
        fmt::format("{:08X}", static_cast<uint32_t>(type)) /
        (file_to_delete.filename().string() + ".header");
    std::filesystem::remove(header_path, ec);  // fine if it doesn't exist
  }

  // Remove the emptied type and title directories, keeping the xuid one.
  for (auto path = file_to_delete.parent_path();
       path.has_parent_path() && path != content_root &&
       path.parent_path() != content_root;
       path = path.parent_path()) {
    const auto dirs = xe::filesystem::ListDirectories(path);
    const bool header_dir =
        dirs.size() == 1 && xe::path_to_utf8(dirs[0].name.filename()) ==
                                kernel::xam::kGameContentHeaderDirName;

    if (!std::filesystem::is_empty(path, ec) && !header_dir) {
      break;
    }
    std::filesystem::remove_all(path, ec);
  }

#if XE_PLATFORM_WINRT
  guide_status_ = fmt::format("Deleted {}.", deleted_name);
  RefreshStorageText();
#endif
}

}  // namespace app
}  // namespace xe
