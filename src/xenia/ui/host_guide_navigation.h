// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UI_HOST_GUIDE_NAVIGATION_H_
#define XENIA_UI_HOST_GUIDE_NAVIGATION_H_

namespace xe::app {

// The host guide remains alive while a selected page and its self-owned
// descendants draw. Returning never depends on an individual child's owner.
class HostGuideNavigation {
 public:
  bool visible() const { return state_ == State::kGuide; }
  bool launch_pending() const { return state_ == State::kLaunchPending; }
  bool input_armed() const { return input_armed_; }
  int selected_option() const { return selected_option_; }

  void OpenChild(int option) {
    selected_option_ = option;
    input_armed_ = false;
    state_ = State::kLaunchPending;
  }
  void ChildLaunched() {
    if (state_ == State::kLaunchPending) {
      state_ = State::kChild;
    }
  }
  bool Resume(bool descendant_dialogs_open, bool popup_open) {
    if (state_ != State::kChild || descendant_dialogs_open || popup_open) {
      return false;
    }
    state_ = State::kGuide;
    input_armed_ = false;
    return true;
  }
  void ObserveInputRelease(bool released) {
    if (visible() && released) {
      input_armed_ = true;
    }
  }
  void Close() {
    state_ = State::kClosed;
    input_armed_ = false;
  }

 private:
  enum class State { kGuide, kLaunchPending, kChild, kClosed };
  State state_ = State::kGuide;
  int selected_option_ = 0;
  bool input_armed_ = false;
};

}  // namespace xe::app
#endif  // XENIA_UI_HOST_GUIDE_NAVIGATION_H_
