// Copyright 2026 Xenia Canary. Released under the BSD license.
#include "xenia/app/screenscraper_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <system_error>

#include "third_party/rapidjson/include/rapidjson/document.h"

namespace xe::app::screenscraper {
namespace {
using Json = rapidjson::Value;
std::string MediaSelectorFromUrl(std::string_view url, std::string_view type) {
  const auto query = url.find('?');
  if (query == url.npos) {
    return {};
  }
  url.remove_prefix(query + 1);
  while (!url.empty()) {
    const auto end = url.find('&');
    const auto parameter = url.substr(0, end);
    if (parameter.starts_with("media=")) {
      const auto encoded = parameter.substr(6);
      std::string value;
      for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
          unsigned byte = 0;
          const auto result = std::from_chars(encoded.data() + i + 1,
                                              encoded.data() + i + 3, byte, 16);
          if (result.ec != std::errc{} ||
              result.ptr != encoded.data() + i + 3) {
            return {};
          }
          value += char(byte);
          i += 2;
        } else {
          value += encoded[i];
        }
      }
      if (value.size() > 128 || !value.starts_with(type) ||
          (value.size() > type.size() && value[type.size()] != '(' &&
           value[type.size()] != '[') ||
          value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQR"
                                  "STUVWXYZ0123456789-_.()[]") != value.npos) {
        return {};
      }
      return value;
    }
    if (end == url.npos) {
      break;
    }
    url.remove_prefix(end + 1);
  }
  return {};
}
const Json& Member(const Json& value, const char* key) {
  static const Json empty;
  if (value.IsObject()) {
    const auto found = value.FindMember(key);
    if (found != value.MemberEnd()) {
      return found->value;
    }
  }
  return empty;
}
std::string Text(const Json& value) {
  if (value.IsString()) {
    return NormalizeText({value.GetString(), value.GetStringLength()});
  }
  if (value.IsUint64()) {
    return std::to_string(value.GetUint64());
  }
  if (value.IsNumber()) {
    return std::to_string(value.GetDouble());
  }
  if (value.IsObject()) {
    return Text(Member(value, "text"));
  }
  return {};
}
uint64_t Number(const Json& value) {
  const auto text = Text(value);
  uint64_t number = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), number);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size()
             ? number
             : 0;
}
std::string PreferredText(const Json& value, const char* key,
                          std::string_view preference,
                          std::string_view fallback) {
  if (!value.IsArray()) {
    return Text(value);
  }
  std::string first, backup;
  for (const auto& item : value.GetArray()) {
    auto text = Text(item);
    if (text.empty()) {
      continue;
    }
    if (first.empty()) {
      first = text;
    }
    const auto tag = Text(Member(item, key));
    if (tag == preference) {
      return text;
    }
    if (backup.empty() && (tag == fallback || tag == "wor")) {
      backup = text;
    }
  }
  return backup.empty() ? first : backup;
}
void AppendUtf8(std::string& output, uint32_t code) {
  if (!code || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
    return;
  }
  if (code < 0x80) {
    output.push_back(char(code));
  } else if (code < 0x800) {
    output.push_back(char(0xC0 | (code >> 6)));
    output.push_back(char(0x80 | (code & 63)));
  } else if (code < 0x10000) {
    output.push_back(char(0xE0 | (code >> 12)));
    output.push_back(char(0x80 | ((code >> 6) & 63)));
    output.push_back(char(0x80 | (code & 63)));
  } else {
    output.push_back(char(0xF0 | (code >> 18)));
    output.push_back(char(0x80 | ((code >> 12) & 63)));
    output.push_back(char(0x80 | ((code >> 6) & 63)));
    output.push_back(char(0x80 | (code & 63)));
  }
}
std::optional<MediaType> ApiMedia(std::string_view type) {
  constexpr std::pair<std::string_view, MediaType> types[] = {
      {"box-2D", MediaType::Cover},
      {"box-2D-back", MediaType::BackCover},
      {"box-3D", MediaType::Box3D},
      {"support-2D", MediaType::PhysicalMedia},
      {"wheel-hd", MediaType::Marquee},
      {"wheel", MediaType::Marquee},
      {"ss", MediaType::Screenshot},
      {"sstitle", MediaType::TitleScreen},
      {"fanart", MediaType::FanArt},
      {"video-normalized", MediaType::Video},
      {"video", MediaType::Video},
      {"manuel", MediaType::Manual},
      {"mixrbv2", MediaType::MixImage},
      {"mixrbv1", MediaType::MixImage}};
  for (auto [name, value] : types) {
    if (name == type) {
      return value;
    }
  }
  return std::nullopt;
}
bool ValidExtension(MediaType type, std::string_view extension) {
  const auto extensions = MediaExtensions(type);
  return std::find(extensions.begin(), extensions.end(), extension) !=
         extensions.end();
}
std::filesystem::path RelativeStem(const Target& target) {
  auto relative = target.relative_game_path;
  if (relative.empty()) {
    if (target.content_root.empty()) {
      relative = target.content_path.filename();
    } else {
      relative = target.content_path.lexically_relative(target.content_root);
    }
  }
  if (relative.empty() || relative.is_absolute() || relative.has_root_name()) {
    return {};
  }
  for (const auto& part : relative) {
    const auto name = part.u8string();
    if (name.empty() || name == u8".." || name == u8"." ||
        name.find_first_of(u8":<>\"|?*") != std::u8string::npos ||
        name.back() == u8'.' || name.back() == u8' ') {
      return {};
    }
  }
  if (!target.directory_as_file) {
    relative.replace_extension();
  }
  return relative;
}
}  // namespace

