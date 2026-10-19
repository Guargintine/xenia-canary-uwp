// Copyright 2026 Xenia Canary. Released under the BSD license.
#include "xenia/base/platform.h"
#if XE_PLATFORM_WINRT
#include "xenia/app/dsu_integration.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <exception>
#include <utility>

#include "third_party/imgui/imgui.h"
#include "xenia-canary-uwp/UWPUtil.h"
#include "xenia-canary-uwp/WinRTKeyboard.h"
#include "xenia-canary-uwp/XeniaUWP.h"
#include "xenia/app/emulator_window.h"
#include "xenia/app/frontend_metadata.h"
#include "xenia/app/screenscraper_frontend.h"
#include "xenia/app/screenscraper_target.h"
#include "xenia/app/stream_destination.h"
#include "xenia/apu/audio_driver.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/ui/presenter.h"

DECLARE_int32(dsu_local_player);

DEFINE_bool(
    dsu_streaming, false,
    "Automatically stream game video and audio while a title is running.",
    "DSU");
DEFINE_string(streaming_url, "",
              "Additional stream destination as udp://IPv4:port.", "Streaming");

namespace xe::app {
namespace {
uint64_t CatalogueId(const std::filesystem::path& path) {
  // FNV-1a keeps catalogue IDs stable across runs and standard libraries.
  auto text = xe::path_to_utf8(path.lexically_normal());
  uint64_t id = 14695981039346656037ull;
  for (unsigned char c : text) {
    c = c == '\\' ? '/' : static_cast<unsigned char>(std::tolower(c));
    id = (id ^ c) * 1099511628211ull;
  }
  return id ? id : 1;
}
}  // namespace

DsuIntegration::DsuIntegration(EmulatorWindow& window)
    : window_(window), streamer_(std::make_shared<GameStreamer>()) {
  auto streamer = streamer_;
  const auto callbacks = [streamer](uint32_t receiver) {
    return DsuCompanion::StreamCallbacks{
        [streamer, receiver](const StreamRequest& request) {
          return streamer->Start(request, receiver);
        },
        [streamer, receiver]() { streamer->Stop(receiver); },
        [streamer, receiver]() { return streamer->GetStatus(receiver); }};
  };
  for (uint32_t i = 0; i < players_.size(); ++i) {
    auto& player = players_[i];
    player.driver = window.emulator()->input_system()->dsu_driver(i);
    player.settings = hid::dsu::DSUInputDriver::ReadConfiguration(i);
    std::strncpy(player.host, player.settings.host.c_str(),
                 sizeof(player.host) - 1);
    const auto port = std::to_string(player.settings.port);
    std::strncpy(player.port, port.c_str(), sizeof(player.port) - 1);
    player.companion = std::make_shared<DsuCompanion>(
        [driver = player.driver](uint32_t type,
                                 const std::vector<uint8_t>& payload) {
          if (driver) {
            driver->SendExtensionPacket(
                hid::dsu::DSUInputDriver::BuildExtensionPacket(type, payload));
          }
        },
        callbacks(i));
    if (player.driver) {
      player.driver->SetExtensionCallback(
          [weak = std::weak_ptr(player.companion)](
              uint32_t type, const std::vector<uint8_t>& payload) {
            if (auto companion = weak.lock()) {
              companion->HandleExtension(type, payload);
            }
          });
    }
  }
  manual_stream_ = std::make_shared<DsuCompanion>(
      [](uint32_t, const std::vector<uint8_t>&) {}, callbacks(4));
  selected_player_ = std::clamp(int(cvars::dsu_local_player), 0, 3);
  edit_streaming_ = cvars::dsu_streaming;
  std::strncpy(stream_url_buffer_, cvars::streaming_url.c_str(),
               sizeof(stream_url_buffer_) - 1);
  ConfigureCompanions();
  RefreshCatalogue();
  apu::AudioDriver::SetCaptureCallback(
      [streamer](const apu::AudioDriver* source, std::span<const float> samples,
                 uint32_t channels, uint32_t rate) {
        streamer->SubmitAudio(source, samples, channels, rate);
      });
  if (auto* presenter = window_.GetGraphicsSystemPresenter()) {
    capture_thread_ =
        std::thread(&DsuIntegration::CaptureWorker, this, presenter);
  }
}

DsuIntegration::~DsuIntegration() {
  apu::AudioDriver::SetCaptureCallback({});
  stopping_ = true;
  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }
  for (auto& player : players_) {
    if (player.driver) {
      player.driver->SetExtensionCallback({});
      player.driver->Stop();
    }
  }
  for (auto& player : players_) {
    player.companion->Stop();
  }
  manual_stream_->Stop();
  streamer_->Stop();
}

