// Copyright 2026 Xenia Canary. Released under the BSD license.
// Streaming loads an independent LGPL FFmpeg runtime.
#include "game_streamer.h"
#include "xenia/app/stream_destination.h"
#include "xenia/app/stream_output.h"
#include "xenia/base/logging.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// This translation unit must use third_party/streaming-ffmpeg/include first.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif
static_assert(LIBAVCODEC_VERSION_MAJOR == 60);
static_assert(LIBAVFORMAT_VERSION_MAJOR == 60);
static_assert(LIBAVUTIL_VERSION_MAJOR == 58);
static_assert(LIBSWSCALE_VERSION_MAJOR == 7);
static_assert(LIBSWRESAMPLE_VERSION_MAJOR == 4);

namespace xe::app {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kAudioQueueLimit = 64;
constexpr size_t kAudioFramesPerSubmission = 4096;
constexpr uint32_t kAudioRate = 48000;

bool ValidRequest(const StreamRequest& request) {
  if (request.fps == 0 || request.fps > 60 || request.bitrate_kbps < 500 ||
      request.bitrate_kbps > 50000 ||
      ((request.width == 0) != (request.height == 0)) || request.width > 4096 ||
      request.height > 4096 ||
      (request.width && (request.width < 2 || request.height < 2))) {
    return false;
  }
  return ParseStreamDestination(request.url).has_value();
}

HMODULE LoadRuntimeLibrary(const wchar_t* name) {
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
  return LoadPackagedLibrary(name, 0);
#else
  std::wstring executable(32768, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  if (!length || length >= executable.size()) {
    return nullptr;
  }
  executable.resize(length);
  const auto slash = executable.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return nullptr;
  }
  executable.resize(slash + 1);
  executable += name;
  return LoadLibraryExW(
      executable.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#endif
}

struct FfmpegApi {
#define FF_PROC(name) decltype(&name) name##_fn = nullptr
  FF_PROC(avcodec_alloc_context3);
  FF_PROC(avcodec_find_encoder_by_name);
  FF_PROC(avcodec_free_context);
  FF_PROC(avcodec_open2);
  FF_PROC(avcodec_parameters_from_context);
  FF_PROC(avcodec_receive_packet);
  FF_PROC(avcodec_send_frame);
  FF_PROC(av_packet_alloc);
  FF_PROC(av_packet_free);
  FF_PROC(av_packet_rescale_ts);
  FF_PROC(av_packet_unref);
  FF_PROC(avformat_alloc_output_context2);
  FF_PROC(avformat_free_context);
  FF_PROC(avformat_new_stream);
  FF_PROC(avformat_write_header);
  FF_PROC(av_interleaved_write_frame);
  FF_PROC(av_write_trailer);
  FF_PROC(avio_alloc_context);
  FF_PROC(avio_context_free);
  FF_PROC(av_malloc);
  FF_PROC(av_free);
  FF_PROC(av_dict_set);
  FF_PROC(av_dict_free);
  FF_PROC(av_frame_alloc);
  FF_PROC(av_frame_free);
  FF_PROC(av_frame_get_buffer);
  FF_PROC(av_frame_make_writable);
  FF_PROC(av_channel_layout_default);
  FF_PROC(av_channel_layout_uninit);
  FF_PROC(av_strerror);
  FF_PROC(sws_getCachedContext);
  FF_PROC(sws_scale);
  FF_PROC(sws_freeContext);
  FF_PROC(swr_alloc_set_opts2);
  FF_PROC(swr_init);
  FF_PROC(swr_convert);
  FF_PROC(swr_get_out_samples);
  FF_PROC(swr_free);
#undef FF_PROC
  std::string error;

  FfmpegApi() {
    // Keep modules loaded: some MF transforms retain callbacks after closure.
    HMODULE util = LoadRuntimeLibrary(L"avutil-58.dll");
    HMODULE codec = LoadRuntimeLibrary(L"avcodec-60.dll");
    HMODULE format = LoadRuntimeLibrary(L"avformat-60.dll");
    HMODULE scale = LoadRuntimeLibrary(L"swscale-7.dll");
    HMODULE resample = LoadRuntimeLibrary(L"swresample-4.dll");
    if (!util || !codec || !format || !scale || !resample) {
      error = "The packaged FFmpeg streaming runtime could not be loaded (" +
              std::to_string(GetLastError()) + ").";
      return;
    }
#define FF_LOAD(module, name)                                               \
  name##_fn =                                                               \
      reinterpret_cast<decltype(name##_fn)>(GetProcAddress(module, #name)); \
  if (!name##_fn) {                                                         \
    error = "Missing FFmpeg streaming export: " #name;                      \
    return;                                                                 \
  }
    FF_LOAD(codec, avcodec_alloc_context3);
    FF_LOAD(codec, avcodec_find_encoder_by_name);
    FF_LOAD(codec, avcodec_free_context);
    FF_LOAD(codec, avcodec_open2);
    FF_LOAD(codec, avcodec_parameters_from_context);
    FF_LOAD(codec, avcodec_receive_packet);
    FF_LOAD(codec, avcodec_send_frame);
    FF_LOAD(codec, av_packet_alloc);
    FF_LOAD(codec, av_packet_free);
    FF_LOAD(codec, av_packet_rescale_ts);
    FF_LOAD(codec, av_packet_unref);
    FF_LOAD(format, avformat_alloc_output_context2);
    FF_LOAD(format, avformat_free_context);
    FF_LOAD(format, avformat_new_stream);
    FF_LOAD(format, avformat_write_header);
    FF_LOAD(format, av_interleaved_write_frame);
    FF_LOAD(format, av_write_trailer);
    FF_LOAD(format, avio_alloc_context);
    FF_LOAD(format, avio_context_free);
    FF_LOAD(util, av_malloc);
    FF_LOAD(util, av_free);
    FF_LOAD(util, av_dict_set);
    FF_LOAD(util, av_dict_free);
    FF_LOAD(util, av_frame_alloc);
    FF_LOAD(util, av_frame_free);
    FF_LOAD(util, av_frame_get_buffer);
    FF_LOAD(util, av_frame_make_writable);
    FF_LOAD(util, av_channel_layout_default);
    FF_LOAD(util, av_channel_layout_uninit);
    FF_LOAD(util, av_strerror);
    FF_LOAD(scale, sws_getCachedContext);
    FF_LOAD(scale, sws_scale);
    FF_LOAD(scale, sws_freeContext);
    FF_LOAD(resample, swr_alloc_set_opts2);
    FF_LOAD(resample, swr_init);
    FF_LOAD(resample, swr_convert);
    FF_LOAD(resample, swr_get_out_samples);
    FF_LOAD(resample, swr_free);
#undef FF_LOAD
  }
  static FfmpegApi& Get() {
    static FfmpegApi api;
    return api;
  }
};

struct VideoFrame {
  uint32_t width, height;
  Clock::time_point time;
  std::vector<uint8_t> pixels;
};
struct AudioChunk {
  const void* source;
  Clock::time_point time;
  uint32_t channels, rate;
  std::vector<float> samples;
};

class Encoder {
 public:
  Encoder(FfmpegApi& api, const std::atomic_bool& stopping,
          StreamOutput& output)
      : api_(api), stopping_(stopping), output_(output) {}
  ~Encoder() {
    // No flush on stop: prioritize prompt shutdown over draining stale media.
    if (format_ && format_->pb) {
      api_.av_free_fn(format_->pb->buffer);
      api_.avio_context_free_fn(&format_->pb);
    }
    if (scale_) {
      api_.sws_freeContext_fn(scale_);
    }
    for (auto& [id, source] : audio_sources_) {
      if (source.resample) {
        api_.swr_free_fn(&source.resample);
      }
    }
    if (video_frame_) {
      api_.av_frame_free_fn(&video_frame_);
    }
    if (audio_frame_) {
      api_.av_frame_free_fn(&audio_frame_);
    }
    if (packet_) {
      api_.av_packet_free_fn(&packet_);
    }
    if (video_) {
      api_.avcodec_free_context_fn(&video_);
    }
    if (audio_) {
      api_.avcodec_free_context_fn(&audio_);
    }
    if (format_) {
      api_.avformat_free_context_fn(format_);
    }
  }

  bool Open(const StreamRequest& request, const VideoFrame& first,
            StreamStatus& status, std::string& error,
            bool prefer_hardware = true) {
    opened_at_ = Clock::now();
    const double aspect = static_cast<double>(first.width) / first.height;
    uint32_t width = request.width ? request.width : first.width;
    uint32_t height = request.height ? request.height : first.height;
    if (static_cast<double>(width) / height > aspect) {
      width = static_cast<uint32_t>(std::lround(height * aspect));
    } else {
      height = static_cast<uint32_t>(std::lround(width / aspect));
    }
    width = std::clamp(width & ~1u, 2u, 4096u);
    height = std::clamp(height & ~1u, 2u, 4096u);
    int result = api_.avformat_alloc_output_context2_fn(&format_, nullptr,
                                                        "mpegts", nullptr);
    if (result < 0 || !format_) {
      return Fail("Creating MPEG-TS output", result, error);
    }
    format_->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    format_->max_delay = 0;
    // Bound the video delay while waiting for interleaved audio.
    format_->max_interleave_delta = 20000;
    format_->interrupt_callback.callback = [](void* context) {
      return static_cast<Encoder*>(context)->stopping_.load() ? 1 : 0;
    };
    format_->interrupt_callback.opaque = this;
    packet_ = api_.av_packet_alloc_fn();
    if (!packet_) {
      return Fail("Allocating packet", AVERROR(ENOMEM), error);
    }

    const AVCodec* codec = api_.avcodec_find_encoder_by_name_fn("h264_mf");
    if (!codec) {
      error = "The Media Foundation H.264 streaming encoder is unavailable.";
      return false;
    }
    AVDictionary* options = nullptr;
    for (bool hardware : {true, false}) {
      if (hardware && !prefer_hardware) {
        continue;
      }
      video_ = api_.avcodec_alloc_context3_fn(codec);
      if (!video_) {
        return Fail("Allocating H.264 encoder", AVERROR(ENOMEM), error);
      }
      video_->width = width;
      video_->height = height;
      video_->pix_fmt = AV_PIX_FMT_NV12;
      video_->time_base = {1, static_cast<int>(request.fps)};
      video_->framerate = {static_cast<int>(request.fps), 1};
      video_->bit_rate = static_cast<int64_t>(request.bitrate_kbps) * 1000;
      video_->gop_size = request.fps;
      video_->max_b_frames = 0;
      video_->profile = FF_PROFILE_H264_BASELINE;
      video_->flags |= AV_CODEC_FLAG_LOW_DELAY;
      api_.av_dict_set_fn(&options, "hw_encoding", hardware ? "1" : "0", 0);
      api_.av_dict_set_fn(&options, "rate_control", "cbr", 0);
      api_.av_dict_set_fn(&options, "scenario", "live_streaming", 0);
      result = api_.avcodec_open2_fn(video_, codec, &options);
      api_.av_dict_free_fn(&options);
      if (result >= 0) {
        hardware_encoding_ = hardware;
        XELOGI("Streaming encoder: H.264 {} {}x{} {} fps, {} kbps",
               hardware ? "hardware" : "software", width, height, request.fps,
               request.bitrate_kbps);
        break;
      }
      Fail("Opening Media Foundation H.264 encoder", result, error);
      api_.avcodec_free_context_fn(&video_);
      if (!hardware || stopping_) {
        return false;
      }
      XELOGW("Streaming hardware encoder unavailable; using software: {}",
             error);
      error.clear();
    }
    video_stream_ = api_.avformat_new_stream_fn(format_, nullptr);
    if (!video_stream_) {
      return Fail("Allocating video stream", AVERROR(ENOMEM), error);
    }
    video_stream_->time_base = video_->time_base;
    result = api_.avcodec_parameters_from_context_fn(video_stream_->codecpar,
                                                     video_);
    if (result < 0) {
      return Fail("Publishing video format", result, error);
    }
    video_frame_ = api_.av_frame_alloc_fn();
    if (!video_frame_) {
      return Fail("Allocating video frame", AVERROR(ENOMEM), error);
    }
    video_frame_->format = video_->pix_fmt;
    video_frame_->width = width;
    video_frame_->height = height;
    result = api_.av_frame_get_buffer_fn(video_frame_, 32);
    if (result < 0) {
      return Fail("Allocating video pixels", result, error);
    }
    if (request.audio && !OpenAudio(error)) {
      return false;
    }

    auto* buffer = static_cast<uint8_t*>(api_.av_malloc_fn(1316));
    if (!buffer) {
      return Fail("Allocating stream output", AVERROR(ENOMEM), error);
    }
    format_->pb = api_.avio_alloc_context_fn(
        buffer, 1316, 1, &output_, nullptr,
        [](void* opaque, uint8_t* data, int size) -> int {
          const int written = static_cast<StreamOutput*>(opaque)->Write(
              std::span(data, size_t(size)));
          return written < 0 ? AVERROR(EIO) : written;
        },
        nullptr);
    if (!format_->pb) {
      api_.av_free_fn(buffer);
      return Fail("Creating stream output", AVERROR(ENOMEM), error);
    }
    format_->pb->max_packet_size = 1316;
    format_->flags |= AVFMT_FLAG_CUSTOM_IO;
    options = nullptr;
    api_.av_dict_set_fn(&options, "mpegts_flags", "resend_headers", 0);
    api_.av_dict_set_fn(&options, "flush_packets", "1", 0);
    result = api_.avformat_write_header_fn(format_, &options);
    api_.av_dict_free_fn(&options);
    if (result < 0) {
      return Fail("Starting MPEG-TS stream", result, error);
    }
    start_ = first.time;
    status.width = width;
    status.height = height;
    status.fps = request.fps;
    status.audio = request.audio;
    status.hardware_encoding = hardware_encoding_;
    return true;
  }

  bool Video(const VideoFrame& frame, std::string& error) {
    int result = api_.av_frame_make_writable_fn(video_frame_);
    if (result < 0) {
      return Fail("Preparing video frame", result, error);
    }
    scale_ = api_.sws_getCachedContext_fn(
        scale_, frame.width, frame.height, AV_PIX_FMT_RGBA, video_->width,
        video_->height, video_->pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr,
        nullptr);
    if (!scale_) {
      return Fail("Converting streaming pixels", AVERROR(ENOMEM), error);
    }
    const uint8_t* input[] = {frame.pixels.data()};
    const int strides[] = {static_cast<int>(frame.width * 4)};
    result = api_.sws_scale_fn(scale_, input, strides, 0, frame.height,
                               video_frame_->data, video_frame_->linesize);
    if (result <= 0) {
      return Fail("Scaling streaming frame", result, error);
    }
    const double elapsed =
        std::chrono::duration<double>(frame.time - start_).count();
    video_frame_->pts = std::max(
        last_video_pts_ + 1,
        static_cast<int64_t>(std::llround(elapsed * video_->framerate.num)));
    last_video_pts_ = video_frame_->pts;
    // MF requires a nonzero sample duration in codec time-base units.
    video_frame_->duration = 1;
    // The MF encoder ignores gop_size. Periodic I-frames let UDP receivers
    // recover or join midstream.
    video_frame_->pict_type = AV_PICTURE_TYPE_NONE;
    const bool keyframe_due = video_frame_->pts >= next_keyframe_pts_;
    if (keyframe_due) {
      video_frame_->pict_type = AV_PICTURE_TYPE_I;
    }
    bool accepted = false;
    const bool succeeded =
        Send(video_, video_stream_, video_frame_, error, &accepted);
    if (keyframe_due && accepted) {
      next_keyframe_pts_ = video_frame_->pts + video_->framerate.num;
    }
    return succeeded;
  }

  bool PumpVideo(std::string& error) {
    // Poll asynchronous output during capture pauses without delaying audio.
    return Drain(video_, video_stream_, error);
  }

  bool Audio(const AudioChunk& chunk, std::string& error) {
    if (!audio_) {
      return true;
    }
    if (chunk.samples.empty()) {
      if (auto found = audio_sources_.find(chunk.source);
          found != audio_sources_.end()) {
        if (found->second.resample) {
          api_.swr_free_fn(&found->second.resample);
        }
        audio_sources_.erase(found);
      }
      return true;
    }
    if (!audio_sources_.contains(chunk.source) && audio_sources_.size() >= 16) {
      return true;
    }
    auto& input = audio_sources_[chunk.source];
    const int64_t chunk_time = std::max<int64_t>(
        0, static_cast<int64_t>(
               std::chrono::duration<double>(chunk.time - start_).count() *
               kAudioRate));
    if (!input.resample || input.channels != chunk.channels ||
        input.rate != chunk.rate) {
      if (input.resample) {
        api_.swr_free_fn(&input.resample);
      }
      input.samples.clear();
      input.first_sample = std::max(audio_pts_, chunk_time);
      AVChannelLayout source{}, destination{};
      api_.av_channel_layout_default_fn(&source, chunk.channels);
      api_.av_channel_layout_default_fn(&destination, 2);
      int result = api_.swr_alloc_set_opts2_fn(
          &input.resample, &destination, AV_SAMPLE_FMT_FLT, kAudioRate, &source,
          AV_SAMPLE_FMT_FLT, chunk.rate, 0, nullptr);
      api_.av_channel_layout_uninit_fn(&source);
      api_.av_channel_layout_uninit_fn(&destination);
      if (result < 0 || !input.resample) {
        return Fail("Allocating audio converter", result, error);
      }
      result = api_.swr_init_fn(input.resample);
      if (result < 0) {
        return Fail("Initializing audio converter", result, error);
      }
      input.channels = chunk.channels;
      input.rate = chunk.rate;
    }
    input.last_chunk = chunk.time;
    if (input.samples.empty()) {
      input.first_sample = std::max(input.first_sample, audio_pts_);
    }
    const int input_frames =
        static_cast<int>(chunk.samples.size() / chunk.channels);
    const int capacity =
        api_.swr_get_out_samples_fn(input.resample, input_frames);
    if (capacity < 0 || capacity > 65536) {
      return Fail("Sizing converted audio", capacity, error);
    }
    std::vector<float> converted(static_cast<size_t>(capacity) * 2);
    uint8_t* destination[] = {reinterpret_cast<uint8_t*>(converted.data())};
    const uint8_t* source[] = {
        reinterpret_cast<const uint8_t*>(chunk.samples.data())};
    const int count = api_.swr_convert_fn(input.resample, destination, capacity,
                                          source, input_frames);
    if (count < 0) {
      return Fail("Converting streaming audio", count, error);
    }
    for (int index = 0; index < count * 2; ++index) {
      input.samples.push_back(std::isfinite(converted[index]) ? converted[index]
                                                              : 0.0f);
    }
    while (input.samples.size() > kAudioRate * 2) {
      input.samples.pop_front();
      input.samples.pop_front();
      ++input.first_sample;
    }
    return true;
  }

  bool PumpAudio(std::string& error) {
    if (!audio_) {
      return true;
    }
    // Mix independent voices on one wall clock. Fill missing samples with
    // silence and discard late data instead of stretching the stream clock.
    const auto now = Clock::now();
    const int64_t ready = static_cast<int64_t>(
        (std::chrono::duration<double>(now - start_).count() - 0.025) *
        kAudioRate);
    const int64_t frame_samples = audio_frame_->nb_samples;
    audio_pts_ = std::max(audio_pts_, ready - 8 * frame_samples);
    while (audio_pts_ + frame_samples <= ready) {
      int result = api_.av_frame_make_writable_fn(audio_frame_);
      if (result < 0) {
        return Fail("Preparing audio frame", result, error);
      }
      auto* left = reinterpret_cast<float*>(audio_frame_->data[0]);
      auto* right = reinterpret_cast<float*>(audio_frame_->data[1]);
      std::fill_n(left, frame_samples, 0.0f);
      std::fill_n(right, frame_samples, 0.0f);
      for (auto it = audio_sources_.begin(); it != audio_sources_.end();) {
        auto& source = it->second;
        while (!source.samples.empty() && source.first_sample < audio_pts_) {
          source.samples.pop_front();
          source.samples.pop_front();
          ++source.first_sample;
        }
        while (!source.samples.empty() &&
               source.first_sample < audio_pts_ + frame_samples) {
          const auto offset =
              static_cast<size_t>(source.first_sample - audio_pts_);
          left[offset] += source.samples.front();
          source.samples.pop_front();
          right[offset] += source.samples.front();
          source.samples.pop_front();
          ++source.first_sample;
        }
        if (source.samples.empty() &&
            now - source.last_chunk > std::chrono::seconds(2)) {
          if (source.resample) {
            api_.swr_free_fn(&source.resample);
          }
          it = audio_sources_.erase(it);
        } else {
          ++it;
        }
      }
      for (int index = 0; index < frame_samples; ++index) {
        left[index] = std::clamp(left[index], -1.0f, 1.0f);
        right[index] = std::clamp(right[index], -1.0f, 1.0f);
      }
      audio_frame_->pts = audio_pts_;
      audio_pts_ += audio_frame_->nb_samples;
      if (!Send(audio_, audio_stream_, audio_frame_, error)) {
        return false;
      }
    }
    return true;
  }

 private:
  bool OpenAudio(std::string& error) {
    const AVCodec* codec = api_.avcodec_find_encoder_by_name_fn("aac");
    if (!codec) {
      error = "The AAC streaming encoder is unavailable.";
      return false;
    }
    audio_ = api_.avcodec_alloc_context3_fn(codec);
    if (!audio_) {
      return Fail("Allocating AAC encoder", AVERROR(ENOMEM), error);
    }
    audio_->sample_rate = kAudioRate;
    audio_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    audio_->time_base = {1, kAudioRate};
    audio_->bit_rate = 128000;
    api_.av_channel_layout_default_fn(&audio_->ch_layout, 2);
    int result = api_.avcodec_open2_fn(audio_, codec, nullptr);
    if (result < 0) {
      return Fail("Opening AAC encoder", result, error);
    }
    audio_stream_ = api_.avformat_new_stream_fn(format_, nullptr);
    if (!audio_stream_) {
      return Fail("Allocating audio stream", AVERROR(ENOMEM), error);
    }
    audio_stream_->time_base = audio_->time_base;
    result = api_.avcodec_parameters_from_context_fn(audio_stream_->codecpar,
                                                     audio_);
    if (result < 0) {
      return Fail("Publishing audio format", result, error);
    }
    audio_frame_ = api_.av_frame_alloc_fn();
    if (!audio_frame_) {
      return Fail("Allocating audio frame", AVERROR(ENOMEM), error);
    }
    audio_frame_->sample_rate = kAudioRate;
    audio_frame_->format = AV_SAMPLE_FMT_FLTP;
    audio_frame_->nb_samples = std::max(audio_->frame_size, 1);
    api_.av_channel_layout_default_fn(&audio_frame_->ch_layout, 2);
    result = api_.av_frame_get_buffer_fn(audio_frame_, 0);
    return result >= 0 || Fail("Allocating audio samples", result, error);
  }
  bool Drain(AVCodecContext* codec, AVStream* stream, std::string& error) {
    for (;;) {
      int result = api_.avcodec_receive_packet_fn(codec, packet_);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return true;
      }
      if (result < 0) {
        return Fail("Reading encoded packet", result, error);
      }
      if (codec == video_ && !video_packet_logged_) {
        video_packet_logged_ = true;
        XELOGI(
            "Streaming encoder: first video packet hardware={} bytes={} "
            "startup_ms={:.1f}",
            hardware_encoding_, packet_->size,
            std::chrono::duration<double, std::milli>(Clock::now() - opened_at_)
                .count());
      }
      if (codec == video_) {
        const auto now = Clock::now();
        ++video_packets_in_period_;
        if (packet_->pts != AV_NOPTS_VALUE) {
          // Quantized capture PTS estimates frame age, excluding
          // network/display.
          const double age_ms = std::max(
              0.0,
              std::chrono::duration<double, std::milli>(now - start_).count() -
                  1000.0 * packet_->pts * codec->time_base.num /
                      codec->time_base.den);
          video_age_sum_ms_ += age_ms;
          video_age_max_ms_ = std::max(video_age_max_ms_, age_ms);
          ++video_ages_in_period_;
        }
        const double period =
            std::chrono::duration<double>(now - video_stats_start_).count();
        if (period >= 5.0) {
          XELOGI(
              "Streaming video output: hardware={} fps={:.1f} "
              "sender_age_ms_avg={:.1f} sender_age_ms_max={:.1f}",
              hardware_encoding_, video_packets_in_period_ / period,
              video_ages_in_period_ ? video_age_sum_ms_ / video_ages_in_period_
                                    : 0.0,
              video_age_max_ms_);
          video_stats_start_ = now;
          video_packets_in_period_ = video_ages_in_period_ = 0;
          video_age_sum_ms_ = video_age_max_ms_ = 0.0;
        }
      }
      api_.av_packet_rescale_ts_fn(packet_, codec->time_base,
                                   stream->time_base);
      packet_->stream_index = stream->index;
      result = api_.av_interleaved_write_frame_fn(format_, packet_);
      api_.av_packet_unref_fn(packet_);
      if (result < 0) {
        return Fail("Sending MPEG-TS packet", result, error);
      }
    }
  }
  bool Send(AVCodecContext* codec, AVStream* stream, AVFrame* frame,
            std::string& error, bool* accepted = nullptr) {
    if (accepted) {
      *accepted = false;
    }
    int result = api_.avcodec_send_frame_fn(codec, frame);
    if (result == AVERROR(EAGAIN)) {
      if (!Drain(codec, stream, error)) {
        return false;
      }
      result = api_.avcodec_send_frame_fn(codec, frame);
    }
    if (result == AVERROR(EAGAIN) && codec == video_) {
      // FFmpeg retains its accepted input. Drop this unaccepted frame so an
      // asynchronous encoder cannot delay the audio clock.
      return true;
    }
    if (accepted) {
      *accepted = result >= 0;
    }
    return result >= 0 ? Drain(codec, stream, error)
                       : Fail("Encoding streaming frame", result, error);
  }
  bool Fail(const char* operation, int result, std::string& error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    api_.av_strerror_fn(result, message, sizeof(message));
    error = std::string(operation) + ": " + message;
    return false;
  }
  FfmpegApi& api_;
  const std::atomic_bool& stopping_;
  StreamOutput& output_;
  AVFormatContext* format_ = nullptr;
  AVCodecContext* video_ = nullptr;
  AVCodecContext* audio_ = nullptr;
  AVStream* video_stream_ = nullptr;
  AVStream* audio_stream_ = nullptr;
  AVFrame* video_frame_ = nullptr;
  AVFrame* audio_frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  SwsContext* scale_ = nullptr;
  struct AudioSource {
    SwrContext* resample = nullptr;
    uint32_t channels = 0, rate = 0;
    int64_t first_sample = 0;
    Clock::time_point last_chunk{};
    std::deque<float> samples;
  };
  std::unordered_map<const void*, AudioSource> audio_sources_;
  int64_t audio_pts_ = 0, last_video_pts_ = -1, next_keyframe_pts_ = 0;
  Clock::time_point start_{};
  Clock::time_point opened_at_{};
  bool hardware_encoding_ = false;
  bool video_packet_logged_ = false;
  Clock::time_point video_stats_start_ = Clock::now();
  uint32_t video_packets_in_period_ = 0, video_ages_in_period_ = 0;
  double video_age_sum_ms_ = 0.0, video_age_max_ms_ = 0.0;
};
}  // namespace

struct GameStreamer::Impl {
  StreamOutput output;
  mutable std::mutex mutex;
  std::mutex lifecycle;
  std::condition_variable wake;
  std::atomic_bool stopping{true};
  uint64_t generation = 0;
  std::thread worker;
  StreamStatus status;
  std::optional<std::string> pending_error;
  std::optional<VideoFrame> video;
  std::deque<AudioChunk> audio;
  Clock::time_point next_video{};

