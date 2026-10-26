#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

namespace winrt {
struct fire_and_forget;
}

namespace xe {
namespace ui {
class Window;
}
}  // namespace xe

namespace UWP {
void StartXenia();
bool HasQuit();
void RequestQuit();
void ShutdownXenia();
void ExecutePendingFunctionsFromUIThread();
void RegisterXeniaWindow(xe::ui::Window* window);
void UpdateImGuiIO();
// Registered and invoked on the UI thread; cleared before emulator teardown.
void SetHostGuideAction(std::function<void()> action);

void RefreshPaths();
std::vector<std::tuple<std::string, std::string>> GetGames();
std::vector<std::string> GetPaths();
void SetGamePaths(std::vector<std::string> paths);
}  // namespace UWP