void DsuIntegration::ConfigureCompanions() {
  for (uint32_t i = 0; i < players_.size(); ++i) {
    const auto settings = hid::dsu::DSUInputDriver::ReadConfiguration(i);
    std::vector<std::weak_ptr<DsuCompanion>> peers;
    bool owner = true;
    for (uint32_t j = 0; j < players_.size(); ++j) {
      const auto other = hid::dsu::DSUInputDriver::ReadConfiguration(j);
      if (other.enabled && other.companion_extensions &&
          other.port == settings.port) {
        peers.push_back(players_[j].companion);
        if (j < i) {
          owner = false;
        }
      }
    }
    players_[i].companion->SetHttpPeers(std::move(peers));
    players_[i].companion->Configure(
        {settings.enabled && settings.companion_extensions,
         cvars::dsu_streaming,
         settings.host,
         uint16_t(settings.port),
         {},
         owner});
  }
  manual_stream_->Configure(
      {false, cvars::dsu_streaming, {}, 26760, cvars::streaming_url, false});
}

void DsuIntegration::CaptureWorker(ui::Presenter* presenter) {
  auto last_frame = std::chrono::steady_clock::now();
  uint64_t last_generation = 0;
  bool logged_attempt = false, logged_success = false;
  while (!stopping_) {
    const auto begin = std::chrono::steady_clock::now();
    const auto status = streamer_->GetStatus();
    if (auto error = streamer_->TakeError()) {
      XELOGE("DSU stream: {}", *error);
    }
    const auto generation = capture_generation_.load();
    if (generation != last_generation) {
      last_generation = generation;
      last_frame = begin;
      logged_attempt = logged_success = false;
    }
    const bool ready = video_ready_generation_.load() == generation;
    const bool capture_requested = capture_requested_.load();
    const bool fresh_guest_image =
        presenter->GetGuestOutputRefreshCount() > launch_refresh_baseline_;
    if (capture_requested && fresh_guest_image &&
        (status.starting || status.running || !ready)) {
      try {
        ui::RawImage image;
        const bool first_attempt = !logged_attempt;
        if (first_attempt) {
          logged_attempt = true;
          XELOGI(
              "DSU capture: first attempt generation={} refresh={} "
              "baseline={}",
              generation, presenter->GetGuestOutputRefreshCount(),
              launch_refresh_baseline_.load());
        }
        capture_in_progress_ = true;
        const bool captured = presenter->CaptureGuestOutput(image);
        capture_in_progress_ = false;
        if (first_attempt && (!captured || !image.width || !image.height)) {
          XELOGW(
              "DSU capture: first attempt returned no image; "
              "generation={}",
              generation);
        }
        if (captured && image.width && image.height) {
          // Reject frames captured before disconnect, launch or
          // reconfiguration.
          std::lock_guard publish_lock(capture_publish_mutex_);
          if (generation == capture_generation_ && capture_requested_) {
            video_ready_generation_ = generation;
            if (!logged_success) {
              logged_success = true;
              XELOGI(
                  "DSU capture: first image generation={} {}x{} "
                  "stride={}",
                  generation, image.width, image.height, image.stride);
            }
            if (status.starting || status.running) {
              streamer_->SubmitVideo(std::move(image.data), image.width,
                                     image.height, image.stride);
            }
          }
          last_frame = begin;
        } else if (status.running &&
                   begin - last_frame > std::chrono::seconds(3)) {
          std::lock_guard publish_lock(capture_publish_mutex_);
          if (generation == capture_generation_ && capture_requested_) {
            streamer_->ReportCaptureFailure(
                "Game video capture stopped producing frames.");
            video_ready_generation_ = 0;
          }
        }
      } catch (const std::exception& error) {
        capture_in_progress_ = false;
        XELOGE("DSU video capture: {}", error.what());
        std::lock_guard publish_lock(capture_publish_mutex_);
        if (generation == capture_generation_ && capture_requested_) {
          streamer_->ReportCaptureFailure("Game video capture failed.");
          video_ready_generation_ = 0;
        }
      } catch (...) {
        capture_in_progress_ = false;
        std::lock_guard publish_lock(capture_publish_mutex_);
        if (generation == capture_generation_ && capture_requested_) {
          streamer_->ReportCaptureFailure("Game video capture failed.");
          video_ready_generation_ = 0;
        }
      }
    }
    if (!status.running) {
      last_frame = begin;
    }
    const uint32_t fps = std::clamp(status.fps, 1u, 60u);
    const auto period = std::chrono::microseconds(
        status.starting || status.running ? (1000000 + fps - 1) / fps : 50000);
    std::this_thread::sleep_until(begin + period);
  }
}

