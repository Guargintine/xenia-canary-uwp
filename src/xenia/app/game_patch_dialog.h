// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_GAME_PATCH_DIALOG_H_
#define XENIA_APP_GAME_PATCH_DIALOG_H_

#include <filesystem>
#include <memory>
#include <string>

namespace xe::ui { class ImGuiDrawer; }
namespace xe::app {
class GamePatchDialog {
 public:
  GamePatchDialog();
  ~GamePatchDialog();
  void Open(const std::filesystem::path& directory, const std::string& title_id);
  bool is_open() const;
  bool Draw(ui::ImGuiDrawer* drawer);
 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace xe::app
#endif
