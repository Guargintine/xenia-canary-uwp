// Copyright 2026 Xenia Canary. Released under the BSD license.
#include "xenia/app/screenscraper_service.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include "third_party/pugixml/src/pugixml.hpp"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include <Windows.h>
#if XE_PLATFORM_WINRT
#include <fileapifromapp.h>
#endif
#endif

namespace xe::app::screenscraper {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kMetadataLimit = 8 * 1024 * 1024;
constexpr size_t kMediaLimit = 128 * 1024 * 1024;
enum class LookupKind { Name, Search, Id };
void Log(std::string message) {
  logging::AppendLogLine(LogLevel::Info, 'i', "ScreenScraper: " + message);
}
std::string SearchName(const std::string& name) {
  static const std::regex disc_suffix(
      R"((?:\s*[-:]\s*|\s+|\s*[\(\[])(?:disc|disk|cd)\s*[0-9]+(?:\s*(?:of|/)\s*[0-9]+)?\s*[\)\]]?\s*$)",
      std::regex::icase);
  auto result = std::regex_replace(name, disc_suffix, "");
  while (!result.empty() &&
         std::isspace(static_cast<unsigned char>(result.back()))) {
    result.pop_back();
  }
  return result.empty() ? name : result;
}
std::string Utf8Path(const std::filesystem::path& path) {
  const auto text = path.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::string Encode(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      encoded.push_back(char(c));
    } else {
      encoded.push_back('%');
      encoded.push_back(hex[c >> 4]);
      encoded.push_back(hex[c & 15]);
    }
  }
  return encoded;
}
std::string ApiUrl(const Configuration& config, std::string_view endpoint) {
  return "https://api.screenscraper.fr/api2/" + std::string(endpoint) +
         "?output=json&devid=" + Encode(config.developer.id) +
         "&devpassword=" + Encode(config.developer.password) +
         "&softname=" + Encode(config.developer.software_name) +
         "&ssid=" + Encode(config.account.user) +
         "&sspassword=" + Encode(config.account.password) + "&systemeid=33";
}
std::string SearchKey(std::string_view name) {
  std::string result;
  for (unsigned char c : name) {
    if (std::isalnum(c) || c >= 128) {
      result.push_back(char(std::tolower(c)));
    }
  }
  return result;
}
std::string SearchName(const Target& target) {
  static const std::regex disc_name(
      R"(^\s*(?:disc|disk|cd)\s*[0-9]+(?:\s*(?:of|/)\s*[0-9]+)?\s*$)",
      std::regex::icase);
  if (std::regex_match(target.name, disc_name) &&
      SearchKey(Utf8Path(target.content_path.filename())) == "defaultxex") {
    const auto folder =
        target.content_path.parent_path().parent_path().filename();
    if (!folder.empty()) {
      return SearchName(Utf8Path(folder));
    }
  }
  return SearchName(target.name);
}
std::filesystem::path CachePath(const Configuration& config,
                                const Target& target, bool local = false) {
  if (!target.id || config.cache_root.empty() ||
      !config.cache_root.is_absolute()) {
    return {};
  }
  std::ostringstream name;
  name << std::hex << std::setw(16) << std::setfill('0') << target.id
       << (local ? ".local.json" : ".json");
  return config.cache_root / "screenscraper" / name.str();
}
bool IsPathWithinStorage(const std::filesystem::path& root,
                         const std::filesystem::path& path,
                         std::error_code* path_error = nullptr) {
  const auto fail = [&](std::error_code error) {
    if (path_error) {
      *path_error = error;
    }
    return false;
  };
  if (path_error) {
    path_error->clear();
  }
  if (!root.is_absolute() || !path.is_absolute()) {
    return fail(std::make_error_code(std::errc::invalid_argument));
  }
  auto normalized_root = root.lexically_normal();
  if (!normalized_root.has_filename() &&
      normalized_root != normalized_root.root_path()) {
    normalized_root = normalized_root.parent_path();
  }
  const auto relative =
      path.lexically_normal().lexically_relative(normalized_root);
  if (relative.empty() || relative == "." || relative.is_absolute()) {
    return fail(std::make_error_code(std::errc::invalid_argument));
  }
  for (const auto& part : relative) {
    if (part.empty() || part == "." || part == "..") {
      return fail(std::make_error_code(std::errc::invalid_argument));
    }
#if XE_PLATFORM_WIN32
    const auto& name = part.native();
    if (name.find(L':') != std::wstring::npos || name.back() == L'.' ||
        name.back() == L' ') {
      return fail(std::make_error_code(std::errc::invalid_argument));
    }
#endif
  }
  // UWP storage ancestors may deny handle access. Inspect only the selected
  // storage boundary and its descendants, without resolving their ancestors.
  auto current = normalized_root;
  auto component = relative.begin();
  while (true) {
#if XE_PLATFORM_WIN32
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
#if XE_PLATFORM_WINRT
    const bool found =
        GetFileAttributesExFromAppW(current.c_str(), GetFileExInfoStandard,
                                    &attributes) != 0;
#else
    const bool found =
        GetFileAttributesExW(current.c_str(), GetFileExInfoStandard,
                             &attributes) != 0;
#endif
    if (!found) {
      const auto error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        return fail({static_cast<int>(error), std::system_category()});
      }
    } else if (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      return fail(
          std::make_error_code(std::errc::too_many_symbolic_link_levels));
    } else if (component != relative.end() &&
               !(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      return fail(std::make_error_code(std::errc::not_a_directory));
    }
#else
    std::error_code error;
    const auto status = std::filesystem::symlink_status(current, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      return fail(error);
    }
    if (std::filesystem::is_symlink(status)) {
      return fail(
          std::make_error_code(std::errc::too_many_symbolic_link_levels));
    }
    if (std::filesystem::exists(status) && component != relative.end() &&
        !std::filesystem::is_directory(status)) {
      return fail(std::make_error_code(std::errc::not_a_directory));
    }
#endif
    if (component == relative.end()) {
      break;
    }
    current /= *component++;
  }
  return true;
}
bool WriteAtomic(const std::filesystem::path& root,
                 const std::filesystem::path& path,
                 const std::vector<uint8_t>& data, bool overwrite,
                 const std::atomic<bool>& cancelled,
                 std::error_code* write_error = nullptr) {
  const auto fail = [&](std::error_code error) {
    if (write_error) {
      *write_error = error;
    }
    return false;
  };
  if (write_error) {
    write_error->clear();
  }
  if (path.empty() || data.empty()) {
    return fail(std::make_error_code(std::errc::invalid_argument));
  }
  if (cancelled) {
    return fail(std::make_error_code(std::errc::operation_canceled));
  }
  if (!IsPathWithinStorage(root, path, write_error)) {
    return false;
  }
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return fail(error);
  }
  if (!IsPathWithinStorage(root, path, write_error)) {
    return false;
  }
  static std::atomic<uint64_t> sequence{0};
  auto temporary = path;
  temporary += ".xenia-ss-" +
               std::to_string(Clock::now().time_since_epoch().count()) + "-" +
               std::to_string(++sequence) + ".tmp";
  const auto remove_temporary = [&] {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
  };
