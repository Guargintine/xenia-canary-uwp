// Copyright 2026 Xenia Canary. Released under the BSD license.
#include "xenia/app/game_title_update_dialog.h"

#include "xenia/base/platform.h"
#if XE_PLATFORM_WINRT
#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "xenia-canary-uwp/UWPUtil.h"
#include "xenia-canary-uwp/title_update_catalogue.h"
#include "xenia-canary-uwp/title_update_catalogue_cache.h"
#include "xenia-canary-uwp/title_update_identity.h"
#include "xenia-canary-uwp/title_update_transaction.h"
#include "xenia/app/guide_dialog.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/content_manager.h"

DECLARE_bool(apply_title_update);
#endif

namespace xe::app {
#if XE_PLATFORM_WINRT
namespace {
namespace fs = std::filesystem;
fs::path HeaderPath(const fs::path& package) {
  auto name = package.filename();
  name += ".header";
  return package.parent_path().parent_path() / "Headers" / "000B0000" / name;
}

bool SameUpdate(const UWP::TitleUpdateIdentity& a,
                const UWP::TitleUpdateIdentity& b) {
  return !a.key.empty() && a.key == b.key;
}
}  // namespace

struct GameTitleUpdateDialog::State {
  enum class Location { kInstalled, kCached, kArchived };
  struct SavedUpdate {
    fs::path path;
    UWP::TitleUpdateIdentity identity;
    Location location;
    std::vector<fs::path> copies;
  };
  struct Completion {
    std::mutex mutex;
    bool listed = false;
    bool downloaded = false;
    bool list_success = false;
    bool download_success = false;
    std::vector<UWP::TitleUpdateCandidate> candidates;
    std::vector<std::string> files;
    std::string list_message;
    std::string download_message;
  };
  bool open = true;
  bool request_open = true;
  bool listing = true;
  bool downloading = false;
  bool refresh_saved = true;
  int install_delay = 0;
  std::string title_id;
  std::string media_id;
  std::string base_version;
  fs::path cache_root;
  GuideDialogState guide;
  std::shared_ptr<Completion> completion = std::make_shared<Completion>();
  std::vector<UWP::TitleUpdateCandidate> candidates;
  std::vector<SavedUpdate> saved;
  std::vector<SavedUpdate> installed;
  std::optional<size_t> online_selection;
  std::optional<size_t> saved_selection;
  fs::path install_pending;
  std::string download_content_id;
  std::string catalogue_error;
  std::string message;

  bool Busy() const { return downloading || !install_pending.empty(); }

  fs::path TitleRoot(Emulator* emulator) const {
    return emulator->content_root() / "0000000000000000" / title_id;
  }

  std::optional<UWP::TitleUpdateIdentity> Inspect(
      const fs::path& path, bool match_media = true) const {
    auto identity = UWP::ReadTitleUpdateIdentity(path);
    if (!identity || fmt::format("{:08X}", identity->title_id) != title_id ||
        (match_media &&
         fmt::format("{:08X}", identity->media_id) != media_id)) {
      return std::nullopt;
    }
    return identity;
  }

  std::vector<SavedUpdate> ReadDirectory(const fs::path& directory,
                                         Location location,
                                         bool match_media = true) const {
    std::vector<SavedUpdate> result;
    std::error_code ec;
    fs::directory_iterator it(directory, ec), end;
    while (!ec && it != end) {
      if (location == Location::kCached && it->path().extension() != ".tu") {
        it.increment(ec);
        continue;
      }
      if (auto identity = Inspect(it->path(), match_media)) {
        result.push_back({it->path(), *identity, location, {it->path()}});
      }
      it.increment(ec);
    }
    if (ec && ec != std::errc::no_such_file_or_directory) {
      throw fs::filesystem_error("Could not read title updates", directory, ec);
    }
    return result;
  }

