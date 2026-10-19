// Copyright 2026 Xenia Canary. Released under the BSD license.
#include "xenia/app/dsu_companion.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"
#include "xenia/base/platform.h"
#if XE_PLATFORM_WINRT
#include "xenia/base/logging.h"
#endif
#if XE_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace xe::app {
namespace {
template <typename... Args>
void TraceStream(std::string_view format, const Args&... args) {
#if XE_PLATFORM_WINRT
  XELOGI(format, args...);
#else
  (void)format;
  ((void)args, ...);
#endif
}
constexpr uint32_t kState = 0x140000;
constexpr uint32_t kStreamStart = 0x150000;
constexpr uint32_t kStreamStop = 0x150001;
constexpr uint32_t kStreamStatus = 0x150002;
constexpr uint32_t kAuxStart = 0x150100;
constexpr uint32_t kAuxStop = 0x150101;
constexpr uint32_t kLaunch = 0x180004;
constexpr uint32_t kLaunchStatus = 0x180005;
constexpr uint32_t kSelection = 0x180006;
constexpr uint32_t kCapabilities = 0xE201;
constexpr uint64_t kMaxMediaBytes = 256ull * 1024 * 1024;
constexpr size_t kMaxCatalogueEntries = 4096;

uint64_t ReadLE(std::span<const uint8_t> data, size_t offset, size_t count) {
  uint64_t value = 0;
  for (size_t i = 0; i < count; ++i) {
    value |= uint64_t(data[offset + i]) << (i * 8);
  }
  return value;
}
void WriteLE(std::vector<uint8_t>& data, size_t offset, uint64_t value,
             size_t count) {
  for (size_t i = 0; i < count; ++i) {
    data[offset + i] = uint8_t(value >> (i * 8));
  }
}
void BoundUtf8(std::string& value, size_t limit) {
  size_t size = std::min(value.size(), limit);
  while (size && size < value.size() &&
         (static_cast<unsigned char>(value[size]) & 0xC0) == 0x80) {
    --size;
  }
  value.resize(size);
}
void WriteString(std::vector<uint8_t>& data, size_t offset, size_t capacity,
                 const std::string& value) {
  size_t count = std::min(value.size(), capacity - 1);
  // Truncation may not split a multibyte UTF-8 character.
  while (count && count < value.size() &&
         (static_cast<unsigned char>(value[count]) & 0xC0) == 0x80) {
    --count;
  }
  std::memcpy(data.data() + offset, value.data(), count);
}
std::string TokenText(uint64_t token) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string text(16, '0');
  for (size_t i = 0; i < 16; ++i) {
    text[15 - i] = hex[(token >> (i * 4)) & 15];
  }
  return text;
}
bool ParseNumber(std::string_view text, uint64_t& result, int base = 10) {
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result, base);
  return !text.empty() && parsed.ec == std::errc{} &&
         parsed.ptr == text.data() + text.size();
}
std::string Lower(std::string value) {
  for (char& c : value) {
    if (c >= 'A' && c <= 'Z') {
      c += 'a' - 'A';
    }
  }
  return value;
}
const char* MediaMime(const std::filesystem::path& path) {
  const auto extension = Lower(path.extension().string());
  if (extension == ".png") {
    return "image/png";
  }
  if (extension == ".jpg" || extension == ".jpeg") {
    return "image/jpeg";
  }
  if (extension == ".webp") {
    return "image/webp";
  }
  if (extension == ".gif") {
    return "image/gif";
  }
  if (extension == ".mp4") {
    return "video/mp4";
  }
  return "application/octet-stream";
}
std::vector<uint8_t> SelectionPayload(uint32_t revision, uint64_t token) {
  std::vector<uint8_t> result(12);
  WriteLE(result, 0, revision, 4);
  WriteLE(result, 4, token, 8);
  return result;
}
#if XE_PLATFORM_WIN32
bool SendAll(SOCKET socket, const char* data, size_t size,
             const std::atomic<bool>& stopping) {
  while (size && !stopping.load()) {
    const int sent = send(socket, data, int(std::min(size, size_t(16384))), 0);
    if (sent <= 0) {
      return false;
    }
    data += sent;
    size -= sent;
  }
  return size == 0;
}
uint32_t ResolvePhone(const std::string& host) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) {
    return 0;
  }
  const auto address =
      reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr;
  freeaddrinfo(result);
  return address;
}
bool IsPhoneStreamUrl(const std::string& url, uint32_t allowed_address) {
  if (!allowed_address || !url.starts_with("udp://") || url.size() > 255) {
    return false;
  }
  const auto colon = url.find(':', 6);
  if (colon == std::string::npos) {
    return false;
  }
  const auto host = url.substr(6, colon - 6);
  IN_ADDR address{};
  uint64_t port = 0;
  return InetPtonA(AF_INET, host.c_str(), &address) == 1 &&
         address.s_addr == allowed_address &&
         ParseNumber(std::string_view(url).substr(colon + 1), port) &&
         port > 0 && port <= 65535;
}
#endif
}  // namespace

