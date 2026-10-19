// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_GAME_STREAMER_H_
#define XENIA_APP_GAME_STREAMER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xe::app {

struct StreamRequest {
  std::string url;
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t bitrate_kbps = 4000;
  uint32_t fps = 30;
  bool audio = true;
};

struct StreamStatus {
  bool starting = false;
  bool running = false;
  std::string url;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps = 0;
  bool audio = false;
  bool hardware_encoding = false;
  std::string error;
  uint64_t video_frames = 0;
};

// MPEG-TS/UDP encoder with bounded, nonblocking submissions. Frames are
// accepted while starting or running; only the worker accesses FFmpeg.
// Start/Stop may join the worker and must not run from an audio callback.
class GameStreamer {
 public:
  GameStreamer();
  ~GameStreamer();
  GameStreamer(const GameStreamer&) = delete;
  GameStreamer& operator=(const GameStreamer&) = delete;

  bool Start(const StreamRequest& request, uint32_t receiver = 0);
  void Stop();
  void Stop(uint32_t receiver);
  StreamStatus GetStatus() const;
  StreamStatus GetStatus(uint32_t receiver) const;
  std::optional<std::string> TakeError();
  // Wakes the active encoder without joining; ignored after Stop.
  void ReportCaptureFailure(std::string error);
  void SubmitVideo(std::span<const uint8_t> rgbx, uint32_t width,
                   uint32_t height, size_t stride);
  // Transfers an already-paced frame directly into the latest-frame slot.
  void SubmitVideo(std::vector<uint8_t>&& rgbx, uint32_t width, uint32_t height,
                   size_t stride);
  // Channel order is standard WAVE order: FL FR FC LFE BL BR for 5.1.
  void SubmitAudio(std::span<const float> interleaved, uint32_t channels,
                   uint32_t sample_rate);
  // Mixes independently clocked voices. An empty span removes source_id;
  // the pointer is an identity only and is never dereferenced.
  void SubmitAudio(const void* source_id, std::span<const float> interleaved,
                   uint32_t channels, uint32_t sample_rate);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xe::app
#endif  // XENIA_APP_GAME_STREAMER_H_