  void RefreshSaved(Emulator* emulator) {
    const auto selected = saved_selection && *saved_selection < saved.size()
                              ? saved[*saved_selection].path
                              : fs::path{};
    saved.clear();
    installed.clear();
    saved_selection.reset();
    try {
      installed = ReadDirectory(TitleRoot(emulator) / "000B0000",
                                Location::kInstalled, false);
      for (const auto& update : installed) {
        if (fmt::format("{:08X}", update.identity.media_id) == media_id) {
          saved.push_back(update);
        }
      }
      auto cached = ReadDirectory(cache_root, Location::kCached);
      saved.insert(saved.end(), cached.begin(), cached.end());
      const auto backups = TitleRoot(emulator) / "TitleUpdateBackups";
      std::error_code ec;
      fs::directory_iterator media(backups, ec), end;
      while (!ec && media != end) {
        std::error_code folder_ec;
        fs::directory_iterator folder(media->path(), folder_ec);
        while (!folder_ec && folder != end) {
          auto archived =
              ReadDirectory(folder->path() / "000B0000", Location::kArchived);
          saved.insert(saved.end(), archived.begin(), archived.end());
          folder.increment(folder_ec);
        }
        if (folder_ec) {
          throw fs::filesystem_error("Could not read saved title updates",
                                     media->path(), folder_ec);
        }
        media.increment(ec);
      }
      if (ec && ec != std::errc::no_such_file_or_directory) {
        throw fs::filesystem_error("Could not read saved title updates",
                                   backups, ec);
      }
    } catch (const std::exception& error) {
      message = error.what();
    }
    std::sort(saved.begin(), saved.end(), [](const auto& a, const auto& b) {
      if (a.location != b.location) {
        return a.location < b.location;
      }
      if (a.identity.version != b.identity.version) {
        return a.identity.version > b.identity.version;
      }
      return a.path < b.path;
    });
    std::vector<SavedUpdate> unique;
    for (auto& update : saved) {
      auto existing =
          std::find_if(unique.begin(), unique.end(), [&](const auto& item) {
            return SameUpdate(item.identity, update.identity);
          });
      if (existing == unique.end()) {
        unique.push_back(std::move(update));
      } else {
        existing->copies.push_back(update.path);
      }
    }
    saved = std::move(unique);
    for (size_t i = 0; i < saved.size(); ++i) {
      if (std::find(saved[i].copies.begin(), saved[i].copies.end(), selected) !=
          saved[i].copies.end()) {
        saved_selection = i;
      }
    }
    refresh_saved = false;
  }

  const SavedUpdate* FindSavedUpdate(
      const UWP::TitleUpdateCandidate& candidate) const {
    const auto filename = fs::path("TU_" + candidate.id + ".tu");
    for (const auto& update : saved) {
      if (!candidate.content_id.empty()) {
        if (candidate.content_id == update.identity.content_id) {
          return &update;
        }
        continue;
      }
      for (const auto& path : update.copies) {
        if (path.filename() == filename) {
          return &update;
        }
      }
    }
    return nullptr;
  }

  std::string UpdateName(const SavedUpdate& update) const {
    for (const auto& candidate : candidates) {
      if (FindSavedUpdate(candidate) == &update) {
        return fmt::format("Update {}", candidate.version);
      }
    }
    return xe::path_to_utf8(update.path.filename());
  }

  void BackupUpdate(UWP::TitleUpdateTransaction& transaction,
                    const SavedUpdate& update) const {
    transaction.Backup(update.path);
    if (update.identity.has_fragments) {
      auto data = update.path;
      data += ".data";
      transaction.Backup(data);
    }
    const auto header = HeaderPath(update.path);
    if (auto identity = Inspect(header, false);
        identity &&
        (SameUpdate(update.identity, *identity) ||
         (identity->media_id == update.identity.media_id &&
          identity->version == update.identity.version &&
          identity->base_version == update.identity.base_version))) {
      transaction.Backup(header);
    }
  }