std::string NormalizeText(std::string_view value) {
  std::string output;
  output.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '&') {
      output.push_back(value[i]);
      continue;
    }
    const auto end = value.find(';', i + 1);
    if (end == value.npos || end - i > 12) {
      output.push_back('&');
      continue;
    }
    const auto entity = value.substr(i + 1, end - i - 1);
    uint32_t code = 0;
    if (entity == "amp") {
      code = '&';
    } else if (entity == "quot") {
      code = '"';
    } else if (entity == "apos") {
      code = '\'';
    } else if (entity == "lt") {
      code = '<';
    } else if (entity == "gt") {
      code = '>';
    } else if (entity == "nbsp") {
      code = ' ';
    } else if (!entity.empty() && entity[0] == '#') {
      auto digits = entity.substr(1);
      int base = 10;
      if (!digits.empty() && (digits[0] == 'x' || digits[0] == 'X')) {
        digits.remove_prefix(1);
        base = 16;
      }
      const auto parsed = std::from_chars(
          digits.data(), digits.data() + digits.size(), code, base);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != digits.data() + digits.size()) {
        code = 0;
      }
    }
    if (!code || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
      output.push_back('&');
      continue;
    }
    AppendUtf8(output, code);
    i = end;
  }
  return output;
}

std::string_view MediaDirectory(MediaType type) {
  constexpr std::array<std::string_view, size_t(MediaType::Count)> paths = {
      "covers",   "backcovers",  "3dboxes",      "physicalmedia",
      "marquees", "screenshots", "titlescreens", "fanart",
      "videos",   "manuals",     "miximages"};
  return size_t(type) < paths.size() ? paths[size_t(type)] : std::string_view{};
}
std::span<const std::string_view> MediaExtensions(MediaType type) {
  static constexpr std::string_view images[] = {".png", ".jpg", ".webp",
                                                ".jpeg"};
  static constexpr std::string_view videos[] = {".mp4", ".mkv", ".avi",
                                                ".wmv", ".mov", ".webm"};
  static constexpr std::string_view manuals[] = {".pdf"};
  if (type == MediaType::Video) {
    return videos;
  }
  if (type == MediaType::Manual) {
    return manuals;
  }
  return size_t(type) < size_t(MediaType::Count)
             ? std::span<const std::string_view>(images)
             : std::span<const std::string_view>{};
}
std::filesystem::path MakeMediaPath(const Configuration& config,
                                    const Target& target, MediaType type,
                                    std::string_view extension) {
  auto relative = RelativeStem(target);
  if (relative.empty() || config.media_root.empty() ||
      !config.media_root.is_absolute() || MediaDirectory(type).empty() ||
      !ValidExtension(type, extension)) {
    return {};
  }
  relative += std::string(extension);
  return config.media_root / "xbox360" / MediaDirectory(type) / relative;
}
std::filesystem::path FindExistingMedia(const Configuration& config,
                                        const Target& target, MediaType type) {
  for (const auto extension : MediaExtensions(type)) {
    const auto path = MakeMediaPath(config, target, type, extension);
    std::error_code error;
    if (!path.empty() && std::filesystem::is_regular_file(path, error) &&
        std::filesystem::file_size(path, error) > 0 && !error) {
      return path;
    }
  }
  return {};
}