void DsuIntegration::RefreshCatalogue() {
  auto& scraper = window_.screen_scraper_service();
  const auto revision = scraper.GetRevision();
  if (revision != scraped_revision_) {
    scraped_.clear();
    scraped_revision_ = revision;
  }
  // Limit disk probes per tick and reuse results until the media revision
  // changes.
  size_t cache_budget = 8;
  std::vector<DsuCatalogueEntry> entries;
  for (const auto& [path, name] : UWP::GetGames()) {
    DsuCatalogueEntry entry;
    entry.path = std::filesystem::u8path(path);
    entry.id = CatalogueId(entry.path);
    entry.name = name;
    auto [cached, inserted] = title_ids_.try_emplace(path);
    if (inserted) {
      cached->second = UWP::GetTitleIdFromPath(path);
    }
    UWP::UnityGameMetadata metadata;
    if (UWP::TryGetUnityMetadata(cached->second, &metadata)) {
      if (!metadata.display_name.empty()) {
        entry.name = metadata.display_name;
      }
      entry.metadata = {{"desc", metadata.description_full},
                        {"dev", metadata.developer},
                        {"pub", metadata.publisher},
                        {"released", metadata.release_date}};
      if (const auto rating =
              NormalizeXboxMarketplaceRating(metadata.user_rating)) {
        entry.metadata["rating"] = std::to_string(*rating);
      }
      if (!metadata.genre.empty()) {
        entry.metadata["genre"] = metadata.genre.front();
      }
      auto media = [&](uint8_t type, const std::string& filename) {
        if (!filename.empty()) {
          entry.media[type] = std::filesystem::u8path(filename);
        }
      };
      media(0, metadata.icon_path);
      media(1, metadata.cover_path);
      media(7, metadata.background_path);
      if (!metadata.gallery_paths.empty()) {
        media(6, metadata.gallery_paths.front());
      }
    }
    const auto target = MakeScreenScraperTarget(entry.path, name);
    entry.file = xe::path_to_utf8(ScreenScraperRelativeGameName(target));
    const auto cache_key = path + "\n" + xe::path_to_utf8(target.content_root) +
                           "\n" + xe::path_to_utf8(target.relative_game_path);
    auto cached_art = scraped_.find(cache_key);
    if (cached_art == scraped_.end() && cache_budget) {
      --cache_budget;
      cached_art =
          scraped_.emplace(cache_key, scraper.LoadCached(target)).first;
    }
    if (cached_art != scraped_.end() && cached_art->second) {
      const auto& scraped = *cached_art->second;
      const auto& meta = scraped.metadata;
      if (!meta.title.empty()) {
        entry.name = meta.title;
      }
      const auto field = [&](const char* key, const std::string& value) {
        if (scraped.local_override || !value.empty()) {
          entry.metadata[key] = value;
        }
      };
      field("desc", meta.description);
      field("dev", meta.developer);
      field("pub", meta.publisher);
      field("genre", meta.genre);
      field("released", meta.release_date);
      field("players", meta.players);
      field("region", meta.region);
      if (scraped.local_override || meta.rating > 0) {
        entry.metadata["rating"] = std::to_string(meta.rating);
      }
      using Media = screenscraper::MediaType;
      static constexpr Media media_types[] = {
          Media::Marquee,    Media::Cover,     Media::Box3D,
          Media::MixImage,   Media::BackCover, Media::PhysicalMedia,
          Media::Screenshot, Media::FanArt,    Media::TitleScreen,
          Media::Video};
      for (uint8_t type = 0; type < 10; ++type) {
        auto file = scraped.media.find(media_types[type]);
        if (file != scraped.media.end()) {
          entry.media[type] = file->second;
        }
      }
      entry.media_revision = scraped.revision;
    }
    entries.push_back(std::move(entry));
  }
  catalogue_.clear();
  for (const auto& entry : entries) {
    catalogue_.emplace(entry.id, entry);
  }
  for (auto& player : players_) {
    player.companion->PublishCatalogue(entries);
  }
}