  void Install(Emulator* emulator, const fs::path& source) {
    std::unique_ptr<UWP::TitleUpdateTransaction> transaction;
    bool activated = false;
    try {
      if (emulator->is_title_open()) {
        throw std::runtime_error(
            "Downloaded. Close the running game, then select Install.");
      }
      auto source_identity = Inspect(source);
      if (!source_identity || source_identity->profile_id != 0) {
        throw std::runtime_error(
            "This is not a shared title update for the selected game and media "
            "ID.");
      }
      emulator->kernel_state()->content_manager()->CloseContent("UPDATE");
      auto active = ReadDirectory(TitleRoot(emulator) / "000B0000",
                                  Location::kInstalled, false);
      const auto existing =
          std::find_if(active.begin(), active.end(), [&](const auto& item) {
            return item.path == source ||
                   SameUpdate(item.identity, *source_identity);
          });
      if (existing != active.end() && active.size() == 1) {
        message = "This title update is already installed.";
      } else {
        transaction = std::make_unique<UWP::TitleUpdateTransaction>(
            TitleRoot(emulator), media_id);
        for (auto it = active.begin(); it != active.end(); ++it) {
          if (it != existing) {
            BackupUpdate(*transaction, *it);
          }
        }
        if (existing == active.end()) {
          Emulator::ContentInstallEntry entry(source);
          if (emulator->ProcessContentPackageHeader(source, entry) !=
              X_STATUS_SUCCESS) {
            throw std::runtime_error(
                "Could not prepare the selected title update.");
          }
          const auto destination =
              entry.data_installation_path_ / entry.filename_;
          transaction->ExpectOutput(destination);
          transaction->ExpectOutput(entry.header_installation_path_);
          if (source_identity->has_fragments) {
            auto data = destination;
            data += ".data";
            transaction->ExpectOutput(data);
          }
          bool can_move = false;
          if (!source_identity->has_fragments && fs::is_regular_file(source)) {
            auto package =
                emulator->kernel_state()->content_manager()->OpenPackage(source);
            if (package && package->IsReadOnly() &&
                package->GetPackageType() ==
                    kernel::xam::ContentPackageType::Package &&
                package->GetContainerMetadata()->volume_type ==
                    kernel::xam::XContentVolumeType::kStfs) {
              auto device = package->MountPackage();
              if (!device || !device->Initialize()) {
                throw std::runtime_error("Could not open the selected update.");
              }
              can_move = true;
            }
          }
          activated = can_move &&
                      transaction->TryActivate(source, destination, cache_root);
          const auto result = activated
                                  ? X_STATUS_SUCCESS
                                  : emulator->InstallContentPackage(source, entry);
          const auto installed_identity = Inspect(destination);
          if (result != X_STATUS_SUCCESS || !installed_identity ||
              installed_identity->version != source_identity->version ||
              installed_identity->base_version !=
                  source_identity->base_version ||
              installed_identity->profile_id != source_identity->profile_id ||
              (!source_identity->key.empty() &&
               !SameUpdate(*source_identity, *installed_identity))) {
            throw std::runtime_error(
                entry.installation_error_message_.empty()
                    ? "The selected update could not be installed."
                    : entry.installation_error_message_);
          }
        }
        transaction->Commit();
        if (activated) {
          emulator->kernel_state()->BroadcastNotification(
              kXNotificationLiveContentInstalled, 0);
        }
        XELOGI("Title update installed: title {} media {} source {} ({})",
               title_id, media_id, xe::path_to_utf8(source),
               activated ? "moved saved package" : "content installer");
        message =
            "Title update installed. Other versions are saved for later use.";
      }
    } catch (const std::exception& error) {
      XELOGE("Title update install failed: title {} media {}: {}", title_id,
             media_id, error.what());
      message = "Not installed: " + std::string(error.what());
      if (transaction) {
        const auto restored = transaction->Restore();
        if (!restored.empty()) {
          message += " Previous files are retained at " +
                     xe::path_to_utf8(transaction->backup_root()) + ": " +
                     restored;
        }
      }
    }
    refresh_saved = true;
  }