#if XE_PLATFORM_WINRT
  const auto output = CreateFile2FromAppW(temporary.c_str(), GENERIC_WRITE, 0,
                                          CREATE_NEW, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    return fail({static_cast<int>(GetLastError()), std::system_category()});
  }
  DWORD written = 0;
  if (data.size() > MAXDWORD) {
    error = std::make_error_code(std::errc::file_too_large);
  } else if (!WriteFile(output, data.data(), DWORD(data.size()), &written,
                        nullptr) ||
             !FlushFileBuffers(output)) {
    error = {static_cast<int>(GetLastError()), std::system_category()};
  } else if (written != data.size()) {
    error = std::make_error_code(std::errc::io_error);
  }
  if (!CloseHandle(output) && !error) {
    error = {static_cast<int>(GetLastError()), std::system_category()};
  }
  if (error || cancelled) {
    remove_temporary();
    return fail(cancelled ? std::make_error_code(std::errc::operation_canceled)
                          : error);
  }
#else
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      return fail(std::make_error_code(std::errc::io_error));
    }
    output.write(reinterpret_cast<const char*>(data.data()),
                 std::streamsize(data.size()));
    output.close();
    if (!output || cancelled) {
      remove_temporary();
      return fail(std::make_error_code(cancelled ? std::errc::operation_canceled
                                                 : std::errc::io_error));
    }
  }
#endif
  bool moved = false;
#if XE_PLATFORM_WINRT
  moved = MoveFileFromAppW(temporary.c_str(), path.c_str()) != 0;
  if (!moved && overwrite) {
    moved = ReplaceFileFromAppW(path.c_str(), temporary.c_str(), nullptr,
                                REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr,
                                nullptr) != 0;
  }
#elif XE_PLATFORM_WIN32
  moved = MoveFileExW(temporary.c_str(), path.c_str(),
                      overwrite ? MOVEFILE_REPLACE_EXISTING : 0) != 0;
#else
  if (overwrite || !std::filesystem::exists(path, error)) {
    std::filesystem::rename(temporary, path, error);
    moved = !error;
  }
