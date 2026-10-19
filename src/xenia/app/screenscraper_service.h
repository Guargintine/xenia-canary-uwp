// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_SCREENSCRAPER_SERVICE_H_
#define XENIA_APP_SCREENSCRAPER_SERVICE_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xe::app::screenscraper {

enum class MediaType : uint8_t {
  Cover,
  BackCover,
  Box3D,
  PhysicalMedia,
  Marquee,
  Screenshot,
  TitleScreen,
  FanArt,
  Video,
  Manual,
  MixImage,
  Count
};
constexpr uint32_t MediaBit(MediaType type) { return 1u << uint32_t(type); }
constexpr uint32_t kDefaultMedia =
    MediaBit(MediaType::Cover) | MediaBit(MediaType::Screenshot) |
    MediaBit(MediaType::Marquee) | MediaBit(MediaType::FanArt);
struct DeveloperIdentity {
  std::string id, password, software_name = "Xenia Canary";
};
DeveloperIdentity GetDeveloperIdentity();
struct Account {
  std::string user, password;
};
struct Configuration {
  std::filesystem::path media_root, cache_root;
  DeveloperIdentity developer;
  Account account;
  std::string language = "en", region = "us";
  uint32_t media_mask = kDefaultMedia;
  bool download_metadata = true, overwrite = false;
};
struct Target {
  uint64_t id = 0;
  std::string name;
  std::filesystem::path content_path, content_root;
  // Optional game path relative to the xbox360 game directory. The final
  // extension is removed, preserving all parent directories and earlier dots.
  std::filesystem::path relative_game_path;
  // Read-only fallback for artwork saved with the older extracted-game name.
  std::filesystem::path legacy_relative_game_path;
  bool directory_as_file = false;
  uint64_t matched_game_id = 0;
};
struct Metadata {
  uint64_t source_game_id = 0;
  std::string title, description, developer, publisher, genre, release_date;
  std::string players, region;
  double rating = 0.0;
};
struct MediaReference {
  MediaType type = MediaType::Cover;
  std::string api_type, region, format;
  std::string selector;
};
struct Match {
  Metadata metadata;
  std::vector<MediaReference> media;
  uint32_t available_media = 0;
  std::vector<std::string> names;
  std::map<MediaType, std::filesystem::path> previews;
  uint32_t requested_previews = 0;
};
struct CachedGame {
  Metadata metadata;
  bool local_override = false;
  std::map<MediaType, std::filesystem::path> media;
  uint64_t revision = 0;
};
enum class State : uint8_t {
  Idle,
  Working,
  Finished,
  Cancelled,
  NoMatches,
  Ambiguous,
  Error,
  QuotaPaused
};
struct Progress {
  State state = State::Idle;
  size_t completed = 0, total = 0, downloaded_files = 0, failed = 0;
  std::string status;
  size_t unavailable_files = 0;
  size_t unmatched_games = 0;
};
struct SearchSnapshot {
  uint64_t generation = 0;
  State state = State::Idle;
  std::string status;
  std::vector<Match> matches;
};

std::string_view MediaDirectory(MediaType type);
std::span<const std::string_view> MediaExtensions(MediaType type);
std::filesystem::path MakeMediaPath(const Configuration& config,
                                    const Target& target, MediaType type,
                                    std::string_view extension = ".png");
std::filesystem::path FindExistingMedia(const Configuration& config,
                                        const Target& target, MediaType type);
// Parses ScreenScraper API2 JSON without retaining URL credentials.
std::vector<Match> ParseResponse(std::string_view json,
                                 std::string_view language,
                                 std::string_view region);
std::string NormalizeText(std::string_view value);

struct HttpResponse {
  long status = 0;
  std::vector<uint8_t> body;
  bool cancelled = false;
  uint32_t retry_after_seconds = 0;
};
// Request URLs contain credentials and must not be logged.
using Transport = std::function<HttpResponse(const std::string&, size_t,
                                             const std::atomic<bool>&)>;
Transport MakeHttpTransport();

class Service {
 public:
  explicit Service(Transport transport = MakeHttpTransport());
  ~Service();
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;
  // Rejects changes during a job; the caller owns credential persistence.
  bool Configure(Configuration config);
  // Only network requests require account and developer credentials.
  bool Start(std::vector<Target> targets);
  bool Search(Target target, uint64_t game_id = 0);
  bool RequestPreview(uint64_t generation, uint64_t game_id, MediaType type);
  void Cancel();
  void CancelSearch() { Cancel(); }
  Progress GetProgress() const;
  SearchSnapshot GetSearchSnapshot() const;
  uint64_t GetRevision() const;
  std::vector<uint64_t> ConsumeUpdates();
  std::optional<CachedGame> LoadCached(const Target& target) const;
  bool SaveMetadata(const Target& target, const Metadata& metadata,
                    const Metadata& previous, std::string* status = nullptr);
  std::filesystem::path FindMedia(const Target& target, MediaType type) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace xe::app::screenscraper
#endif