DsuCompanion::DsuCompanion(SendPacket send, StreamCallbacks streaming)
    : send_(std::move(send)), streaming_(std::move(streaming)) {
  revision_ =
      uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()) | 1;
  worker_ = std::thread([this] { Worker(); });
}
DsuCompanion::~DsuCompanion() { Stop(); }
void DsuCompanion::Stop() {
  stopping_ = true;
  if (worker_.joinable()) {
    worker_.join();
  }
}
void DsuCompanion::Configure(DsuCompanionConfig config) {
  // Finish the active command before replacing its peer and queued commands.
  std::lock_guard tick_lock(tick_mutex_);
  std::lock_guard lock(mutex_);
  if (config.enabled == config_.enabled &&
      config.streaming_enabled == config_.streaming_enabled &&
      config.host == config_.host && config.port == config_.port &&
      config.stream_url == config_.stream_url &&
      config.serve_catalogue == config_.serve_catalogue) {
    return;
  }
  config_ = std::move(config);
  status_ = config_.enabled ? "Updating companion configuration" : "Disabled";
  ++config_generation_;
  phone_address_ = 0;
  connected_ = false;
  video_ready_ = false;
  automatic_stream_attempted_ = false;
  last_state_ = {};
  pending_launch_.reset();
  launch_in_progress_.reset();
  pending_selection_.reset();
  selection_ack_pending_ = false;
  stream_command_ = StreamCommand{true, {}};
}
void DsuCompanion::SetHttpPeers(
    std::vector<std::weak_ptr<DsuCompanion>> peers) {
  std::lock_guard lock(mutex_);
  http_peers_ = std::move(peers);
}