#endif
  if (!moved) {
#if XE_PLATFORM_WIN32
    const auto move_error = std::error_code(static_cast<int>(GetLastError()),
                                            std::system_category());
#else
    const auto move_error =
        error ? error : std::make_error_code(std::errc::file_exists);
#endif
    remove_temporary();
    return fail(move_error);
  }
  return moved;
}
size_t PngEncodedSize(const std::vector<uint8_t>& data) {
  const std::string_view bytes(reinterpret_cast<const char*>(data.data()),
                               data.size());
  if (!bytes.starts_with("\x89PNG\r\n\x1a\n")) {
    return 0;
  }
  bool image_data = false;
  for (size_t offset = 8;
       offset <= bytes.size() && bytes.size() - offset >= 12;) {
    const auto length = (uint32_t(data[offset]) << 24) |
                        (uint32_t(data[offset + 1]) << 16) |
                        (uint32_t(data[offset + 2]) << 8) | data[offset + 3];
    if (length > bytes.size() - offset - 12) {
      return 0;
    }
    const auto chunk = bytes.substr(offset + 4, 4);
    if (offset == 8 && (chunk != "IHDR" || length != 13)) {
      return 0;
    }
    if (chunk == "IDAT") {
      image_data = true;
    }
    if (chunk == "IEND") {
      return image_data && length == 0 &&
                     bytes.substr(offset + 8, 4) == "\xae\x42\x60\x82"
                 ? offset + 12
                 : 0;
    }
    offset += size_t(length) + 12;
  }
  return 0;
}
size_t JpegEncodedSize(const std::vector<uint8_t>& data) {
  if (data.size() < 2 || data[0] != 0xFF || data[1] != 0xD8) {
    return 0;
  }
  bool scan = false, image_data = false;
  for (size_t offset = 2; offset < data.size();) {
    if (scan && data[offset] != 0xFF) {
      ++offset;
      continue;
    }
    if (data[offset++] != 0xFF) {
      return 0;
    }
    while (offset < data.size() && data[offset] == 0xFF) {
      ++offset;
    }
    if (offset == data.size()) {
      return 0;
    }
    const auto marker = data[offset++];
    if (scan && (marker == 0 || (marker >= 0xD0 && marker <= 0xD7))) {
      continue;
    }
    if (marker == 0xD9) {
      return image_data ? offset : 0;
    }
    if (marker == 0 || (marker >= 0xD0 && marker <= 0xD8)) {
      return 0;
    }
    if (marker == 1) {
      continue;
    }
    if (data.size() - offset < 2) {
      return 0;
    }
    const size_t length = (size_t(data[offset]) << 8) | data[offset + 1];
    if (length < 2 || length > data.size() - offset) {
      return 0;
    }
    scan = marker == 0xDA || (scan && marker == 0xDC);
    image_data |= scan;
    offset += length;
  }
  return 0;
}
bool IsValidMediaData(const std::vector<uint8_t>& data, MediaType type,
                      std::string_view format) {
  if (data.size() < 16) {
    return false;
  }
  const std::string_view bytes(reinterpret_cast<const char*>(data.data()),
                               data.size());
  if (type == MediaType::Manual) {
    return bytes.starts_with("%PDF-") &&
           bytes.substr(bytes.size() > 1024 ? bytes.size() - 1024 : 0)
                   .find("%%EOF") != bytes.npos;
  }
  if (type == MediaType::Video) {
    if (format == "mp4" || format == "mov") {
      const auto atom = bytes.substr(4, 4);
      return bytes.size() >= 24 &&
             (atom == "ftyp" ||
              (format == "mov" &&
               (atom == "moov" || atom == "mdat" || atom == "wide")));
    }
    if (format == "mkv" || format == "webm") {
      return bytes.starts_with("\x1a\x45\xdf\xa3");
    }
    if (format == "avi") {
      return bytes.starts_with("RIFF") && bytes.substr(8, 4) == "AVI ";
    }
    if (format == "wmv") {
      return bytes.starts_with(
          std::string_view("\x30\x26\xb2\x75\x8e\x66\xcf\x11"
                           "\xa6\xd9\x00\xaa\x00\x62\xce\x6c",
                           16));
    }
    return false;
  }
  if (format == "png") {
    return PngEncodedSize(data) != 0;
  }
  if (format == "jpg" || format == "jpeg") {
    return JpegEncodedSize(data) != 0;
  }
  if (format == "webp") {
    const auto length = uint32_t(data[4]) | (uint32_t(data[5]) << 8) |
                        (uint32_t(data[6]) << 16) | (uint32_t(data[7]) << 24);
    return bytes.size() >= 20 && bytes.starts_with("RIFF") &&
           bytes.substr(8, 4) == "WEBP" && length == bytes.size() - 8;
  }
  return false;
}
std::string_view MediaFormat(const std::vector<uint8_t>& data, MediaType type,
                             std::string_view expected) {
  if (type == MediaType::Video || type == MediaType::Manual) {
    return IsValidMediaData(data, type, expected) ? expected
                                                  : std::string_view{};
  }
  for (const auto format : {"png", "jpg", "webp"}) {
    if (IsValidMediaData(data, type, format)) {
      return format;
    }
  }
  return {};
}
std::string_view PrepareMedia(std::vector<uint8_t>& data, MediaType type,
                              std::string_view expected) {
  const auto format = MediaFormat(data, type, expected);
  if (format == "png") {
    data.resize(PngEncodedSize(data));
  } else if (format == "jpg") {
    data.resize(JpegEncodedSize(data));
  }
  return format;
}
std::string MediaSelector(const MediaReference& media) {
  if (!media.selector.empty()) {
    return media.selector;
  }
  return media.region.empty() ? media.api_type
                              : media.api_type + "(" + media.region + ")";
}
std::vector<const MediaReference*> MediaCandidates(const Match& match,
                                                   MediaType type,
                                                   const std::string& region) {
  std::vector<const MediaReference*> candidates;
  std::set<std::string> variants;
  for (const auto& media : match.media) {
    if (media.type == type && variants.insert(MediaSelector(media)).second) {
      candidates.push_back(&media);
    }
  }
  const auto score = [&](const MediaReference* media) {
    return (media->region == region                           ? 100
            : media->region == "wor" || media->region.empty() ? 50
                                                              : 0) +
           (media->api_type == "wheel-hd" ||
                    media->api_type == "video-normalized" ||
                    media->api_type == "mixrbv2"
                ? 1
                : 0);
  };
  std::stable_sort(
      candidates.begin(), candidates.end(),
      [&](const auto* a, const auto* b) { return score(a) > score(b); });
  return candidates;
}
bool MediaUnavailable(const std::vector<uint8_t>& data) {
  if (data.empty()) {
    return false;
  }
  std::string_view text(reinterpret_cast<const char*>(data.data()),
                        data.size());
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text == "NOMEDIA";
}
std::vector<uint8_t> Serialize(const Target& target, const Metadata& metadata) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  const auto string = [&](const char* key, const std::string& value) {
    writer.Key(key);
    writer.String(value.data(), rapidjson::SizeType(value.size()));
  };
  writer.Key("version");
  writer.Uint(1);
  writer.Key("source_game_id");
  writer.Uint64(metadata.source_game_id);
  string("content_path", Utf8Path(target.content_path));
  string("title", metadata.title);
  string("description", metadata.description);
  string("developer", metadata.developer);
  string("publisher", metadata.publisher);
  string("genre", metadata.genre);
  string("release_date", metadata.release_date);
  string("players", metadata.players);
  string("region", metadata.region);
  writer.Key("rating");
  writer.Double(metadata.rating);
  writer.EndObject();
  return {buffer.GetString(), buffer.GetString() + buffer.GetSize()};
}
std::optional<Metadata> ReadCache(const Configuration& config,
                                  const Target& target, bool local = false) {
  const auto path = CachePath(config, target, local);
  if (path.empty()) {
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return std::nullopt;
  }
  const auto length = input.tellg();
  if (length <= 0 || length > std::streamoff(kMetadataLimit)) {
    return std::nullopt;
  }
  std::string bytes(size_t(length), '\0');
  input.seekg(0);
  if (!input.read(bytes.data(), length)) {
    return std::nullopt;
  }
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseValidateEncodingFlag>(bytes.data(), bytes.size());
  if (doc.HasParseError() || !doc.IsObject()) {
    return std::nullopt;
  }
  const auto text = [&](const char* key) -> std::string {
    const auto found = doc.FindMember(key);
    return found != doc.MemberEnd() && found->value.IsString()
               ? std::string(found->value.GetString(),
                             found->value.GetStringLength())
               : "";
  };
  if (text("content_path") != Utf8Path(target.content_path)) {
    return std::nullopt;
  }
  Metadata metadata;
  const auto id = doc.FindMember("source_game_id");
  if (id == doc.MemberEnd() || !id->value.IsUint64()) {
    return std::nullopt;
  }
  metadata.source_game_id = id->value.GetUint64();
  metadata.title = text("title");
  metadata.description = text("description");
  metadata.developer = text("developer");
  metadata.publisher = text("publisher");
  metadata.genre = text("genre");
  metadata.release_date = text("release_date");
  metadata.players = text("players");
  metadata.region = text("region");
  const auto rating = doc.FindMember("rating");
  if (rating != doc.MemberEnd() && rating->value.IsNumber()) {
    metadata.rating = std::clamp(rating->value.GetDouble(), 0.0, 1.0);
  }
  return metadata;
}