  void Uninstall(Emulator* emulator) {
    std::unique_ptr<UWP::TitleUpdateTransaction> transaction;
    try {
      if (emulator->is_title_open()) {
        throw std::runtime_error(
            "Close the running game before uninstalling its update.");
      }
      emulator->kernel_state()->content_manager()->CloseContent("UPDATE");
      auto active = ReadDirectory(TitleRoot(emulator) / "000B0000",
                                  Location::kInstalled, false);
      transaction = std::make_unique<UWP::TitleUpdateTransaction>(
          TitleRoot(emulator), media_id);
      for (const auto& update : active) {
        BackupUpdate(*transaction, update);
      }
      transaction->Commit();
      message =
          "Title update uninstalled. Saved versions are available to "
          "reinstall.";
    } catch (const std::exception& error) {
      message = "Could not uninstall: " + std::string(error.what());
      if (transaction) {
        const auto restored = transaction->Restore();
        if (!restored.empty()) {
          message += " " + restored;
        }
      }
    }
    refresh_saved = true;
  }

  void RequestCatalogue() {
    listing = true;
    catalogue_error.clear();
    UWP::ListTitleUpdatesForGame(
        title_id, media_id, base_version,
        [result = completion](bool success,
                              std::vector<UWP::TitleUpdateCandidate> candidates,
                              std::string message) {
          std::lock_guard lock(result->mutex);
          result->list_success = success;
          result->candidates = std::move(candidates);
          result->list_message = std::move(message);
          result->listed = true;
        });
  }