bool DsuCompanion::HandleHttp(uintptr_t socket, uint32_t peer_address) {
  uint64_t generation;
  uint32_t allowed_address;
  {
    std::lock_guard lock(mutex_);
    if (!config_.enabled || !phone_address_ || peer_address != phone_address_) {
      return false;
    }
    generation = config_generation_;
    allowed_address = phone_address_;
  }
  ServeHttp(socket, peer_address, allowed_address, generation);
  return true;
}
void DsuCompanion::PublishCatalogue(std::vector<DsuCatalogueEntry> entries) {
  std::set<uint64_t> ids;
  std::erase_if(entries, [&](const auto& entry) {
    return !entry.id || entry.path.empty() || !ids.insert(entry.id).second;
  });
  if (entries.size() > kMaxCatalogueEntries) {
    entries.resize(kMaxCatalogueEntries);
  }
  size_t metadata_budget = 4 * 1024 * 1024;
  for (auto& entry : entries) {
    BoundUtf8(entry.name, 512);
    if (entry.platform.empty()) {
      entry.platform = "Microsoft - Xbox 360";
    }
    BoundUtf8(entry.platform, 128);
    size_t fields = 0;
    std::erase_if(entry.metadata, [&](const auto& pair) {
      return ++fields > 32 || pair.first.size() > 64;
    });
    for (auto& [key, value] : entry.metadata) {
      BoundUtf8(value, std::min(size_t(2048), metadata_budget));
      metadata_budget -= value.size();
    }
    std::erase_if(entry.media, [](const auto& pair) { return pair.first > 9; });
  }
  std::lock_guard lock(mutex_);
  if (entries_ == entries) {
    return;
  }
  entries_ = std::move(entries);
  if (++revision_ == 0) {
    ++revision_;
  }
  if (selected_ &&
      std::none_of(entries_.begin(), entries_.end(),
                   [&](const auto& e) { return e.id == *selected_; })) {
    selected_.reset();
  }
  last_state_ = {};
}
void DsuCompanion::UpdateRunning(uint64_t id, std::string name, bool running) {
  std::lock_guard lock(mutex_);
  if (running_ != running || running_id_ != id) {
    automatic_stream_attempted_ = false;
    video_ready_ = false;
    if (running_) {
      stream_command_ = StreamCommand{true, {}};
    }
  }
  if (running_ != running || running_id_ != id || running_name_ != name) {
    last_state_ = {};
  }
  running_ = running;
  running_id_ = id;
  running_name_ = std::move(name);
}
void DsuCompanion::UpdateConnection(bool connected) {
  std::lock_guard lock(mutex_);
  if (connected_ == connected) {
    return;
  }
  connected_ = connected;
  last_state_ = {};
  if (!config_.stream_url.empty()) {
    return;
  }
  automatic_stream_attempted_ = false;
  video_ready_ = false;
  if (!connected) {
    stream_command_ = StreamCommand{true, {}};
  }
}
void DsuCompanion::UpdateVideoReady(bool ready) {
  std::lock_guard lock(mutex_);
  video_ready_ = ready;
}
void DsuCompanion::UpdateSelection(uint64_t id) {
  std::vector<uint8_t> selection;
  {
    std::lock_guard lock(mutex_);
    // A newer pending request takes precedence over this UI snapshot.
    if (pending_selection_ || (selected_ == id && !selection_ack_pending_)) {
      return;
    }
    if (std::none_of(entries_.begin(), entries_.end(),
                     [id](const auto& e) { return e.id == id; })) {
      return;
    }
    selected_ = id;
    selection_ack_pending_ = false;
    if (config_.enabled) {
      selection = SelectionPayload(revision_, id);
    }
  }
  // Acknowledge the applied token immediately, without waiting for a heartbeat.
  if (!selection.empty()) {
    Send(kSelection, std::move(selection));
  }
}
std::optional<uint64_t> DsuCompanion::TakePendingLaunch() {
  std::lock_guard lock(mutex_);
  auto result = std::exchange(pending_launch_, std::nullopt);
  if (result) {
    launch_in_progress_ = result;
  }
  return result;
}
std::optional<uint64_t> DsuCompanion::TakePendingSelection() {
  std::lock_guard lock(mutex_);
  return std::exchange(pending_selection_, std::nullopt);
}
void DsuCompanion::CompleteLaunch(bool succeeded) {
  std::optional<uint64_t> token;
  uint32_t revision;
  {
    std::lock_guard lock(mutex_);
    token = std::exchange(launch_in_progress_, std::nullopt);
    revision = launch_revision_;
  }
  if (token && !succeeded) {
    SendLaunchStatus(3, revision, *token);
  }
}
void DsuCompanion::Send(uint32_t type, std::vector<uint8_t> payload) const {
  if (send_ && !stopping_) {
    send_(type, payload);
  }
}
void DsuCompanion::SendLaunchStatus(uint32_t status, uint32_t revision,
                                    uint64_t token) {
  static const std::array<std::string, 5> messages = {
      "Launch accepted", "Catalogue changed; refresh the library",
      "Unknown game", "Launch unavailable or failed",
      "A game is already running or launching"};
  std::vector<uint8_t> result(144);
  WriteLE(result, 0, status, 4);
  WriteLE(result, 4, revision, 4);
  WriteLE(result, 8, token, 8);
  WriteString(result, 16, 128, messages[std::min(status, uint32_t(4))]);
  Send(kLaunchStatus, std::move(result));
}
void DsuCompanion::SendStreamStatus(uint32_t state, uint32_t error,
                                    uint8_t screen,
                                    const StreamStatus* status) {
  std::vector<uint8_t> result(280);
  WriteLE(result, 0, state, 4);
  WriteLE(result, 4, error, 4);
  WriteLE(result, 8, 4, 4);
  result[12] = screen;
  if (status) {
    WriteString(result, 16, 256, status->url);
    WriteLE(result, 272, status->width, 2);
    WriteLE(result, 274, status->height, 2);
    const float aspect =
        status->height ? float(status->width) / float(status->height) : 0;
    uint32_t bits;
    std::memcpy(&bits, &aspect, sizeof(bits));
    WriteLE(result, 276, bits, 4);
  }
  Send(kStreamStatus, std::move(result));
}
void DsuCompanion::HandleExtension(uint32_t type,
                                   std::span<const uint8_t> payload) {
  DsuCompanionConfig config;
  uint64_t generation;
  uint32_t phone_address;
  {
    std::lock_guard lock(mutex_);
    config = config_;
    generation = config_generation_;
    phone_address = phone_address_;
  }
  if (!config.enabled) {
    return;
  }
  if (type == kLaunch || type == kSelection) {
    if (payload.size() != 12) {
      return;
    }
    const auto revision = uint32_t(ReadLE(payload, 0, 4));
    const auto token = ReadLE(payload, 4, 8);
    uint32_t status = 0;
    {
      std::lock_guard lock(mutex_);
      if (!config_.enabled || config_generation_ != generation) {
        return;
      }
      const bool known =
          std::any_of(entries_.begin(), entries_.end(),
                      [token](const auto& e) { return e.id == token; });
      if (revision != revision_) {
        status = 1;
      } else if (!known) {
        status = 2;
      } else if (type == kSelection) {
        if (!running_ && !pending_launch_ && !launch_in_progress_) {
          pending_selection_ = token;
          selection_ack_pending_ = true;
        }
      } else if ((pending_launch_ == token || launch_in_progress_ == token) &&
                 launch_revision_ == revision) {
        // UDP retries are acknowledged without scheduling a second launch.
      } else if (running_ || pending_launch_ || launch_in_progress_) {
        status = 4;
      } else {
        pending_launch_ = token;
        launch_revision_ = revision;
      }
    }
    if (type == kLaunch) {
      SendLaunchStatus(status, revision, token);
    }
    return;
  }
  if (type == kAuxStart || type == kAuxStop) {
    SendStreamStatus(2, 6, payload.empty() ? 1 : payload[0]);
    return;
  }
  if (!config.stream_url.empty() &&
      (type == kStreamStart || type == kStreamStop)) {
    SendStreamStatus(0, 0, 0);
    return;
  }
  if (type == kStreamStop && payload.empty()) {
    std::lock_guard lock(mutex_);
    if (!config_.enabled || config_generation_ != generation) {
      return;
    }
    stream_command_ = StreamCommand{true, {}};
    // Explicit STOP lasts until the next game, connection or configuration.
    automatic_stream_attempted_ = true;
    TraceStream(
        "Companion stream: explicit STOP received; automatic start "
        "paused for this game/connection cycle");
  } else if (type == kStreamStart) {
    // Extended stream requests append audio and an optional authorization
    // token. Xenia authorizes the configured peer, without a token handshake.
    if (payload.size() != 269 && payload.size() != 270 &&
        payload.size() != 274) {
      SendStreamStatus(2, 1, 0);
      return;
    }
    if (!config.streaming_enabled || !streaming_.start || !streaming_.status) {
      SendStreamStatus(2, 3, 0);
      return;
    }
    if (ReadLE(payload, 0, 4) != 4) {
      SendStreamStatus(2, 6, 0);
      return;
    }
    StreamRequest request;
    const auto first = payload.begin() + 4;
    const auto end = std::find(first, first + 256, uint8_t(0));
    if (end == first + 256) {
      SendStreamStatus(2, 1, 0);
      return;
    }
    request.url.assign(first, end);
#if XE_PLATFORM_WIN32
    if (!IsPhoneStreamUrl(request.url, phone_address)) {
      SendStreamStatus(2, 2, 0);
      return;
    }
#else
    SendStreamStatus(2, 6, 0);
    return;
#endif
    request.bitrate_kbps = uint32_t(ReadLE(payload, 260, 4));
    request.width = uint32_t(ReadLE(payload, 264, 2));
    request.height = uint32_t(ReadLE(payload, 266, 2));
    request.fps = payload[268];
    request.audio = payload.size() >= 270 ? payload[269] != 0 : true;
    std::lock_guard lock(mutex_);
    if (!config_.enabled || config_generation_ != generation) {
      return;
    }
    stream_command_ = StreamCommand{false, std::move(request)};
    automatic_stream_attempted_ = true;
  }
}
void DsuCompanion::Tick() {
  std::lock_guard tick_lock(tick_mutex_);
  std::optional<StreamCommand> command;
  std::vector<uint8_t> state;
  std::vector<uint8_t> selection;
  bool announce_stream = false;
  bool invalid_automatic_target = false;
  bool custom_destination = false;
  {
    std::lock_guard lock(mutex_);
    custom_destination = !config_.stream_url.empty();
    command = std::exchange(stream_command_, std::nullopt);
    const uint32_t gates =
        uint32_t(config_.enabled) | (uint32_t(config_.streaming_enabled) << 1) |
        (uint32_t(connected_) << 2) | (uint32_t(running_) << 3) |
        (uint32_t(video_ready_) << 4) | (uint32_t(phone_address_ != 0) << 5) |
        (uint32_t(automatic_stream_attempted_) << 6) |
        (uint32_t(stream_requested_) << 7) |
        (uint32_t(command.has_value()) << 8);
    if (gates != logged_stream_gates_) {
      logged_stream_gates_ = gates;
      TraceStream(
          "Companion stream gates: enabled={} streaming={} connected={} "
          "running={} ready={} peer_resolved={} attempted={} "
          "requested={} command={}",
          config_.enabled, config_.streaming_enabled, connected_, running_,
          video_ready_, phone_address_ != 0, automatic_stream_attempted_,
          stream_requested_, command.has_value());
    }
    if (!command && !stream_requested_ && !automatic_stream_attempted_ &&
        custom_destination && config_.streaming_enabled && running_ &&
        video_ready_ && streaming_.start && streaming_.status) {
      automatic_stream_attempted_ = true;
      StreamRequest request;
      request.url = config_.stream_url;
      command = StreamCommand{false, std::move(request)};
    }
    if (!custom_destination && !command && !stream_requested_ &&
        !automatic_stream_attempted_ && config_.enabled &&
        config_.streaming_enabled && connected_ && running_ && video_ready_ &&
        phone_address_ && streaming_.start && streaming_.status) {
      automatic_stream_attempted_ = true;
      // The main receiver uses DSU port + 200; wide arithmetic prevents wrap.
      const uint32_t video_port = uint32_t(config_.port) + 200;
#if XE_PLATFORM_WIN32
      IN_ADDR address{};
      address.s_addr = phone_address_;
      std::array<char, INET_ADDRSTRLEN> host{};
      if (config_.port && video_port <= 65535 &&
          InetNtopA(AF_INET, &address, host.data(), DWORD(host.size()))) {
        StreamRequest request;
        request.url = "udp://" + std::string(host.data()) + ":" +
                      std::to_string(video_port);
        request.width = 1280;
        request.height = 720;
        request.fps = 30;
        request.bitrate_kbps = 4000;
        request.audio = true;
        TraceStream(
            "Companion stream: automatic start {} {}x{} fps={} "
            "bitrate_kbps={} audio={}",
            request.url, request.width, request.height, request.fps,
            request.bitrate_kbps, request.audio);
        command = StreamCommand{false, std::move(request)};
      } else
#endif
      {
        invalid_automatic_target = true;
        status_ =
            "Automatic streaming requires a server IPv4 address and a "
            "DSU port no higher than 65335";
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (config_.enabled && now - last_state_ >= std::chrono::seconds(1)) {
      last_state_ = now;
      announce_stream = true;
      state.resize(261);
      state[0] = running_ ? 1 : 0;
      WriteString(state, 1, 64, running_ ? running_name_ : "");
      WriteString(state, 65, 64, "Microsoft - Xbox 360");
      WriteString(state, 129, 64, "Xenia Canary");
      WriteString(state, 193, 64, "Xenia Canary");
      uint32_t capabilities = kCapabilities;
      if (!custom_destination && config_.streaming_enabled &&
          streaming_.start && streaming_.status) {
        capabilities |= 1 << 4;
      }
      WriteLE(state, 257, capabilities, 4);
      if (selected_) {
        selection = SelectionPayload(revision_, *selected_);
      }
    }
  }
  if (!state.empty()) {
    Send(kState, std::move(state));
  }
  if (!selection.empty()) {
    Send(kSelection, std::move(selection));
  }
  if (invalid_automatic_target) {
    idle_stream_error_ = 1;
    SendStreamStatus(2, 1, 0);
  }
  if (command) {
    last_stream_state_.clear();
    idle_stream_error_ = 0;
    if (command->stop) {
      TraceStream("Companion stream: applying stop command");
      if (streaming_.stop) {
        streaming_.stop();
      }
      stream_requested_ = false;
      SendStreamStatus(0, 0, 0);
    } else {
      stream_requested_ =
          streaming_.start && streaming_.start(command->request);
      TraceStream("Companion stream: start accepted={}", stream_requested_);
      if (!stream_requested_) {
        idle_stream_error_ = 4;
        if (!custom_destination) {
          SendStreamStatus(2, 4, 0);
        }
      }
    }
  }
  if (custom_destination) {
    if (announce_stream) {
      SendStreamStatus(0, 0, 0);
    }
    return;
  }
  if (stream_requested_ && streaming_.status) {
    const auto status = streaming_.status();
    const std::string key =
        (status.running ? "running:" : "waiting:") + status.error + status.url;
    if ((key != last_stream_state_ || announce_stream) &&
        (status.running || !status.error.empty())) {
      SendStreamStatus(status.running ? 1 : 2, status.running ? 0 : 5, 0,
                       &status);
      last_stream_state_ = key;
    }
  } else if (announce_stream && !command && !invalid_automatic_target) {
    SendStreamStatus(idle_stream_error_ ? 2 : 0, idle_stream_error_, 0);
  }
}
std::string DsuCompanion::GetStatus() const {
  std::lock_guard lock(mutex_);
  return status_;
}
std::string DsuCompanion::CatalogueJson() const {
  std::vector<DsuCatalogueEntry> entries;
  uint32_t revision;
  {
    std::lock_guard lock(mutex_);
    entries = entries_;
    revision = revision_;
  }
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> json(buffer);
  const auto string = [&](const char* key, const std::string& value) {
    json.Key(key);
    json.String(value.data(), rapidjson::SizeType(value.size()));
  };
  json.StartObject();
  json.Key("version");
  json.Uint(1);
  string("application", "Xenia Canary");
  json.Key("playlists");
  json.StartArray();
  json.StartObject();
  string("name", "Microsoft - Xbox 360");
  json.Key("count");
  json.Uint(unsigned(entries.size()));
  json.Key("entries");
  json.StartArray();
  static const std::set<std::string> metadata_keys = {
      "crc32",         "runtime",        "last_played",
      "desc",          "genre",          "dev",
      "pub",           "players",        "released",
      "rating",        "content_rating", "modes",
      "themes",        "region",         "format",
      "compatibility", "franchise",      "serial",
      "year",          "month"};
  for (const auto& entry : entries) {
    json.StartObject();
    string("label", entry.name);
    string("path", "xenia:" + TokenText(entry.id));
    string("platform", entry.platform);
    string("db", entry.platform);
    string("file", entry.file);
    string("core", "Xenia Canary");
    string("core_path", "");
    string("app", "Xenia Canary");
    json.Key("launch_revision");
    json.Uint(revision);
    string("launch_token", TokenText(entry.id));
    uint32_t media = 0;
    for (const auto& [kind, path] : entry.media) {
      media |= 1 << kind;
    }
    json.Key("media");
    json.Uint(media);
    for (const auto& [key, value] : entry.metadata) {
      if (metadata_keys.contains(key)) {
        string(key.c_str(), value.substr(0, 16384));
      }
    }
    json.EndObject();
  }
  json.EndArray();
  json.EndObject();
  json.EndArray();
  json.EndObject();
  return std::string(buffer.GetString(), buffer.GetSize());
}

void DsuCompanion::Worker() {
#if XE_PLATFORM_WIN32
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::lock_guard lock(mutex_);
    status_ = "Network initialization failed";
    return;
  }
  SOCKET listener = INVALID_SOCKET;
  uint64_t generation = std::numeric_limits<uint64_t>::max();
  uint32_t allowed_address = 0;
  auto retry_at = std::chrono::steady_clock::time_point{};
  while (!stopping_) {
    DsuCompanionConfig config;
    uint64_t current_generation;
    {
      std::lock_guard lock(mutex_);
      config = config_;
      current_generation = config_generation_;
    }
    const auto now = std::chrono::steady_clock::now();
    if (generation != current_generation ||
        (config.enabled && config.serve_catalogue &&
         listener == INVALID_SOCKET && now >= retry_at)) {
      if (listener != INVALID_SOCKET) {
        closesocket(listener);
      }
      listener = INVALID_SOCKET;
      generation = current_generation;
      allowed_address = config.enabled ? ResolvePhone(config.host) : 0;
      retry_at = now + std::chrono::seconds(5);
      std::string status = "Disabled";
      if (config.enabled && config.port && allowed_address &&
          config.serve_catalogue) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port =
            htons(uint16_t(std::min(uint32_t(config.port) + 1, 65535u)));
        if (listener == INVALID_SOCKET ||
            bind(listener, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) == SOCKET_ERROR ||
            listen(listener, 4) == SOCKET_ERROR) {
          if (listener != INVALID_SOCKET) {
            closesocket(listener);
          }
          listener = INVALID_SOCKET;
          status = "Catalogue port unavailable";
        } else {
          status = "Catalogue ready on port " +
                   std::to_string(ntohs(address.sin_port));
        }
      } else if (config.enabled && allowed_address && !config.serve_catalogue) {
        status = "Catalogue shared";
      } else if (config.enabled) {
        status = "Configure a valid DSU server IPv4 address and port";
      }
      std::lock_guard lock(mutex_);
      if (config_generation_ == generation) {
        phone_address_ = allowed_address;
        status_ = std::move(status);
      }
    }
    Tick();
    if (listener == INVALID_SOCKET) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(listener, &read_set);
    timeval timeout{0, 100000};
    if (select(0, &read_set, nullptr, nullptr, &timeout) > 0) {
      sockaddr_in peer{};
      int peer_size = sizeof(peer);
      SOCKET client =
          accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
      if (client != INVALID_SOCKET) {
        const DWORD socket_timeout = 2000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&socket_timeout),
                   sizeof(socket_timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&socket_timeout),
                   sizeof(socket_timeout));
        std::vector<std::weak_ptr<DsuCompanion>> peers;
        {
          std::lock_guard lock(mutex_);
          peers = http_peers_;
        }
        bool handled = false;
        for (const auto& weak : peers) {
          if (auto companion = weak.lock();
              companion &&
              companion->HandleHttp(uintptr_t(client), peer.sin_addr.s_addr)) {
            handled = true;
            break;
          }
        }
        if (!handled) {
          ServeHttp(uintptr_t(client), peer.sin_addr.s_addr, allowed_address,
                    generation);
        }
        closesocket(client);
      }
    }
  }
  if (listener != INVALID_SOCKET) {
    closesocket(listener);
  }
  if (streaming_.stop) {
    streaming_.stop();
  }
  WSACleanup();
