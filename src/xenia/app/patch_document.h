// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_PATCH_DOCUMENT_H_
#define XENIA_APP_PATCH_DOCUMENT_H_

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include <Windows.h>
#if XE_PLATFORM_WINRT
#include <fileapifromapp.h>
#endif
#endif

namespace xe::app {
class PatchDocument {
 public:
  bool Parse(const std::string& text, std::string& error) {
    try {
      auto parsed = toml::parse(text);
      auto* options = parsed.get_as<toml::array>("patch");
      if (!options) {
        error = "The file has no patch options.";
        return false;
      }
      for (const auto& node : *options) {
        const auto* option = node.as_table();
        if (!option) {
          error = "Each patch option must be a table.";
          return false;
        }
        for (const auto* field : {"name", "desc", "author"}) {
          if (option->contains(field) && !option->get_as<std::string>(field)) {
            error = std::string(field) + " must be text.";
            return false;
          }
        }
        if (option->contains("is_enabled") && !option->get_as<bool>("is_enabled")) {
          error = "is_enabled must be true or false.";
          return false;
        }
        for (const auto* type : {"be8", "be16", "be32", "be64", "f32", "f64",
                                  "string", "u16string", "array"}) {
          if (!option->contains(type)) continue;
          const auto* entries = option->get_as<toml::array>(type);
          if (!entries) {
            error = std::string(type) + " must be an array of patch entries.";
            return false;
          }
          for (const auto& entry : *entries) {
            const auto* fields = entry.as_table();
            const auto* address = fields ? fields->get_as<int64_t>("address") : nullptr;
            const auto* value = fields ? fields->get("value") : nullptr;
            const bool text_value = std::string_view(type) == "string" ||
                                    std::string_view(type) == "u16string" ||
                                    std::string_view(type) == "array";
            const bool float_value = type[0] == 'f';
            if (!address || !value ||
                (text_value ? !value->is_string()
                            : float_value ? !value->is_number()
                                          : !value->is_integer())) {
              error = std::string(type) +
                      " entries need an integer address and a valid value.";
              return false;
            }
          }
        }
      }
      table_ = std::move(parsed);
      error.clear();
      return true;
    } catch (const toml::parse_error& e) {
      error = std::string(e.description());
      return false;
    }
  }
  toml::array& options() { return *table_.get_as<toml::array>("patch"); }
  std::string Text() const {
    std::ostringstream output;
    output << table_;
    return output.str();
  }
  std::string OptionText(size_t index) const {
    toml::table single;
    toml::array array;
    array.push_back(*table_.get_as<toml::array>("patch")->get(index));
    single.insert("patch", std::move(array));
    std::ostringstream output;
    output << single;
    return output.str();
  }
  bool ReplaceOption(size_t index, const std::string& text, std::string& error) {
    PatchDocument candidate;
    if (!candidate.Parse(text, error)) return false;
    if (candidate.table_.size() != 1 || candidate.options().size() != 1 ||
        index >= options().size()) {
      error = "Keep exactly one [[patch]] option in this editor.";
      return false;
    }
    *options().get_as<toml::table>(index) =
        *candidate.options().get_as<toml::table>(0);
    return true;
  }
  void RemoveOption(size_t index) {
    options().erase(options().cbegin() + index);
  }
  static bool MatchesFile(const std::filesystem::path& path,
                          const std::string& expected, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      error = "The patch file is no longer available.";
      return false;
    }
    const std::string current((std::istreambuf_iterator<char>(input)), {});
    if (input.bad() || current != expected) {
      error = "The patch file changed outside this editor. Reopen it before saving.";
      return false;
    }
    return true;
  }
  static bool Save(const std::filesystem::path& path, const std::string& text,
                   const std::string& expected, std::string& error) {
    PatchDocument validated;
    if (!validated.Parse(text, error) || !MatchesFile(path, expected, error)) {
      return false;
    }
    static std::atomic<uint64_t> sequence{0};
    auto temporary = path;
    temporary += ".editing-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(++sequence);
    bool saved = false;
#if XE_PLATFORM_WIN32
#if XE_PLATFORM_WINRT
    const auto file = CreateFile2FromAppW(temporary.c_str(), GENERIC_WRITE, 0,
                                         CREATE_NEW, nullptr);
#else
    const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
    if (file == INVALID_HANDLE_VALUE) {
      error = "Could not create a temporary patch file.";
      return false;
    }
    DWORD written = 0;
    saved = text.size() <= MAXDWORD &&
            WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr) &&
            written == text.size() && FlushFileBuffers(file);
    saved = CloseHandle(file) && saved;
    if (saved && MatchesFile(path, expected, error)) {
#if XE_PLATFORM_WINRT
      saved = ReplaceFileFromAppW(path.c_str(), temporary.c_str(), nullptr,
                                  REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr,
                                  nullptr) != 0;
      // ReplaceFile can fail after removing the original; the new text then
      // takes its place rather than being deleted with the temporary file.
      std::error_code exists_error;
      if (!saved && !std::filesystem::exists(path, exists_error) &&
          !exists_error) {
        saved = MoveFileFromAppW(temporary.c_str(), path.c_str()) != 0;
      }
#else
      saved = MoveFileExW(temporary.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#endif
    } else {
      saved = false;
    }
#else
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(text.data(), std::streamsize(text.size()));
    output.close();
    saved = !output.fail() && MatchesFile(path, expected, error);
    if (saved) {
      std::error_code ec;
      std::filesystem::rename(temporary, path, ec);
      saved = !ec;
    }
#endif
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    if (saved) error.clear();
    else if (error.empty()) error = "Could not save the patch file.";
    return saved;
  }
 private:
  toml::table table_;
};
}  // namespace xe::app
#endif
