// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_DSU_INTEGRATION_H_
#define XENIA_APP_DSU_INTEGRATION_H_

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "xenia/app/dsu_companion.h"
#include "xenia/app/screenscraper_service.h"
#include "xenia/hid/dsu/dsu_input_driver.h"

namespace xe::ui {
class Presenter;
}
namespace xe::app {
class EmulatorWindow;

// UI-thread owner; network callbacks exchange snapshots and commands only.
// Join capture before destroying the presenter.
class DsuIntegration {
 public:
  explicit DsuIntegration(EmulatorWindow& window);
  ~DsuIntegration();
  void Tick();
  void DrawSettings();
  void OnLaunching();
  void OnLaunch(const std::filesystem::path& path, bool succeeded);

 private:
  void ApplyConfiguration();
  void ConfigureCompanions();
  void RefreshCatalogue();
  void CaptureWorker(ui::Presenter* presenter);
  EmulatorWindow& window_;
  struct Player {
    hid::dsu::DSUInputDriver* driver = nullptr;
    std::shared_ptr<DsuCompanion> companion;
    hid::dsu::Configuration settings;
    char host[256]{};
    char port[6]{};
  };
  std::array<Player, 4> players_;
  int selected_player_ = 0;
  std::shared_ptr<GameStreamer> streamer_;
  std::shared_ptr<DsuCompanion> manual_stream_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> capture_requested_{false};
  std::atomic<bool> capture_in_progress_{false};
  std::atomic<uint64_t> video_ready_generation_{0};
  std::atomic<uint64_t> capture_generation_{1};
  // Protects invalidation/publication, never GPU waits or encoder shutdown.
  std::mutex capture_publish_mutex_;
  std::atomic<uint64_t> launch_refresh_baseline_{0};
  std::thread capture_thread_;
  std::map<uint64_t, DsuCatalogueEntry> catalogue_;
  std::map<std::string, std::string> title_ids_;
  std::map<std::string, std::optional<screenscraper::CachedGame>> scraped_;
  uint64_t scraped_revision_ = 0;
  std::chrono::steady_clock::time_point catalogue_refresh_{};
  uint64_t running_id_ = 0;
  std::string running_name_;
  bool was_running_ = false;
  uint32_t logged_capture_gates_ = UINT32_MAX;
  uint64_t logged_capture_generation_ = 0;
  std::chrono::steady_clock::time_point capture_wait_started_{};
  bool capture_wait_warned_ = false;
  bool edit_streaming_ = false;
  char stream_url_buffer_[256]{};
};
}  // namespace xe::app
#endif
