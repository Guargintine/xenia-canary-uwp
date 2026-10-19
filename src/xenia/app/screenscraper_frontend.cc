// Copyright 2026 Xenia Canary. Released under the BSD license.
#include "xenia/base/platform.h"
#if XE_PLATFORM_WINRT
#include "xenia/app/screenscraper_artwork.h"
#include "xenia/app/screenscraper_frontend.h"
#include "xenia/app/screenscraper_target.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Security.Credentials.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include "third_party/imgui/imgui.h"
#include "xenia-canary-uwp/UWPUtil.h"
#include "xenia-canary-uwp/WinRTKeyboard.h"
#include "xenia-canary-uwp/XeniaUWP.h"
#include "xenia-canary-uwp/game_storage.h"
#include "xenia/app/guide_dialog.h"
#include "xenia/app/ui_text_effect_helpers.h"
#include "xenia/base/cvar.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"
#include "xenia/config.h"

DEFINE_bool(screenscraper_enabled, false,
            "Enable user-requested ScreenScraper downloads.", "ScreenScraper");
DEFINE_string(screenscraper_media_root, "E:/ES-DE/downloaded_media",
              "Shared ES-DE downloaded_media root containing xbox360/.",
              "ScreenScraper");
DEFINE_string(screenscraper_language, "en", "ScreenScraper metadata language.",
              "ScreenScraper");
DEFINE_string(screenscraper_region, "us",
              "Preferred ScreenScraper media region.", "ScreenScraper");
DEFINE_uint32(screenscraper_media_mask, xe::app::screenscraper::kDefaultMedia,
              "ScreenScraper media selection bit mask.", "ScreenScraper");
DEFINE_bool(screenscraper_metadata, true, "Download ScreenScraper metadata.",
            "ScreenScraper");
DEFINE_bool(screenscraper_overwrite, false,
            "Replace previously downloaded media when explicitly downloading.",
            "ScreenScraper");
DEFINE_string(ui_gamelist_art_type, "covers", "Artwork used in the game list.",
              "UI");
DEFINE_string(ui_detail_art_type, "screenshots",
              "Artwork used above game details.", "UI");
DEFINE_string(ui_dynamic_background_type, "fanart",
              "Dynamic background: off, fanart, screenshots, titlescreens, "
              "miximages or videos.",
              "UI");

