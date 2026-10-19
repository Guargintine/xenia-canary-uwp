// Copyright 2026 Xenia Canary. Released under the BSD license.
#include "xenia/app/game_patch_dialog.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_WINRT
#include <fstream>
#include <optional>
#include <vector>
#include "xenia-canary-uwp/WinRTKeyboard.h"
#include "xenia/app/guide_dialog.h"
#include "xenia/app/patch_document.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/utf8.h"

namespace xe::app {
struct GamePatchDialog::State {
  bool open = false, pending = false, pending_reload = false;
  bool raw = false, valid = false;
  bool dirty = false, confirm_delete = false, confirm_discard = false;
  bool option_dirty_start = false, confirm_option_cancel = false;
  bool prompt_shown = false;
  size_t selected = 0;
  std::optional<size_t> editing_option, deleting_option, pending_selection;
  std::filesystem::path directory;
  std::vector<std::filesystem::path> files;
  std::vector<char> text;
  std::string original, option_original;
  PatchDocument document;
  GuideDialogState guide;
  std::string status;

  void SetText(const std::string& source) {
    text.assign(source.begin(), source.end());
    text.push_back(0);
  }
  static int ResizeText(ImGuiInputTextCallbackData* data) {
    auto& buffer = *static_cast<std::vector<char>*>(data->UserData);
    buffer.resize(size_t(data->BufSize));
    data->Buf = buffer.data();
    return 0;
  }
  bool ApplyText() {
    const std::string source = text.data();
    const bool parsed = editing_option
        ? document.ReplaceOption(*editing_option, source, status)
        : document.Parse(source, status);
    if (!parsed) return false;
    valid = true;
    raw = false;
    editing_option.reset();
    return true;
  }
  void CancelOption() {
    editing_option.reset();
    raw = false;
    dirty = option_dirty_start;
    confirm_option_cancel = false;
    status.clear();
  }
  void Select(size_t index) {
    selected = index;
    confirm_delete = confirm_discard = confirm_option_cancel = dirty = false;
    editing_option.reset();
    deleting_option.reset();
    pending_selection.reset();
    std::ifstream input(files[index], std::ios::binary);
    original.assign(std::istreambuf_iterator<char>(input), {});
    valid = input.is_open() && !input.bad() && document.Parse(original, status);
    if (!input.is_open() || input.bad()) status = "Could not read the patch file.";
    SetText(original);
    raw = !valid;
  }
  // Whether the selected file is still a file directly in the patch folder.
  // Compares the paths as listed: canonical() fails for the app's own folders
  // on Xbox, which reported present files as unavailable.
  bool IsLocalFile() const {
    if (selected >= files.size()) return false;
    const auto folder = [](std::filesystem::path path) {
      path = path.lexically_normal();
      return xe::path_to_utf8(path.has_filename() ? path : path.parent_path());
    };
    std::error_code ec;
    return xe::utf8::equal_case(folder(files[selected].parent_path()),
                                folder(directory)) &&
           std::filesystem::is_regular_file(files[selected], ec) && !ec;
  }
  bool AllFilesValid() {
    for (const auto& path : files) {
      std::ifstream input(path, std::ios::binary);
      const std::string source((std::istreambuf_iterator<char>(input)), {});
      PatchDocument candidate;
      std::string error;
      if (!input.is_open() || input.bad() || !candidate.Parse(source, error)) {
        status = "Patches not reloaded: " + xe::path_to_utf8(path.filename()) +
                 ": " + (error.empty() ? "Could not read file." : error);
        return false;
      }
    }
    return true;
  }
};

GamePatchDialog::GamePatchDialog() : state_(std::make_unique<State>()) {}
GamePatchDialog::~GamePatchDialog() = default;
bool GamePatchDialog::is_open() const { return state_->open; }

void GamePatchDialog::Open(const std::filesystem::path& directory,
                           const std::string& title_id) {
  auto& s = *state_;
  s.directory = directory;
  s.files.clear();
  s.status.clear();
  s.open = s.pending = true;
  s.pending_reload = false;
  s.confirm_delete = s.confirm_discard = s.confirm_option_cancel = s.dirty = false;
  s.editing_option.reset();
  s.deleting_option.reset();
  s.pending_selection.reset();
  std::error_code ec;
  if (title_id.size() == 8) {
    for (const auto& file : std::filesystem::directory_iterator(directory, ec)) {
      if (!file.is_regular_file(ec)) continue;
      const auto name = xe::utf8::lower_ascii(xe::path_to_utf8(file.path().filename()));
      if (name.starts_with(xe::utf8::lower_ascii(title_id)) &&
          name.ends_with(".patch.toml")) {
        s.files.push_back(file.path());
      }
    }
  }
  std::sort(s.files.begin(), s.files.end());
  if (!s.files.empty()) {
    s.Select(0);
    s.pending_reload = s.AllFilesValid();
  }
  else s.status = "No patch files are installed for this game.";
}

bool GamePatchDialog::Draw(ui::ImGuiDrawer* drawer) {
  auto& s = *state_;
  if (!s.open) return false;
  if (s.pending) {
    ImGui::OpenPopup("Edit Patches");
    s.pending = false;
  }
  bool changed = s.pending_reload, close = false;
  s.pending_reload = false;
  if (BeginGuidePopup("Edit Patches", nullptr, s.guide, drawer, true)) {
    const bool back = ConsumeGuideBackPress();
    if (back) {
      if (s.confirm_option_cancel) {
        s.confirm_option_cancel = false;
      } else if (s.confirm_discard) {
        s.confirm_discard = false;
        s.pending_selection.reset();
      } else if (s.confirm_delete || s.deleting_option) {
        s.confirm_delete = false;
        s.deleting_option.reset();
      } else if (s.editing_option) {
        if (std::string(s.text.data()) != s.option_original) {
          s.confirm_option_cancel = true;
        } else s.CancelOption();
      } else if (s.dirty) {
        s.confirm_discard = true;
      } else close = true;
    }
    // A confirmation replaces the rest of the dialog and takes focus, so it is
    // never left out of view below a long list of options.
    const bool prompt = s.confirm_option_cancel || s.confirm_discard ||
                        s.confirm_delete || s.deleting_option.has_value();
    const bool focus_prompt = prompt && !s.prompt_shown;
    s.prompt_shown = prompt;
    const auto prompt_button = [&](const char* label) {
      if (focus_prompt) {
        ImGui::SetKeyboardFocusHere();
      }
      return GuideButton(label);
    };
    const std::string filename =
        s.files.empty() ? std::string()
                        : xe::path_to_utf8(s.files[s.selected].filename());
    if (s.confirm_option_cancel) {
      ImGui::TextUnformatted("Discard this option's text changes?");
      if (prompt_button("Discard option changes")) s.CancelOption();
      if (GuideButton("Keep editing option")) s.confirm_option_cancel = false;
    } else if (s.deleting_option) {
      const auto index = *s.deleting_option;
      const auto name = (*s.document.options().get_as<toml::table>(index))["name"]
                            .value_or(std::string("Patch option"));
      ImGui::TextWrapped("Delete option %s?", name.c_str());
      if (prompt_button("Delete option now")) {
        s.document.RemoveOption(index);
        s.deleting_option.reset();
        s.dirty = true;
        s.status = "Option removed. Select Save to update the file.";
      }
      if (GuideButton("Cancel option deletion")) s.deleting_option.reset();
    } else if (s.confirm_delete) {
      ImGui::TextWrapped("Delete %s?", filename.c_str());
      if (prompt_button("Delete file now")) {
        std::error_code ec;
        const bool removed = s.IsLocalFile() &&
            PatchDocument::MatchesFile(s.files[s.selected], s.original, s.status) &&
            std::filesystem::remove(s.files[s.selected], ec);
        s.confirm_delete = false;
        if (removed) {
          s.files.erase(s.files.begin() + s.selected);
          if (!s.files.empty()) s.Select(0);
          else s.dirty = false;
          s.status = "Patch file deleted.";
          changed = s.AllFilesValid();
        } else if (s.status.empty()) s.status = "Could not delete the patch file.";
      }
      if (GuideButton("Cancel deletion")) s.confirm_delete = false;
    } else if (s.confirm_discard) {
      ImGui::TextUnformatted("Discard unsaved changes?");
      if (prompt_button("Discard changes")) {
        s.confirm_discard = false;
        if (s.pending_selection) s.Select(*s.pending_selection);
        else close = true;
      }
      if (GuideButton("Keep editing")) {
        s.confirm_discard = false;
        s.pending_selection.reset();
      }
    } else if (!s.files.empty()) {
      if (ImGui::BeginCombo("Patch file", filename.c_str())) {
        for (size_t i = 0; i < s.files.size(); ++i) {
          const auto name = xe::path_to_utf8(s.files[i].filename());
          if (ImGui::Selectable(name.c_str(), i == s.selected) && i != s.selected) {
            if (s.dirty) {
              s.pending_selection = i;
              s.confirm_discard = true;
            } else s.Select(i);
          }
        }
        ImGui::EndCombo();
      }
      // Text editing of one option, or of the whole file if it failed to parse.
      if (s.raw) {
        if (s.editing_option) {
          ImGui::TextUnformatted("Edit patch option");
        } else {
          ImGui::TextWrapped(
              "The patch file could not be parsed. Fix the text and save it.");
        }
        if (GuideButton("Edit text")) {
          UWP::ShowKeyboard();
          ImGui::SetKeyboardFocusHere();
        }
        // A starts editing the field and B leaves it.
        PrepareGuideTextInput("##patch_text");
        if (ImGui::InputTextMultiline("##patch_text", s.text.data(), s.text.size(),
              ImVec2(-1, ImGui::GetTextLineHeight() * 12),
              ImGuiInputTextFlags_CallbackResize,
              State::ResizeText, &s.text)) {
          s.dirty = true;
        }
        if (ImGui::IsItemActivated()) UWP::ShowKeyboard();
        if (s.editing_option) {
          if (GuideButton("Apply option changes")) s.ApplyText();
          if (GuideButton("Cancel option edit")) {
            s.CancelOption();
          }
        }
      } else if (s.valid) {
        size_t index = 0;
        for (auto& node : s.document.options()) {
          auto* option = node.as_table();
          if (!option) continue;
          ImGui::PushID(static_cast<int>(index));
          const auto name = (*option)["name"].value_or(std::string("Patch option"));
          bool enabled = (*option)["is_enabled"].value_or(false);
          if (ImGui::Checkbox(name.c_str(), &enabled)) {
            option->insert_or_assign("is_enabled", enabled);
            s.dirty = true;
          }
          const auto desc = (*option)["desc"].value_or(std::string());
          if (!desc.empty()) ImGui::TextWrapped("%s", desc.c_str());
          if (GuideButton("Edit option")) {
            s.option_original = s.document.OptionText(index);
            s.SetText(s.option_original);
            s.option_dirty_start = s.dirty;
            s.editing_option = index;
            s.raw = true;
          }
          if (GuideButton("Delete option")) s.deleting_option = index;
          ImGui::Spacing();
          ImGui::PopID();
          ++index;
          if (s.editing_option || s.deleting_option) break;
        }
      }
      ImGui::BeginDisabled(s.editing_option.has_value());
      if (GuideButton("Save")) {
        const bool parsed = s.raw ? s.ApplyText() : s.valid;
        if (parsed && s.IsLocalFile()) {
          const auto output = s.document.Text();
          if (PatchDocument::Save(s.files[s.selected], output, s.original, s.status)) {
            s.original = output;
            s.dirty = false;
            s.status = "Saved. Changes apply on the next game launch.";
            changed = s.AllFilesValid();
          }
        } else if (parsed) {
          s.status = "The selected patch file is no longer available.";
        }
      }
      if (GuideButton("Delete patch file")) s.confirm_delete = true;
      ImGui::EndDisabled();
    }
    if (!s.status.empty()) ImGui::TextWrapped("%s", s.status.c_str());
    if (close) ImGui::CloseCurrentPopup();
    EndGuidePopup();
  }
  if (close) s.open = false;
  return changed;
}
}  // namespace xe::app
#endif