std::filesystem::path GameListPath(const Configuration& config) {
  const auto path = config.media_root.parent_path() / "gamelists" / "xbox360" /
                    "gamelist.xml";
  auto name = Utf8Path(config.media_root.filename());
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  std::error_code error;
  return name == "downloaded_media" || std::filesystem::exists(path, error)
             ? path
             : std::filesystem::path{};
}
std::string GameListEntryPath(const Target& target) {
  if (target.content_root.empty()) {
    return {};
  }
  auto path = target.directory_as_file ? target.content_path.parent_path()
                                       : target.content_path;
  const auto relative = target.relative_game_path.empty()
                            ? path.lexically_relative(target.content_root)
                            : target.relative_game_path;
  if (relative.empty() || relative.is_absolute()) {
    return {};
  }
  for (const auto& part : relative) {
    if (part == "..") {
      return {};
    }
  }
  return "./" + Utf8Path(relative);
}
std::string ComparableGameListPath(std::string_view text) {
  auto path = Utf8Path(std::filesystem::u8path(text).lexically_normal());
  std::transform(path.begin(), path.end(), path.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return path;
}
std::filesystem::path FindDisplayMedia(const Configuration& config,
                                       const Target& target, MediaType type) {
  auto path = FindExistingMedia(config, target, type);
  if (path.empty() && !target.legacy_relative_game_path.empty()) {
    auto legacy = target;
    legacy.relative_game_path = target.legacy_relative_game_path;
    legacy.directory_as_file = true;
    path = FindExistingMedia(config, legacy, type);
  }
  return path;
}
bool UpdateGameList(const Configuration& config, const Target& target,
                    const Metadata& metadata, const Metadata& previous) {
  const auto path = GameListPath(config);
  const auto entry_path = GameListEntryPath(target);
  if (path.empty() || entry_path.empty()) {
    return true;
  }
  pugi::xml_document document;
  std::error_code error;
  if (std::filesystem::exists(path, error)) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() > std::streamoff(kMetadataLimit)) {
      return false;
    }
    file.seekg(0);
    if (!document.load(file, pugi::parse_default | pugi::parse_comments)) {
      return false;
    }
  }
  auto list = document.child("gameList");
  if (!list) {
    if (document.document_element()) {
      return false;
    }
    list = document.append_child("gameList");
  }
  pugi::xml_node game;
  const auto wanted = ComparableGameListPath(entry_path);
  const auto executable = ComparableGameListPath(
      "./" +
      Utf8Path(target.content_path.lexically_relative(target.content_root)));
  for (auto node : list.children("game")) {
    const auto candidate = ComparableGameListPath(node.child_value("path"));
    if (candidate == wanted || candidate == executable) {
      game = node;
      break;
    }
  }
  if (!game && !target.legacy_relative_game_path.empty()) {
    auto legacy = target;
    legacy.relative_game_path = target.legacy_relative_game_path;
    legacy.directory_as_file = true;
    const auto legacy_path = ComparableGameListPath(GameListEntryPath(legacy));
    for (auto node : list.children("game")) {
      if (ComparableGameListPath(node.child_value("path")) == legacy_path) {
        game = node;
        break;
      }
    }
  }
  const bool created = !game;
  if (created) {
    game = list.append_child("game");
    game.append_child("path").text().set(entry_path.c_str());
  }
  const auto field = [&](const char* name, const std::string& value,
                         const std::string& before) {
    // Preserve existing gamelist fields when an empty value was not edited.
    if ((!created && value == before) || (value.empty() && before.empty())) {
      return;
    }
    auto node = game.child(name);
    if (!node) {
      node = game.append_child(name);
    }
    node.text().set(value.c_str());
  };
  field("name", metadata.title, previous.title);
  field("desc", metadata.description, previous.description);
  field("developer", metadata.developer, previous.developer);
  field("publisher", metadata.publisher, previous.publisher);
  field("genre", metadata.genre, previous.genre);
  field("players", metadata.players, previous.players);
  const auto date = [](std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
    return value.size() == 8 ? value + "T000000" : value;
  };
  field("releasedate", date(metadata.release_date),
        date(previous.release_date));
  if (metadata.rating != previous.rating || (created && metadata.rating > 0)) {
    field("rating", std::to_string(metadata.rating),
          std::to_string(previous.rating));
  }
  std::ostringstream output;
  document.save(output, "  ", pugi::format_default, pugi::encoding_utf8);
  const auto bytes = output.str();
  const std::atomic<bool> cancelled{false};
  return WriteAtomic(config.media_root.parent_path(), path,
                     {bytes.begin(), bytes.end()}, true, cancelled);
}
}  // namespace

struct Service::Impl {
  explicit Impl(Transport value) : transport(std::move(value)) {}
  mutable std::mutex mutex, lifecycle;
  std::condition_variable wake;
  Configuration config;
  Progress progress;
  SearchSnapshot search;
  std::thread worker;
  std::atomic<bool> cancelled{false};
  bool running = false;
  uint64_t revision = 0;
  std::vector<uint64_t> updates;
  Transport transport;
  Clock::time_point last_request{};
  std::chrono::milliseconds request_interval{1000};