#endif
}

void DsuCompanion::ServeHttp(uintptr_t socket_value, uint32_t peer_address,
                             uint32_t allowed_address, uint64_t generation) {
#if XE_PLATFORM_WIN32
  const SOCKET client = SOCKET(socket_value);
  const auto reply = [&](const char* status, const char* content_type,
                         std::string_view body) {
    const std::string header =
        std::string("HTTP/1.1 ") + status +
        "\r\nContent-Type: " + content_type +
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
    SendAll(client, header.data(), header.size(), stopping_);
    SendAll(client, body.data(), body.size(), stopping_);
  };
  bool configured;
  {
    std::lock_guard lock(mutex_);
    configured = config_.enabled && config_generation_ == generation;
  }
  if (!configured || !allowed_address || peer_address != allowed_address) {
    reply("403 Forbidden", "text/plain", "Configured companion only");
    return;
  }
  std::string request;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (request.find("\r\n\r\n") == std::string::npos &&
         request.size() < 8192 && !stopping_) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return;
    }
    std::array<char, 1024> chunk{};
    const int count = recv(client, chunk.data(), int(chunk.size()), 0);
    if (count <= 0) {
      return;
    }
    request.append(chunk.data(), size_t(count));
    Tick();
  }
  if (request.size() >= 8192 || request.find("\r\n\r\n") == std::string::npos) {
    reply("431 Request Header Fields Too Large", "text/plain",
          "Request too large");
    return;
  }
  std::istringstream first_line(request.substr(0, request.find("\r\n")));
  std::string method, target, version;
  first_line >> method >> target >> version;
  if (method != "GET" && method != "HEAD") {
    reply("405 Method Not Allowed", "text/plain", "GET or HEAD required");
    return;
  }
  if (target == "/catalogue") {
    const auto json = CatalogueJson();
    if (method == "HEAD") {
      const std::string header =
          "HTTP/1.1 200 OK\r\nContent-Type: "
          "application/json\r\nContent-Length: " +
          std::to_string(json.size()) +
          "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
      SendAll(client, header.data(), header.size(), stopping_);
    } else {
      reply("200 OK", "application/json", json);
    }
    return;
  }
  if (target.size() < 10 || target.substr(0, 7) != "/media/" ||
      target[7] < '0' || target[7] > '9' || target[8] != '?') {
    reply("404 Not Found", "text/plain", "Unknown resource");
    return;
  }
  std::map<std::string, std::string> query;
  std::istringstream query_stream(target.substr(9));
  std::string pair;
  while (std::getline(query_stream, pair, '&')) {
    const auto equal = pair.find('=');
    if (equal == std::string::npos ||
        !query.emplace(pair.substr(0, equal), pair.substr(equal + 1)).second) {
      reply("400 Bad Request", "text/plain", "Invalid query");
      return;
    }
  }
  uint64_t revision = 0, token = 0;
  if (query.size() != 2 || !ParseNumber(query["revision"], revision) ||
      revision > UINT32_MAX || query["token"].size() != 16 ||
      !ParseNumber(query["token"], token, 16)) {
    reply("400 Bad Request", "text/plain", "Invalid revision or token");
    return;
  }
  std::filesystem::path path;
  bool stale;
  {
    std::lock_guard lock(mutex_);
    stale = revision != revision_;
    if (!stale) {
      for (const auto& entry : entries_) {
        if (entry.id != token) {
          continue;
        }
        const auto media = entry.media.find(uint8_t(target[7] - '0'));
        if (media != entry.media.end()) {
          path = media->second;
        }
        break;
      }
    }
  }
  if (stale) {
    reply("409 Conflict", "text/plain", "Catalogue changed");
    return;
  }
  std::error_code error;
  const auto size = path.empty() ? 0 : std::filesystem::file_size(path, error);
  if (path.empty() || error || !size || size > kMaxMediaBytes ||
      !std::filesystem::is_regular_file(path, error)) {
    reply("404 Not Found", "text/plain", "Media unavailable");
    return;
  }
  uint64_t start = 0, end = size - 1;
  bool partial = false;
  const auto lower_request = Lower(request);
  const auto range_position = lower_request.find("\r\nrange:");
  if (range_position != std::string::npos) {
    auto range = lower_request.substr(range_position + 8);
    range.resize(range.find("\r\n"));
    while (!range.empty() && range.front() == ' ') {
      range.erase(range.begin());
    }
    const auto dash = range.find('-');
    if (!range.starts_with("bytes=") || dash == std::string::npos ||
        !ParseNumber(std::string_view(range).substr(6, dash - 6), start) ||
        (dash + 1 < range.size() &&
         !ParseNumber(std::string_view(range).substr(dash + 1), end)) ||
        start >= size || end < start) {
      reply("416 Range Not Satisfiable", "text/plain", "Invalid range");
      return;
    }
    end = std::min(end, uint64_t(size - 1));
    partial = true;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    reply("404 Not Found", "text/plain", "Media unavailable");
    return;
  }
  file.seekg(std::streamoff(start));
  const std::string header =
      std::string("HTTP/1.1 ") + (partial ? "206 Partial Content" : "200 OK") +
      "\r\nContent-Type: " + MediaMime(path) +
      "\r\nContent-Length: " + std::to_string(end - start + 1) +
      "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n" +
      (partial ? "Content-Range: bytes " + std::to_string(start) + "-" +
                     std::to_string(end) + "/" + std::to_string(size) + "\r\n"
               : "") +
      "\r\n";
  if (!SendAll(client, header.data(), header.size(), stopping_) ||
      method == "HEAD") {
    return;
  }
  std::array<char, 16384> data;
  uint64_t remaining = end - start + 1;
  while (remaining && !stopping_) {
    {
      std::lock_guard lock(mutex_);
      if (!config_.enabled || config_generation_ != generation) {
        break;
      }
    }
    file.read(data.data(),
              std::streamsize(std::min(remaining, uint64_t(data.size()))));
    const auto count = file.gcount();
    if (count <= 0 || !SendAll(client, data.data(), size_t(count), stopping_)) {
      break;
    }
    remaining -= uint64_t(count);
    Tick();
  }
#endif
}

}  // namespace xe::app