void DsuIntegration::Tick() {
  const auto now = std::chrono::steady_clock::now();
  // Resolve commands first; defer later catalogue maintenance until afterward.
  if (catalogue_.empty() && now >= catalogue_refresh_) {
    RefreshCatalogue();
    catalogue_refresh_ = now + std::chrono::seconds(2);
  }
  const bool running = window_.emulator()->is_title_open();
  bool connected = false;
  for (auto& player : players_) {
    const auto state = player.driver ? player.driver->GetConnectionStatus()
                                     : hid::dsu::ConnectionStatus{};
    player.companion->UpdateConnection(state.connected);
    const auto settings =
        hid::dsu::DSUInputDriver::ReadConfiguration(state.local_player);
    connected |= state.connected && settings.companion_extensions;
  }
  const bool capture = running && cvars::dsu_streaming &&
                       (!cvars::streaming_url.empty() || connected);
  {
    std::lock_guard publish_lock(capture_publish_mutex_);
    if (capture_requested_.exchange(capture) != capture) {
      ++capture_generation_;
    }
  }
  if (running != was_running_) {
    for (auto& player : players_) {
      player.companion->UpdateRunning(running_id_, running_name_, running);
    }
    manual_stream_->UpdateRunning(running_id_, running_name_, running);
    was_running_ = running;
  }
  const auto generation = capture_generation_.load();
  const bool video_ready =
      capture && video_ready_generation_.load() == generation;
  for (auto& player : players_) {
    player.companion->UpdateVideoReady(video_ready);
  }
  manual_stream_->UpdateVideoReady(video_ready);
  const uint32_t gates = uint32_t(running) | (uint32_t(connected) << 1) |
                         (uint32_t(cvars::dsu_streaming) << 2) |
                         (uint32_t(capture) << 3) |
                         (uint32_t(video_ready) << 4);
  if (generation != logged_capture_generation_) {
    logged_capture_generation_ = generation;
    capture_wait_started_ = now;
    capture_wait_warned_ = false;
  }
  if (gates != logged_capture_gates_) {
    logged_capture_gates_ = gates;
    XELOGI(
        "DSU capture gates: generation={} running={} input_connected={} "
        "streaming={} requested={} ready={} worker={}",
        generation, running, connected, bool(cvars::dsu_streaming), capture,
        video_ready, capture_thread_.joinable());
  }
  if (capture && !video_ready && !capture_wait_warned_ &&
      now - capture_wait_started_ >= std::chrono::seconds(3)) {
    capture_wait_warned_ = true;
    auto* presenter = window_.GetGraphicsSystemPresenter();
    XELOGW(
        "DSU capture waiting after 3 seconds: generation={} worker={} "
        "presenter={} refresh={} baseline={} readback_in_progress={}",
        generation, capture_thread_.joinable(), presenter != nullptr,
        presenter ? presenter->GetGuestOutputRefreshCount() : 0,
        launch_refresh_baseline_.load(), capture_in_progress_.load());
  }
  for (auto& player : players_) {
    if (auto id = player.companion->TakePendingSelection()) {
      auto entry = catalogue_.find(*id);
      if (!window_.emulator()->is_title_open() && entry != catalogue_.end()) {
        window_.SelectRemoteGame(entry->second.path, entry->second.name);
      }
    }
  }
  const auto selected = window_.SelectedFrontendGame();
  if (!selected.empty()) {
    for (auto& player : players_) {
      player.companion->UpdateSelection(CatalogueId(selected));
    }
  }
  for (auto& player : players_) {
    if (auto id = player.companion->TakePendingLaunch()) {
      const auto entry = catalogue_.find(*id);
      const bool accepted =
          !window_.emulator()->is_title_open() && entry != catalogue_.end();
      player.companion->CompleteLaunch(
          accepted && window_.RunTitle(entry->second.path) == X_STATUS_SUCCESS);
    }
  }
  if (now >= catalogue_refresh_) {
    RefreshCatalogue();
    catalogue_refresh_ = now + std::chrono::seconds(2);
  }
}