  bool Wait(std::chrono::milliseconds duration) {
    std::unique_lock lock(mutex);
    return !wake.wait_for(lock, duration, [&] { return cancelled.load(); });
  }
  void Status(State state, std::string message) {
    Log(message);
    std::lock_guard lock(mutex);
    progress.state = state;
    progress.status = std::move(message);
  }
  HttpResponse Fetch(const std::string& url, size_t limit) {
    for (unsigned attempt = 0; attempt < 3 && !cancelled; ++attempt) {
      const auto remaining =
          request_interval -
          std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                last_request);
      if (remaining.count() > 0 && !Wait(remaining)) {
        break;
      }
      last_request = Clock::now();
      auto response = transport(url, limit, cancelled);
      if (response.cancelled || cancelled) {
        return {0, {}, true};
      }
      if (response.body.size() > limit) {
        return {};
      }
      if (response.status == 423 || response.status == 429 ||
          response.status == 430 || response.status == 431) {
        Status(State::QuotaPaused,
               "ScreenScraper quota reached; try again later");
        return response;
      }
      if (response.status == 403 || response.status == 426) {
        Status(State::Error,
               "ScreenScraper rejected the developer or user credentials");
        return response;
      }
      if ((response.status == 0 || response.status == 401 ||
           response.status >= 500) &&
          attempt < 2) {
        if (!Wait(std::chrono::seconds(
                std::max(1u << attempt, response.retry_after_seconds)))) {
          break;
        }
        continue;
      }
      if (response.status == 401) {
        Status(State::Error,
               "ScreenScraper is busy or requires a signed-in account");
      }
      return response;
    }
    return {0, {}, cancelled};
  }
  bool Aborted() const {
    std::lock_guard lock(mutex);
    return cancelled || progress.state == State::QuotaPaused ||
           progress.state == State::Error;
  }
  bool ValidateUser(const rapidjson::Value& api_response, bool required) {
    if (api_response.HasMember("ssuser") && api_response["ssuser"].IsObject()) {
      const auto& user = api_response["ssuser"];
      const auto count = [&](const char* key) -> uint64_t {
        if (!user.HasMember(key)) {
          return 0;
        }
        const auto& value = user[key];
        if (value.IsUint64()) {
          return value.GetUint64();
        }
        if (value.IsString()) {
          uint64_t parsed = 0;
          const auto result = std::from_chars(
              value.GetString(), value.GetString() + value.GetStringLength(),
              parsed);
          if (result.ec == std::errc{} &&
              result.ptr == value.GetString() + value.GetStringLength()) {
            return parsed;
          }
        }
        return 0;
      };
      // niveau=0 indicates invalid credentials even with HTTP 200.
      if (!count("niveau") || !user.HasMember("id") || !user["id"].IsString() ||
          !user["id"].GetStringLength()) {
        Status(State::Error,
               "ScreenScraper sign-in failed; check the saved username and "
               "password");
        return false;
      }
      const auto documented_rate = count("maxrequestspermin");
      if (const auto per_minute = std::min<uint64_t>(
              documented_rate ? documented_rate : count("maxrequestsperdmin"),
              60000)) {
        request_interval = std::chrono::milliseconds(std::clamp<uint64_t>(
            (60000 + per_minute - 1) / per_minute, 1000, 60000));
      }
      const auto daily = count("maxrequestsperday");
      const auto negative = count("maxrequestskoperday");
      if ((daily && count("requeststoday") >= daily) ||
          (negative && count("requestskotoday") >= negative)) {
        Status(State::QuotaPaused,
               "ScreenScraper daily quota reached; cached media remains "
               "available");
      }
      return !Aborted();
    }
    if (required || api_response.HasMember("ssuser")) {
      Status(State::Error,
             "ScreenScraper did not confirm the signed-in account; try again "
             "later");
      return false;
    }
    return true;
  }
  bool Authenticate(const Configuration& settings) {
    Status(State::Working, "Verifying ScreenScraper account");
    const auto response =
        Fetch(ApiUrl(settings, "ssuserInfos.php"), kMetadataLimit);
    if (response.status != 200) {
      if (!Aborted()) {
        Status(State::Error,
               "ScreenScraper could not verify the saved account; try again "
               "later");
      }
      return false;
    }
    rapidjson::Document document;
    document.Parse<rapidjson::kParseValidateEncodingFlag>(
        reinterpret_cast<const char*>(response.body.data()),
        response.body.size());
    if (document.HasParseError() || !document.IsObject() ||
        !document.HasMember("response") || !document["response"].IsObject()) {
      Status(State::Error,
             "ScreenScraper returned an invalid account response");
      return false;
    }
    const bool valid = ValidateUser(document["response"], true);
    if (valid) {
      Log("account authenticated");
    }
    return valid;
  }
  std::vector<Match> Lookup(const Configuration& settings, const Target& target,
                            LookupKind kind) {
    const bool matched = kind == LookupKind::Id;
    const bool searching = kind == LookupKind::Search;
    auto url =
        ApiUrl(settings, searching ? "jeuRecherche.php" : "jeuInfos.php");
    url += matched ? "&gameid=" + std::to_string(target.matched_game_id)
                   : std::string(searching ? "&recherche="
                                           : "&romtype=rom&romnom=") +
                         Encode(SearchName(target));
    Status(State::Working, "Looking up " + target.name);
    Log(std::string(matched     ? "game details"
                    : searching ? "game search"
                                : "game identification") +
        " for target " + std::to_string(target.id));
    const auto response = Fetch(url, kMetadataLimit);
    if (response.status != 200) {
      if (!Aborted() && response.status != 404) {
        Status(State::Error, "ScreenScraper lookup failed; try again later");
      }
      return {};
    }
    rapidjson::Document document;
    document.Parse<rapidjson::kParseValidateEncodingFlag>(
        reinterpret_cast<const char*>(response.body.data()),
        response.body.size());
    if (document.HasParseError() || !document.IsObject() ||
        !document.HasMember("response") || !document["response"].IsObject()) {
      Status(State::Error, "ScreenScraper returned invalid metadata");
      return {};
    }
    // Game replies may omit ssuser; the per-job account request verified login.
    if (!ValidateUser(document["response"], false)) {
      return {};
    }
    auto matches =
        ParseResponse({reinterpret_cast<const char*>(response.body.data()),
                       response.body.size()},
                      settings.language, settings.region);
    if (matched) {
      std::erase_if(matches, [&](const Match& m) {
        return m.metadata.source_game_id != target.matched_game_id;
      });
    }
    Log("lookup returned " + std::to_string(matches.size()) +
        " matches for target " + std::to_string(target.id));
    return matches;
  }
  void Updated(uint64_t id) {
    std::lock_guard lock(mutex);
    ++revision;
    if (std::find(updates.begin(), updates.end(), id) == updates.end()) {
      updates.push_back(id);
    }
  }
  bool Download(const Configuration& settings, const Target& target,
                const Match& match) {
    Status(State::Working, "Downloading media for " + target.name);
    bool changed = false, success = true;
    if (settings.download_metadata &&
        (settings.overwrite || !ReadCache(settings, target))) {
      std::error_code error;
      if (WriteAtomic(settings.cache_root, CachePath(settings, target),
                      Serialize(target, match.metadata), true, cancelled,
                      &error)) {
        changed = true;
        Log("metadata saved for target " + std::to_string(target.id));
      } else {
        success = false;
        Log("metadata save failed for target " + std::to_string(target.id) +
            ", error " + std::to_string(error.value()));
      }
    }
    for (unsigned i = 0; i < unsigned(MediaType::Count) && !Aborted(); ++i) {
      const auto type = MediaType(i);
      if (!(settings.media_mask & MediaBit(type))) {
        continue;
      }
      if (!settings.overwrite &&
          !FindExistingMedia(settings, target, type).empty()) {
        continue;
      }
      const auto candidates = MediaCandidates(match, type, settings.region);
      const auto description = "target " + std::to_string(target.id) + "/" +
                               std::string(MediaDirectory(type));
      bool saved = false, failed = false;
      for (const auto* selected : candidates) {
        if (Aborted()) {
          break;
        }
        const bool image =
            type != MediaType::Video && type != MediaType::Manual;
        const auto media = MediaSelector(*selected);
        auto url =
            ApiUrl(settings, type == MediaType::Video    ? "mediaVideoJeu.php"
                             : type == MediaType::Manual ? "mediaManuelJeu.php"
                                                         : "mediaJeu.php");
        url += "&jeuid=" + std::to_string(match.metadata.source_game_id) +
               "&media=" + Encode(media);
        if (image) {
          url += "&outputformat=png";
        }
        auto response = Fetch(
            url, type == MediaType::Video ? kMediaLimit : 32 * 1024 * 1024);
        if (Aborted()) {
          break;
        }
        if (response.status == 404) {
          continue;
        }
        if (response.status != 200) {
          Log("download failed for " + description + ", HTTP " +
              std::to_string(response.status));
          failed = true;
          continue;
        }
        if (MediaUnavailable(response.body)) {
          continue;
        }
        const auto format = PrepareMedia(response.body, type, selected->format);
        if (format.empty()) {
          std::ostringstream signature;
          signature << std::hex << std::setfill('0');
          if (response.body.size() >= 4) {
            for (const auto offset : {size_t{0}, response.body.size() - 4}) {
              for (size_t i = offset; i < offset + 4; ++i) {
                signature << std::setw(2) << unsigned(response.body[i]);
              }
              signature << ' ';
            }
          }
          Log("invalid media response for " + description + ", " +
              std::to_string(response.body.size()) + " bytes, expected " +
              selected->format + ", signature " + signature.str());
          failed = true;
          continue;
        }
        const auto path =
            MakeMediaPath(settings, target, type, "." + std::string(format));
        std::error_code write_error;
        if (!WriteAtomic(settings.media_root, path, response.body,
                         settings.overwrite, cancelled, &write_error)) {
          Log("media save failed for " + description + ", error " +
              std::to_string(write_error.value()));
          failed = true;
          break;
        }
        Log("saved " + Utf8Path(path) + ", " +
            std::to_string(response.body.size()) + " bytes");
        changed = true;
        if (settings.overwrite) {
          for (const auto extension : MediaExtensions(type)) {
            const auto previous =
                MakeMediaPath(settings, target, type, extension);
            if (previous != path &&
                IsPathWithinStorage(settings.media_root, previous)) {
              std::error_code error;
              if (std::filesystem::is_regular_file(previous, error)) {
                std::filesystem::remove(previous, error);
                success &= !error;
              }
            }
          }
        }
        std::lock_guard lock(mutex);
        ++progress.downloaded_files;
        saved = true;
        break;
      }
      if (!saved && !Aborted()) {
        if (failed) {
          success = false;
        } else {
          Log("media unavailable for " + description);
          std::lock_guard lock(mutex);
          ++progress.unavailable_files;
        }
      }
    }
    if (changed) {
      Updated(target.id);
    }
    return success;
  }
  void Run(Configuration settings, std::vector<Target> targets,
           bool search_only) {
    try {
      bool authenticated = false;
      for (const auto& target : targets) {
        if (Aborted()) {
          break;
        }
        bool cached = !search_only && !settings.overwrite &&
                      (!settings.download_metadata ||
                       ReadCache(settings, target).has_value());
        for (unsigned i = 0; cached && i < unsigned(MediaType::Count); ++i) {
          if ((settings.media_mask & MediaBit(MediaType(i))) &&
              FindExistingMedia(settings, target, MediaType(i)).empty()) {
            cached = false;
          }
        }
        if (cached) {
          Log("using saved assets for target " + std::to_string(target.id));
          std::lock_guard lock(mutex);
          ++progress.completed;
          continue;
        }
        if (!authenticated) {
          authenticated = Authenticate(settings);
          if (!authenticated) {
            break;
          }
        }
        auto kind = target.matched_game_id ? LookupKind::Id
                    : search_only         ? LookupKind::Search
                                          : LookupKind::Name;
        auto matches = Lookup(settings, target, kind);
        if (search_only) {
          std::lock_guard lock(mutex);
          search.matches = std::move(matches);
          search.state = cancelled ? State::Cancelled
                         : progress.state == State::Error ||
                                 progress.state == State::QuotaPaused
                             ? progress.state
                         : search.matches.empty() ? State::NoMatches
                                                  : State::Finished;
          search.status =
              search.state == State::Finished    ? "Select a matching game"
              : search.state == State::NoMatches ? "No Xbox 360 matches found"
                                                 : progress.status;
          progress.state = search.state;
          progress.status = search.status;
          progress.completed = 1;
          break;
        }
        if (Aborted()) {
          break;
        }
        if (matches.empty() && kind == LookupKind::Name) {
          kind = LookupKind::Search;
          matches = Lookup(settings, target, kind);
        }
        if (kind == LookupKind::Search) {
          const auto key = SearchKey(SearchName(target));
          std::erase_if(matches, [&](const Match& m) {
            return SearchKey(m.metadata.title) != key &&
                   std::none_of(m.names.begin(), m.names.end(),
                                [&](const auto& name) {
                                  return SearchKey(name) == key;
                                });
          });
        }
        bool success = false;
        if (matches.size() == 1) {
          if (kind == LookupKind::Search) {
            auto matched_target = target;
            matched_target.matched_game_id =
                matches.front().metadata.source_game_id;
            matches = Lookup(settings, matched_target, LookupKind::Id);
          }
          if (matches.size() == 1 && !Aborted()) {
            success = Download(settings, target, matches.front());
          }
        }
        Log("target " + std::to_string(target.id) + ": " +
            std::to_string(matches.size()) + " matches, " +
            (success ? "complete" : "failed"));
        if (Aborted()) {
          break;
        }
        {
          std::lock_guard lock(mutex);
          ++progress.completed;
          if (!success) {
            ++progress.failed;
            if (matches.size() != 1) {
              ++progress.unmatched_games;
              Log("game needs a match: " + target.name);
            }
          }
        }
      }
    } catch (...) {
      Status(State::Error, "ScreenScraper operation failed");
    }
    std::lock_guard lock(mutex);
    if (cancelled) {
      progress.state = State::Cancelled;
      progress.status = "Download cancelled";
      search.state = State::Cancelled;
    } else if (progress.state == State::Working) {
      progress.state = State::Finished;
      progress.status = "ScreenScraper download complete";
      if (progress.unmatched_games) {
        progress.status += "; " + std::to_string(progress.unmatched_games) +
                           (progress.unmatched_games == 1
                                ? " game needs a match in Edit metadata"
                                : " games need a match in Edit metadata");
      }
      if (progress.failed > progress.unmatched_games) {
        progress.status += "; some media could not be downloaded";
      }
      if (progress.unavailable_files) {
        progress.status += "; some media is unavailable";
      }
    }
    if (search_only) {
      search.state = progress.state;
      search.status = progress.status;
    }
    running = false;
    Log(progress.status + "; " + std::to_string(progress.completed) + "/" +
        std::to_string(progress.total) + " games, " +
        std::to_string(progress.downloaded_files) + " files saved, " +
        std::to_string(progress.failed) + " games failed, " +
        std::to_string(progress.unavailable_files) + " media unavailable");
    FlushLog();
  }
  bool Launch(std::vector<Target> targets, bool search_only) {
    std::lock_guard life(lifecycle);
    Configuration settings;
    {
      std::lock_guard lock(mutex);
      if (running) {
        Log("request ignored: a job is already running");
        return false;
      }
      if (targets.empty() || targets.size() > 1024) {
        progress = {
            State::Error, 0, 0, 0, 0, "Select between 1 and 1024 games"};
        Log(progress.status);
        return false;
      }
      if (config.account.user.empty() || config.account.password.empty()) {
        progress = {State::Error,
                    0,
                    0,
                    0,
                    0,
                    "Sign in to ScreenScraper before searching or downloading"};
        Log(progress.status);
        return false;
      }
      if (config.developer.id.empty() || config.developer.password.empty()) {
        progress = {State::Error,
                    0,
                    0,
                    0,
                    0,
                    "ScreenScraper developer credentials are not configured"};
        Log(progress.status);
        return false;
      }
      std::set<uint64_t> ids;
      std::set<std::string> paths;
      for (const auto& target : targets) {
        const auto path = MakeMediaPath(config, target, MediaType::Cover);
        auto path_key = Utf8Path(path);
#if XE_PLATFORM_WIN32
        std::transform(path_key.begin(), path_key.end(), path_key.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
#endif
        if (!target.id || target.name.empty() || target.name.size() > 256 ||
            path.empty() || CachePath(config, target).empty() ||
            !ids.insert(target.id).second || !paths.insert(path_key).second) {
          progress = {
              State::Error, 0, 0, 0, 0, "Invalid or duplicate game/media path"};
          Log(progress.status + ": target " + std::to_string(target.id));
          return false;
        }
      }
      settings = config;
      cancelled = false;
      running = true;
      progress = {State::Working,
                  0,
                  targets.size(),
                  0,
                  0,
                  search_only ? "Searching ScreenScraper"
                              : "Downloading ScreenScraper media"};
      Log(progress.status + "; queued " + std::to_string(targets.size()) +
          " games, media mask " + std::to_string(settings.media_mask) +
          ", metadata " +
          (settings.download_metadata ? "enabled" : "disabled"));
      FlushLog();
      if (search_only) {
        ++search.generation;
        search.state = State::Working;
        search.status = progress.status;
        search.matches.clear();
      }
    }
    if (worker.joinable()) {
      worker.join();
    }
    try {
      worker =
          std::thread([this, settings = std::move(settings),
                       targets = std::move(targets), search_only]() mutable {
            Run(std::move(settings), std::move(targets), search_only);
          });
    } catch (...) {
      std::lock_guard lock(mutex);
      running = false;
      progress.state = State::Error;
      progress.status = "Unable to start ScreenScraper worker";
      return false;
    }
    return true;
  }
  void Preview(Configuration settings, uint64_t generation, Match match,
               MediaType type) {
    std::filesystem::path saved;
    try {
      if (match.media.empty()) {
        Target target;
        target.matched_game_id = match.metadata.source_game_id;
        auto details = Lookup(settings, target, LookupKind::Id);
        if (details.size() == 1) {
          match.media = std::move(details.front().media);
        }
      }
      for (const auto* media : MediaCandidates(match, type, settings.region)) {
        if (Aborted()) {
          break;
        }
        const auto root = settings.cache_root / "screenscraper" / "previews";
        const auto stem =
            root / std::to_string(match.metadata.source_game_id) /
            (std::string(MediaDirectory(type)) + "-" + settings.region);
        for (const auto extension : {".png", ".jpg"}) {
          auto path = stem;
          path += extension;
          std::error_code error;
          if (std::filesystem::is_regular_file(path, error) &&
              std::filesystem::file_size(path, error) > 0 && !error) {
            saved = path;
            break;
          }
        }
        if (!saved.empty()) {
          break;
        }
        auto response = Fetch(
            ApiUrl(settings, "mediaJeu.php") +
                "&jeuid=" + std::to_string(match.metadata.source_game_id) +
                "&media=" + Encode(MediaSelector(*media)) +
                "&maxheight=480&outputformat=png",
            8 * 1024 * 1024);
        if (Aborted()) {
          break;
        }
        if (response.status != 200) {
          continue;
        }
        const auto format = PrepareMedia(response.body, type, media->format);
        if (format != "png" && format != "jpg") {
          continue;
        }
        auto path = stem;
        path += "." + std::string(format);
        if (WriteAtomic(root, path, response.body, true, cancelled)) {
          saved = path;
        }
        if (!saved.empty()) {
          break;
        }
      }
    } catch (...) {
      Log("artwork preview failed");
    }
    std::lock_guard lock(mutex);
    if (!cancelled && search.generation == generation) {
      for (auto& result : search.matches) {
        if (result.metadata.source_game_id != match.metadata.source_game_id) {
          continue;
        }
        result.media = std::move(match.media);
        if (!saved.empty()) {
          result.previews[type] = std::move(saved);
        }
        break;
      }
    }
    if (progress.state == State::Error ||
        progress.state == State::QuotaPaused) {
      search.state = progress.state;
      search.status = progress.status;
    } else {
      progress.state = cancelled ? State::Cancelled : State::Finished;
      progress.status = search.status;
    }
    running = false;
  }
  bool LaunchPreview(uint64_t generation, uint64_t game_id, MediaType type) {
    std::lock_guard life(lifecycle);
    Configuration settings;
    Match match;
    {
      std::lock_guard lock(mutex);
      if (running || cancelled || search.generation != generation ||
          search.state != State::Finished || type >= MediaType::Count ||
          type == MediaType::Video || type == MediaType::Manual) {
        return false;
      }
      auto found = std::find_if(
          search.matches.begin(), search.matches.end(), [&](const Match& item) {
            return item.metadata.source_game_id == game_id;
          });
      if (found == search.matches.end() ||
          (found->requested_previews & MediaBit(type))) {
        return false;
      }
      found->requested_previews |= MediaBit(type);
      match = *found;
      settings = config;
      running = true;
      progress.state = State::Working;
    }
    if (worker.joinable()) {
      worker.join();
    }
    try {
      worker = std::thread([this, settings = std::move(settings), generation,
                            match = std::move(match), type]() mutable {
        Preview(std::move(settings), generation, std::move(match), type);
      });
    } catch (...) {
      std::lock_guard lock(mutex);
      running = false;
      progress.state = State::Error;
      progress.status = "Unable to start artwork preview";
    }
    return true;
  }
};

