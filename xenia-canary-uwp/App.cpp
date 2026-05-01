#define NOMINMAX

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/windows.graphics.display.core.h>

#include <Gamingdeviceinformation.h>

#include "UWPUtil.h"
#include "WinRTKeyboard.h"
#include "XeniaUWP.h"
#include "protocol_activation.h"

using namespace winrt;

using namespace Windows;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation::Numerics;
using namespace Windows::UI;
using namespace Windows::UI::Core;
using namespace Windows::UI::Composition;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;

struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
  CompositionTarget m_target{nullptr};
  VisualCollection m_visuals{nullptr};
  Visual m_selected{nullptr};
  float2 m_offset{};
  hstring m_launchOnExit;

  IFrameworkView CreateView() { return *this; }

  void Initialize(CoreApplicationView const& v) {
    v.Activated({this, &App::OnActivate});
  }

  void Load(hstring const&) {}

  void Uninitialize() {}

  void Run() {
    auto navigation =
        winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();

    // UWP on Xbox One triggers a back request whenever the B button is
    // pressed which can result in the app being suspended if unhandled
    navigation.BackRequested(
        [](const winrt::Windows::Foundation::IInspectable&,
           const BackRequestedEventArgs& args) { args.Handled(true); });

    winrt::Windows::Graphics::Display::DisplayInformation displayInfo = winrt::
        Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
    if (displayInfo) {
      UWP::SetDPI(displayInfo.LogicalDpi());
    }

    UWP::StartXenia();

    while (!UWP::HasQuit()) {
      CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(
          CoreProcessEventsOption::ProcessAllIfPresent);

      UWP::ExecutePendingFunctionsFromUIThread();

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!m_launchOnExit.empty()) {
      try {
        auto operation = winrt::Windows::System::Launcher::LaunchUriAsync(
            winrt::Windows::Foundation::Uri{m_launchOnExit});
        // Keep the UI apartment responsive until the frontend handoff
        // completes, rather than exiting before Windows handles the URI.
        while (operation.Status() ==
               winrt::Windows::Foundation::AsyncStatus::Started) {
          CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(
              CoreProcessEventsOption::ProcessAllIfPresent);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        operation.GetResults();
      } catch (const winrt::hresult_error&) {
        // An unavailable frontend must not prevent Xenia from closing.
      }
    }
    // The upstream application teardown ends the process, so complete any
    // requested frontend handoff before invoking it.
    UWP::ShutdownXenia();
    CoreApplication::Exit();
  }

  void OnActivate(
      const winrt::Windows::ApplicationModel::Core::CoreApplicationView& v,
      const winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs&
          args) {
    if (args.Kind() ==
        Windows::ApplicationModel::Activation::ActivationKind::Protocol) {
      auto protocol_args = args.as<
          Windows::ApplicationModel::Activation::ProtocolActivatedEventArgs>();
      auto query = protocol_args.Uri().QueryParsed();
      std::vector<std::pair<std::string, std::string>> arguments;
      arguments.reserve(query.Size());
      for (const auto& argument : query) {
        arguments.emplace_back(winrt::to_string(argument.Name()),
                               winrt::to_string(argument.Value()));
      }
      auto activation = UWP::ParseProtocolActivation(arguments);
      m_launchOnExit = winrt::to_hstring(activation.launch_on_exit);
      UWP::SetAutomaticLaunch(activation.game_path.value_or(std::string{}));
    }

    auto appView = winrt::Windows::UI::ViewManagement::ApplicationView::
        GetForCurrentView();
    appView.PreferredLaunchWindowingMode(
        winrt::Windows::UI::ViewManagement::ApplicationViewWindowingMode::
            FullScreen);

    CoreWindow window = CoreWindow::GetForCurrentThread();
    window.Activate();
    appView.TryEnterFullScreenMode();
  }

  void OnCharacterReceived(
      winrt::Windows::UI::Core::CoreWindow const& /* sender */,
      winrt::Windows::UI::Core::CharacterReceivedEventArgs const&
          e /* args */) {
    UWP::HandleCharacter(e.KeyCode());
  }

  void SetWindow(CoreWindow const& window) {
    window.CharacterReceived({this, &App::OnCharacterReceived});
    window.Closed([](auto const&, auto const&) { UWP::RequestQuit(); });
  }
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  winrt::init_apartment();
  CoreApplication::Run(make<App>());
  winrt::uninit_apartment();
}
