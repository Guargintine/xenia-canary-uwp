// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_GAME_TITLE_UPDATE_DIALOG_H_
#define XENIA_APP_GAME_TITLE_UPDATE_DIALOG_H_

#include <filesystem>
#include <memory>
#include <string>

namespace xe {
class Emulator;
namespace ui {
class ImGuiDrawer;
}
namespace app {

class GameTitleUpdateDialog {
 public:
  GameTitleUpdateDialog();
  ~GameTitleUpdateDialog();
  void Open(const std::string& title_id, const std::string& media_id,
            const std::string& base_version,
            const std::filesystem::path& storage_root);
  void ProcessPending(Emulator* emulator);
  bool Draw(Emulator* emulator, ui::ImGuiDrawer* drawer);
  bool is_open() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace app
}  // namespace xe
#endif  // XENIA_APP_GAME_TITLE_UPDATE_DIALOG_H_