Service::Service(Transport transport)
    : impl_(std::make_unique<Impl>(std::move(transport))) {}
Service::~Service() {
  Cancel();
  std::lock_guard life(impl_->lifecycle);
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
}
bool Service::Configure(Configuration config) {
  std::lock_guard life(impl_->lifecycle);
  std::lock_guard lock(impl_->mutex);
  if (impl_->running) {
    return false;
  }
  impl_->config = std::move(config);
  ++impl_->revision;
  return true;
}
bool Service::Start(std::vector<Target> targets) {
  return impl_->Launch(std::move(targets), false);
}
bool Service::Search(Target target, uint64_t game_id) {
  target.matched_game_id = game_id;
  return impl_->Launch({std::move(target)}, true);
}
bool Service::RequestPreview(uint64_t generation, uint64_t game_id,
                             MediaType type) {
  return impl_->LaunchPreview(generation, game_id, type);
}
void Service::Cancel() {
  impl_->cancelled = true;
  impl_->wake.notify_all();
}
Progress Service::GetProgress() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->progress;
}
SearchSnapshot Service::GetSearchSnapshot() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->search;
}
uint64_t Service::GetRevision() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->revision;
}
std::vector<uint64_t> Service::ConsumeUpdates() {
  std::lock_guard lock(impl_->mutex);
  return std::exchange(impl_->updates, {});
}
std::optional<CachedGame> Service::LoadCached(const Target& target) const {
  Configuration config;
  CachedGame cached;
  {
    std::lock_guard lock(impl_->mutex);
    config = impl_->config;
    cached.revision = impl_->revision;
  }
  auto metadata = ReadCache(config, target, true);
  cached.local_override = metadata.has_value();
  if (!metadata) {
    metadata = ReadCache(config, target);
  }
  if (metadata) {
    cached.metadata = *metadata;
  }
  for (unsigned i = 0; i < unsigned(MediaType::Count); ++i) {
    auto path = FindDisplayMedia(config, target, MediaType(i));
    if (!path.empty()) {
      cached.media.emplace(MediaType(i), std::move(path));
    }
  }
  return metadata || !cached.media.empty() ? std::optional(std::move(cached))
                                           : std::nullopt;
}
bool Service::SaveMetadata(const Target& target, const Metadata& metadata,
                           const Metadata& previous, std::string* status) {
  std::lock_guard life(impl_->lifecycle);
  Configuration config;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->running) {
      if (status) {
        *status = "Wait for the current search or download to finish.";
      }
      return false;
    }
    config = impl_->config;
  }
  const std::atomic<bool> cancelled{false};
  if (target.content_path.empty() ||
      !WriteAtomic(config.cache_root, CachePath(config, target, true),
                   Serialize(target, metadata), true, cancelled)) {
    if (status) {
      *status = "Could not save metadata.";
    }
    return false;
  }
  const bool xml_saved = UpdateGameList(config, target, metadata, previous);
  {
    std::lock_guard lock(impl_->mutex);
    ++impl_->revision;
    impl_->updates.push_back(target.id);
    impl_->progress.status =
        xml_saved
            ? "Metadata saved."
            : "Metadata saved locally; ES-DE gamelist could not be updated.";
    if (status) {
      *status = impl_->progress.status;
    }
  }
  return true;
}
std::filesystem::path Service::FindMedia(const Target& target,
                                         MediaType type) const {
  Configuration config;
  {
    std::lock_guard lock(impl_->mutex);
    config = impl_->config;
  }
  return FindDisplayMedia(config, target, type);
}
}  // namespace xe::app::screenscraper