std::vector<Match> ParseResponse(std::string_view json,
                                 std::string_view language,
                                 std::string_view region) {
  std::vector<Match> result;
  if (json.size() > 8 * 1024 * 1024) {
    return result;
  }
  rapidjson::Document document;
  document.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(),
                                                        json.size());
  if (document.HasParseError() || !document.IsObject()) {
    return result;
  }
  const auto& response = Member(document, "response");
  const auto add = [&](const Json& game) {
    if (!game.IsObject() || result.size() >= 30) {
      return;
    }
    const auto system = Number(Member(game, "systeme"));
    const auto system_id = Number(Member(Member(game, "systeme"), "id"));
    if ((system && system != 33) || (system_id && system_id != 33)) {
      return;
    }
    Match match;
    auto& m = match.metadata;
    m.source_game_id = Number(Member(game, "id"));
    if (!m.source_game_id) {
      return;
    }
    m.title = PreferredText(Member(game, "noms"), "region", region, "wor");
    if (m.title.empty()) {
      return;
    }
    const auto& names = Member(game, "noms");
    if (names.IsArray()) {
      for (const auto& name : names.GetArray()) {
        auto text = Text(name);
        if (!text.empty() && match.names.size() < 64 &&
            std::find(match.names.begin(), match.names.end(), text) ==
                match.names.end()) {
          match.names.push_back(std::move(text));
        }
      }
    }
    m.region = std::string(region);
    m.description =
        PreferredText(Member(game, "synopsis"), "langue", language, "en");
    m.developer = Text(Member(game, "developpeur"));
    m.publisher = Text(Member(game, "editeur"));
    m.players = Text(Member(game, "joueurs"));
    m.release_date =
        PreferredText(Member(game, "dates"), "region", region, "wor");
    const auto& genres = Member(game, "genres");
    if (genres.IsArray()) {
      for (const auto& genre : genres.GetArray()) {
        const auto name =
            PreferredText(Member(genre, "noms"), "langue", language, "en");
        if (!name.empty()) {
          if (!m.genre.empty()) {
            m.genre += ", ";
          }
          m.genre += name;
        }
      }
    }
    const auto rating = Text(Member(game, "note"));
    char* end = nullptr;
    const auto value = std::strtod(rating.c_str(), &end);
    if (!rating.empty() && end == rating.c_str() + rating.size() &&
        std::isfinite(value)) {
      m.rating = std::clamp(value / 20.0, 0.0, 1.0);
    }
    const auto& media = Member(game, "medias");
    if (media.IsArray()) {
      for (const auto& item : media.GetArray()) {
        if (match.media.size() >= 128) {
          break;
        }
        const auto api_type = Text(Member(item, "type"));
        const auto type = ApiMedia(api_type);
        if (!type) {
          continue;
        }
        auto format = Text(Member(item, "format"));
        std::transform(format.begin(), format.end(), format.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (!ValidExtension(*type, "." + format)) {
          continue;
        }
        const auto media_region = Text(Member(item, "region"));
        if (media_region.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) {
          continue;
        }
        match.media.push_back(
            {*type, api_type, media_region, format,
             MediaSelectorFromUrl(Text(Member(item, "url")), api_type)});
        match.available_media |= MediaBit(*type);
      }
    }
    result.push_back(std::move(match));
  };
  const auto& game = Member(response, "jeu");
  if (game.IsObject()) {
    add(game);
  }
  const auto& games = Member(response, "jeux");
  if (games.IsArray()) {
    for (const auto& item : games.GetArray()) {
      add(item);
    }
  }
  return result;
}
}  // namespace xe::app::screenscraper