void DsuIntegration::OnLaunching() {
  {
    std::lock_guard publish_lock(capture_publish_mutex_);
    capture_requested_ = false;
    ++capture_generation_;
    if (auto* presenter = window_.GetGraphicsSystemPresenter()) {
      launch_refresh_baseline_ = presenter->GetGuestOutputRefreshCount();
    }
  }
  for (auto& player : players_) {
    player.companion->UpdateVideoReady(false);
    player.companion->UpdateRunning(0, {}, false);
  }
  manual_stream_->UpdateVideoReady(false);
  manual_stream_->UpdateRunning(0, {}, false);
  was_running_ = false;
  XELOGI("DSU launch beginning: generation={} refresh_baseline={}",
         capture_generation_.load(), launch_refresh_baseline_.load());
}

void DsuIntegration::OnLaunch(const std::filesystem::path& path,
                              bool succeeded) {
  XELOGI("DSU launch completed: succeeded={} generation={}", succeeded,
         capture_generation_.load());
  if (!succeeded) {
    return;
  }
  {
    std::lock_guard publish_lock(capture_publish_mutex_);
    ++capture_generation_;
  }
  for (auto& player : players_) {
    player.companion->UpdateVideoReady(false);
  }
  manual_stream_->UpdateVideoReady(false);
  running_id_ = CatalogueId(path);
  auto it = catalogue_.find(running_id_);
  running_name_ = it == catalogue_.end() ? window_.emulator()->title_name()
                                         : it->second.name;
  for (auto& player : players_) {
    player.companion->UpdateRunning(running_id_, running_name_, true);
  }
  manual_stream_->UpdateRunning(running_id_, running_name_, true);
  was_running_ = true;
}

void DsuIntegration::ApplyConfiguration() {
  {
    std::lock_guard publish_lock(capture_publish_mutex_);
    capture_requested_ = false;
    ++capture_generation_;
  }
  std::array<hid::dsu::Configuration, 4> settings;
  std::array<bool, 4> changed;
  for (uint32_t i = 0; i < players_.size(); ++i) {
    auto& player = players_[i];
    player.settings.host = player.host;
    const char* end = player.port + std::strlen(player.port);
    std::from_chars(player.port, end, player.settings.port);
    settings[i] = player.settings;
    changed[i] = settings[i] != hid::dsu::DSUInputDriver::ReadConfiguration(i);
  }
  hid::dsu::DSUInputDriver::SaveConfigurations(settings);
  OVERRIDE_bool(dsu_streaming, edit_streaming_);
  OVERRIDE_string(streaming_url, std::string(stream_url_buffer_));
  config::SaveConfig();
  for (uint32_t i = 0; i < players_.size(); ++i) {
    auto& player = players_[i];
    if (player.driver && changed[i]) {
      player.driver->Reconfigure(player.settings);
    }
  }
  ConfigureCompanions();
}