  void Fail(std::string error, bool only_if_active = false) {
    std::lock_guard lock(mutex);
    if (only_if_active && !status.starting && !status.running) {
      return;
    }
    status.starting = false;
    status.running = false;
    status.error = error;
    pending_error = std::move(error);
    video.reset();
    audio.clear();
    stopping = true;
    wake.notify_all();
  }
  void Work(StreamRequest request) {
    // Create and use MF transforms in the same worker apartment.
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ApartmentScope {
      bool initialized;
      ~ApartmentScope() {
        if (initialized) {
          CoUninitialize();
        }
      }
    } apartment_scope{SUCCEEDED(apartment)};
    try {
      if (FAILED(apartment)) {
        Fail("Could not initialize the streaming COM apartment.");
        return;
      }
      auto& api = FfmpegApi::Get();
      if (!api.error.empty()) {
        Fail(api.error);
        return;
      }
      std::optional<VideoFrame> first;
      {
        std::unique_lock lock(mutex);
        if (!wake.wait_for(lock, std::chrono::seconds(5), [&] {
              return stopping.load() || video.has_value();
            })) {
          lock.unlock();
          Fail("No game video frames arrived for streaming.");
          return;
        }
        if (stopping) {
          return;
        }
        first = std::move(video);
        video.reset();
      }
      auto encoder = std::make_unique<Encoder>(api, stopping, output);
      StreamStatus initialized;
      std::string error;
      bool opened = encoder->Open(request, *first, initialized, error);
      bool encoded = opened && encoder->Video(*first, error);
      if (opened && !encoded && initialized.hardware_encoding && !stopping) {
        XELOGW(
            "Streaming hardware encoder rejected its first frame; "
            "restarting with software: {}",
            error);
        encoder.reset();
        encoder = std::make_unique<Encoder>(api, stopping, output);
        error.clear();
        encoded = encoder->Open(request, *first, initialized, error, false) &&
                  encoder->Video(*first, error);
      }
      if (!encoded) {
        if (!stopping) {
          Fail(std::move(error));
        }
        return;
      }
      first.reset();
      {
        std::lock_guard lock(mutex);
        if (stopping) {
          return;
        }
        status.starting = false;
        status.running = true;
        status.width = initialized.width;
        status.height = initialized.height;
        status.hardware_encoding = initialized.hardware_encoding;
        status.video_frames = 1;
      }
      for (;;) {
        std::optional<VideoFrame> next_frame;
        std::deque<AudioChunk> pending_audio;
        {
          std::unique_lock lock(mutex);
          wake.wait_for(lock, std::chrono::milliseconds(5), [&] {
            return stopping.load() || video || !audio.empty();
          });
          if (stopping) {
            return;
          }
          next_frame = std::move(video);
          video.reset();
          pending_audio.swap(audio);
        }
        bool succeeded = true;
        for (const auto& chunk : pending_audio) {
          if (!encoder->Audio(chunk, error)) {
            succeeded = false;
            break;
          }
        }
        if (!succeeded || (next_frame && !encoder->Video(*next_frame, error)) ||
            !encoder->PumpVideo(error) || !encoder->PumpAudio(error)) {
          if (!stopping) {
            Fail(std::move(error));
          }
          return;
        }
        if (next_frame) {
          std::lock_guard lock(mutex);
          ++status.video_frames;
        }
      }
    } catch (const std::exception& error) {
      Fail(std::string("Streaming failed: ") + error.what());
    } catch (...) {
      Fail("Streaming failed with an unexpected error.");
    }
  }
  void StopWorker() {
    stopping = true;
    wake.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
    std::lock_guard lock(mutex);
    video.reset();
    audio.clear();
    status.starting = false;
    status.running = false;
  }
};

GameStreamer::GameStreamer() : impl_(std::make_unique<Impl>()) {}
GameStreamer::~GameStreamer() { Stop(); }
bool GameStreamer::Start(const StreamRequest& request, uint32_t receiver) {
  std::lock_guard lifecycle(impl_->lifecycle);
  if (!ValidRequest(request) ||
      !impl_->output.SetDestination(receiver, request.url)) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->status.starting && !impl_->status.running) {
      impl_->status.error = "Invalid stream target or format.";
      impl_->pending_error = impl_->status.error;
    }
    return false;
  }
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->status.starting || impl_->status.running) {
      return true;
    }
  }
  impl_->StopWorker();
  {
    std::lock_guard lock(impl_->mutex);
    impl_->status = {};
    ++impl_->generation;
    impl_->pending_error.reset();
    impl_->next_video = {};
  }
  {
    std::lock_guard lock(impl_->mutex);
    impl_->status.starting = true;
    impl_->status.url = request.url;
    impl_->status.width = request.width;
    impl_->status.height = request.height;
    impl_->status.fps = request.fps;
    impl_->status.audio = request.audio;
    impl_->stopping = false;
  }
  try {
    impl_->worker =
        std::thread([impl = impl_.get(), request] { impl->Work(request); });
  } catch (const std::exception& error) {
    impl_->Fail(error.what());
    return false;
  }
  return true;
}
void GameStreamer::Stop() {
  std::lock_guard lifecycle(impl_->lifecycle);
  impl_->output.Clear();
  impl_->StopWorker();
}
void GameStreamer::Stop(uint32_t receiver) {
  std::lock_guard lifecycle(impl_->lifecycle);
  impl_->output.RemoveDestination(receiver);
  if (impl_->output.Empty()) {
    impl_->StopWorker();
  }
}
StreamStatus GameStreamer::GetStatus(uint32_t receiver) const {
  std::lock_guard lifecycle(impl_->lifecycle);
  const auto url = impl_->output.GetDestination(receiver);
  if (url.empty()) {
    return {};
  }
  auto status = GetStatus();
  status.url = url;
  return status;
}
StreamStatus GameStreamer::GetStatus() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->status;
}
std::optional<std::string> GameStreamer::TakeError() {
  std::lock_guard lock(impl_->mutex);
  return std::exchange(impl_->pending_error, std::nullopt);
}
void GameStreamer::ReportCaptureFailure(std::string error) {
  impl_->Fail(std::move(error), true);
}
void GameStreamer::SubmitVideo(std::span<const uint8_t> rgbx, uint32_t width,
                               uint32_t height, size_t stride) {
  if (impl_->stopping || !width || !height || width > 4096 || height > 4096 ||
      stride < static_cast<size_t>(width) * 4 || stride > 131072 ||
      rgbx.size() < (height - 1) * stride + static_cast<size_t>(width) * 4) {
    return;
  }
  std::unique_lock lock(impl_->mutex, std::try_to_lock);
  if (!lock || impl_->stopping) {
    return;
  }
  const auto now = Clock::now();
  if (now < impl_->next_video) {
    return;
  }
  impl_->next_video =
      now + std::chrono::microseconds(1000000 / impl_->status.fps);
  const auto generation = impl_->generation;
  lock.unlock();
  // Copy outside the audio mutex; reject work invalidated by stop/restart.
  VideoFrame frame{width, height, now, {}};
  frame.pixels.resize(static_cast<size_t>(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(frame.pixels.data() + static_cast<size_t>(y) * width * 4,
                rgbx.data() + static_cast<size_t>(y) * stride, width * 4);
  }
  if (!lock.try_lock() || impl_->stopping || generation != impl_->generation) {
    return;
  }
  impl_->video = std::move(frame);
  impl_->wake.notify_one();
}
void GameStreamer::SubmitVideo(std::vector<uint8_t>&& rgbx, uint32_t width,
                               uint32_t height, size_t stride) {
  if (stride != static_cast<size_t>(width) * 4) {
    SubmitVideo(std::span<const uint8_t>(rgbx), width, height, stride);
    return;
  }
  if (impl_->stopping || !width || !height || width > 4096 || height > 4096 ||
      rgbx.size() < static_cast<size_t>(width) * height * 4) {
    return;
  }
  std::unique_lock lock(impl_->mutex, std::try_to_lock);
  if (!lock || impl_->stopping) {
    return;
  }
  // Capture already paces frames; move its pixels without throttling again.
  impl_->video = VideoFrame{width, height, Clock::now(), std::move(rgbx)};
  impl_->wake.notify_one();
}
void GameStreamer::SubmitAudio(std::span<const float> interleaved,
                               uint32_t channels, uint32_t sample_rate) {
  SubmitAudio(nullptr, interleaved, channels, sample_rate);
}
void GameStreamer::SubmitAudio(const void* source_id,
                               std::span<const float> interleaved,
                               uint32_t channels, uint32_t sample_rate) {
  if (impl_->stopping ||
      (!interleaved.empty() &&
       (!channels || channels > 8 || sample_rate < 8000 ||
        sample_rate > 192000 || interleaved.size() % channels ||
        interleaved.size() / channels > kAudioFramesPerSubmission))) {
    return;
  }
  std::unique_lock lock(impl_->mutex, std::try_to_lock);
  if (!lock || impl_->stopping || !impl_->status.audio) {
    return;
  }
  if (impl_->audio.size() == kAudioQueueLimit) {
    impl_->audio.pop_front();
  }
  impl_->audio.push_back(AudioChunk{source_id,
                                    Clock::now(),
                                    channels,
                                    sample_rate,
                                    {interleaved.begin(), interleaved.end()}});
  impl_->wake.notify_one();
}

}  // namespace xe::app

