// Copyright 2026 Xenia Canary. Released under the BSD license.
#include "frontend_video_player.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}
static_assert(LIBAVCODEC_VERSION_MAJOR == 60);
static_assert(LIBAVFORMAT_VERSION_MAJOR == 60);
static_assert(LIBAVUTIL_VERSION_MAJOR == 58);
static_assert(LIBSWSCALE_VERSION_MAJOR == 7);
#endif

namespace xe::app {
namespace {
using Clock = std::chrono::steady_clock;
#if defined(_WIN32)
HMODULE LoadVideoLibrary(const wchar_t* name) {
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
  return LoadPackagedLibrary(name, 0);
#else
  std::wstring path(32768, L'\0');
  const DWORD size =
      GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  if (!size || size >= path.size()) {
    return nullptr;
  }
  path.resize(size);
  const auto slash = path.find_last_of(L"/\\");
  if (slash == std::wstring::npos) {
    return nullptr;
  }
  path.resize(slash + 1);
  path += name;
  return LoadLibraryExW(
      path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#endif
}
struct VideoApi {
#define PROC(name) decltype(&name) name##_p = nullptr
  PROC(avformat_alloc_context);
  PROC(avformat_open_input);
  PROC(avformat_find_stream_info);
  PROC(avformat_close_input);
  PROC(av_find_best_stream);
  PROC(av_guess_frame_rate);
  PROC(av_read_frame);
  PROC(av_seek_frame);
  PROC(avcodec_alloc_context3);
  PROC(avcodec_parameters_to_context);
  PROC(avcodec_open2);
  PROC(avcodec_free_context);
  PROC(avcodec_send_packet);
  PROC(avcodec_receive_frame);
  PROC(avcodec_flush_buffers);
  PROC(av_packet_alloc);
  PROC(av_packet_free);
  PROC(av_packet_unref);
  PROC(av_frame_alloc);
  PROC(av_frame_free);
  PROC(av_frame_unref);
  PROC(av_dict_set);
  PROC(av_dict_free);
  PROC(sws_getCachedContext);
  PROC(sws_scale);
  PROC(sws_freeContext);
#undef PROC
  std::string error;
  VideoApi() {
    // References stay loaded for the process lifetime, shared with streaming.
    const HMODULE util = LoadVideoLibrary(L"avutil-58.dll");
    LoadVideoLibrary(L"swresample-4.dll");
    const HMODULE scale = LoadVideoLibrary(L"swscale-7.dll");
    const HMODULE codec = LoadVideoLibrary(L"avcodec-60.dll");
    const HMODULE format = LoadVideoLibrary(L"avformat-60.dll");
    if (!util || !scale || !codec || !format) {
      error = "The packaged video decoder could not be loaded.";
      return;
    }
#define LOAD(module, name)                                                 \
  name##_p =                                                               \
      reinterpret_cast<decltype(name##_p)>(GetProcAddress(module, #name)); \
  if (!name##_p) {                                                         \
    error = "The packaged video decoder is missing " #name ".";            \
    return;                                                                \
  }
    LOAD(format, avformat_alloc_context);
    LOAD(format, avformat_open_input);
    LOAD(format, avformat_find_stream_info);
    LOAD(format, avformat_close_input);
    LOAD(format, av_find_best_stream);
    LOAD(format, av_guess_frame_rate);
    LOAD(format, av_read_frame);
    LOAD(format, av_seek_frame);
    LOAD(codec, avcodec_alloc_context3);
    LOAD(codec, avcodec_parameters_to_context);
    LOAD(codec, avcodec_open2);
    LOAD(codec, avcodec_free_context);
    LOAD(codec, avcodec_send_packet);
    LOAD(codec, avcodec_receive_frame);
    LOAD(codec, avcodec_flush_buffers);
    LOAD(codec, av_packet_alloc);
    LOAD(codec, av_packet_free);
    LOAD(codec, av_packet_unref);
    LOAD(util, av_frame_alloc);
    LOAD(util, av_frame_free);
    LOAD(util, av_frame_unref);
    LOAD(util, av_dict_set);
    LOAD(util, av_dict_free);
    LOAD(scale, sws_getCachedContext);
    LOAD(scale, sws_scale);
    LOAD(scale, sws_freeContext);
#undef LOAD
  }
  static VideoApi& Get() {
    static VideoApi api;
    return api;
  }
};
#endif
}  // namespace

struct FrontendVideoPlayer::Impl {
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> stopping{false};
  std::atomic<uint64_t> generation{1};
  std::filesystem::path requested_path;
  uint64_t requested_revision = 0;
  std::optional<Frame> frame;
  std::string error;
  std::thread worker;

  Impl() : worker([this] { Work(); }) {}
  ~Impl() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    wake.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
  }
  bool Cancelled(uint64_t token) const {
    return stopping || generation.load() != token;
  }
  void Fail(uint64_t token, std::string message) {
    std::lock_guard lock(mutex);
    if (!Cancelled(token)) {
      frame.reset();
      error = std::move(message);
    }
  }
  bool WaitUntil(uint64_t token, Clock::time_point time) {
    std::unique_lock lock(mutex);
    wake.wait_until(lock, time, [&] { return Cancelled(token); });
    return !Cancelled(token);
  }
  void Work() {
    uint64_t previous = 0;
    while (!stopping) {
      std::filesystem::path path;
      uint64_t token;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return stopping || generation != previous; });
        if (stopping) {
          return;
        }
        previous = token = generation;
        path = requested_path;
      }
      if (path.empty()) {
        continue;
      }
      try {
        Decode(path, token);
      } catch (...) {
        Fail(token, "The selected video background could not be decoded.");
      }
    }
  }
  void Decode(const std::filesystem::path& path, uint64_t token) {
#if defined(_WIN32)
    std::error_code ec;
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (!std::filesystem::is_regular_file(path, ec) || ec ||
        std::filesystem::file_size(path, ec) > 256ull * 1024 * 1024 || ec ||
        (extension != ".mp4" && extension != ".m4v" && extension != ".webm" &&
         extension != ".mkv" && extension != ".avi" && extension != ".mov")) {
      Fail(token, "Choose a supported, locally cached video background.");
      return;
    }
    auto& api = VideoApi::Get();
    if (!api.error.empty()) {
      Fail(token, api.error);
      return;
    }
    struct Session {
      VideoApi& api;
      AVFormatContext* format = nullptr;
      AVCodecContext* codec = nullptr;
      AVPacket* packet = nullptr;
      AVFrame* decoded = nullptr;
      SwsContext* scale = nullptr;
      AVDictionary* options = nullptr;
      ~Session() {
        if (scale) {
          api.sws_freeContext_p(scale);
        }
        api.av_dict_free_p(&options);
        api.av_frame_free_p(&decoded);
        api.av_packet_free_p(&packet);
        api.avcodec_free_context_p(&codec);
        api.avformat_close_input_p(&format);
      }
    } session{api};
    struct Interrupt {
      Impl* owner;
      uint64_t token;
      Clock::time_point deadline;
    } interrupt{this, token, Clock::now() + std::chrono::seconds(5)};
    session.format = api.avformat_alloc_context_p();
    if (!session.format) {
      Fail(token, "Not enough memory for the video background.");
      return;
    }
    session.format->interrupt_callback = {
        [](void* opaque) {
          auto* state = static_cast<Interrupt*>(opaque);
          return int(state->owner->Cancelled(state->token) ||
                     Clock::now() >= state->deadline);
        },
        &interrupt};
    // Local files only: playlists must not initiate network requests.
    api.av_dict_set_p(&session.options, "protocol_whitelist", "file", 0);
    api.av_dict_set_p(&session.options, "probesize", "1048576", 0);
    api.av_dict_set_p(&session.options, "analyzeduration", "2000000", 0);
    const auto utf8 = path.u8string();
    if (api.avformat_open_input_p(&session.format,
                                  reinterpret_cast<const char*>(utf8.c_str()),
                                  nullptr, &session.options) < 0 ||
        api.avformat_find_stream_info_p(session.format, nullptr) < 0) {
      Fail(token, "The selected video background could not be opened.");
      return;
    }
    const AVCodec* decoder = nullptr;
    const int stream = api.av_find_best_stream_p(
        session.format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder) {
      Fail(token, "The selected file contains no supported video track.");
      return;
    }
    auto* track = session.format->streams[stream];
    if (track->codecpar->width > 4096 || track->codecpar->height > 4096) {
      Fail(token, "The video background dimensions are unsupported.");
      return;
    }
    session.codec = api.avcodec_alloc_context3_p(decoder);
    if (!session.codec || api.avcodec_parameters_to_context_p(
                              session.codec, track->codecpar) < 0) {
      Fail(token, "The video decoder could not be configured.");
      return;
    }
    session.codec->thread_count = 2;
    session.codec->max_pixels = 4096 * 4096;
    if (api.avcodec_open2_p(session.codec, decoder, nullptr) < 0) {
      Fail(token, "The selected video codec is unavailable.");
      return;
    }
    session.packet = api.av_packet_alloc_p();
    session.decoded = api.av_frame_alloc_p();
    if (!session.packet || !session.decoded) {
      Fail(token, "Not enough memory for the video background.");
      return;
    }
    auto rate = api.av_guess_frame_rate_p(session.format, track, nullptr);
    double fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 30;
    fps = std::clamp(fps, 1.0, 60.0);
    const double time_base =
        track->time_base.num > 0 && track->time_base.den > 0
            ? av_q2d(track->time_base)
            : 1.0 / fps;
    auto origin = Clock::now();
    auto next_publish = origin;
    int64_t first_pts = AV_NOPTS_VALUE;
    uint64_t frame_index = 0;
    auto loop_end = origin;
    bool draining = false;
    while (!Cancelled(token)) {
      interrupt.deadline = Clock::now() + std::chrono::seconds(5);
      int result = api.avcodec_receive_frame_p(session.codec, session.decoded);
      if (result == AVERROR_EOF) {
        // Hold the final frame's duration and retain throttling across loops.
        if (!WaitUntil(token, std::max(loop_end, next_publish))) {
          return;
        }
        if (!frame_index ||
            api.av_seek_frame_p(
                session.format, stream,
                track->start_time == AV_NOPTS_VALUE ? 0 : track->start_time,
                AVSEEK_FLAG_BACKWARD) < 0) {
          Fail(token, "The video background could not be looped.");
          return;
        }
        api.avcodec_flush_buffers_p(session.codec);
        first_pts = AV_NOPTS_VALUE;
        frame_index = 0;
        origin = Clock::now();
        draining = false;
        continue;
      }
      if (result == AVERROR(EAGAIN)) {
        if (draining) {
          Fail(token, "The video decoder stopped producing frames.");
          return;
        }
        do {
          api.av_packet_unref_p(session.packet);
          result = api.av_read_frame_p(session.format, session.packet);
        } while (result >= 0 && session.packet->stream_index != stream &&
                 !Cancelled(token));
        if (Cancelled(token)) {
          return;
        }
        if (result == AVERROR_EOF) {
          draining = true;
          result = api.avcodec_send_packet_p(session.codec, nullptr);
        } else if (result >= 0) {
          result = api.avcodec_send_packet_p(session.codec, session.packet);
        }
        if (result < 0 && result != AVERROR(EAGAIN)) {
          Fail(token, "The video background contains an unreadable frame.");
          return;
        }
        continue;
      }
      if (result < 0) {
        Fail(token, "The video background could not be decoded.");
        return;
      }
      auto* decoded = session.decoded;
      if (decoded->width <= 0 || decoded->height <= 0 ||
          decoded->width > 4096 || decoded->height > 4096) {
        Fail(token, "The video background dimensions are unsupported.");
        return;
      }
      const auto pts = decoded->best_effort_timestamp;
      if (first_pts == AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE) {
        first_pts = pts;
      }
      double seconds = pts != AV_NOPTS_VALUE && first_pts != AV_NOPTS_VALUE
                           ? double(pts - first_pts) * time_base
                           : double(frame_index) / fps;
      ++frame_index;
      // Invalid timestamps must not delay playback or cancellation.
      if (!std::isfinite(seconds) || seconds < 0 || seconds > 3600) {
        seconds = double(frame_index - 1) / fps;
      }
      auto due = origin + std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double>(seconds));
      loop_end = due + std::chrono::duration_cast<Clock::duration>(
                           std::chrono::duration<double>(1.0 / fps));
      if (!WaitUntil(token, due)) {
        return;
      }
      const auto now = Clock::now();
      if (now >= next_publish) {
        const double scale =
            std::min({1.0, 1280.0 / decoded->width, 720.0 / decoded->height});
        Frame output;
        output.width = std::max(1u, uint32_t(decoded->width * scale));
        output.height = std::max(1u, uint32_t(decoded->height * scale));
        const double sar = decoded->sample_aspect_ratio.num > 0 &&
                                   decoded->sample_aspect_ratio.den > 0
                               ? av_q2d(decoded->sample_aspect_ratio)
                               : 1.0;
        output.aspect_ratio =
            float(double(decoded->width) / decoded->height * sar);
        output.rgba.resize(size_t(output.width) * output.height * 4);
        session.scale = api.sws_getCachedContext_p(
            session.scale, decoded->width, decoded->height,
            AVPixelFormat(decoded->format), int(output.width),
            int(output.height), AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr,
            nullptr);
        uint8_t* destination[] = {output.rgba.data(), nullptr, nullptr,
                                  nullptr};
        const int strides[] = {int(output.width * 4), 0, 0, 0};
        if (!session.scale ||
            api.sws_scale_p(session.scale, decoded->data, decoded->linesize, 0,
                            decoded->height, destination, strides) <= 0) {
          Fail(token, "The video background could not be converted.");
          return;
        }
        {
          std::lock_guard lock(mutex);
          if (Cancelled(token)) {
            return;
          }
          frame = std::move(output);
        }
        next_publish = now + std::chrono::microseconds(33333);
      }
      api.av_frame_unref_p(decoded);
    }
#else
    Fail(token, "Video backgrounds are available in the Windows frontend.");
#endif
  }
};

FrontendVideoPlayer::FrontendVideoPlayer() : impl_(std::make_unique<Impl>()) {}
FrontendVideoPlayer::~FrontendVideoPlayer() = default;
void FrontendVideoPlayer::SetPath(const std::filesystem::path& path,
                                  uint64_t revision) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->requested_path == path && impl_->requested_revision == revision) {
    return;
  }
  impl_->requested_path = path;
  impl_->requested_revision = revision;
  ++impl_->generation;
  impl_->frame.reset();
  impl_->error.clear();
  impl_->wake.notify_all();
}
void FrontendVideoPlayer::Stop() { SetPath({}); }
std::optional<FrontendVideoPlayer::Frame> FrontendVideoPlayer::TakeFrame() {
  std::lock_guard lock(impl_->mutex);
  return std::exchange(impl_->frame, std::nullopt);
}
std::string FrontendVideoPlayer::GetError() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->error;
}
}  // namespace xe::app
