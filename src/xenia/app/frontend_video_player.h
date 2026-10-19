// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_FRONTEND_VIDEO_PLAYER_H_
#define XENIA_APP_FRONTEND_VIDEO_PLAYER_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xe::app {
// Muted, looping playback of locally cached frontend media. All file access and
// decoding run on the worker; selection changes never wait for the decoder.
class FrontendVideoPlayer {
 public:
  struct Frame {
    uint32_t width = 0, height = 0;
    float aspect_ratio = 0;
    std::vector<uint8_t> rgba;
  };
  FrontendVideoPlayer();
  ~FrontendVideoPlayer();
  FrontendVideoPlayer(const FrontendVideoPlayer&) = delete;
  FrontendVideoPlayer& operator=(const FrontendVideoPlayer&) = delete;
  void SetPath(const std::filesystem::path& path, uint64_t revision = 0);
  void Stop();
  std::optional<Frame> TakeFrame();
  std::string GetError() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace xe::app
#endif