#else
#include <mutex>
#include <utility>
namespace xe::app {
struct GameStreamer::Impl {
  std::mutex mutex;
  StreamStatus status;
  std::optional<std::string> pending_error;
};
GameStreamer::GameStreamer() : impl_(std::make_unique<Impl>()) {}
GameStreamer::~GameStreamer() = default;
bool GameStreamer::Start(const StreamRequest&, uint32_t) {
  std::lock_guard lock(impl_->mutex);
  impl_->status.error = "The streaming encoder is available only on Windows.";
  impl_->pending_error = impl_->status.error;
  return false;
}
void GameStreamer::Stop() {}
void GameStreamer::Stop(uint32_t) {}
StreamStatus GameStreamer::GetStatus(uint32_t) const { return GetStatus(); }
void GameStreamer::ReportCaptureFailure(std::string) {}
StreamStatus GameStreamer::GetStatus() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->status;
}
std::optional<std::string> GameStreamer::TakeError() {
  std::lock_guard lock(impl_->mutex);
  return std::exchange(impl_->pending_error, std::nullopt);
}
void GameStreamer::SubmitVideo(std::span<const uint8_t>, uint32_t, uint32_t,
                               size_t) {}
void GameStreamer::SubmitVideo(std::vector<uint8_t>&&, uint32_t, uint32_t,
                               size_t) {}
void GameStreamer::SubmitAudio(std::span<const float>, uint32_t, uint32_t) {}
void GameStreamer::SubmitAudio(const void*, std::span<const float>, uint32_t,
                               uint32_t) {}
}  // namespace xe::app
#endif