void DsuIntegration::DrawSettings() {
  int player_number = selected_player_ + 1;
  if (ImGui::SliderInt("Xbox Player", &player_number, 1, 4)) {
    selected_player_ = player_number - 1;
  }
  auto& player = players_[selected_player_];
  auto& settings = player.settings;
  ImGui::Checkbox("Allow motion for right stick", &settings.motion_right_stick);
  ImGui::BeginDisabled(!settings.motion_right_stick);
  ImGui::SliderFloat("Horizontal Motion Sensitivity",
                     &settings.motion_sensitivity_x, 0.1f, 5.0f, "%.1fx");
  ImGui::SliderFloat("Vertical Motion Sensitivity",
                     &settings.motion_sensitivity_y, 0.1f, 5.0f, "%.1fx");
  ImGui::SliderFloat("Motion Deadzone", &settings.motion_deadzone, 0.0f, 10.0f,
                     "%.1f deg/s");
  ImGui::Checkbox("Invert Horizontal Motion", &settings.motion_invert_x);
  ImGui::Checkbox("Invert Vertical Motion", &settings.motion_invert_y);
  ImGui::EndDisabled();
  ImGui::Checkbox("DSU Input", &settings.enabled);
  ImGui::BeginDisabled(!settings.enabled);
  if (ImGui::Button("Edit DSU Server IP Address")) {
    UWP::ShowKeyboard();
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::InputText("DSU Server IP Address", player.host, sizeof(player.host));
  if (ImGui::Button("Edit DSU Server Port")) {
    UWP::ShowKeyboard();
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::InputText("DSU Server Port", player.port, sizeof(player.port),
                   ImGuiInputTextFlags_CharsDecimal);
  int remote = int(settings.remote_slot);
  if (ImGui::SliderInt("DSU Controller Slot", &remote, 0,
                       settings.companion_extensions ? 15 : 3)) {
    settings.remote_slot = remote;
  }
  ImGui::Checkbox("Companion Protocol Extensions",
                  &settings.companion_extensions);
  ImGui::EndDisabled();
  ImGui::Spacing();
  ImGui::Checkbox("Automatic Game Streaming", &edit_streaming_);
  if (ImGui::Button("Edit Stream Destination URL")) {
    UWP::ShowKeyboard();
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::InputTextWithHint("Stream Destination URL",
                           "udp://192.168.1.100:26960", stream_url_buffer_,
                           sizeof(stream_url_buffer_));
  bool valid = !stream_url_buffer_[0] ||
               ParseStreamDestination(stream_url_buffer_).has_value();
  if (!valid) {
    ImGui::TextWrapped("Enter udp://IPv4:port, with a port from 1 to 65535.");
  }
  for (uint32_t i = 0; i < players_.size(); ++i) {
    const auto& current = players_[i];
    if (!current.settings.enabled) {
      continue;
    }
    uint32_t port = 0;
    const char* end = current.port + std::strlen(current.port);
    const auto parsed = std::from_chars(current.port, end, port);
    const uint32_t max_port = current.settings.companion_extensions
                                  ? (edit_streaming_ ? 65335 : 65534)
                                  : 65535;
    const auto address =
        ParseStreamDestination("udp://" + std::string(current.host) + ":1");
    if (!address || parsed.ec != std::errc{} || parsed.ptr != end || !port ||
        port > max_port ||
        current.settings.remote_slot >=
            (current.settings.companion_extensions ? 16u : 4u)) {
      ImGui::TextWrapped(
          "Check the server address, port and controller slot for player %u.",
          i + 1);
      valid = false;
    }
    for (uint32_t j = 0; j < i; ++j) {
      const auto& other = players_[j];
      if (other.settings.enabled &&
          std::strcmp(current.host, other.host) == 0 &&
          std::strcmp(current.port, other.port) == 0 &&
          (current.settings.companion_extensions ||
           other.settings.companion_extensions ||
           current.settings.remote_slot == other.settings.remote_slot)) {
        ImGui::TextWrapped(
            "Players %u and %u need separate controller connections.", j + 1,
            i + 1);
        valid = false;
      }
    }
  }
  ImGui::BeginDisabled(!valid);
  if (ImGui::Button("Apply Settings")) {
    ApplyConfiguration();
  }
  ImGui::EndDisabled();
  const auto state = player.driver ? player.driver->GetConnectionStatus()
                                   : hid::dsu::ConnectionStatus{};
  ImGui::TextWrapped("DSU Input: %s", state.connected ? "Connected"
                                      : state.enabled ? "Waiting for DSU server"
                                                      : "Disabled");
  if (!state.error.empty()) {
    ImGui::TextWrapped("%s", state.error.c_str());
  }
  ImGui::TextWrapped("Companion Catalogue: %s",
                     player.companion->GetStatus().c_str());
  const auto stream = streamer_->GetStatus(selected_player_);
  ImGui::TextWrapped("Stream: %s", stream.running    ? "Running"
                                   : stream.starting ? "Starting"
                                                     : "Stopped");
  if (!stream.url.empty()) {
    ImGui::TextWrapped("Stream Destination: %s", stream.url.c_str());
  }
  if (!stream.error.empty()) {
    ImGui::TextWrapped("%s", stream.error.c_str());
  }
}
}  // namespace xe::app
#endif  // XE_PLATFORM_WINRT
