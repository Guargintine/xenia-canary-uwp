/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_CONTENT_LIST_DIALOG_H_
#define XENIA_APP_CONTENT_LIST_DIALOG_H_

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/platform.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/xbox.h"

#if XE_PLATFORM_WINRT
#include "xenia/app/guide_dialog.h"
#endif

namespace xe {
namespace kernel {
namespace xam {
class ContentManager;
}
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace app {

class EmulatorWindow;

struct ContentListEntry {
  size_t index;
  // XUID of the containing content directory, 0 for shared content.
  uint64_t xuid;
  uint32_t title_id;
  size_t size;
  bool is_package;
  std::u16string name;
  std::u16string title_name;
  // Owning profile's gamertag (or XUID), empty for shared content.
  std::string owner;
  std::filesystem::path path;
  std::unique_ptr<ui::ImmediateTexture> icon;
};

class ContentListDialog final : public ui::ImGuiDialog {
 public:
  // title_filter limits the list to one title ID, 0 lists all.
  ContentListDialog(ui::ImGuiDrawer* imgui_drawer,
                    EmulatorWindow& emulator_window,
                    kernel::xam::ContentManager* content_manager,
                    uint32_t title_filter = 0)
      : ui::ImGuiDialog(imgui_drawer),
        emulator_window_(emulator_window),
        content_manager_(content_manager),
        title_filter_(title_filter) {
    Initialize();
  }

  ~ContentListDialog() {
    for (auto& type : enumerated_entries_) {
      for (auto& entry : type.second) {
        entry.icon.release();
      }
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnCategoryDraw(ImGuiIO& io, const XContentType type,
                      const std::vector<ContentListEntry>& entries);
  void OnEntryDraw(ImGuiIO& io, const XContentType type,
                   const ContentListEntry& entry);

 private:
  using CategoryEntry = std::pair<XContentType, const ContentListEntry*>;

  void Initialize();
  void AddEntry(XContentType type, uint64_t xuid, uint32_t title_id,
                const std::filesystem::path& path, size_t& next_index);

  void ExecutePendingStart();
  void ExecutePendingDeletion();
  // Entries of one category in display order.
  std::vector<CategoryEntry> CollectCategory(size_t category) const;

#if XE_PLATFORM_WINRT
  void OnDrawGuide(ImGuiIO& io);
  // Draws one list row. Returns true when the row is selected.
  bool OnGuideEntryDraw(const ContentListEntry& entry, float row_height,
                        float icon_size);
  void RequestExtraction(const std::filesystem::path& package_path);
  void RefreshStorageText();

  GuideDialogState guide_state_;
  bool guide_popup_opened_ = false;
  bool guide_confirm_delete_ = false;
  bool guide_confirm_focus_requested_ = false;
  size_t guide_category_ = 0;
  std::string guide_status_;
  std::string guide_storage_;
#endif

  EmulatorWindow& emulator_window_;
  kernel::xam::ContentManager* content_manager_;
  uint32_t title_filter_ = 0;

  std::optional<size_t> selected_entry_ = std::nullopt;

  std::map<XContentType, std::vector<ContentListEntry>> enumerated_entries_;
  std::optional<const ContentListEntry*> pending_boot_ = std::nullopt;
  std::optional<std::pair<XContentType, const ContentListEntry*>>
      pending_deletion_ = std::nullopt;

  const std::vector<std::pair<std::string, std::set<XContentType>>>
      typesToEnumerate = {
          {"Xbox 360 Games",
           {XContentType::kXbox360Title, XContentType::kInstalledGame}},
          {"XBLA Games", {XContentType::kArcadeTitle}},
          {"Demos", {XContentType::kGameDemo}},
          {"DLCs", {XContentType::kMarketplaceContent}},
          {"Title Updates", {XContentType::kInstaller}},
          {"Saved Games", {XContentType::kSavedGame}},
      };
};

}  // namespace app
}  // namespace xe

#endif
