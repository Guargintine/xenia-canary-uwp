// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_SCREENSCRAPER_FRONTEND_H_
#define XENIA_APP_SCREENSCRAPER_FRONTEND_H_

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include "xenia/app/screenscraper_service.h"

namespace xe::ui {
class ImGuiDrawer;
class ImmediateTexture;
}  // namespace xe::ui

namespace xe::app {
// Resolve media paths relative to the longest matching games directory.
screenscraper::Target MakeScreenScraperTarget(const std::filesystem::path& path,
                                              const std::string& name);

class ScreenScraperFrontend {
 public:
  ScreenScraperFrontend();
  ~ScreenScraperFrontend();
  screenscraper::Service& service() { return service_; }
  void BeginFrame();
  const screenscraper::CachedGame* Cached(const screenscraper::Target& target,
                                          bool priority = false,
                                          bool* lookup_complete = nullptr);
  uint64_t revision() const { return revision_; }
  std::filesystem::path media_root() const;
  bool SetMediaRoot(const std::filesystem::path& path);
  std::string ArtPath(const screenscraper::CachedGame& game, bool detail) const;
  std::string BackgroundPath(const screenscraper::CachedGame& game) const;
  std::string VideoPath(const screenscraper::CachedGame& game) const;
  bool BackgroundEnabled() const;
  void DrawUISettings();
  void OpenMetadataEditor(
      const screenscraper::Target& target,
      screenscraper::Metadata metadata,
      std::shared_ptr<ui::ImmediateTexture> list_art = {},
      std::shared_ptr<ui::ImmediateTexture> detail_art = {});
  bool MetadataEditorOpen() const;
  bool DrawMetadataEditor(ui::ImGuiDrawer* drawer);
  void DrawSettings(const screenscraper::Target& selected,
                    const std::function<void()>& select_custom_folder,
                    bool folder_picker_pending);

 private:
  bool Configure(bool replace_selected_match = false);
  void LoadAccount();
  void SaveAccount(bool clear);
  screenscraper::Service service_;
  uint64_t revision_ = 0;
  size_t cache_budget_ = 0;
  std::unordered_map<std::string, std::optional<screenscraper::CachedGame>>
      cache_;
  char user_[128] = {};
  char password_[256] = {};
  screenscraper::Account saved_account_;
  std::string account_status_;
  struct MetadataEditor;
  std::unique_ptr<MetadataEditor> editor_;
};
}  // namespace xe::app
#endif
