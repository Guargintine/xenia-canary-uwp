// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_DSU_COMPANION_H_
#define XENIA_APP_DSU_COMPANION_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "xenia/app/game_streamer.h"

namespace xe::app {

struct DsuCompanionConfig {
  bool enabled = false;
  bool streaming_enabled = false;
  std::string host;
  uint16_t port = 26760;
  std::string stream_url;
  bool serve_catalogue = true;
};

struct DsuCatalogueEntry {
  uint64_t id = 0;
  std::string name;
  std::string platform = "Microsoft - Xbox 360";
  // Relative media identity; absolute host paths must not be published.
  std::string file;
  std::filesystem::path path;
  // Companion metadata fields: desc, dev, pub, genre and released.
  std::map<std::string, std::string> metadata;
  // Only published files for protocol media types 0..9 may be served.
  std::map<uint8_t, std::filesystem::path> media;
  // Replacing a file at the same path must invalidate its media URL.
  uint64_t media_revision = 0;
  bool operator==(const DsuCatalogueEntry&) const = default;
};

class DsuCompanion {
 public:
  using SendPacket = std::function<void(uint32_t, const std::vector<uint8_t>&)>;
  struct StreamCallbacks {
    std::function<bool(const StreamRequest&)> start;
    std::function<void()> stop;
    std::function<StreamStatus()> status;
  };
  explicit DsuCompanion(SendPacket send, StreamCallbacks streaming = {});
  ~DsuCompanion();
  void Stop();
  DsuCompanion(const DsuCompanion&) = delete;
  DsuCompanion& operator=(const DsuCompanion&) = delete;

  void Configure(DsuCompanionConfig config);
  void SetHttpPeers(std::vector<std::weak_ptr<DsuCompanion>> peers);
  bool HandleHttp(uintptr_t socket, uint32_t peer_address);
  void PublishCatalogue(std::vector<DsuCatalogueEntry> entries);
  void UpdateRunning(uint64_t id, std::string name, bool running);
  // Each connection/game/configuration gets one attempt after a frame is ready.
  void UpdateConnection(bool connected);
  void UpdateVideoReady(bool ready);
  void UpdateSelection(uint64_t id);
  std::optional<uint64_t> TakePendingLaunch();
  std::optional<uint64_t> TakePendingSelection();
  void CompleteLaunch(bool succeeded);
  // The DSU transport validates peer, framing and CRC before dispatch.
  void HandleExtension(uint32_t type, std::span<const uint8_t> payload);
  // The HTTP worker also ticks while the UI is hidden.
  void Tick();
  std::string GetStatus() const;
  std::string CatalogueJson() const;

 private:
  struct StreamCommand {
    bool stop = false;
    StreamRequest request;
  };
  void Worker();
  void SendLaunchStatus(uint32_t status, uint32_t revision, uint64_t token);
  void SendStreamStatus(uint32_t state, uint32_t error, uint8_t screen,
                        const StreamStatus* status = nullptr);
  void ServeHttp(uintptr_t socket, uint32_t peer_address,
                 uint32_t allowed_address, uint64_t generation);
  void Send(uint32_t type, std::vector<uint8_t> payload) const;

  SendPacket send_;
  StreamCallbacks streaming_;
  mutable std::mutex mutex_;
  std::mutex tick_mutex_;
  DsuCompanionConfig config_;
  std::vector<std::weak_ptr<DsuCompanion>> http_peers_;
  uint64_t config_generation_ = 0;
  uint32_t phone_address_ = 0;
  uint32_t revision_ = 1;
  std::vector<DsuCatalogueEntry> entries_;
  std::optional<uint64_t> selected_;
  std::optional<uint64_t> pending_selection_;
  bool selection_ack_pending_ = false;
  std::optional<uint64_t> pending_launch_;
  std::optional<uint64_t> launch_in_progress_;
  uint32_t launch_revision_ = 0;
  uint64_t running_id_ = 0;
  std::string running_name_;
  bool running_ = false;
  bool connected_ = false;
  bool video_ready_ = false;
  bool automatic_stream_attempted_ = false;
  std::optional<StreamCommand> stream_command_;
  bool stream_requested_ = false;
  // Repeat idle status after UDP loss; preserve errors until the next command.
  uint32_t idle_stream_error_ = 0;
  uint32_t logged_stream_gates_ = UINT32_MAX;
  std::string last_stream_state_;
  std::chrono::steady_clock::time_point last_state_{};
  std::string status_ = "Disabled";
  std::atomic<bool> stopping_{false};
  std::thread worker_;
};

}  // namespace xe::app
#endif  // XENIA_APP_DSU_COMPANION_H_