namespace xe::app {
namespace {
using screenscraper::MediaType;
struct ArtOption {
  const char* name;
  const char* value;
  MediaType type;
};
constexpr ArtOption kArt[] = {
    {"2D box art", "covers", MediaType::Cover},
    {"3D box art", "3dboxes", MediaType::Box3D},
    {"Mix image", "miximages", MediaType::MixImage},
    {"Fan art", "fanart", MediaType::FanArt},
    {"Screenshot", "screenshots", MediaType::Screenshot},
    {"Title screen", "titlescreens", MediaType::TitleScreen},
    {"Logo / marquee", "marquees", MediaType::Marquee},
    {"Back cover", "backcovers", MediaType::BackCover},
    {"Physical media", "physicalmedia", MediaType::PhysicalMedia},
};
constexpr std::array kBoxArtFallback = {
    MediaType::Cover,   MediaType::Box3D,      MediaType::MixImage,
    MediaType::FanArt,  MediaType::Screenshot, MediaType::TitleScreen,
    MediaType::Marquee, MediaType::BackCover,  MediaType::PhysicalMedia};
constexpr std::array kLandscapeArtFallback = {
    MediaType::FanArt,   MediaType::Screenshot, MediaType::TitleScreen,
    MediaType::MixImage, MediaType::Cover,      MediaType::Box3D,
    MediaType::Marquee,  MediaType::BackCover,  MediaType::PhysicalMedia};
constexpr const wchar_t* kCredentialResource = L"Xenia.ScreenScraper";
template <typename T>
T Get(const char* name) {
  auto* value = dynamic_cast<cvar::ConfigVar<T>*>(cvar::ConfigVars->at(name));
  return value->GetTypedConfigValue();
}
template <typename T>
void Set(const char* name, T value) {
  dynamic_cast<cvar::ConfigVar<T>*>(cvar::ConfigVars->at(name))
      ->SetConfigValue(std::move(value));
}
float ControlWidth() {
  return std::max(1.0f, std::min(ImGui::GetContentRegionAvail().x, 520.0f));
}
bool Toggle(const char* label, const char* name) {
  bool value = Get<bool>(name);
  if (!ImGui::Checkbox(label, &value)) {
    return false;
  }
  Set(name, value);
  return true;
}
std::string CachedMediaPath(const screenscraper::CachedGame& game,
                            MediaType type) {
  auto found = game.media.find(type);
  if (found == game.media.end()) {
    return {};
  }
  if (type != MediaType::Video && type != MediaType::Manual &&
      xe::utf8::lower_ascii(xe::path_to_utf8(found->second.extension())) ==
          ".webp") {
    return {};
  }
  return xe::path_to_utf8(found->second);
}
bool ArtworkCombo(const char* label, const char* cvar) {
  std::string value = Get<std::string>(cvar);
  const char* preview = "2D box art";
  for (const auto& option : kArt) {
    if (value == option.value) {
      preview = option.name;
    }
  }
  ImGui::PushID(cvar);
  ScopedAccentComboStyle accent_combo_style;
  bool changed = false;
  if (ImGui::BeginCombo(label, preview)) {
    for (const auto& option : kArt) {
      if (ImGui::Selectable(option.name, value == option.value)) {
        Set(cvar, std::string(option.value));
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::PopID();
  return changed;
}
}  // namespace

screenscraper::Target MakeScreenScraperTarget(const std::filesystem::path& path,
                                              const std::string& name) {
  const auto roots = UWP::GetPaths();
  return MakeScreenScraperTarget(path, name, roots);
}

struct ScreenScraperFrontend::MetadataEditor {
  screenscraper::Target target;
  screenscraper::Metadata original, base;
  GuideDialogState guide;
  std::shared_ptr<ui::ImmediateTexture> list_art, detail_art;
  std::shared_ptr<ui::ImmediateTexture> fallback_list_art, fallback_detail_art;
  std::unordered_map<std::string, std::shared_ptr<ui::ImmediateTexture>>
      textures;
  uint64_t art_revision = UINT64_MAX, selected_result = 0;
  ImGuiID results_window = 0, preview_window = 0;
  bool save_requested = false;
  std::optional<MediaType> preview_type;
  MediaType list_type = MediaType::Cover, detail_type = MediaType::Screenshot;
  std::array<char, 512> title{}, query{};
  std::array<char, 8192> description{};
  std::array<char, 256> developer{}, publisher{}, genre{};
  std::array<char, 64> release_date{}, players{};
  int rating = 0;
  bool rating_changed = false;
  bool open = true, request_open = true, search_view = false;
  bool focus_requested = false, search_by_id = false;
  bool selected_match = false, owns_search = false;
  uint64_t generation = 0;
  uint64_t applied_result = 0;
  std::string status, observed_query, submitted_query;
  std::chrono::steady_clock::time_point query_changed;

  template <size_t N>
  static void Copy(std::array<char, N>& buffer, const std::string& value) {
    size_t count = std::min(value.size(), N - 1);
    while (count < value.size() && count &&
           (static_cast<unsigned char>(value[count]) & 0xC0) == 0x80) {
      --count;
    }
    std::memcpy(buffer.data(), value.data(), count);
    buffer[count] = 0;
  }
  static std::string FormatReleaseDate(const std::string& value) {
    return value.size() == 10 && value[4] == '-' && value[7] == '-'
               ? value.substr(8, 2) + "/" + value.substr(5, 2) + "/" +
                     value.substr(0, 4)
               : value;
  }
  void Populate(const screenscraper::Metadata& metadata) {
    base = metadata;
    Copy(title, metadata.title);
    Copy(description, metadata.description);
    Copy(release_date, FormatReleaseDate(metadata.release_date));
    Copy(developer, metadata.developer);
    Copy(publisher, metadata.publisher);
    Copy(genre, metadata.genre);
    Copy(players, metadata.players);
    rating = std::clamp(int(std::lround(metadata.rating * 100.0)), 0, 100);
    rating_changed = false;
  }
  bool ReadEditedMetadata(screenscraper::Metadata& result) {
    result = base;
    const auto field = [](std::string& destination, const auto& buffer) {
      auto initial = buffer;
      Copy(initial, destination);
      if (std::strcmp(initial.data(), buffer.data()) != 0) {
        destination = buffer.data();
      }
    };
    field(result.title, title);
    field(result.description, description);
    field(result.developer, developer);
    field(result.publisher, publisher);
    field(result.genre, genre);
    field(result.players, players);
    if (rating_changed) {
      result.rating = rating / 100.0;
    }
    const std::string date = release_date.data();
    if (date != FormatReleaseDate(base.release_date)) {
      result.release_date = date;
      if (!date.empty()) {
        int year = 0, month = 0, day = 0;
        const auto format = [&](size_t first, size_t second, char separator) {
          if (date.size() != 10) {
            return false;
          }
          for (size_t i = 0; i < date.size(); ++i) {
            if (i == first || i == second) {
              if (date[i] != separator) {
                return false;
              }
            } else if (!std::isdigit(static_cast<unsigned char>(date[i]))) {
              return false;
            }
          }
          return true;
        };
        if (format(2, 5, '/')) {
          std::sscanf(date.c_str(), "%d/%d/%d", &day, &month, &year);
        } else if (format(4, 7, '-')) {
          std::sscanf(date.c_str(), "%d-%d-%d", &year, &month, &day);
        }
        if (!std::chrono::year_month_day(std::chrono::year(year),
                                         std::chrono::month(unsigned(month)),
                                         std::chrono::day(unsigned(day)))
                 .ok() ||
            year < 1) {
          status = "Use DD/MM/YYYY or YYYY-MM-DD for the release date.";
          return false;
        }
        char normalized[16];
        std::snprintf(normalized, sizeof(normalized), "%04d-%02d-%02d", year,
                      month, day);
        result.release_date = normalized;
      }
    }
    return true;
  }
};

ScreenScraperFrontend::ScreenScraperFrontend() {
  LoadAccount();
  Configure();
}
ScreenScraperFrontend::~ScreenScraperFrontend() { service_.Cancel(); }
bool ScreenScraperFrontend::Configure(bool replace_selected_match) {
  screenscraper::Configuration config;
  config.media_root = media_root();
  config.cache_root = std::filesystem::u8path(UWP::GetLocalState());
  config.developer = screenscraper::GetDeveloperIdentity();
  config.account = saved_account_;
  config.language = Get<std::string>("screenscraper_language");
  config.region = Get<std::string>("screenscraper_region");
  config.media_mask = Get<uint32_t>("screenscraper_media_mask");
  config.download_metadata = Get<bool>("screenscraper_metadata");
  config.overwrite =
      replace_selected_match || Get<bool>("screenscraper_overwrite");
  return service_.Configure(std::move(config));
}
void ScreenScraperFrontend::LoadAccount() {
  try {
    winrt::Windows::Security::Credentials::PasswordVault vault;
    auto saved = vault.FindAllByResource(kCredentialResource);
    if (!saved.Size()) {
      return;
    }
    auto credential = saved.GetAt(0);
    credential.RetrievePassword();
    const auto user = winrt::to_string(credential.UserName());
    const auto password = winrt::to_string(credential.Password());
    saved_account_ = {user, password};
    std::strncpy(user_, user.c_str(), sizeof(user_) - 1);
    std::strncpy(password_, password.c_str(), sizeof(password_) - 1);
  } catch (...) { /* An absent saved credential is the normal first run. */
  }
}
void ScreenScraperFrontend::SaveAccount(bool clear) {
  try {
    winrt::Windows::Security::Credentials::PasswordVault vault;
    if (!clear && (!user_[0] || !password_[0])) {
      account_status_ = "Enter both your ScreenScraper username and password.";
      return;
    }
    if (!clear) {
      vault.Add(winrt::Windows::Security::Credentials::PasswordCredential(
          kCredentialResource, winrt::to_hstring(user_),
          winrt::to_hstring(password_)));
    }
    try {
      auto saved = vault.FindAllByResource(kCredentialResource);
      for (const auto& credential : saved) {
        if (clear || winrt::to_string(credential.UserName()) != user_) {
          vault.Remove(credential);
        }
      }
    } catch (const winrt::hresult_error& error) {
      if (!clear || error.code() != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        throw;
      }
    }
    if (clear) {
      std::memset(user_, 0, sizeof(user_));
      std::memset(password_, 0, sizeof(password_));
    }
    saved_account_ = clear ? screenscraper::Account{}
                           : screenscraper::Account{user_, password_};
    Configure();
    account_status_ = clear ? "Saved account cleared."
                            : "Account saved in Windows PasswordVault.";
  } catch (...) {
    account_status_ =
        "Windows could not update the saved account. Please try again.";
  }
}
void ScreenScraperFrontend::BeginFrame() {
  const auto revision = service_.GetRevision();
  if (revision != revision_) {
    cache_.clear();
    revision_ = revision;
  }
  cache_budget_ = 2;
}
const screenscraper::CachedGame* ScreenScraperFrontend::Cached(
    const screenscraper::Target& target, bool priority, bool* lookup_complete) {
  if (lookup_complete) {
    *lookup_complete = false;
  }
  const auto key = xe::path_to_utf8(target.content_path) + "\n" +
                   xe::path_to_utf8(target.content_root) + "\n" +
                   xe::path_to_utf8(target.relative_game_path) + "\n" +
                   (target.directory_as_file ? "directory" : "file");
  auto found = cache_.find(key);
  if (found == cache_.end()) {
    if (!priority && !cache_budget_) {
      return nullptr;
    }
    if (cache_budget_) {
      --cache_budget_;
    }
    found = cache_.emplace(key, service_.LoadCached(target)).first;
  }
  if (lookup_complete) {
    *lookup_complete = true;
  }
  return found->second ? &*found->second : nullptr;
}
std::filesystem::path ScreenScraperFrontend::media_root() const {
  return std::filesystem::u8path(Get<std::string>("screenscraper_media_root"));
}
bool ScreenScraperFrontend::SetMediaRoot(const std::filesystem::path& path) {
  if (service_.GetProgress().state == screenscraper::State::Working) {
    return false;
  }
  const auto previous = Get<std::string>("screenscraper_media_root");
  Set("screenscraper_media_root", xe::path_to_utf8(path));
  if (!Configure()) {
    Set("screenscraper_media_root", previous);
    return false;
  }
  config::SaveConfig();
  return true;
}
std::string ScreenScraperFrontend::ArtPath(
    const screenscraper::CachedGame& game, bool detail) const {
  const auto kind =
      Get<std::string>(detail ? "ui_detail_art_type" : "ui_gamelist_art_type");
  const bool landscape =
      kind == "fanart" || kind == "screenshots" || kind == "titlescreens";
  return xe::path_to_utf8(screenscraper::SelectedArtwork(
      game, kind, landscape ? kLandscapeArtFallback : kBoxArtFallback));
}
bool ScreenScraperFrontend::BackgroundEnabled() const {
  return Get<std::string>("ui_dynamic_background_type") != "off";
}
std::string ScreenScraperFrontend::BackgroundPath(
    const screenscraper::CachedGame& game) const {
  const auto kind = Get<std::string>("ui_dynamic_background_type");
  if (kind == "off") {
    return {};
  }
  return xe::path_to_utf8(screenscraper::SelectedArtwork(
      game, kind == "videos" ? "fanart" : kind));
}
std::string ScreenScraperFrontend::VideoPath(
    const screenscraper::CachedGame& game) const {
  return Get<std::string>("ui_dynamic_background_type") == "videos"
             ? CachedMediaPath(game, MediaType::Video)
             : std::string();
}

void ScreenScraperFrontend::OpenMetadataEditor(
    const screenscraper::Target& target, screenscraper::Metadata metadata,
    std::shared_ptr<ui::ImmediateTexture> list_art,
    std::shared_ptr<ui::ImmediateTexture> detail_art) {
  if (target.content_path.empty()) {
    return;
  }
  editor_ = std::make_unique<MetadataEditor>();
  editor_->target = target;
  editor_->fallback_list_art = std::move(list_art);
  editor_->fallback_detail_art = std::move(detail_art);
  editor_->list_type =
      screenscraper::ArtworkType(Get<std::string>("ui_gamelist_art_type"))
          .value_or(MediaType::Cover);
  editor_->detail_type =
      screenscraper::ArtworkType(Get<std::string>("ui_detail_art_type"))
          .value_or(MediaType::Screenshot);
  if (metadata.title.empty()) {
    metadata.title = target.name;
  }
  editor_->original = metadata;
  editor_->Populate(metadata);
}

bool ScreenScraperFrontend::MetadataEditorOpen() const {
  return editor_ && editor_->open;
}

bool ScreenScraperFrontend::DrawMetadataEditor(ui::ImGuiDrawer* drawer) {
  if (!MetadataEditorOpen()) {
    return false;
  }
  auto& editor = *editor_;
  constexpr const char* popup = "Edit Metadata##screenscraper_metadata";
  const auto texture = [&](const std::filesystem::path& path) {
    const auto key = xe::path_to_utf8(path);
    const auto [found, inserted] = editor.textures.try_emplace(key);
    if (inserted && !key.empty()) {
      found->second = LoadOverlayTexture(drawer, key.c_str(), false);
    }
    return found->second;
  };
  if (!editor.selected_match && editor.art_revision != revision_) {
    editor.list_art = editor.fallback_list_art;
    editor.detail_art = editor.fallback_detail_art;
    editor.textures.clear();
    if (const auto* cached = Cached(editor.target, true)) {
      if (auto art =
              texture(std::filesystem::u8path(ArtPath(*cached, false)))) {
        editor.list_art = art;
      }
      if (auto art = texture(std::filesystem::u8path(ArtPath(*cached, true)))) {
        editor.detail_art = art;
      }
    }
    if (!editor.list_art) {
      editor.list_art = editor.detail_art;
    }
    if (!editor.detail_art) {
      editor.detail_art = editor.list_art;
    }
    editor.art_revision = revision_;
  }
  if (editor.selected_match && !editor.search_view && !editor.save_requested) {
    const auto search = service_.GetSearchSnapshot();
    if (search.generation == editor.generation) {
      for (const auto& match : search.matches) {
        if (match.metadata.source_game_id != editor.applied_result) {
          continue;
        }
        const auto [list, detail] = screenscraper::EditorArtwork(
            match, editor.list_type, editor.detail_type);
        editor.list_art = texture(list);
        editor.detail_art = texture(detail);
        for (const auto type : {editor.list_type, editor.detail_type}) {
          if (service_.RequestPreview(editor.generation, editor.applied_result,
                                      type)) {
            break;
          }
        }
        break;
      }
    }
  }
  if (editor.request_open) {
    ImGui::OpenPopup(popup);
    editor.request_open = false;
  }
  if (!BeginGuidePopup(popup, nullptr, editor.guide, drawer, true)) {
    return true;
  }
  if (editor.focus_requested && InitializeGuideNavigation()) {
    editor.focus_requested = false;
  }
  bool close = false, download_started = false;
  const bool busy =
      service_.GetProgress().state == screenscraper::State::Working;
  const bool can_search = Get<bool>("screenscraper_enabled") &&
                          !saved_account_.user.empty() &&
                          !saved_account_.password.empty();
  const float gap = ImGui::GetStyle().ItemSpacing.y;
  const float line = ImGui::GetTextLineHeight();
  const auto child_flags =
      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar;
  const auto text_field = [](const char* label, auto& buffer,
                             bool multiline = false) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);
    PrepareGuideTextInput("##value");
    if (multiline) {
      ImGui::InputTextMultiline(
          "##value", buffer.data(), buffer.size(),
          ImVec2(-1.0f, ImGui::GetTextLineHeight() * 3.5f),
          ImGuiInputTextFlags_NoHorizontalScroll);
    } else {
      ImGui::InputText("##value", buffer.data(), buffer.size());
    }
    if (ImGui::IsItemActivated()) {
      UWP::ShowKeyboard();
    }
    ImGui::PopID();
  };
  const auto preview = [&](const char* label,
                           const std::shared_ptr<ui::ImmediateTexture>& art,
                           float height, bool loading = false) {
    ImGui::TextUnformatted(label);
    const auto start = ImGui::GetCursorScreenPos();
    const float width = GuideContentWidth();
    height = std::max(1.0f, height);
    if (art) {
      drawer->RetainTextureForFrame(art);
      const float scale = std::min(width / std::max(1.0f, float(art->width)),
                                   height / std::max(1.0f, float(art->height)));
      const ImVec2 size(art->width * scale, art->height * scale);
      const ImVec2 min(start.x + (width - size.x) * 0.5f,
                       start.y + (height - size.y) * 0.5f);
      ImGui::GetWindowDrawList()->AddImage(
          reinterpret_cast<ImTextureID>(art.get()), min,
          ImVec2(min.x + size.x, min.y + size.y));
    } else {
      const char* message =
          loading ? "Loading artwork..." : "No artwork available";
      const auto size = ImGui::CalcTextSize(message);
      ImGui::GetWindowDrawList()->AddText(
          ImVec2(start.x + std::max(0.0f, (width - size.x) * 0.5f),
                 start.y + height * 0.5f),
          ImGui::GetColorU32(ImGuiCol_TextDisabled), message);
    }
    ImGui::Dummy(ImVec2(width, height));
  };
  const auto search_art = [&](const screenscraper::Match& match,
                              MediaType type) {
    return texture(screenscraper::ArtworkPreview(match, type));
  };
  if (editor.search_view) {
    bool search_mode_changed = false;
    {
      ScopedAccentComboStyle style;
      ImGui::TextUnformatted("Search by");
      if (ImGui::BeginCombo("##search_by", editor.search_by_id
                                            ? "ScreenScraper game ID"
                                            : "Title")) {
        for (bool by_id : {false, true}) {
          if (ImGui::Selectable(by_id ? "ScreenScraper game ID" : "Title",
                                editor.search_by_id == by_id) &&
              editor.search_by_id != by_id) {
            editor.search_by_id = by_id;
            editor.query.fill(0);
            search_mode_changed = true;
          }
        }
        ImGui::EndCombo();
      }
    }
    text_field(editor.search_by_id ? "ScreenScraper game ID" : "Search title",
               editor.query);
    const std::string query = editor.query.data();
    const auto now = std::chrono::steady_clock::now();
    if (search_mode_changed || query != editor.observed_query) {
      if (editor.owns_search) {
        service_.CancelSearch();
      }
      editor.generation = 0;
      editor.selected_result = 0;
      editor.preview_type.reset();
      editor.observed_query = query;
      editor.submitted_query.clear();
      editor.status.clear();
      editor.query_changed = now;
    }
    const auto letters =
        std::count_if(query.begin(), query.end(),
                      [](unsigned char c) { return !std::isspace(c); });
    uint64_t game_id = 0;
    const auto parsed_id =
        std::from_chars(query.data(), query.data() + query.size(), game_id);
    const bool valid_query =
        editor.search_by_id ? parsed_id.ec == std::errc{} && game_id &&
                                  parsed_id.ptr == query.data() + query.size()
                            : letters >= 2 && query.size() <= 256;
    if (can_search && !busy && valid_query && query != editor.submitted_query &&
        now - editor.query_changed >= std::chrono::milliseconds(200)) {
      auto target = editor.target;
      target.name = query;
      if (Configure() &&
          service_.Search(target, editor.search_by_id ? game_id : 0)) {
        editor.owns_search = true;
        editor.generation = service_.GetSearchSnapshot().generation;
      } else {
        editor.status = service_.GetProgress().status;
      }
      editor.submitted_query = query;
    }
    const auto search = service_.GetSearchSnapshot();
    const bool current =
        editor.generation && search.generation == editor.generation;
    std::vector<uint64_t> visible_results;
    ImGui::TextWrapped("%s", current                  ? search.status.c_str()
                             : !editor.status.empty() ? editor.status.c_str()
                             : editor.search_by_id
                                 ? "Enter a numeric ScreenScraper game ID"
                                 : "Enter a title to search");
    const float height =
        std::max(line * 5, ImGui::GetContentRegionAvail().y -
                               ImGui::GetFrameHeightWithSpacing() - gap);
    const screenscraper::Match* selected = nullptr;
    if (current && !search.matches.empty()) {
      for (const auto& match : search.matches) {
        if (match.metadata.source_game_id == editor.selected_result) {
          selected = &match;
        }
      }
      if (!selected) {
        selected = &search.matches.front();
        editor.selected_result = selected->metadata.source_game_id;
      }
    }
    const float width = GuideContentWidth();
    ImGui::BeginChild("##search_results", ImVec2(width * 0.43f, height),
                      ImGuiChildFlags_NavFlattened, child_flags);
    editor.results_window = ImGui::GetCurrentWindow()->ID;
    if (current) {
      for (const auto& match : search.matches) {
        ImGui::PushID(std::to_string(match.metadata.source_game_id).c_str());
        const auto start = ImGui::GetCursorScreenPos();
        const float row_height = line * 3.6f;
        const float row_width = GuideContentWidth();
        const bool activated = ImGui::Selectable(
            "##result", editor.selected_result == match.metadata.source_game_id,
            ImGuiSelectableFlags_NoAutoClosePopups,
            ImVec2(row_width, row_height));
        if (editor.selected_result == match.metadata.source_game_id) {
          LinkGuideNavigationFromWindow(editor.preview_window, ImGuiDir_Left);
        }
        if (activated || ImGui::IsItemFocused()) {
          if (editor.selected_result != match.metadata.source_game_id) {
            editor.preview_type.reset();
          }
          editor.selected_result = match.metadata.source_game_id;
          selected = &match;
        }
        if (ImGui::IsItemVisible()) {
          visible_results.push_back(match.metadata.source_game_id);
          const float image_width = row_height * 0.72f;
          if (const auto art = search_art(match, editor.list_type)) {
            drawer->RetainTextureForFrame(art);
            const float scale =
                std::min((image_width - gap) / float(art->width),
                         (row_height - gap) / float(art->height));
            ImGui::GetWindowDrawList()->AddImage(
                reinterpret_cast<ImTextureID>(art.get()), start,
                ImVec2(start.x + art->width * scale,
                       start.y + art->height * scale));
          }
          const ImVec4 clip(start.x + image_width, start.y, start.x + row_width,
                            start.y + row_height);
          const auto draw_text = [&](float y, const std::string& value) {
            ImGui::GetWindowDrawList()->AddText(
                ImGui::GetFont(), line, ImVec2(clip.x, start.y + y),
                ImGui::GetColorU32(ImGuiCol_Text), value.c_str(), nullptr, 0.0f,
                &clip);
          };
          draw_text(0, match.metadata.title);
          draw_text(line * 1.15f, "Xbox 360");
          draw_text(line * 2.3f, MetadataEditor::FormatReleaseDate(
                                     match.metadata.release_date));
        }
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##match_preview", ImVec2(0, height),
                      ImGuiChildFlags_NavFlattened, child_flags);
    editor.preview_window = ImGui::GetCurrentWindow()->ID;
    if (selected) {
      auto preview_type = editor.preview_type.value_or(editor.detail_type);
      const char* artwork_label = "2D box art";
      for (const auto& option : kArt) {
        if (option.type == preview_type) {
          artwork_label = option.name;
        }
      }
      {
        ScopedAccentComboStyle style;
        const bool artwork_open =
            ImGui::BeginCombo("##preview_type", artwork_label);
        if (!artwork_open) {
          LinkGuideNavigationFromWindow(editor.results_window, ImGuiDir_Right);
        }
        if (artwork_open) {
          for (const auto& option : kArt) {
            if (ImGui::Selectable(option.name, option.type == preview_type)) {
              editor.preview_type = preview_type = option.type;
            }
          }
          ImGui::EndCombo();
        }
      }
      for (const auto type :
           {preview_type, editor.list_type, editor.detail_type}) {
        if (service_.RequestPreview(editor.generation, editor.selected_result,
                                    type)) {
          break;
        }
      }
      const float preview_height = height * 0.32f;
      preview("Artwork preview", search_art(*selected, preview_type),
              preview_height,
              service_.GetProgress().state == screenscraper::State::Working);
      const float details_height =
          std::max(line, ImGui::GetContentRegionAvail().y -
                             ImGui::GetFrameHeightWithSpacing());
      ImGui::BeginChild("##match_details", ImVec2(0, details_height),
                        ImGuiChildFlags_NavFlattened, child_flags);
      ImGui::TextWrapped("%s", selected->metadata.title.c_str());
      ImGui::Text("%.1f / 5 stars", selected->metadata.rating * 5.0);
      ImGui::TextWrapped("%s", selected->metadata.description.c_str());
      const auto field = [&](const char* name, const std::string& value) {
        if (!value.empty()) {
          ImGui::TextWrapped("%s: %s", name, value.c_str());
        }
      };
      field("Release date",
            MetadataEditor::FormatReleaseDate(selected->metadata.release_date));
      field("Publisher", selected->metadata.publisher);
      field("Developer", selected->metadata.developer);
      field("Genre", selected->metadata.genre);
      field("Players", selected->metadata.players);
      ImGui::EndChild();
      if (GuideButton("Use This Entry")) {
        editor.Populate(selected->metadata);
        const auto [list, detail] = screenscraper::EditorArtwork(
            *selected, editor.list_type, editor.detail_type);
        editor.list_art = texture(list);
        editor.detail_art = texture(detail);
        editor.applied_result = selected->metadata.source_game_id;
        editor.selected_match = true;
        editor.search_view = false;
        editor.focus_requested = true;
      }
    }
    ImGui::EndChild();
    if (current && editor.search_view) {
      for (const auto game_id : visible_results) {
        if (game_id == editor.selected_result) {
          continue;
        }
        if (service_.RequestPreview(editor.generation, game_id,
                                    editor.list_type)) {
          break;
        }
      }
    }
    if (GuideButton("Back to metadata") || ConsumeGuideBackPress()) {
      if (editor.owns_search) {
        service_.CancelSearch();
      }
      editor.search_view = false;
      editor.focus_requested = true;
    }
  } else {
    const float height = std::max(line, ImGui::GetContentRegionAvail().y);
    const float left_width = GuideContentWidth() * 0.27f;
    ImGui::BeginChild("##metadata_artwork", ImVec2(left_width, height),
                      ImGuiChildFlags_NavFlattened, child_flags);
    const float image_height =
        std::max(line, (height - 2 * line - 4 * gap) * 0.5f);
    preview("Game list artwork", editor.list_art, image_height);
    preview("Details artwork", editor.detail_art, image_height);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##metadata_editor", ImVec2(0, height),
                      ImGuiChildFlags_NavFlattened, child_flags);
    const float fields_height = std::max(
        line, height - 2 * ImGui::GetFrameHeightWithSpacing() - 2 * line - gap);
    ImGui::BeginChild("##metadata_fields", ImVec2(0, fields_height),
                      ImGuiChildFlags_NavFlattened, child_flags);
    text_field("Title", editor.title);
    text_field("Description", editor.description, true);
    ImGui::TextUnformatted("Rating");
    char rating_label[32];
    std::snprintf(rating_label, sizeof(rating_label), "%.2f / 5 stars",
                  editor.rating / 20.0);
    ImGui::SetNextItemWidth(-1.0f);
    editor.rating_changed |=
        ImGui::SliderInt("##rating", &editor.rating, 0, 100, rating_label);
    text_field("Release date", editor.release_date);
    text_field("Developer", editor.developer);
    text_field("Publisher", editor.publisher);
    text_field("Genre", editor.genre);
    text_field("Players", editor.players);
    ImGui::EndChild();
    ImGui::BeginDisabled(!can_search || (!editor.owns_search && busy));
    if (GuideButton("Find ScreenScraper match")) {
      editor.search_view = true;
      editor.focus_requested = true;
      editor.search_by_id = false;
      editor.preview_type.reset();
      MetadataEditor::Copy(editor.query, editor.title.data());
      editor.observed_query.clear();
      editor.submitted_query.clear();
      editor.status.clear();
      editor.query_changed = std::chrono::steady_clock::now();
      editor.generation = 0;
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!editor.owns_search && busy);
    const float button_width =
        (GuideContentWidth() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (GuideButton("Save", ImVec2(button_width, 0))) {
      if (editor.owns_search) {
        service_.CancelSearch();
      }
      editor.save_requested = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (GuideButton("Cancel", ImVec2(button_width, 0))) {
      close = true;
    }
    if (editor.save_requested && !busy) {
      editor.save_requested = false;
      screenscraper::Metadata metadata;
      if (editor.ReadEditedMetadata(metadata) &&
          service_.SaveMetadata(editor.target, metadata, editor.original,
                                &editor.status)) {
        cache_.clear();
        if (editor.selected_match && can_search) {
          auto target = editor.target;
          target.name = metadata.title;
          target.matched_game_id = metadata.source_game_id;
          if (Configure(true)) {
            download_started = service_.Start({target});
          }
        }
        close = true;
      }
    }
    if (!editor.status.empty()) {
      ImGui::TextWrapped("%s", editor.status.c_str());
    }
    ImGui::EndChild();
    if (ConsumeGuideBackPress()) {
      close = true;
    }
  }
  if (close) {
    if (editor.owns_search && !download_started) {
      service_.CancelSearch();
    }
    editor.open = false;
    ImGui::CloseCurrentPopup();
  }
  EndGuidePopup();
  return editor.open;
}

void ScreenScraperFrontend::DrawUISettings() {
  bool changed = false;
  auto select_preference = [&](const char* label, const char* name,
                               const char* const* labels,
                               const char* const* values, int count) {
    const auto value = Get<std::string>(name);
    int selection = 0;
    for (int i = 0; i < count; ++i) {
      if (value == values[i]) {
        selection = i;
      }
    }
    ImGui::PushID(name);
    ScopedAccentComboStyle accent_combo_style;
    if (ImGui::BeginCombo(label, labels[selection])) {
      for (int i = 0; i < count; ++i) {
        if (ImGui::Selectable(labels[i], selection == i)) {
          Set(name, std::string(values[i]));
          changed = true;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  };
  const char* regions[] = {"Auto", "USA", "Europe", "Japan", "World"};
  const char* region_values[] = {"", "us", "eu", "jp", "wor"};
  select_preference("Preferred region", "screenscraper_region", regions,
                    region_values, 5);
  const char* languages[] = {"English", "French",     "German",  "Spanish",
                             "Italian", "Portuguese", "Japanese"};
  const char* language_values[] = {"en", "fr", "de", "es", "it", "pt", "ja"};
  select_preference("Metadata language", "screenscraper_language", languages,
                    language_values, 7);

  changed |= ArtworkCombo("Game list artwork", "ui_gamelist_art_type");
  changed |= ArtworkCombo("Details artwork", "ui_detail_art_type");
  const char* backgrounds[] = {"Off",          "Fan art",   "Screenshot",
                               "Title screen", "Mix image", "Video (muted)"};
  const char* background_values[] = {"off",          "fanart",    "screenshots",
                                     "titlescreens", "miximages", "videos"};
  select_preference("Dynamic background", "ui_dynamic_background_type",
                    backgrounds, background_values, 6);
  if (changed) {
    config::SaveConfig();
    if (service_.GetProgress().state != screenscraper::State::Working) {
      Configure();
    }
  }
}

void ScreenScraperFrontend::DrawSettings(
    const screenscraper::Target&,
    const std::function<void()>& select_custom_folder, bool picker_pending) {
  const auto progress = service_.GetProgress();
  const bool busy = progress.state == screenscraper::State::Working;
  const bool logged_in =
      !saved_account_.user.empty() && !saved_account_.password.empty();
  bool changed = false;
  ImGui::BeginDisabled(busy || picker_pending);
  changed |= Toggle("Enable ScreenScraper downloads", "screenscraper_enabled");
  ImGui::TextUnformatted("Account");
  if (logged_in) {
    ImGui::TextWrapped("Saved login: %s", saved_account_.user.c_str());
  } else {
    ImGui::TextWrapped(
        "Enter your ScreenScraper username and password, then select Save "
        "account.");
  }
  if (ImGui::Button("Edit username")) {
    UWP::ShowKeyboard();
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::InputText("Username##screenscraper_username", user_, sizeof(user_));
  if (ImGui::Button("Edit password")) {
    UWP::ShowKeyboard();
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::InputText("Password##screenscraper_password", password_,
                   sizeof(password_), ImGuiInputTextFlags_Password);
  if (ImGui::Button("Save account")) {
    SaveAccount(false);
  }
  if (ImGui::Button("Clear saved account")) {
    SaveAccount(true);
  }
  if (!account_status_.empty()) {
    ImGui::TextWrapped("%s", account_status_.c_str());
  }
  if (screenscraper::GetDeveloperIdentity().id.empty()) {
    ImGui::TextWrapped(
        "ScreenScraper is unavailable in this build: developer credentials "
        "were not configured.");
  }
  const auto current = xe::path_to_utf8(media_root());
  constexpr const char* roots[] = {"D:/DevelopmentFiles/ES-DE/downloaded_media",
                                   "E:/ES-DE/downloaded_media"};
  int preset = UWP::SameStoragePath(current, roots[0])   ? 0
               : UWP::SameStoragePath(current, roots[1]) ? 1
                                                         : 2;
  constexpr const char* modes[] = {"Development files", "USB path", "Custom"};
  {
    ScopedAccentComboStyle accent_combo_style;
    if (ImGui::BeginCombo("Downloaded media location##screenscraper_location",
                          modes[preset])) {
      for (int index = 0; index < 3; ++index) {
        if (ImGui::Selectable(modes[index], preset == index)) {
          if (index == 2) {
            select_custom_folder();
          } else {
            SetMediaRoot(std::filesystem::u8path(roots[index]));
          }
        }
      }
      ImGui::EndCombo();
    }
  }
  ImGui::TextWrapped("%s", current.c_str());
  if (ImGui::Button("Reload cached artwork")) {
    cache_.clear();
    Configure();
  }
  changed |= Toggle("Download metadata", "screenscraper_metadata");
  uint32_t mask = Get<uint32_t>("screenscraper_media_mask");
  for (const auto& option : kArt) {
    bool value = (mask & screenscraper::MediaBit(option.type)) != 0;
    if (ImGui::Checkbox(option.name, &value)) {
      mask = value ? mask | screenscraper::MediaBit(option.type)
                   : mask & ~screenscraper::MediaBit(option.type);
      changed = true;
    }
  }
  for (const auto& option :
       {ArtOption{"Video", "videos", MediaType::Video},
        ArtOption{"Manual", "manuals", MediaType::Manual}}) {
    bool value = (mask & screenscraper::MediaBit(option.type)) != 0;
    if (ImGui::Checkbox(option.name, &value)) {
      mask = value ? mask | screenscraper::MediaBit(option.type)
                   : mask & ~screenscraper::MediaBit(option.type);
      changed = true;
    }
  }
  Set("screenscraper_media_mask", mask);
  changed |= Toggle("Overwrite existing downloads", "screenscraper_overwrite");
  ImGui::EndDisabled();
  if (changed) {
    config::SaveConfig();
    if (!busy) {
      Configure();
    }
  }

  ImGui::BeginDisabled(busy || picker_pending ||
                       !Get<bool>("screenscraper_enabled") || !logged_in);
  if (ImGui::Button("Download library assets") && Configure()) {
    std::vector<screenscraper::Target> targets;
    for (const auto& [path, name] : UWP::GetGames()) {
      auto target =
          MakeScreenScraperTarget(std::filesystem::u8path(path), name);
      const auto* cached = Cached(target, true);
      if (cached &&
          (cached->local_override || cached->metadata.source_game_id ||
           !cached->metadata.title.empty())) {
        target.matched_game_id = cached->metadata.source_game_id;
        if (!cached->metadata.title.empty()) {
          target.name = cached->metadata.title;
        }
      } else {
        const auto title_id = UWP::GetTitleIdFromPath(path);
        UWP::UnityGameMetadata metadata;
        if (!title_id.empty() &&
            UWP::TryGetUnityMetadata(title_id, &metadata) &&
            !metadata.display_name.empty() &&
            metadata.display_name.find(name) != std::string::npos) {
          target.name = metadata.display_name;
        }
      }
      targets.push_back(std::move(target));
    }
    service_.Start(std::move(targets));
  }
  ImGui::EndDisabled();
  if (busy && ImGui::Button("Cancel download or search")) {
    service_.Cancel();
  }
  if (!progress.status.empty()) {
    ImGui::TextWrapped("%s", progress.status.c_str());
  }
  if (progress.total) {
    ImGui::ProgressBar(float(progress.completed) / float(progress.total),
                       ImVec2(ControlWidth(), 0));
    ImGui::TextWrapped("%zu / %zu games, %zu files downloaded, %zu failed",
                       progress.completed, progress.total,
                       progress.downloaded_files, progress.failed);
    if (progress.unavailable_files) {
      ImGui::TextWrapped("Assets unavailable on ScreenScraper: %zu",
                         progress.unavailable_files);
    }
  }
}
}  // namespace xe::app
#endif