  void QueueInstall(const fs::path& path) {
    install_pending = path;
    install_delay = 1;
    message = "Installing title update...";
  }
};

GameTitleUpdateDialog::GameTitleUpdateDialog() = default;
GameTitleUpdateDialog::~GameTitleUpdateDialog() = default;

void GameTitleUpdateDialog::Open(const std::string& title_id,
                                 const std::string& media_id,
                                 const std::string& base_version,
                                 const fs::path& storage_root) {
  const auto normalized_title = UWP::NormalizeTitleUpdateId(title_id);
  const auto normalized_media = UWP::NormalizeTitleUpdateId(media_id);
  const auto normalized_base = UWP::NormalizeTitleUpdateBaseVersion(base_version);
  if (state_ && (state_->Busy() || (state_->title_id == normalized_title &&
                                    state_->media_id == normalized_media &&
                                    state_->base_version == normalized_base &&
                                    !state_->candidates.empty()))) {
    state_->open = true;
    state_->request_open = true;
    state_->refresh_saved = true;
    return;
  }
  state_ = std::make_unique<State>();
  auto& state = *state_;
  state.title_id = normalized_title;
  state.media_id = normalized_media;
  state.base_version = normalized_base;
  state.cache_root =
      storage_root / "title_updates" / state.title_id / state.media_id;
  state.candidates = UWP::LoadTitleUpdateCatalogue(
      state.cache_root / "catalogue.json", state.title_id, state.media_id,
      state.base_version);
  state.RequestCatalogue();
}

bool GameTitleUpdateDialog::is_open() const { return state_ && state_->open; }

void GameTitleUpdateDialog::ProcessPending(Emulator* emulator) {
  if (!state_ || !emulator) {
    return;
  }
  auto& state = *state_;
  if (!state.install_pending.empty() && !emulator->is_title_open()) {
    if (state.install_delay) {
      --state.install_delay;
    } else {
      state.Install(emulator, std::exchange(state.install_pending, {}));
    }
  }
  {
    std::lock_guard lock(state.completion->mutex);
    if (state.completion->listed) {
      if (state.completion->list_success) {
        state.candidates = std::move(state.completion->candidates);
        state.online_selection.reset();
        try {
          UWP::SaveTitleUpdateCatalogue(state.cache_root / "catalogue.json",
                                        state.title_id, state.media_id,
                                        state.candidates);
        } catch (const std::exception&) {
          state.catalogue_error =
              "Update names could not be saved for offline use.";
        }
      } else if (state.completion->list_message.starts_with(
                     "Could not retrieve")) {
        state.catalogue_error = state.candidates.empty()
                                    ? "The update server is unavailable. Saved "
                                      "updates can still be installed."
                                    : "The update server is unavailable. "
                                      "Showing the saved update list.";
      } else {
        state.catalogue_error = std::move(state.completion->list_message);
      }
      state.listing = false;
      state.completion->listed = false;
    }
    if (state.completion->downloaded) {
      state.downloading = false;
      state.message = std::move(state.completion->download_message);
      if (state.completion->download_success &&
          state.completion->files.size() == 1) {
        const auto path = fs::u8path(state.completion->files.front());
        const auto identity = state.Inspect(path);
        if (!identity || (!state.download_content_id.empty() &&
                          identity->content_id != state.download_content_id)) {
          state.message =
              "Not installed: the download does not match the selected update.";
        } else {
          state.QueueInstall(path);
        }
      }
      state.completion->downloaded = false;
      state.refresh_saved = true;
    }
  }
  if (state.refresh_saved) {
    state.RefreshSaved(emulator);
  }
}

bool GameTitleUpdateDialog::Draw(Emulator* emulator, ui::ImGuiDrawer* drawer) {
  if (!is_open() || !emulator) {
    return false;
  }
  auto& state = *state_;
  constexpr char kPopup[] = "Title Updates##game_title_updates";
  if (state.request_open) {
    ImGui::OpenPopup(kPopup);
    state.request_open = false;
  }
  if (!BeginGuidePopup(kPopup, &state.open, state.guide, drawer, true)) {
    state.open = false;
    return false;
  }
  ImGui::Text("Title: %s  Media: %s", state.title_id.c_str(),
              state.media_id.empty() ? "unavailable" : state.media_id.c_str());
  if (state.installed.empty()) {
    ImGui::TextUnformatted("No title update installed.");
  } else if (state.installed.size() > 1) {
    ImGui::TextWrapped(
        "Multiple updates are installed. Select one version to use.");
  } else {
    const auto& installed = state.installed.front();
    const auto selected = std::find_if(
        state.saved.begin(), state.saved.end(),
        [&](const auto& saved) { return saved.path == installed.path; });
    const auto name =
        selected == state.saved.end()
            ? fmt::format("{} (media {:08X})",
                          xe::path_to_utf8(installed.path.filename()),
                          installed.identity.media_id)
            : state.UpdateName(*selected);
    ImGui::TextWrapped("Installed: %s", name.c_str());
  }
  if (!state.installed.empty() && !cvars::apply_title_update) {
    ImGui::TextWrapped(
        "Applying title updates is disabled in the emulator configuration.");
  }
  if (!state.message.empty()) {
    ImGui::TextWrapped("%s", state.message.c_str());
  }
  if (!state.catalogue_error.empty()) {
    ImGui::TextWrapped("%s", state.catalogue_error.c_str());
    ImGui::BeginDisabled(state.Busy() || state.listing);
    if (GuideButton("Refresh update list")) {
      state.RequestCatalogue();
    }
    ImGui::EndDisabled();
  }
  if (state.listing) {
    ImGui::TextUnformatted("Checking available updates...");
  }
  const bool blocked = state.Busy() || emulator->is_title_open();
  if (emulator->is_title_open()) {
    ImGui::TextWrapped("Close the running game before changing title updates.");
  }
  if (state.downloading) {
    ImGui::ProgressBar(UWP::GetDownloadProgress(), ImVec2(-1, 0),
                       "Downloading...");
  }
  ImGui::BeginDisabled(blocked);
  if (!state.installed.empty() && GuideButton("Uninstall title update")) {
    state.Uninstall(emulator);
  }
  if (!state.candidates.empty()) {
    ImGui::TextUnformatted("Available updates");
    const auto online_name = [&](size_t index) {
      const auto& update = state.candidates[index];
      const auto saved = state.FindSavedUpdate(update);
      const char* status =
          !saved                                           ? ""
          : saved->location == State::Location::kInstalled ? " - Installed"
          : saved->location == State::Location::kCached    ? " - Downloaded"
                                                           : " - Saved";
      return fmt::format("Update {} - {:.1f} MiB{}", update.version,
                         update.size_kb / 1024.0, status);
    };
    const auto preview = state.online_selection
                             ? online_name(*state.online_selection)
                             : "Select an update";
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                        ImVec2(GuideContentWidth(), FLT_MAX));
    if (ImGui::BeginCombo("##online_versions", preview.c_str())) {
      for (size_t i = 0; i < state.candidates.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(online_name(i).c_str(),
                              state.online_selection == i)) {
          state.online_selection = i;
          state.saved_selection.reset();
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    if (state.online_selection) {
      const auto& update = state.candidates[*state.online_selection];
      ImGui::TextWrapped(
          "Uploaded: %s\nBase version: %s",
          update.upload_date.empty() ? "Unknown" : update.upload_date.c_str(),
          update.base_version.empty() ? "Unknown"
                                      : update.base_version.c_str());
      const auto saved = state.FindSavedUpdate(update);
      const bool installed = saved &&
                             saved->location == State::Location::kInstalled &&
                             state.installed.size() == 1;
      const char* action = installed                 ? "Installed"
                           : !saved                  ? "Download and install"
                           : state.installed.empty() ? "Install"
                                                     : "Switch to this version";
      ImGui::BeginDisabled(installed);
      if (GuideButton(action)) {
        if (saved) {
          state.QueueInstall(saved->path);
        } else {
          state.downloading = true;
          state.download_content_id = update.content_id;
          state.message.clear();
          auto completion = state.completion;
          UWP::DownloadTitleUpdateForGame(
              state.title_id, state.media_id, update,
              xe::path_to_utf8(state.cache_root),
              [completion](bool success, std::vector<std::string> files,
                           std::string message) {
                std::lock_guard lock(completion->mutex);
                completion->download_success = success;
                completion->files = std::move(files);
                completion->download_message = std::move(message);
                completion->downloaded = true;
              });
        }
      }
      ImGui::EndDisabled();
    }
  }
  if (!state.saved.empty()) {
    ImGui::TextUnformatted("Saved versions");
    const auto saved_name = [&](size_t index) {
      const auto& update = state.saved[index];
      return fmt::format("{}{}", state.UpdateName(update),
                         update.location == State::Location::kInstalled
                             ? " - Installed"
                             : " - Ready to install");
    };
    const auto preview = state.saved_selection
                             ? saved_name(*state.saved_selection)
                             : "Select a saved version";
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                        ImVec2(GuideContentWidth(), FLT_MAX));
    if (ImGui::BeginCombo("##saved_versions", preview.c_str())) {
      for (size_t i = 0; i < state.saved.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(saved_name(i).c_str(),
                              state.saved_selection == i)) {
          state.saved_selection = i;
          state.online_selection.reset();
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    if (state.saved_selection) {
      const auto& update = state.saved[*state.saved_selection];
      const bool installed = update.location == State::Location::kInstalled &&
                             state.installed.size() == 1;
      ImGui::BeginDisabled(installed);
      if (GuideButton(installed ? "Installed##saved"
                      : state.installed.empty()
                          ? "Install##saved"
                          : "Switch to this version##saved")) {
        state.QueueInstall(update.path);
      }
      ImGui::EndDisabled();
    }
  }
  ImGui::EndDisabled();
  if (ConsumeGuideBackPress()) {
    ImGui::CloseCurrentPopup();
    state.open = false;
  }
  EndGuidePopup();
  return state.open;
}
#else
struct GameTitleUpdateDialog::State {};
GameTitleUpdateDialog::GameTitleUpdateDialog() = default;
GameTitleUpdateDialog::~GameTitleUpdateDialog() = default;
void GameTitleUpdateDialog::Open(const std::string&, const std::string&,
                                 const std::string&,
                                 const std::filesystem::path&) {}
void GameTitleUpdateDialog::ProcessPending(Emulator*) {}
bool GameTitleUpdateDialog::Draw(Emulator*, ui::ImGuiDrawer*) { return false; }
bool GameTitleUpdateDialog::is_open() const { return false; }
#endif
}  // namespace xe::app
