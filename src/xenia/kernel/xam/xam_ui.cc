/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/app/guide_dialog.h"
#include "xenia/app/ui_text_effect_helpers.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/png_utils.h"
#include "xenia/base/system.h"
#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/friends_util.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_guest_notification.h"
#include "xenia/ui/imgui_host_notification.h"

#include "xenia/kernel/xam/ui/community_sessions_ui.h"
#include "xenia/kernel/xam/ui/create_profile_ui.h"
#include "xenia/kernel/xam/ui/friend_removal_confirmation.h"
#include "xenia/kernel/xam/ui/friends_ui.h"
#include "xenia/kernel/xam/ui/game_achievements_ui.h"
#include "xenia/kernel/xam/ui/gamercard_from_xuid_ui.h"
#include "xenia/kernel/xam/ui/gamercard_ui.h"
#include "xenia/kernel/xam/ui/passcode_ui.h"
#include "xenia/kernel/xam/ui/signin_ui.h"

DEFINE_bool(storage_selection_dialog, false,
            "Show storage device selection dialog when the game requests it.",
            "UI");

DECLARE_int32(license_mask);

DECLARE_int32(network_mode);

DECLARE_bool(upnp);

constexpr std::chrono::milliseconds kUIDelayMillis(200);

namespace xe {
namespace kernel {
namespace xam {

namespace {

float DrawWrappedConfiguredText(ImDrawList* draw_list, ImFont* font,
                                float font_size, ImVec2 pos, float max_width,
                                ImU32 color, const std::string& text) {
  if (text.empty()) {
    return 0.0f;
  }

  const char* text_begin = text.c_str();
  const char* text_end = text_begin + text.size();
  const float font_scale =
      font->FontSize > 0.0f ? font_size / font->FontSize : 1.0f;
  const float line_height = font_size + 4.0f;
  float cursor_y = pos.y;

  while (text_begin < text_end) {
    const char* paragraph_end = text_begin;
    while (paragraph_end < text_end && *paragraph_end != '\n') {
      ++paragraph_end;
    }

    if (paragraph_end == text_begin) {
      cursor_y += line_height;
    } else {
      const char* line_begin = text_begin;
      while (line_begin < paragraph_end) {
        const char* wrap_end = font->CalcWordWrapPositionA(
            font_scale, line_begin, paragraph_end, max_width);
        if (wrap_end == line_begin) {
          wrap_end = std::min(line_begin + 1, paragraph_end);
        }
        while (wrap_end > line_begin &&
               (*(wrap_end - 1) == ' ' || *(wrap_end - 1) == '\t')) {
          --wrap_end;
        }
        std::string line(line_begin, wrap_end);
        xe::app::DrawTextWithConfiguredEffect(draw_list, font, font_size,
                                              ImVec2(pos.x, cursor_y), color,
                                              line.c_str());
        cursor_y += line_height;
        line_begin = wrap_end;
        while (line_begin < paragraph_end &&
               (*line_begin == ' ' || *line_begin == '\t')) {
          ++line_begin;
        }
      }
    }

    if (paragraph_end < text_end && *paragraph_end == '\n') {
      text_begin = paragraph_end + 1;
    } else {
      text_begin = paragraph_end;
    }
  }

  return cursor_y - pos.y;
}

}  // namespace

// TODO(gibbed): This is all one giant WIP that seems to work better than the
// previous immediate synchronous completion of dialogs.
//
// The deferred execution of dialog handling is done in such a way that there is
// a pre-, peri- (completion), and post- callback steps.
//
// pre();
// result = completion();
// CompleteOverlapped(result);
// post();
//
// There are games that are batshit insane enough to wait for the X_OVERLAPPED
// to be completed (ie not X_ERROR_PENDING) before creating a listener to
// receive a notification, which is why we have distinct pre- and post- steps.
//
// We deliberately delay the XN_SYS_UI = false notification to give games time
// to create a listener (if they're insane enough do this).
static void QueueSystemUIDismissed(KernelState* state) {
  state->async_work().Start([state](std::stop_token stop) {
    if (AsyncWorkGroup::Delay(stop, kUIDelayMillis)) {
      state->BroadcastNotification(kXNotificationSystemUI, false);
    }
  });
}

struct XamDialogCompletion {
  X_RESULT result = X_ERROR_CANCELLED;
  uint32_t extended_error = 0;
  uint32_t length = 0;
  xe::threading::Fence closed;
};

// Attaches the close handling on the UI thread at dispatch, so a dialog closed
// before a deferred run() still reports its result.
template <typename T>
std::shared_ptr<XamDialogCompletion> AttachXamDialogCompletion(
    KernelState* state, T* dialog,
    std::function<X_RESULT(T*, XamDialogCompletion&)> on_close) {
  auto completion = std::make_shared<XamDialogCompletion>();
  if (!state->emulator()->display_window()->app_context()
           .CallInUIThreadSynchronous([dialog, on_close, completion]() {
             dialog->set_close_callback([dialog, on_close, completion]() {
               completion->result = on_close(dialog, *completion);
             });
             // Runs after OnClose and keeps the completion alive until then.
             dialog->Then([completion]() { completion->closed.Signal(); });
           })) {
    delete dialog;
    completion->closed.Signal();
  }
  return completion;
}

template <typename T>
X_RESULT xeXamDispatchDialog(T* dialog,
                             std::function<X_RESULT(T*)> close_callback,
                             uint32_t overlapped) {
  auto* state = kernel_state();
  auto completion = AttachXamDialogCompletion<T>(
      state, dialog, [close_callback](T* closed, XamDialogCompletion&) {
        return close_callback(closed);
      });
  auto pre = [state]() {
    state->BroadcastNotification(kXNotificationSystemUI, true);
  };
  auto run = [state, completion]() -> X_RESULT {
    completion->closed.Wait();
    state->xam_state()->is_xam_dialog_present_.store(false);
    return completion->result;
  };
  auto post = [state]() { QueueSystemUIDismissed(state); };
  if (!overlapped) {
    pre();
    auto result = run();
    post();
    return result;
  } else {
    state->CompleteOverlappedDeferred(run, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

template <typename T>
X_RESULT xeXamDispatchDialogEx(
    T* dialog, std::function<X_RESULT(T*, uint32_t&, uint32_t&)> close_callback,
    uint32_t overlapped) {
  auto* state = kernel_state();
  auto completion = AttachXamDialogCompletion<T>(
      state, dialog,
      [close_callback](T* closed, XamDialogCompletion& out) {
        return close_callback(closed, out.extended_error, out.length);
      });
  auto pre = [state]() {
    state->BroadcastNotification(kXNotificationSystemUI, true);
  };
  auto run = [state, completion](uint32_t& extended_error,
                                 uint32_t& length) -> X_RESULT {
    completion->closed.Wait();
    state->xam_state()->is_xam_dialog_present_.store(false);
    extended_error = completion->extended_error;
    length = completion->length;
    return completion->result;
  };
  auto post = [state]() {
    xe::threading::Sleep(kUIDelayMillis);
    state->BroadcastNotification(kXNotificationSystemUI, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    auto result = run(extended_error, length);
    post();
    // TODO(gibbed): do something with extended_error/length?
    return result;
  } else {
    state->CompleteOverlappedDeferredEx(run, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

X_RESULT xeXamDispatchHeadless(std::function<X_RESULT()> run_callback,
                               uint32_t overlapped) {
  auto* state = kernel_state();
  auto pre = [state]() {
    state->BroadcastNotification(kXNotificationSystemUI, true);
    xe::threading::Sleep(std::chrono::milliseconds(25));
  };
  auto post = [state]() { QueueSystemUIDismissed(state); };
  if (!overlapped) {
    pre();
    auto result = run_callback();
    post();
    return result;
  } else {
    state->CompleteOverlappedDeferred(run_callback, overlapped, pre,
                                               post);
    return X_ERROR_IO_PENDING;
  }
}

X_RESULT xeXamDispatchHeadlessEx(
    std::function<X_RESULT(uint32_t&, uint32_t&)> run_callback,
    uint32_t overlapped) {
  auto* state = kernel_state();
  auto pre = [state]() {
    state->BroadcastNotification(kXNotificationSystemUI, true);
  };
  auto post = [state]() {
    xe::threading::Sleep(kUIDelayMillis);
    state->BroadcastNotification(kXNotificationSystemUI, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    auto result = run_callback(extended_error, length);
    post();
    // TODO(gibbed): do something with extended_error/length?
    return result;
  } else {
    state->CompleteOverlappedDeferredEx(run_callback, overlapped, pre,
                                                 post);
    return X_ERROR_IO_PENDING;
  }
}

template <typename T>
X_RESULT xeXamDispatchDialogAsync(T* dialog,
                                  std::function<void(T*)> close_callback) {
  auto* state = kernel_state();
  state->BroadcastNotification(kXNotificationSystemUI, true);
  // Important to pass captured vars by value here since we return from this
  // without waiting for the dialog to close so the original local vars will be
  // destroyed.
  dialog->set_close_callback([state, dialog, close_callback,
                              stop = state->async_work().stop_token()]() {
    if (stop.stop_requested()) {
      return;
    }
    close_callback(dialog);

    state->xam_state()->is_xam_dialog_present_.store(false);
    QueueSystemUIDismissed(state);
  });

  return X_ERROR_SUCCESS;
}

X_RESULT xeXamDispatchHeadlessAsync(std::function<void()> run_callback) {
  auto* state = kernel_state();
  state->BroadcastNotification(kXNotificationSystemUI, true);

  auto display_window = state->emulator()->display_window();
  display_window->app_context().CallInUIThread(
      [state, run_callback, stop = state->async_work().stop_token()]() {
    if (stop.stop_requested()) {
      return;
    }
    run_callback();

    state->xam_state()->is_xam_dialog_present_.store(false);
    QueueSystemUIDismissed(state);
  });

  return X_ERROR_SUCCESS;
}

void MessageBoxDialog::OnDraw(ImGuiIO& io) {
#if XE_PLATFORM_WINRT
  if (!has_opened_) {
    has_opened_ = true;
    focus_requested_ = true;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  const ImGuiWindowFlags dim_flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
      ImGuiWindowFlags_NoCollapse;
  if (ImGui::Begin("##message_box_dim", nullptr, dim_flags)) {
    ImDrawList* dim_draw_list = ImGui::GetWindowDrawList();
    const ImVec2 dim_min = ImGui::GetWindowPos();
    const ImVec2 dim_max(dim_min.x + ImGui::GetWindowSize().x,
                         dim_min.y + ImGui::GetWindowSize().y);
    dim_draw_list->AddRectFilled(
        dim_min, dim_max, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f)));
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);

  static xe::app::GuideBackgroundTextureCache guide_background;
  const auto guide_bg_tex = guide_background.Get(imgui_drawer());
  static std::shared_ptr<xe::ui::ImmediateTexture> button_a_tex = nullptr;
  static std::shared_ptr<xe::ui::ImmediateTexture> button_b_tex = nullptr;
  if (!button_a_tex) {
    button_a_tex = xe::app::LoadButtonTexture(imgui_drawer(), 'A');
  }
  if (!button_b_tex) {
    button_b_tex = xe::app::LoadButtonTexture(imgui_drawer(), 'B');
  }

  const ImVec2 panel_size =
      xe::app::GetGuidePanelSize(guide_bg_tex, io.DisplaySize.y);
  const ImVec2 panel_padding = xe::app::GetGuidePanelPadding();
  const float ux = io.DisplaySize.x / 1024.0f;
  const float uy = io.DisplaySize.y / 576.0f;
  const ImVec2 overlay_pos = viewport->Pos;

  ImGui::SetNextWindowPos(overlay_pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  const ImGuiWindowFlags panel_flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove;
  if (!ImGui::Begin("##message_box_overlay", nullptr, panel_flags)) {
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    Close();
    return;
  }

  ImDrawList* overlay_draw_list = ImGui::GetWindowDrawList();
  const ImVec2 overlay_min = ImGui::GetWindowPos();
  const ImVec2 overlay_max(overlay_min.x + ImGui::GetWindowSize().x,
                           overlay_min.y + ImGui::GetWindowSize().y);

  xe::app::DrawGuidePanelBackground(overlay_draw_list, guide_bg_tex,
                                    overlay_min, overlay_max);

  const xe::app::OverlayHeaderLayout header_layout =
      xe::app::DrawOverlayHeader(overlay_draw_list, overlay_min, panel_size,
                                 panel_padding, ux, uy, title_.c_str());

  const float content_start_y =
      header_layout.position.y + header_layout.font_size + 55.0f;
  const float content_width = std::max(
      0.0f, std::min(430.0f * ux, panel_size.x - panel_padding.x * 2.0f) -
                (50.0f * ux));
  const float description_font_size = 22.0f * uy;
  const ImVec2 description_pos(overlay_min.x + panel_padding.x,
                               content_start_y);
  float description_height = 0.0f;
  if (!description_.empty()) {
    description_height = DrawWrappedConfiguredText(
        overlay_draw_list, ImGui::GetFont(), description_font_size,
        description_pos, content_width, IM_COL32(228, 228, 228, 255),
        description_);
  }

  if (buttons_.empty()) {
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    return;
  }

  if (chosen_button_ >= buttons_.size()) {
    chosen_button_ = static_cast<uint32_t>(buttons_.size() - 1);
  }

  const bool confirm_down = ImGui::IsKeyDown(ImGuiKey_Enter) ||
                            ImGui::IsKeyDown(ImGuiKey_KeypadEnter) ||
                            ImGui::IsKeyDown(ImGuiKey_Space) ||
                            ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
  const bool cancel_down = ImGui::IsKeyDown(ImGuiKey_Escape) ||
                           ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight);
  const bool should_focus_button =
      focus_requested_ &&
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0) &&
      !(confirm_down || cancel_down);
  if (should_focus_button) {
    focus_requested_ = false;
  }

  const auto choose_button = [&](uint32_t index) {
    chosen_button_ = index;
    Close();
  };

  const float button_gap = 8.0f * ux;
  const ImVec2 button_size((content_width - button_gap) / 2.0f, 0.0f);
  ImGui::SetCursorScreenPos(
      ImVec2(overlay_min.x + panel_padding.x,
             description_pos.y + description_height + (18.0f * uy)));

  for (size_t i = 0; i < buttons_.size(); ++i) {
    if (i > 0) {
      ImGui::SameLine(0.0f, button_gap);
    }
    ImGui::PushID(static_cast<int>(i));
    if (should_focus_button && i == chosen_button_) {
      ImGui::SetKeyboardFocusHere();
    }
    if (xe::app::DrawTextEffectButton(buttons_[i].c_str(), button_size)) {
      choose_button(static_cast<uint32_t>(i));
      ImGui::PopID();
      ImGui::End();
      ImGui::PopStyleColor();
      ImGui::PopStyleVar(3);
      return;
    }
    if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) {
      chosen_button_ = static_cast<uint32_t>(i);
    }
    ImGui::PopID();
  }

  const float footer_text_size = 13.5f * uy;
  const float footer_icon_size = 15.6f * uy;
  const float footer_spacing_y = 17.0f * uy;
  const float footer_select_y = overlay_max.y - (32.0f * uy);
  const float footer_back_y = footer_select_y - footer_spacing_y;
  const float footer_base_x = overlay_min.x + panel_size.x * 0.9f;
  const float footer_back_text_x = footer_base_x - (70.0f * ux);
  const float footer_back_icon_offset = 35.0f * ux;
  const float footer_select_text_x = footer_base_x - (60.0f * ux);
  const float footer_select_icon_offset = 42.0f * ux;
  xe::app::DrawFooterPrompt(overlay_draw_list, button_b_tex, footer_text_size,
                            footer_icon_size, "Back", footer_back_y,
                            footer_back_text_x, footer_back_icon_offset);
  xe::app::DrawFooterPrompt(overlay_draw_list, button_a_tex, footer_text_size,
                            footer_icon_size, "Select", footer_select_y,
                            footer_select_text_x, footer_select_icon_offset);

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
#else
  static_cast<void>(io);
  if (!has_opened_) {
    has_opened_ = true;
    focus_requested_ = true;
    ImGui::OpenPopup(title_.c_str());
  }

  if (!ImGui::BeginPopupModal(title_.c_str(), nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    Close();
    return;
  }

  if (!description_.empty()) {
    ImGui::TextWrapped("%s", description_.c_str());
    ImGui::Spacing();
  }

  if (buttons_.empty()) {
    ImGui::EndPopup();
    return;
  }

  if (chosen_button_ >= buttons_.size()) {
    chosen_button_ = static_cast<uint32_t>(buttons_.size() - 1);
  }

  for (size_t i = 0; i < buttons_.size(); ++i) {
    if (i > 0) {
      ImGui::SameLine();
    }
    ImGui::PushID(static_cast<int>(i));
    if (focus_requested_ && i == chosen_button_) {
      ImGui::SetKeyboardFocusHere();
      focus_requested_ = false;
    }
    if (ImGui::Button(buttons_[i].c_str())) {
      chosen_button_ = static_cast<uint32_t>(i);
      ImGui::CloseCurrentPopup();
      Close();
      ImGui::PopID();
      ImGui::EndPopup();
      return;
    }
    if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) {
      chosen_button_ = static_cast<uint32_t>(i);
    }
    ImGui::PopID();
  }

  if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
      ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    chosen_button_ = default_button_ < buttons_.size() ? default_button_ : 0;
    ImGui::CloseCurrentPopup();
    Close();
    ImGui::EndPopup();
    return;
  }

  ImGui::EndPopup();
#endif
}

void KeyboardInputDialog::OnDraw(ImGuiIO& io) {
  bool first_draw = false;
  if (!has_opened_) {
    ImGui::OpenPopup(title_.c_str());
    has_opened_ = true;
    first_draw = true;
  }
  if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (description_.size()) {
      ImGui::TextWrapped("%s", description_.c_str());
    }
    if (first_draw) {
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::PushID("input_text");
    bool input_submitted =
        ImGui::InputText("##body", text_buffer_.data(), text_buffer_.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::BeginPopupContextItem("input_context_menu")) {
      if (ImGui::MenuItem("Paste")) {
        if (ImGui::GetClipboardText() != nullptr) {
          std::string clipboard_text = ImGui::GetClipboardText();
          xe::string_util::copy_truncating(text_buffer_.data(), clipboard_text,
                                           text_buffer_.size());
        }
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
    if (input_submitted) {
      text_ = std::string(text_buffer_.data(), text_buffer_.size());
      cancelled_ = false;
      ImGui::CloseCurrentPopup();
      Close();
    }
    if (ImGui::Button("OK")) {
      text_ = std::string(text_buffer_.data(), text_buffer_.size());
      cancelled_ = false;
      ImGui::CloseCurrentPopup();
      Close();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      text_ = "";
      cancelled_ = true;
      ImGui::CloseCurrentPopup();
      Close();
    }
    ImGui::Spacing();
    ImGui::EndPopup();
  } else {
    Close();
  }
}

static dword_result_t XamShowMessageBoxUi(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  std::string title = title_ptr ? xe::to_utf8(title_ptr.value()) : "";
  std::string text = text_ptr ? xe::to_utf8(text_ptr.value()) : "";

  std::vector<std::string> buttons;
  for (uint32_t i = 0; i < button_count; ++i) {
    uint32_t button_ptr = button_ptrs[i];
    auto button = xe::load_and_swap<std::u16string>(
        kernel_state()->memory()->TranslateVirtual(button_ptr));

    if (!button.empty()) {
      buttons.push_back(xe::to_utf8(button));
    }
  }

  if (buttons.empty()) {
    buttons.push_back("OK");
  }

  X_RESULT result;
  if (cvars::headless) {
    // Auto-pick the focused button.
    auto run = [result_ptr, active_button]() -> X_RESULT {
      result_ptr->ButtonPressed = static_cast<uint32_t>(active_button);
      return X_ERROR_SUCCESS;
    };

    result = xeXamDispatchHeadless(run, overlapped);
  } else {
    switch (flags & 0xF) {
      case XMBox_NOICON: {
      } break;
      case XMBox_ERRORICON: {
      } break;
      case XMBox_WARNINGICON: {
      } break;
      case XMBox_ALERTICON: {
      } break;
    }

    if (kernel_state()->xam_state()->IsUIActive()) {
      return X_ERROR_ACCESS_DENIED;
    }

    kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

    const Emulator* emulator = kernel_state()->emulator();
    xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

    if (flags & XMBox_PASSCODEMODE || flags & XMBox_VERIFYPASSCODEMODE) {
      auto close = [result_ptr,
                    active_button](ui::ProfilePasscodeUI* dialog) -> X_RESULT {
        if (dialog->SelectedSignedIn()) {
          // Logged in
          return X_ERROR_SUCCESS;
        } else {
          return X_ERROR_FUNCTION_FAILED;
        }
      };

      result = xeXamDispatchDialog<ui::ProfilePasscodeUI>(
          new ui::ProfilePasscodeUI(imgui_drawer, title, text, result_ptr),
          close, overlapped);
    } else {
      auto close = [result_ptr](MessageBoxDialog* dialog) -> X_RESULT {
        result_ptr->ButtonPressed = dialog->chosen_button();
        kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
        return X_ERROR_SUCCESS;
      };

      result = xeXamDispatchDialog<MessageBoxDialog>(
          new MessageBoxDialog(imgui_drawer, title, text, buttons,
                               static_cast<uint32_t>(active_button)),
          close, overlapped);
    }
  }

  return result;
}

dword_result_t XamIsUIActive_entry() {
  return kernel_state()->xam_state()->IsUIActive();
}
DECLARE_XAM_EXPORT2(XamIsUIActive, kUI, kImplemented, kHighFrequency);

// https://www.se7ensins.com/forums/threads/working-xshowmessageboxui.844116/
dword_result_t XamShowMessageBoxUI_entry(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  return XamShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                             button_ptrs, active_button, flags, result_ptr,
                             overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMessageBoxUI, kUI, kImplemented);

dword_result_t XamShowMessageBoxUIEx_entry(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, dword_t unknown_unused,
    pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  return XamShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                             button_ptrs, active_button, flags, result_ptr,
                             overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMessageBoxUIEx, kUI, kImplemented);

dword_result_t XNotifyQueueUI_entry(dword_t exnq, dword_t dwUserIndex,
                                    qword_t qwAreas,
                                    lpu16string_t displayText_ptr,
                                    lpvoid_t contextData) {
  std::string displayText = "";
  const uint8_t position_id = static_cast<uint8_t>(qwAreas);

  if (displayText_ptr) {
    displayText = xe::to_utf8(displayText_ptr.value());
  }

  XELOGI("XNotifyQueueUI: {}", displayText);

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

  new xe::ui::XNotifyWindow(imgui_drawer, "", displayText, dwUserIndex,
                            position_id);

  // XNotifyQueueUI -> XNotifyQueueUIEx -> XMsgProcessRequest ->
  // XMsgStartIORequestEx & XMsgInProcessCall
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XNotifyQueueUI, kUI, kSketchy);

// https://www.se7ensins.com/forums/threads/release-how-to-use-xshowkeyboardui-release.906568/
dword_result_t XamShowKeyboardUI_entry(
    dword_t user_index, dword_t flags, lpu16string_t default_text,
    lpu16string_t title, lpu16string_t description, lpu16string_t buffer,
    dword_t buffer_length, pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  assert_not_null(overlapped);

  auto buffer_size = static_cast<size_t>(buffer_length) * sizeof(char16_t);

  X_RESULT result;
  if (cvars::headless) {
    auto run = [default_text, buffer, buffer_length,
                buffer_size]() -> X_RESULT {
      // Redirect default_text back into the buffer.
      if (!default_text) {
        std::memset(buffer, 0, buffer_size);
      } else {
        string_util::copy_and_swap_truncating(buffer, default_text.value(),
                                              buffer_length);
      }
      return X_ERROR_SUCCESS;
    };
    result = xeXamDispatchHeadless(run, overlapped);
  } else {
    auto close = [buffer, buffer_length](KeyboardInputDialog* dialog,
                                         uint32_t& extended_error,
                                         uint32_t& length) -> X_RESULT {
      if (dialog->cancelled()) {
        extended_error = X_ERROR_CANCELLED;
        length = 0;
        return X_ERROR_SUCCESS;
      } else {
        // Zero the output buffer.
        auto text = xe::to_utf16(dialog->text());
        string_util::copy_and_swap_truncating(buffer, text, buffer_length);
        extended_error = X_ERROR_SUCCESS;
        length = 0;
        return X_ERROR_SUCCESS;
      }
    };

    if (kernel_state()->xam_state()->IsUIActive()) {
      return X_ERROR_ACCESS_DENIED;
    }

    kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

    const Emulator* emulator = kernel_state()->emulator();
    xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

    std::string title_str = title ? xe::to_utf8(title.value()) : "";
    std::string desc_str = description ? xe::to_utf8(description.value()) : "";
    std::string def_text_str =
        default_text ? xe::to_utf8(default_text.value()) : "";

    result = xeXamDispatchDialogEx<KeyboardInputDialog>(
        new KeyboardInputDialog(imgui_drawer, title_str, desc_str, def_text_str,
                                buffer_length),
        close, overlapped);
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamShowKeyboardUI, kUI, kImplemented);

dword_result_t XamShowDeviceSelectorUI_entry(
    dword_t user_index, dword_t content_type, dword_t content_flags,
    qword_t total_requested, lpdword_t device_id_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!overlapped) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if ((user_index >= XUserMaxUserCount && user_index != XUserIndexAny) ||
      (content_flags & 0x83F00008) != 0 || !device_id_ptr) {
    XOverlappedSetExtendedError(overlapped, X_ERROR_INVALID_PARAMETER);
    return X_ERROR_INVALID_PARAMETER;
  }

  std::vector<const DummyDeviceInfo*> devices = ListStorageDevices();

  if (cvars::headless || !cvars::storage_selection_dialog) {
    // Default to the first storage device (HDD) if headless.
    return xeXamDispatchHeadless(
        [device_id_ptr, devices]() -> X_RESULT {
          if (devices.empty()) {
            return X_ERROR_CANCELLED;
          }

          const DummyDeviceInfo* device_info = devices.front();
          *device_id_ptr = static_cast<uint32_t>(device_info->device_id);
          return X_ERROR_SUCCESS;
        },
        overlapped);
  }

  auto close = [device_id_ptr, devices](MessageBoxDialog* dialog) -> X_RESULT {
    uint32_t button = dialog->chosen_button();
    if (button >= devices.size()) {
      return X_ERROR_CANCELLED;
    }

    const DummyDeviceInfo* device_info = devices.at(button);
    *device_id_ptr = static_cast<uint32_t>(device_info->device_id);
    return X_ERROR_SUCCESS;
  };

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  std::string title = "Select storage device";
  std::string desc = "";

  cxxopts::OptionNames buttons;
  for (auto& device_info : devices) {
    buttons.push_back(to_utf8(device_info->name));
  }
  buttons.push_back("Cancel");

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  return xeXamDispatchDialog<MessageBoxDialog>(
      new MessageBoxDialog(imgui_drawer, title, desc, buttons, 0), close,
      overlapped);
}
DECLARE_XAM_EXPORT1(XamShowDeviceSelectorUI, kUI, kImplemented);

void XamShowDirtyDiscErrorUI_entry(dword_t user_index) {
  if (cvars::headless) {
    assert_always();
    exit(1);
    return;
  }

  std::string title = "Disc Read Error";
  std::string desc =
      "There's been an issue reading content from the game disc.\nThis is "
      "likely caused by bad or unimplemented file IO calls.";

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  xeXamDispatchDialog<MessageBoxDialog>(
      new MessageBoxDialog(imgui_drawer, title, desc, {"OK"}, 0),
      [](MessageBoxDialog*) -> X_RESULT { return X_ERROR_SUCCESS; }, 0);
  // This is death, and should never return.
  // TODO(benvanik): cleaner exit.
  exit(1);
}
DECLARE_XAM_EXPORT1(XamShowDirtyDiscErrorUI, kUI, kImplemented);

dword_result_t XamShowPartyUI_entry(dword_t user_index) {
  if (!kernel_state()->xam_state()->user_tracker()->LoggedInToLive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowPartyUI, kNone, kStub);

// this is supposed to do a lot more, calls another function that triggers some
// cbs
dword_result_t XamSetDashContext_entry(dword_t value,
                                       const ppc_context_t& ctx) {
  ctx->kernel_state->dash_context_ = value;
  kernel_state()->BroadcastNotification(kXNotificationSystemDashContextChanged,
                                        0);
  return 0;
}

DECLARE_XAM_EXPORT1(XamSetDashContext, kNone, kImplemented);

dword_result_t XamGetDashContext_entry(const ppc_context_t& ctx) {
  return ctx->kernel_state->dash_context_;
}

DECLARE_XAM_EXPORT1(XamGetDashContext, kNone, kImplemented);

// https://gitlab.com/GlitchyScripts/xlivelessness/-/blob/master/xlivelessness/xlive/xdefs.hpp?ref_type=heads#L1235
dword_result_t XamShowMarketplaceUIEx_entry(dword_t user_index, dword_t ui_type,
                                            qword_t offer_id,
                                            dword_t offer_type,
                                            dword_t content_category,
                                            unknown_t unk6, unknown_t unk7,
                                            dword_t title_id) {
  // ui_type:
  // 0 - view all content for the current title
  // 1 - view content specified by offer id
  // offer_types:
  // filter for content list, usually just -1
  // content_category:
  // filter on item types for games (e.g. cars, maps, weapons, etc)
  if (user_index >= XUserMaxUserCount || ui_type >= MaxEntrypoint) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }

  if (cvars::headless) {
    return xeXamDispatchHeadlessAsync([]() {});
  }

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  bool is_xbla_unlock_offer =
      (offer_id == ((uint64_t(kernel_state()->title_id()) << 32) | 1ull));

  auto close = [ui_type,
                is_xbla_unlock_offer](MessageBoxDialog* dialog) -> void {
    if (ui_type == 1 && is_xbla_unlock_offer) {
      uint32_t button = dialog->chosen_button();
      if (button == 0) {
        cvars::license_mask = 1;

        kernel_state()->BroadcastNotification(
            kXNotificationLiveContentInstalled, 0);
      }
    }
  };

  std::string title = "Xbox Marketplace";
  std::string desc = "";
  cxxopts::OptionNames buttons;

  switch (ui_type) {
    case X_MARKETPLACE_ENTRYPOINT::ContentList:
      desc =
          "Game requested to open marketplace page with all content for the "
          "current title ID.";
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentItem:
      desc = fmt::format(
          "Game requested to open marketplace page for offer ID 0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::MembershipList:
      desc =
          "Game requested to open marketplace page with all Xbox Live "
          "memberships.";
      break;
    case X_MARKETPLACE_ENTRYPOINT::MembershipItem:
      desc = fmt::format(
          "Game requested to open marketplace page for an Xbox Live "
          "membership offer 0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentList_Background:
      // Used when accessing microsoft points
      desc = fmt::format(
          "Xbox Marketplace requested access to Microsoft Points offer page "
          "0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentItem_Background:
      // Used when accessing credit card information and calls
      // XamShowCreditCardUI
      desc = fmt::format(
          "Xbox Marketplace requested access to credit card information page "
          "0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::ForcedNameChangeV1:
      // Used by XamShowForcedNameChangeUI v1888
      desc = fmt::format("Changing gamertag currently not implemented.");
      break;
    case X_MARKETPLACE_ENTRYPOINT::ForcedNameChangeV2:
      // Used by XamShowForcedNameChangeUI NXE and up
      desc = fmt::format("Changing gamertag currently not implemented.");
      break;
    case X_MARKETPLACE_ENTRYPOINT::GamerTagPurchase:
      // Used by dashboard when selecting change gamertag in profile menu
      desc = fmt::format("Changing gamertag currently not implemented.");
      break;
    case X_MARKETPLACE_ENTRYPOINT::ActiveDownloads:
      // Used in profile tabs when clicking active downloads
      desc = fmt::format(
          "There are no current plans to download files from Xbox "
          "Marketplace.");
      break;
    default:
      desc = fmt::format("Unknown marketplace op {:d}",
                         static_cast<uint32_t>(ui_type));
      break;
  }

  desc +=
      "\nNote that since Xenia cannot access Xbox Marketplace, any DLC must be "
      "installed manually using File -> Install Content.";

  switch (ui_type) {
    case X_MARKETPLACE_ENTRYPOINT::ContentList:
    default:
      buttons.push_back("OK");
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentItem:
      if (is_xbla_unlock_offer) {
        desc +=
            "\n\nTo start trial games in full mode, set license_mask to 1 in "
            "Xenia config file.\n\nDo you wish to change license_mask to 1 for "
            "*this session*?";
        buttons.push_back("Yes");
        buttons.push_back("No");
      } else {
        buttons.push_back("OK");
      }
      break;
  }

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  return xeXamDispatchDialogAsync<MessageBoxDialog>(
      new MessageBoxDialog(imgui_drawer, title, desc, buttons, 0), close);
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceUIEx, kUI, kSketchy);

dword_result_t XamShowMarketplaceUI_entry(dword_t user_index, dword_t ui_type,
                                          qword_t offer_id, dword_t offer_type,
                                          dword_t content_category,
                                          dword_t title_id) {
  return XamShowMarketplaceUIEx_entry(user_index, ui_type, offer_id, offer_type,
                                      content_category, 0, 0, title_id);
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceUI, kUI, kSketchy);

dword_result_t XamShowMarketplaceDownloadItemsUI_entry(
    dword_t user_index, dword_t ui_type, lpqword_t offers, dword_t num_offers,
    lpdword_t hresult_ptr, pointer_t<XAM_OVERLAPPED> overlapped) {
  if (user_index >= XUserMaxUserCount || !offers || num_offers > 6) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediate(overlapped,
                                                  X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  if (cvars::headless) {
    return xeXamDispatchHeadless(
        [hresult_ptr]() -> X_RESULT {
          if (hresult_ptr) {
            *hresult_ptr = X_E_SUCCESS;
          }
          return X_ERROR_SUCCESS;
        },
        overlapped);
  }

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  auto close = [hresult_ptr](MessageBoxDialog* dialog) -> X_RESULT {
    if (hresult_ptr) {
      // TODO
      *hresult_ptr = X_E_SUCCESS;
    }
    return X_ERROR_SUCCESS;
  };

  std::string title = "Xbox Marketplace";
  std::string desc = "";
  cxxopts::OptionNames buttons = {"OK"};

  switch (ui_type) {
    case X_MARKETPLACE_DOWNLOAD_ITEMS_ENTRYPOINTS::FREEITEMS:
      desc =
          "Game requested to open download page for the following free offer "
          "IDs:";
      break;
    case X_MARKETPLACE_DOWNLOAD_ITEMS_ENTRYPOINTS::PAIDITEMS:
      desc =
          "Game requested to open download page for the following offer IDs:";
      break;
    default:
      return X_ERROR_INVALID_PARAMETER;
  }

  for (uint32_t i = 0; i < num_offers; i++) {
    desc += fmt::format("\n0x{:16X}", offers[i].get());
  }

  desc +=
      "\n\nNote that since Xenia cannot access Xbox Marketplace, any DLC "
      "must "
      "be installed manually using File -> Install Content.";

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  return xeXamDispatchDialog<MessageBoxDialog>(
      new MessageBoxDialog(imgui_drawer, title, desc, buttons, 0), close,
      overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceDownloadItemsUI, kUI, kSketchy);

dword_result_t XamShowForcedNameChangeUI_entry(dword_t user_index) {
  // Changes from 6 to 8 past NXE
  return XamShowMarketplaceUIEx_entry(user_index, 6, 0, 0xffffffff, 0, 0, 0, 0);
}
DECLARE_XAM_EXPORT1(XamShowForcedNameChangeUI, kUI, kImplemented);

bool xeDrawProfileContent(xe::ui::ImGuiDrawer* imgui_drawer,
                          const uint64_t xuid, const uint8_t user_index,
                          const X_XAMACCOUNTINFO* account,
                          const xe::ui::ImmediateTexture* profile_icon,
                          std::function<bool()> context_menu,
                          std::function<void()> on_profile_change,
                          uint64_t* selected_xuid, bool request_focus) {
  if (selected_xuid || context_menu) {
    const bool is_selected = selected_xuid && *selected_xuid == xuid;
    if (ImGui::Selectable("##profile_item", is_selected,
                          ImGuiSelectableFlags_SpanAllColumns)) {
      if (selected_xuid) {
        *selected_xuid = xuid;
      }
      if (context_menu) {
        ImGui::OpenPopup("Profile Menu");
      }
    }

    if (request_focus) {
      ImGui::SetItemDefaultFocus();
    }

    if (is_selected || ImGui::IsItemFocused()) {
      const ImVec2 padding(3.0f, 3.0f);
      xe::app::DrawConfiguredComboHighlight(
          ImVec2(ImGui::GetItemRectMin().x - padding.x,
                 ImGui::GetItemRectMin().y - padding.y),
          ImVec2(ImGui::GetItemRectMax().x + padding.x,
                 ImGui::GetItemRectMax().y + padding.y),
          1.6f, 4.0f);
    }
  }

  ImGui::BeginGroup();
  {
    if (profile_icon) {
      ImGui::Image(reinterpret_cast<ImTextureID>(profile_icon),
                   xe::ui::default_image_icon_size);
    } else {
      if (user_index < XUserMaxUserCount) {
        const auto icon = imgui_drawer->GetNotificationIcon(user_index);
        ImGui::Image(reinterpret_cast<ImTextureID>(icon),
                     xe::ui::default_image_icon_size);
      } else {
        ImGui::Dummy(xe::ui::default_image_icon_size);
      }
    }

    ImGui::SameLine(0.0f, 10.0f * ImGui::GetIO().DisplayFramebufferScale.x);

    ImGui::BeginGroup();
    {
      ImGui::TextUnformatted(
          fmt::format("User: {}\n", account->GetGamertagString()).c_str());
      ImGui::TextUnformatted(fmt::format("XUID: {:016X}  \n", xuid).c_str());

      const std::string live_enabled = fmt::format(
          "Xbox Live Enabled: {}", account->IsLiveEnabled() ? "True" : "False");

      ImGui::TextUnformatted(live_enabled.c_str());

      if (user_index != XUserIndexAny) {
        ImGui::TextUnformatted(
            fmt::format("Assigned to slot: {}\n", user_index + 1).c_str());
      } else {
        ImGui::TextUnformatted(fmt::format("Profile is not signed in").c_str());
      }
    }
    ImGui::EndGroup();
  }
  ImGui::EndGroup();

  if (context_menu) {
    return context_menu();
  }

  return true;
}

ui::FriendDialogPresentation xeMakeNetplayGuidePresentation(
    xe::ui::ImGuiDrawer* imgui_drawer) {
  static xe::app::GuideDialogState state;
  ui::FriendDialogPresentation presentation;
  presentation.begin_guide = [imgui_drawer](const char* name, bool* open) {
    return xe::app::BeginGuidePopup(name, open, state, imgui_drawer);
  };
  presentation.end_guide = []() { xe::app::EndGuidePopup(); };
  presentation.consume_back = []() { return xe::app::ConsumeGuideBackPress(); };
  presentation.button = [](const char* label, ImVec2 size) {
    return xe::app::GuideButton(label, size);
  };
  return presentation;
}

bool xeDrawFriendContent(xe::ui::ImGuiDrawer* imgui_drawer,
                         UserProfile* profile,
                         std::shared_ptr<xe::ui::ImmediateTexture> icon_texture,
                         const X_ONLINE_FRIEND& presence,
                         uint64_t* selected_xuid_, uint64_t* removed_xuid_,
                         const ui::FriendDialogPresentation* presentation) {
  const float content_left = ImGui::GetCursorPosX();
  if (icon_texture) {
    ImGui::Image(reinterpret_cast<ImTextureID>(icon_texture.get()),
                 xe::ui::default_image_icon_size);
    ImGui::SameLine();
  } else {
    ImGui::Image(
        reinterpret_cast<ImTextureID>(imgui_drawer->GetLoadingTileIcon()),
        xe::ui::default_image_icon_size);
    ImGui::SameLine();
  }

  ImVec2 start_drawing_position = ImGui::GetCursorPos();

  ImGui::TextUnformatted(presence.Gamertag);

  uint32_t index = 1;

  const uint32_t title_id = presence.title_id.get();

  if (title_id) {
    ImGui::SameLine();
    ImGui::SetCursorPos(start_drawing_position);
    ImGui::SetCursorPosY(start_drawing_position.y + ImGui::GetTextLineHeight());

    if (title_id == kernel_state()->title_id()) {
      ImGui::TextUnformatted(
          fmt::format("Game: {}", kernel_state()->emulator()->title_name())
              .c_str());
    } else {
      ImGui::TextUnformatted(fmt::format("Title ID: {}", title_id).c_str());
    }

    index++;
  }

  ImGui::SameLine();
  ImGui::SetCursorPos(start_drawing_position);
  ImGui::SetCursorPosY(start_drawing_position.y +
                       index * ImGui::GetTextLineHeight());

  const uint64_t friend_xuid = presence.xuid.get();
  const std::string friend_xuid_str = fmt::format("{:016X}", friend_xuid);

  ImGui::TextUnformatted(
      fmt::format("Online XUID: {:016X}\n", friend_xuid).c_str());
  index++;

  const std::u16string rich_presence = xe::string_util::read_u16string_and_swap(
      reinterpret_cast<const char16_t*>(presence.wszRichPresence));

  if (!rich_presence.empty()) {
    ImGui::SameLine();
    ImGui::SetCursorPos(start_drawing_position);
    ImGui::SetCursorPosY(start_drawing_position.y +
                         index * ImGui::GetTextLineHeight());

    std::string presense_string =
        xe::string_util::trim(xe::to_utf8(rich_presence));

    presense_string =
        std::regex_replace(presense_string, std::regex("\\n"), ", ");

    ImGui::TextWrapped(fmt::format("Status: {}", presense_string).c_str());

    index++;
  }

  const ImVec2 friend_info_drawing_end_position = ImGui::GetCursorPos();

  if (presentation) {
    ImGui::SetCursorPosX(content_left);
  }
  ImGui::BeginGroup();

  float btn_height = presentation ? ImGui::GetFrameHeight() : 25.0f;
  float btn_width = (ImGui::GetContentRegionAvail().x * 0.5f) -
                    (ImGui::GetStyle().ItemSpacing.x * 0.5f);
  ImVec2 half_width_btn = ImVec2(btn_width, btn_height);

  bool are_friends =
      kernel_state()->friends_manager()->IsFriend(profile->xuid(), friend_xuid);
  bool is_self = profile->GetOnlineXUID() == presence.xuid.get();

  const std::string join_label =
      std::format("Join Session##{}", friend_xuid_str);

  const std::string remove_label = std::format("Remove##{}", friend_xuid_str);

  const std::string add_label = std::format("Add##{}", friend_xuid_str);

  const bool same_title = title_id == kernel_state()->title_id();

  if (!is_self) {
    ImGui::Spacing();

    ImGui::BeginDisabled(!presence.session_id.as_uint64() || !same_title);
    if (ui::FriendPopupButton(join_label.c_str(), half_width_btn,
                              presentation)) {
      X_INVITE_INFO invite = {};

      invite.xuid_invitee = profile->GetOnlineXUID();
      invite.xuid_inviter = presence.xuid.get();
      invite.title_id = kernel_state()->title_id();
      invite.from_game_invite = true;

      profile->SetSelfInvite(invite);

      const uint32_t user_index =
          kernel_state()->xam_state()->GetUserIndexAssignedToProfileFromXUID(
              profile->xuid());

      kernel_state()->BroadcastNotification(kXNotificationLiveInviteAccepted,
                                            user_index);
    }
    ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (kernel_state()->title_id() == 0 || title_id == 0 || same_title) {
        ImGui::SetTooltip("Join gaming session");
      } else {
        ImGui::SetTooltip(
            fmt::format("{} is playing a different game", presence.Gamertag)
                .c_str());
      }
    }
  }

  if (!is_self) {
    ImGui::SameLine();
  }

  if (are_friends && !is_self) {
    const std::string confirmation_label =
        std::format("Remove Friend##{}", friend_xuid_str);
    if (ui::FriendPopupButton(remove_label.c_str(), half_width_btn,
                              presentation)) {
      ImGui::OpenPopup(confirmation_label.c_str());
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Remove Friend");
    }
    const std::string confirmation =
        std::format("Remove {} from your friends?", presence.Gamertag);
    if (ui::DrawFriendRemovalConfirmation(confirmation_label.c_str(),
                                          confirmation.c_str(), presentation)) {
      if (kernel_state()->friends_manager()->RemoveFriend(profile->xuid(),
                                                          friend_xuid)) {
        if (removed_xuid_) {
          *removed_xuid_ = friend_xuid;
        }

        const std::string gamertag(presence.Gamertag);

        std::string description = !gamertag.empty() ? gamertag : "Success";

        kernel_state()
            ->emulator()
            ->display_window()
            ->app_context()
            .CallInUIThread([imgui_drawer, description]() {
              new xe::ui::HostNotificationWindow(imgui_drawer, "Removed Friend",
                                                 description, 0);
            });
      }
    }
  }

  if (!are_friends && !is_self) {
    if (ui::FriendPopupButton(add_label.c_str(), half_width_btn,
                              presentation)) {
      bool added = kernel_state()->friends_manager()->AddFriend(profile->xuid(),
                                                                friend_xuid);

      const std::string gamertag(presence.Gamertag);

      std::string description = !gamertag.empty() ? gamertag : "Success";

      if (!added) {
        description = "Failed!";
      }

      kernel_state()
          ->emulator()
          ->display_window()
          ->app_context()
          .CallInUIThread([imgui_drawer, description]() {
            new xe::ui::HostNotificationWindow(imgui_drawer, "Added Friend",
                                               description, 0);
          });
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Add Friend");
    }
  }
  ImGui::EndGroup();

  ImVec2 buttons_row_size = ImGui::GetItemRectSize();
  ImVec2 drawing_end_position = ImGui::GetCursorPos();

  if (selected_xuid_) {
    ImGui::SetCursorPos(
        ImVec2(start_drawing_position.x + 2, start_drawing_position.y));

    const std::string selectable_label =
        std::format("##Selectable{}", friend_xuid_str);
    const std::string context_label =
        std::format("Friend Menu##{}", friend_xuid_str);

    auto selectable_area =
        ImVec2(buttons_row_size.x - (xe::ui::default_image_icon_size.x + 14),
               friend_info_drawing_end_position.y - start_drawing_position.y);

    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(50, 100, 200, 50));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0, 0, 0, 0));
    if (ImGui::Selectable(selectable_label.c_str(), false,
                          ImGuiSelectableFlags_NoAutoClosePopups,
                          selectable_area)) {
      *selected_xuid_ = friend_xuid;
    }
    ImGui::PopStyleColor(2);

    if (ImGui::BeginPopupContextItem(context_label.c_str())) {
      if (ImGui::BeginMenu("Copy")) {
        if (ImGui::MenuItem("Gamertag")) {
          ImGui::SetClipboardText(presence.Gamertag);
        }

        if (!presentation) {
          ImGui::Separator();
        }

        if (ImGui::MenuItem("XUID Online")) {
          ImGui::SetClipboardText(fmt::format("{:016X}", friend_xuid).c_str());
        }

        ImGui::EndMenu();
      }
      ImGui::EndPopup();
    }

    ImGui::SetCursorPos(drawing_end_position);
  }

  return true;
}

bool xeDrawAddFriend(xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile,
                     ui::AddFriendArgs& args,
                     const ui::FriendDialogPresentation* presentation) {
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();

  if (!args.add_friend_open) {
    args.add_friend_first_draw = false;
  }

  if (!presentation) {
    ImGui::SetNextWindowContentSize(ImVec2(200, 0));
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  }
  if (ui::BeginFriendPopup("Add Friend", &args.add_friend_open,
                           ImGuiWindowFlags_AlwaysAutoResize, presentation)) {
    if (ui::ConsumeFriendPopupBackPress(presentation)) {
      args.add_friend_open = false;
      ImGui::CloseCurrentPopup();
      ui::EndFriendPopup(presentation);
      return true;
    }

    ImGui::SetWindowFontScale(1.05f);
    const bool opening_input =
        presentation && ui::FriendPopupOpeningInputHeld();

    ImVec2 btn_size = ImVec2(ImGui::GetContentRegionAvail().x,
                             presentation ? ImGui::GetFrameHeight() : 25.0f);

    bool max_friends = kernel_state()->friends_manager()->GetFriendsCount(
                           profile->xuid()) >= X_ONLINE_MAX_FRIENDS;

    if (max_friends) {
      ImGui::Text("Max Friends Reached!");
      if (!presentation) {
        ImGui::Separator();
      }
    } else if (args.are_friends) {
      ImGui::Text("Friend Added!");
      if (!presentation) {
        ImGui::Separator();
      }
    }

    const std::string xuid_string = std::string(args.add_xuid_);

    uint64_t xuid = 0;

    if (xuid_string.length() == 16) {
      if (xuid_string.starts_with("0009")) {
        xuid = string_util::from_string<uint64_t>(xuid_string, true);

        args.valid_xuid = IsOnlineXUID(xuid);
        args.are_friends =
            kernel_state()->friends_manager()->IsFriend(profile->xuid(), xuid);
      }

      if (!args.valid_xuid) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 50, 50, 255));
        if (xuid_string.starts_with("E")) {
          ImGui::Text("This is an offline XUID!");
        } else {
          ImGui::Text("Invalid XUID!");
        }
        ImGui::PopStyleColor();

        if (!presentation) {
          ImGui::Separator();
        }
      }
    } else {
      args.valid_xuid = false;
      args.are_friends = false;
    }

    ImGui::Text("Friend's Online XUID:");

    ImGui::SameLine();

    const float window_width = ImGui::GetContentRegionAvail().x;

    const size_t friends_count =
        kernel_state()->friends_manager()->GetFriendsCount(profile->xuid());

    const std::string friends_total = fmt::format("{}/100", friends_count);

    ImGui::SetCursorPosX((ImGui::GetCursorPosX() + window_width -
                          ImGui::CalcTextSize(friends_total.c_str()).x));

    ImGui::Text(friends_total.c_str());

    const bool focus_input = ImGui::IsWindowAppearing();

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputTextWithHint("##AddFriend", "0009XXXXXXXXXXXX", args.add_xuid_,
                             sizeof(args.add_xuid_),
                             ImGuiInputTextFlags_CharsHexadecimal |
                                 ImGuiInputTextFlags_CharsUppercase);
    if (focus_input) {
      ImGui::SetItemDefaultFocus();
    }
    ImGui::PopItemWidth();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Right Click/Gamepad A");
    }

    if (!opening_input &&
        ((ImGui::IsItemFocused() &&
          ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_GamepadFaceDown, false)) ||
         (presentation && ImGui::IsItemHovered() &&
          ImGui::IsMouseClicked(ImGuiMouseButton_Right)))) {
      ImGui::OpenPopup("XUID Options##AddFriendContexts");
    }

    if (presentation
            ? ui::BeginFriendPopup("XUID Options##AddFriendContexts", nullptr,
                                   0, presentation)
            : ImGui::BeginPopupContextItem("XUID Options##AddFriendContexts")) {
      args.add_friend_context_open = true;

      const bool back = ui::ConsumeFriendPopupBackPress(presentation);
      if (back) {
        ImGui::CloseCurrentPopup();
      }
      const ImVec2 option_size(ImGui::GetContentRegionAvail().x,
                               ImGui::GetFrameHeight());
      ImGui::BeginDisabled(back ||
                           (presentation && ui::FriendPopupOpeningInputHeld()));
      if (presentation
              ? ui::FriendPopupButton("Paste", option_size, presentation)
              : ImGui::MenuItem("Paste")) {
        if (presentation) {
          ImGui::CloseCurrentPopup();
        }
        const char* clipboard = ImGui::GetClipboardText();

        if (clipboard) {
          xe::string_util::copy_truncating(args.add_xuid_, clipboard,
                                           sizeof(args.add_xuid_));
        }
      }

      if (!presentation) {
        ImGui::Separator();
      }

      if (presentation
              ? ui::FriendPopupButton("Clear", option_size, presentation)
              : ImGui::MenuItem("Clear")) {
        if (presentation) {
          ImGui::CloseCurrentPopup();
        }
        memset(args.add_xuid_, 0, sizeof(args.add_xuid_));
      }

      ImGui::EndDisabled();

      ui::EndFriendPopup(presentation);
    } else {
      args.add_friend_context_open = false;
    }

    ImGui::BeginDisabled(!args.valid_xuid || args.are_friends || max_friends);
    if (ui::FriendPopupButton("Add", btn_size, presentation)) {
      args.added_friend =
          kernel_state()->friends_manager()->AddFriend(profile->xuid(), xuid);

      std::string desc = xuid_string;

      if (!args.added_friend) {
        desc = "Failed!";
      }

      kernel_state()
          ->emulator()
          ->display_window()
          ->app_context()
          .CallInUIThread([imgui_drawer, desc]() {
            new xe::ui::HostNotificationWindow(imgui_drawer, "Added Friend",
                                               desc, 0);
          });
    }
    ImGui::EndDisabled();

    ui::EndFriendPopup(presentation);
  }

  return true;
}

bool xeDrawFriendsContent(
    xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile,
    ui::FriendsContentArgs& args, std::vector<X_ONLINE_FRIEND>& presences,
    std::map<uint64_t, std::shared_ptr<xe::ui::ImmediateTexture>>&
        immediate_gamerpics) {
  if (!profile) {
    return false;
  }

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();

  ui::FriendDialogPresentation guide;
  const ui::FriendDialogPresentation* presentation = nullptr;
#if XE_PLATFORM_WINRT
  args.guide_style = true;
#endif
  if (args.guide_style) {
    guide = xeMakeNetplayGuidePresentation(imgui_drawer);
    presentation = &guide;
  } else {
    ImGui::SetNextWindowSizeConstraints(ImVec2(400, 205), ImVec2(400, 600));
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  }
  if (ui::BeginFriendPopup("Friends", &args.friends_open,
                           ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_AlwaysVerticalScrollbar,
                           presentation)) {
    ImGui::SetWindowFontScale(1.05f);
    const bool opening_input =
        presentation && ui::FriendPopupOpeningInputHeld();

    if (ui::ConsumeFriendPopupBackPress(presentation)) {
      args.friends_open = false;
      ImGui::CloseCurrentPopup();
      ui::EndFriendPopup(presentation);
      return true;
    }

    const float window_width = ImGui::GetContentRegionAvail().x;

    float btn_height = presentation ? ImGui::GetFrameHeight() : 25.0f;
    float btn_width =
        (window_width * 0.5f) - (ImGui::GetStyle().ItemSpacing.x * 0.5f);
    ImVec2 half_width_btn = ImVec2(btn_width, btn_height);

    ImGui::Text("Search:");

    const bool focus_search = args.first_draw;
    args.first_draw = false;

    ImVec2 search_pos_input_start = ImGui::GetCursorPos();

    args.filter.Draw("##Search", window_width);
    if (focus_search) {
      // Select the search field without entering text-edit mode, which would
      // consume gamepad navigation intended for the friends list.
      ImGui::SetItemDefaultFocus();
    }

    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Right Click/Gamepad A");
    }

    ImVec2 search_pos_input_end = ImGui::GetCursorPos();

    if (!opening_input &&
        ((ImGui::IsItemFocused() &&
          ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_GamepadFaceDown, false)) ||
         (presentation && ImGui::IsItemHovered() &&
          ImGui::IsMouseClicked(ImGuiMouseButton_Right)))) {
      ImGui::OpenPopup("Search Options##SearchFilter");
    }

    if (presentation
            ? ui::BeginFriendPopup("Search Options##SearchFilter", nullptr, 0,
                                   presentation)
            : ImGui::BeginPopupContextItem("Search Options##SearchFilter")) {
      args.add_friend_args.search_filter_context_open = true;

      const bool back = ui::ConsumeFriendPopupBackPress(presentation);
      if (back) {
        ImGui::CloseCurrentPopup();
      }
      const ImVec2 option_size(ImGui::GetContentRegionAvail().x,
                               ImGui::GetFrameHeight());
      ImGui::BeginDisabled(back ||
                           (presentation && ui::FriendPopupOpeningInputHeld()));
      if (presentation
              ? ui::FriendPopupButton("Paste", option_size, presentation)
              : ImGui::MenuItem("Paste")) {
        if (presentation) {
          ImGui::CloseCurrentPopup();
        }
        const char* clipboard = ImGui::GetClipboardText();

        if (clipboard) {
          xe::string_util::copy_truncating(args.filter.InputBuf, clipboard,
                                           sizeof(args.filter.InputBuf));

          args.filter.Build();
        }
      }

      if (!presentation) {
        ImGui::Separator();
      }

      if (presentation
              ? ui::FriendPopupButton("Clear", option_size, presentation)
              : ImGui::MenuItem("Clear")) {
        if (presentation) {
          ImGui::CloseCurrentPopup();
        }
        memset(args.filter.InputBuf, 0, sizeof(args.filter.InputBuf));
        args.filter.Build();
      }

      ImGui::EndDisabled();

      ui::EndFriendPopup(presentation);
    } else {
      args.add_friend_args.search_filter_context_open = false;
    }

    if (std::string(args.filter.InputBuf).empty()) {
      ImGui::SetCursorPos(ImVec2(search_pos_input_start.x + 4,
                                 search_pos_input_start.y + 3.5f));
      ImGui::TextDisabled("Gamertag or XUID...");
    }

    ImGui::SetCursorPos(search_pos_input_end);

    const size_t friends_count =
        kernel_state()->friends_manager()->GetFriendsCount(profile->xuid());

    const std::string friends_total = fmt::format("{}/100", friends_count);

    ImGui::SetCursorPosX((ImGui::GetCursorPosX() + window_width -
                          ImGui::CalcTextSize(friends_total.c_str()).x));
    ImGui::Text(friends_total.c_str());

    ImGui::SetCursorPosY((ImGui::GetCursorPosY() - ImGui::GetTextLineHeight()) -
                         4);

    ImGui::Text("Filters:");

    ImGui::Checkbox("Joinable", &args.filter_joinable);
    if (!presentation) {
      ImGui::SameLine();
    }
    ImGui::Checkbox("Same Game", &args.filter_title);
    if (!presentation) {
      ImGui::SameLine();
    }
    ImGui::Checkbox("Hide Offline", &args.filter_offline);

    ImGui::Spacing();
    ImGui::Spacing();

    if (ui::FriendPopupButton(
            "Add Friend", ImVec2(ImGui::GetContentRegionAvail().x, btn_height),
            presentation)) {
      args.add_friend_args.add_friend_open = true;
      ImGui::OpenPopup("Add Friend");
    }

    ImGui::BeginDisabled(!friends_count);
    if (ui::FriendPopupButton("Refresh", half_width_btn, presentation)) {
      args.refresh_presence = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!friends_count);
    if (ui::FriendPopupButton("Remove All", half_width_btn, presentation)) {
      ImGui::OpenPopup("Remove All Friends");
    }
    ImGui::EndDisabled();

    xeDrawAddFriend(imgui_drawer, profile, args.add_friend_args, presentation);

    if (args.add_friend_args.added_friend) {
      args.refresh_presence = true;
      args.add_friend_args.added_friend = false;
    }

    ImGui::Spacing();
    if (!presentation) {
      ImGui::Separator();
    }
    ImGui::Spacing();

    uint64_t removed_xuid = 0;
    bool open_friend_details = false;
    const bool draw_list =
        !presentation ||
        ImGui::BeginChild(
            "##FriendsList",
            ImVec2(0.0f, std::max(1.0f, ImGui::GetContentRegionAvail().y)),
            ImGuiChildFlags_NavFlattened);
    if (draw_list) {
      for (auto& presence : presences) {
        bool filter_gamertags = args.filter.PassFilter(presence.Gamertag);
        bool filter_xuid = args.filter.PassFilter(
            fmt::format("{:016X}", presence.xuid.get()).c_str());

        if (filter_gamertags || filter_xuid) {
          if (profile->GetOnlineXUID() == presence.xuid.get()) {
            continue;
          }

          const bool same_title =
              presence.title_id.get() == kernel_state()->title_id();

          if (args.filter_joinable &&
              (!presence.session_id.as_uint64() || !same_title)) {
            continue;
          }

          if (args.filter_title && !same_title) {
            continue;
          }

          if (args.filter_offline &&
              (!presence.state.get() || !IsValidXUID(presence.xuid.get()))) {
            continue;
          }

          std::shared_ptr<xe::ui::ImmediateTexture> gamerpic = {};

          if (immediate_gamerpics.contains(presence.xuid.get())) {
            gamerpic = immediate_gamerpics.at(presence.xuid.get());
          }

          if (presentation) {
            const std::string label = fmt::format(
                "{}##{:016X}", presence.Gamertag, presence.xuid.get());
            if (ImGui::Selectable(label.c_str(), false,
                                  ImGuiSelectableFlags_NoAutoClosePopups,
                                  ImVec2(ImGui::GetContentRegionAvail().x,
                                         ImGui::GetFrameHeight()))) {
              args.selected_friend_xuid = presence.xuid.get();
              args.friend_details_open = true;
              open_friend_details = true;
            }
          } else {
            uint64_t selected_xuid_ = 0;
            xeDrawFriendContent(imgui_drawer, profile, gamerpic, presence,
                                &selected_xuid_, &removed_xuid, presentation);
          }

          if (removed_xuid) {
            break;
          }

          if (!presentation) {
            ImGui::Separator();
          }
          ImGui::Spacing();
          ImGui::Spacing();
        }
      }
    }
    if (presentation) {
      ImGui::EndChild();
    }

    if (open_friend_details) {
      ImGui::OpenPopup("Friend Details");
    }
    if (presentation &&
        ui::BeginFriendPopup("Friend Details", &args.friend_details_open, 0,
                             presentation)) {
      if (ui::ConsumeFriendPopupBackPress(presentation)) {
        args.friend_details_open = false;
        ImGui::CloseCurrentPopup();
      } else {
        const auto selected = std::find_if(
            presences.begin(), presences.end(), [&args](const auto& presence) {
              return presence.xuid.get() == args.selected_friend_xuid;
            });
        if (selected != presences.end()) {
          std::shared_ptr<xe::ui::ImmediateTexture> gamerpic;
          if (immediate_gamerpics.contains(args.selected_friend_xuid)) {
            gamerpic = immediate_gamerpics.at(args.selected_friend_xuid);
          }
          ImGui::BeginDisabled(ui::FriendPopupOpeningInputHeld());
          xeDrawFriendContent(imgui_drawer, profile, gamerpic, *selected,
                              nullptr, &removed_xuid, presentation);
          ImGui::EndDisabled();
          if (removed_xuid) {
            args.friend_details_open = false;
            ImGui::CloseCurrentPopup();
          }
        } else {
          ImGui::TextWrapped("This friend is no longer in the list.");
        }
      }
      ui::EndFriendPopup(presentation);
    }

    // Drawing may remove a friend. Erase only after leaving the range-for so
    // its iterators and the current presence reference remain valid.
    if (removed_xuid) {
      std::erase_if(presences, [removed_xuid](const X_ONLINE_FRIEND& presence) {
        return presence.xuid.get() == removed_xuid;
      });
    }

    if (ui::DrawFriendRemovalConfirmation(
            "Remove All Friends", "Remove all friends?", presentation)) {
      kernel_state()->friends_manager()->ClearFriends(profile->xuid());

      kernel_state()
          ->emulator()
          ->display_window()
          ->app_context()
          .CallInUIThread([imgui_drawer]() {
            new xe::ui::HostNotificationWindow(
                imgui_drawer, "Removed All Friends", "Success", 0);
          });
    }

    ui::EndFriendPopup(presentation);
  }

  return true;
}

bool xeDrawSessionContent(xe::ui::ImGuiDrawer* imgui_drawer,
                          UserProfile* profile,
                          std::unique_ptr<SessionObjectJSON>& session,
                          const ui::FriendDialogPresentation* presentation) {
  if (!profile || !kernel_state()->emulator()->is_title_open()) {
    return false;
  }
  const float content_left = ImGui::GetCursorPosX();
  const uint32_t user_index =
      kernel_state()->xam_state()->GetUserIndexAssignedToProfileFromXUID(
          profile->GetOnlineXUID());

  const auto& title_version = kernel_state()->emulator()->title_version();

  const auto& media_id = kernel_state()
                             ->GetExecutableModule()
                             ->xex_module()
                             ->opt_execution_info()
                             ->media_id;

  const std::string mediaId_str = fmt::format("{:08X}", media_id.get());

  bool version_mismatch = title_version != session->Version();
  bool media_id_mismatch = mediaId_str != session->MediaID();

  uint32_t num_players =
      session->FilledPublicSlotsCount() + session->FilledPrivateSlotsCount();

  ImGui::Text(fmt::format("Players: {}", num_players).c_str());

  if (!session->Version().empty()) {
    ImGui::Text(fmt::format("Version: {}", session->Version()).c_str());
  }

  if (!session->MediaID().empty()) {
    ImGui::Text(fmt::format("Media ID: {}", session->MediaID()).c_str());
  }

  ImGui::Text(fmt::format("Open Private Slots: {}",
                          session->OpenPrivateSlotsCount().get())
                  .c_str());
  ImGui::Text(fmt::format("Open Public Slots: {}",
                          session->OpenPublicSlotsCount().get())
                  .c_str());

  ImGui::Spacing();
  ImGui::Spacing();

  const std::string join_label =
      std::format("Join Session##{}", session->SessionID());

  bool caller = MacAddress(session->MacAddress()) == GetConsoleMacAddress();

  std::string version_text = "Version mismatch!";
  std::string media_text = "Media ID mismatch!";

  auto version_width_btn = (ImGui::GetContentRegionAvail().x -
                            ImGui::CalcTextSize(version_text.c_str()).x) *
                           0.5f;

  auto media_id_width_btn = (ImGui::GetContentRegionAvail().x -
                             ImGui::CalcTextSize(media_text.c_str()).x) *
                            0.5f;

  ImGui::SetCursorPosX(content_left + std::max(0.0f, version_width_btn));
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 50, 50, 255));
  if (version_mismatch && !session->Version().empty()) {
    ImGui::Text("Version mismatch!");
  }

  ImGui::SetCursorPosX(content_left + std::max(0.0f, media_id_width_btn));
  if (media_id_mismatch && !session->MediaID().empty()) {
    ImGui::Text("Media ID mismatch!");
  }
  ImGui::PopStyleColor();

  ImGui::Spacing();
  ImGui::Spacing();

  ImGui::SetCursorPosX(content_left);

  // What is player presence session is null?
  ImGui::BeginDisabled(!session->SessionID_UInt() || caller);
  if (ui::FriendPopupButton(
          join_label.c_str(),
          ImVec2(ImGui::GetContentRegionAvail().x,
                 presentation ? ImGui::GetFrameHeight() : 25.0f),
          presentation)) {
    X_INVITE_INFO invite = {};

    invite.xuid_invitee = profile->GetOnlineXUID();
    invite.xuid_inviter = session->XUID_UInt();
    invite.title_id = kernel_state()->title_id();
    invite.from_game_invite = true;

    profile->SetSelfInvite(invite);

    kernel_state()->BroadcastNotification(kXNotificationLiveInviteAccepted,
                                          user_index);
  }
  ImGui::EndDisabled();

  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    if (caller) {
      ImGui::SetTooltip("Cannot join session from the same console.");
    } else {
      ImGui::SetTooltip("Join gaming session");
    }
  }

  return true;
}

bool xeDrawSessionsContent(
    xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile,
    ui::SessionsContentArgs& sessions_args,
    std::vector<std::unique_ptr<SessionObjectJSON>>* sessions) {
  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ui::FriendDialogPresentation guide;
  const ui::FriendDialogPresentation* presentation = nullptr;
#if XE_PLATFORM_WINRT
  guide = xeMakeNetplayGuidePresentation(imgui_drawer);
  presentation = &guide;
#endif

  if (!presentation) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(300, 150), ImVec2(300, 600));
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  }
  if (ui::BeginFriendPopup("Sessions", &sessions_args.sessions_open,
                           ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_AlwaysVerticalScrollbar,
                           presentation)) {
    ImGui::SetWindowFontScale(1.05f);

    if (ui::ConsumeFriendPopupBackPress(presentation)) {
      sessions_args.sessions_open = false;
      ImGui::CloseCurrentPopup();
      ui::EndFriendPopup(presentation);
      return true;
    }

    bool in_game = kernel_state()->emulator()->title_id();

    if (in_game) {
      ImGui::Text(
          fmt::format("{}", kernel_state()->emulator()->title_name()).c_str());
    }

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::Text(
        fmt::format("Available Sessions: {}", sessions->size()).c_str());

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::Checkbox("Hide My Sessions", &sessions_args.filter_own);

    ImGui::Spacing();

    if (ui::FriendPopupButton(
            "Refresh",
            ImVec2(ImGui::GetContentRegionAvail().x,
                   presentation ? ImGui::GetFrameHeight() : 25.0f),
            presentation)) {
      sessions->clear();
      sessions_args.refresh_sessions = true;
    }

    if (!presentation) {
      ImGui::Separator();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    if (sessions_args.pending_sessions.valid() &&
        sessions_args.pending_sessions.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
      *sessions = sessions_args.pending_sessions.get();
    }

    if (sessions_args.refresh_sessions || sessions_args.refresh_sessions_sync) {
      auto fetch_sessions = []() {
        return kernel_state()->GetXboxLiveAPI()->GetTitleSessions();
      };
      if (sessions_args.refresh_sessions_sync) {
        *sessions = fetch_sessions();
        sessions_args.refresh_sessions_sync = false;
      } else if (!sessions_args.pending_sessions.valid()) {
        // Publish results on the UI thread. A detached writer can outlive its
        // dialog or race the list being rendered/cleared when B closes it.
        sessions_args.pending_sessions =
            std::async(std::launch::async, fetch_sessions);
      }
      sessions_args.refresh_sessions = false;
    }

    for (auto& session : *sessions) {
      bool caller = MacAddress(session->MacAddress()) == GetConsoleMacAddress();
      if (sessions_args.filter_own && caller) {
        continue;
      }

      xeDrawSessionContent(imgui_drawer, profile, session, presentation);

      if (!presentation) {
        ImGui::Separator();
      }
      ImGui::Spacing();
      ImGui::Spacing();
    }

    ui::EndFriendPopup(presentation);
  }

  return true;
}

bool xeDrawMyDeletedProfiles(
    xe::ui::ImGuiDrawer* imgui_drawer, ui::MyDeletedProfilesArgs& args,
    std::map<uint64_t, std::string>* deleted_profiles) {
  if (!deleted_profiles) {
    return false;
  }

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();
  ui::FriendDialogPresentation guide;
  const ui::FriendDialogPresentation* presentation = nullptr;
#if XE_PLATFORM_WINRT
  guide = xeMakeNetplayGuidePresentation(imgui_drawer);
  presentation = &guide;
#endif

  float btn_height = 25;
  if (!presentation) {
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(250, 115), ImVec2(250, 415));
  }
  if (ui::BeginFriendPopup("Deleted Profiles", &args.deleted_profiles_open,
                           ImGuiWindowFlags_AlwaysAutoResize, presentation)) {
    if (ui::ConsumeFriendPopupBackPress(presentation)) {
      args.deleted_profiles_open = false;
      ImGui::CloseCurrentPopup();
      ui::EndFriendPopup(presentation);
      return true;
    }

    float btn_width = (ImGui::GetContentRegionAvail().x * 0.5f) -
                      (ImGui::GetStyle().ItemSpacing.x * 0.5f);
    ImVec2 btn_size = ImVec2(btn_width, btn_height);

    const std::string desc =
        fmt::format("Deleted Profiles: {}", deleted_profiles->size());

    ImVec2 desc_size = ImGui::CalcTextSize(desc.c_str());

    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - desc_size.x) * 0.5f);
    ImGui::Text(desc.c_str());
    if (!presentation) {
      ImGui::Separator();
    }
    ImGui::Spacing();

    for (const auto& [xuid, gamertag] : *deleted_profiles) {
      ImGui::Spacing();
      ImGui::Spacing();

      std::string xuid_str = fmt::format("XUID: {:016X}", xuid);
      std::string gamertag_str = fmt::format("Gamertag: {}", gamertag);

      ImGui::Text(xuid_str.c_str());
      ImGui::Text(gamertag_str.c_str());

      if (!presentation) {
        ImGui::Separator();
      }
    }

    ui::EndFriendPopup(presentation);
  }

  return true;
}

void xeDrawUPnPAndPorts(xe::ui::ImGuiDrawer* imgui_drawer,
                        ui::UPnPAndPortsArgs& args, UPnP* upnp) {
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 center = viewport->GetCenter();
  ui::FriendDialogPresentation guide;
  const ui::FriendDialogPresentation* presentation = nullptr;
#if XE_PLATFORM_WINRT
  guide = xeMakeNetplayGuidePresentation(imgui_drawer);
  presentation = &guide;
#endif

  float btn_height_padding = ImGui::GetStyle().FramePadding.x * 2.5f;
  float btn_width_padding = ImGui::GetStyle().FramePadding.x * 5.0f;

  auto centre_text = [presentation](std::string text) {
    ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
    if (!presentation) {
      ImGui::SetCursorPosX((ImGui::GetWindowWidth() - text_size.x) * 0.5f);
    }
    ImGui::TextWrapped("%s", text.c_str());
    if (!presentation) {
      ImGui::Separator();
    }
  };

  if (args.first_draw) {
    args.first_draw = false;

    if (cvars::upnp && upnp) {
      upnp->Start();
    }
  }

  float btn_height = 25;
  if (!presentation) {
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(500, 0), ImVec2(500, 500));
  }
  if (ui::BeginFriendPopup("UPnP and Ports", &args.dialog_open,
                           ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove,
                           presentation)) {
    if (ui::ConsumeFriendPopupBackPress(presentation)) {
      args.dialog_open = false;
      ImGui::CloseCurrentPopup();
      ui::EndFriendPopup(presentation);
      return;
    }

    std::string lbl_toggle_upnp = cvars::upnp ? "Disable UPnP" : "Enable UPnP";
    ImVec2 toggle_upnp_lbl_size = ImGui::CalcTextSize(lbl_toggle_upnp.c_str());
    ImVec2 toggle_upnp_btn_size =
        ImVec2(toggle_upnp_lbl_size.x + btn_width_padding,
               toggle_upnp_lbl_size.y + btn_height_padding);

    if (presentation) {
      toggle_upnp_btn_size =
          ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
    }
    if (ui::FriendPopupButton(lbl_toggle_upnp.c_str(), toggle_upnp_btn_size,
                              presentation)) {
      const bool upnp_state = !cvars::upnp;

      UPnP::SetUPnPState(upnp_state);

      if (upnp) {
        if (upnp_state) {
          if (!upnp->IsActive()) {
            upnp->Start();
          }

          upnp->OpenTrackedPorts();
        } else {
          upnp->CloseOpenPorts();
        }
      }
    }

    if (cvars::upnp && upnp && upnp->IsActive() &&
        kernel_state()->emulator()->is_title_open()) {
      std::string lbl_refresh_ports = "Refresh Ports";
      ImVec2 refresh_ports_lbl_size =
          ImGui::CalcTextSize(lbl_refresh_ports.c_str());
      ImVec2 refresh_upnp_btn_size =
          ImVec2(refresh_ports_lbl_size.x + btn_width_padding,
                 refresh_ports_lbl_size.y + btn_height_padding);

      if (presentation) {
        refresh_upnp_btn_size =
            ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
      } else {
        ImGui::SameLine();
      }

      ImGui::BeginDisabled(upnp->GetOpenedPorts().empty());
      if (ui::FriendPopupButton("Refresh Ports", refresh_upnp_btn_size,
                                presentation)) {
        upnp->RefreshPorts();
      }
      ImGui::EndDisabled();
    }

    if (!presentation) {
      ImGui::SameLine();
    }

    std::string lbl_wiki_desc = "UPnP & Port Forwarding";
    ImVec2 wiki_desc_size = ImGui::CalcTextSize(lbl_wiki_desc.c_str());

    const float pos_x = ImGui::GetCursorPosX();
    if (!presentation) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                           ImGui::GetContentRegionAvail().x - wiki_desc_size.x);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() -
                           ImGui::GetStyle().ItemSpacing.y);
    }
    ImGui::TextLinkOpenURL(lbl_wiki_desc.c_str(),
                           "https://github.com/AdrianCassar/xenia-canary/"
                           "wiki/UPnP-&-Port-Forwarding");
    ImGui::SetCursorPosX(pos_x);

    ImGui::Spacing();
    if (!presentation) {
      ImGui::Separator();
    }

    if (!upnp) {
      centre_text("UPnP failed to initialize!");
      ui::EndFriendPopup(presentation);
      return;
    }

    if (cvars::upnp) {
      if (upnp->IsActive()) {
        ImGui::Text("UPnP device found!");
        ImGui::Spacing();
      } else {
        ImGui::TextWrapped(
            "No UPnP devices discovered! Please check UPnP is supported or "
            "enabled on your router.");
        ImGui::Spacing();
      }
    }

    if (!kernel_state()->emulator()->is_title_open()) {
      centre_text("Game is not running!");
      ui::EndFriendPopup(presentation);
      return;
    }

    if (kernel_state()->emulator()->is_title_open()) {
      if (!cvars::upnp || !upnp->IsActive()) {
        if (!upnp->GetTrackedPorts().empty()) {
          ImGui::Text("Tracked Ports:");
          ImGui::Spacing();

          auto& tracked_ports = upnp->GetTrackedPorts();

          if (ImGui::BeginTable(
                  "##PortsTable", 2,
                  ImGuiTableFlags_SizingStretchProp |
                      (presentation ? ImGuiTableFlags_None
                                    : ImGuiTableFlags_Borders))) {
            ImGui::TableSetupColumn("Protocol");
            ImGui::TableSetupColumn("Port");

            ImGui::TableHeadersRow();

            for (const auto& [protocol, internal_ports] : tracked_ports) {
              for (const auto& internal_port : internal_ports) {
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::Text(protocol.c_str());

                ImGui::TableNextColumn();
                ImGui::Text(fmt::format("{}", internal_port).c_str());
              }
            }
            ImGui::EndTable();
          }
        } else {
          centre_text("No ports are tracked!");
        }
      } else if (upnp->IsActive()) {
        if (!upnp->GetTrackedPorts().empty()) {
          auto& tracked_ports = upnp->GetTrackedPorts();

          ImGui::Spacing();

          if (upnp->IsVariableLeaseSupported()) {
            ImGui::TextWrapped(
                "Your router supports variable UPnP lease times. UPnP ports "
                "have "
                "a lease time of 1 hour, every 45 minutes UPnP will reset the "
                "lease time to 1 hour.");
          } else {
            ImGui::TextWrapped(
                "Your router only supports permanent lease times.");
          }

          ImGui::Spacing();

          ImGui::TextWrapped("All ports are closed upon exit.");

          ImGui::Spacing();
          ImGui::Spacing();

          if (presentation ||
              ImGui::BeginTable("##PortsTable", 5,
                                ImGuiTableFlags_SizingStretchProp |
                                    ImGuiTableFlags_Borders)) {
            if (!presentation) {
              ImGui::TableSetupColumn("Protocol");
              ImGui::TableSetupColumn("Port");
              ImGui::TableSetupColumn("Error Code");
              ImGui::TableSetupColumn("Status");
              ImGui::TableSetupColumn("Action",
                                      ImGuiTableColumnFlags_WidthFixed, 150.0f);

              ImGui::TableHeadersRow();
            }

            auto& port_results = upnp->GetPortBindingResults();
            const auto opened_ports = upnp->GetOpenedPorts();

            for (const auto& [protocol, internal_ports] : tracked_ports) {
              for (const auto& internal_port : internal_ports) {
                bool is_port_open = false;

                if (opened_ports.contains(protocol)) {
                  if (opened_ports.at(protocol).contains(internal_port)) {
                    is_port_open = true;
                  }
                }

                int32_t error_code = -1;

                if (port_results.contains(protocol)) {
                  if (port_results.at(protocol).contains(internal_port)) {
                    error_code = port_results.at(protocol).at(internal_port);
                  }
                }

                if (presentation) {
                  ImGui::Text("%s %u", protocol.c_str(), internal_port);
                  ImGui::Text("Error code: %d", error_code);
                } else {
                  ImGui::TableNextRow();
                  ImGui::TableNextColumn();
                  ImGui::TextUnformatted(protocol.c_str());
                  ImGui::TableNextColumn();
                  ImGui::Text("%u", internal_port);
                  ImGui::TableNextColumn();
                  ImGui::Text("%d", error_code);
                  ImGui::TableNextColumn();
                }
                std::string error_status;

                if (!is_port_open && error_code == -1) {
                  error_status = "Port Closed";
                } else {
                  if (error_code < 0) {
                    error_status =
                        UPnP::GetMiniUPnPcErrorCodeToDesc(error_code);
                  } else {
                    error_status = UPnP::GetUPnPErrorCodeToDesc(error_code);
                  }
                }

                ImGui::TextWrapped(error_status.c_str());

                if (!presentation) {
                  ImGui::TableNextColumn();
                }

                float width = (ImGui::GetContentRegionAvail().x -
                               ImGui::GetStyle().ItemSpacing.x) /
                              2.0f;

                std::string lbl_open_port = "Open";
                ImVec2 open_port_lbl_size =
                    ImGui::CalcTextSize(lbl_open_port.c_str());
                ImVec2 open_ports_btn_size =
                    ImVec2(width, open_port_lbl_size.y + btn_height_padding);

                std::string open_lbl = fmt::format("{}## {}({})", lbl_open_port,
                                                   internal_port, protocol);

                ImGui::BeginDisabled(is_port_open);
                if (ui::FriendPopupButton(open_lbl.c_str(), open_ports_btn_size,
                                          presentation)) {
                  upnp->AddPort(upnp->GetLocalIP(), internal_port, protocol);
                }
                ImGui::EndDisabled();

                ImGui::SameLine();

                std::string lbl_close_port = "Close";
                ImVec2 close_port_lbl_size =
                    ImGui::CalcTextSize(lbl_close_port.c_str());
                ImVec2 close_port_btn_size =
                    ImVec2(width, close_port_lbl_size.y + btn_height_padding);

                std::string lbl_close = fmt::format(
                    "{}## {}({})", lbl_close_port, internal_port, protocol);

                ImGui::BeginDisabled(!is_port_open);
                if (ui::FriendPopupButton(lbl_close.c_str(),
                                          close_port_btn_size, presentation)) {
                  upnp->RemovePort(internal_port, protocol);
                }
                ImGui::EndDisabled();
                if (presentation) {
                  ImGui::Spacing();
                }
              }
            }
            if (!presentation) {
              ImGui::EndTable();
            }
          }
        } else {
          centre_text("No ports have opened!");
        }
      }
    }

    ui::EndFriendPopup(presentation);
  }
}

X_RESULT xeXamShowSigninUI(uint32_t user_index, uint32_t users_needed,
                           uint32_t flags) {
  // Mask values vary. Probably matching user types? Local/remote?
  // Games seem to sit and loop until we trigger sign in notification.
  if (users_needed != 1 && users_needed != 2 && users_needed != 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (cvars::headless) {
    return xeXamDispatchHeadlessAsync([users_needed]() {
      std::map<uint8_t, uint64_t> xuids;

      for (uint32_t i = 0; i < XUserMaxUserCount; i++) {
        UserProfile* profile = kernel_state()->xam_state()->GetUserProfile(i);
        if (profile) {
          xuids[i] = profile->xuid();
          if (xuids.size() >= users_needed) {
            break;
          }
        }
      }

      kernel_state()->xam_state()->profile_manager()->LoginMultiple(xuids);
    });
  }

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  auto close = [](ui::SigninUI* dialog) -> void {};

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  return xeXamDispatchDialogAsync<ui::SigninUI>(
      new ui::SigninUI(
          imgui_drawer, kernel_state()->xam_state()->profile_manager(),
          emulator->input_system()->GetLastUsedSlot(), users_needed, flags),
      close);
}

X_RESULT xeXamShowCreateProfileUIEx(uint32_t user_index, dword_t flag,
                                    char* unkn2_ptr) {
  Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

  if (cvars::headless) {
    return X_ERROR_SUCCESS;
  }

  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  auto close = [](ui::CreateProfileUI* dialog) -> void {};

  return xeXamDispatchDialogAsync<ui::CreateProfileUI>(
      new ui::CreateProfileUI(imgui_drawer, emulator), close);
}

dword_result_t XamShowSigninUI_entry(dword_t users_needed, dword_t flags) {
  return xeXamShowSigninUI(XUserIndexAny, users_needed, flags);
}
DECLARE_XAM_EXPORT1(XamShowSigninUI, kUserProfiles, kImplemented);

dword_result_t XamShowSigninUIEx_entry(
    dword_t users_needed, dword_t flags,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  X_RESULT result = xeXamShowSigninUI(XUserIndexAny, users_needed, flags);
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamShowSigninUIEx, kUserProfiles, kSketchy);

dword_result_t XamShowNuiSigninUI_entry(dword_t unk, dword_t user_index,
                                        dword_t flags) {
  uint32_t users_needed = 1;
  uint32_t sent_flags = flags | static_cast<uint32_t>(SigninUiFlags::NUI);
  // xeXamNuiHudCheck(unk) = success then continue else return
  return xeXamShowSigninUI(user_index, users_needed, sent_flags);
}
DECLARE_XAM_EXPORT1(XamShowNuiSigninUI, kUserProfiles, kSketchy);

dword_result_t XamShowSigninUIp_entry(dword_t user_index, dword_t users_needed,
                                      dword_t flags) {
  return xeXamShowSigninUI(user_index, users_needed, flags);
}
DECLARE_XAM_EXPORT1(XamShowSigninUIp, kUserProfiles, kImplemented);

dword_result_t XamShowCreateProfileUIEx_entry(dword_t user_index, dword_t flag,
                                              lpstring_t unkn2_ptr) {
  return xeXamShowCreateProfileUIEx(user_index, flag, unkn2_ptr);
}
DECLARE_XAM_EXPORT1(XamShowCreateProfileUIEx, kUserProfiles, kImplemented);

dword_result_t XamShowCreateProfileUI_entry(dword_t user_index, dword_t flag) {
  return xeXamShowCreateProfileUIEx(user_index, flag, 0);
}
DECLARE_XAM_EXPORT1(XamShowCreateProfileUI, kUserProfiles, kImplemented);

dword_result_t XamShowAchievementsUI_entry(dword_t user_index,
                                           dword_t title_id) {
  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_NO_SUCH_USER;
  }

  uint32_t proper_title_id =
      title_id ? title_id.value()
               : kernel_state()->xam_state()->spa_info()->title_id();

  const auto info =
      kernel_state()->xam_state()->user_tracker()->GetUserTitleInfo(
          user->xuid(), proper_title_id);

  if (!info) {
    return X_ERROR_NO_SUCH_USER;
  }

  xe::ui::ImGuiDrawer* imgui_drawer =
      kernel_state()->emulator()->imgui_drawer();

  auto close = [](ui::GameAchievementsUI* dialog) -> void {};
  return xeXamDispatchDialogAsync<ui::GameAchievementsUI>(
      new ui::GameAchievementsUI(imgui_drawer, ImVec2(100.f, 100.f),
                                 &info.value(), user),
      close);
}
DECLARE_XAM_EXPORT1(XamShowAchievementsUI, kUserProfiles, kStub);

dword_result_t XamShowGamerCardUI_entry(dword_t user_index) {
  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_ACCESS_DENIED;
  }

  xe::ui::ImGuiDrawer* imgui_drawer =
      kernel_state()->emulator()->imgui_drawer();

  auto close = [](ui::GamercardUI* dialog) -> void {};
  return xeXamDispatchDialogAsync<ui::GamercardUI>(
      new ui::GamercardUI(kernel_state()->emulator()->display_window(),
                          imgui_drawer, kernel_state(), user->xuid()),
      close);
}
DECLARE_XAM_EXPORT1(XamShowGamerCardUI, kUserProfiles, kImplemented);

dword_result_t XamShowEditProfileUI_entry(dword_t user_index) {
  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_ACCESS_DENIED;
  }

  xe::ui::ImGuiDrawer* imgui_drawer =
      kernel_state()->emulator()->imgui_drawer();

  auto close = [](ui::GamercardUI* dialog) -> void {};
  return xeXamDispatchDialogAsync<ui::GamercardUI>(
      new ui::GamercardUI(kernel_state()->emulator()->display_window(),
                          imgui_drawer, kernel_state(), user->xuid()),
      close);
}
DECLARE_XAM_EXPORT1(XamShowEditProfileUI, kUserProfiles, kImplemented);

dword_result_t XamShowGamerCardUIForXUID_entry(dword_t user_index,
                                               qword_t xuid_player) {
  uint64_t xuid = xuid_player;

  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (IsGuestXUID(xuid)) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!xuid) {
    xuid = user->xuid();
  }

  if (!IsValidXUID(xuid)) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (IsOnlineXUID(xuid) &&
      !kernel_state()->xam_state()->user_tracker()->LoggedInToLive()) {
    return X_ERROR_ACCESS_DENIED;
  }

  if (xuid || xuid == user->xuid() || xuid == user->GetOnlineXUID()) {
    if (kernel_state()->xam_state()->IsUIActive()) {
      return X_ERROR_ACCESS_DENIED;
    }

    kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

    auto close = [](ui::GamercardFromXUIDUI* dialog) -> void {};

    const Emulator* emulator = kernel_state()->emulator();
    xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
    return xeXamDispatchDialogAsync<ui::GamercardFromXUIDUI>(
        new ui::GamercardFromXUIDUI(imgui_drawer, xuid, user), close);
  }

  return X_ERROR_INVALID_PARAMETER;
}
DECLARE_XAM_EXPORT1(XamShowGamerCardUIForXUID, kUserProfiles, kStub);

dword_result_t XamShowFriendsUI_entry(dword_t user_index) {
  if (user_index >= XUserMaxUserCount && user_index != XUserIndexAny) {
    return X_ERROR_FUNCTION_FAILED;
  }

  UserProfile* user = nullptr;

  if (user_index == XUserIndexAny) {
    if (kernel_state()
            ->xam_state()
            ->profile_manager()
            ->IsAnyProfileSignedIn()) {
      user =
          kernel_state()->xam_state()->GetUserProfile(static_cast<uint32_t>(0));
    }
  } else {
    user = kernel_state()->xam_state()->GetUserProfile(user_index);
  }

  if (!user) {
    return X_ERROR_FUNCTION_FAILED;
  }

  auto* xam_state = kernel_state()->xam_state();
  bool ui_inactive = false;
  if (!xam_state->is_xam_dialog_present_.compare_exchange_strong(ui_inactive,
                                                               true)) {
    XELOGD("XamShowFriendsUI: ignoring request while system UI is active");
    return X_ERROR_ACCESS_DENIED;
  }

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

  X_RESULT result = X_ERROR_FUNCTION_FAILED;
  // Reserve the UI before dispatch so repeated guest calls cannot append
  // multiple FriendsUI instances to the same popup. Construct and install the
  // close callback together on the UI thread, before the drawer can use it.
  if (!emulator->display_window()->app_context().CallInUIThreadSynchronous(
          [imgui_drawer, user, &result]() {
            auto close = [](ui::FriendsUI* dialog) -> void {};
            result = xeXamDispatchDialogAsync<ui::FriendsUI>(
                new ui::FriendsUI(imgui_drawer, user), close);
            XELOGI("XamShowFriendsUI: opened Friends for guest request");
          })) {
    xam_state->is_xam_dialog_present_.store(false);
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamShowFriendsUI, kUserProfiles, kImplemented);

dword_result_t XamShowCommunitySessionsUI_entry(dword_t user_index,
                                                dword_t social_sessions_flags) {
  if (user_index >= XUserMaxUserCount && user_index != XUserIndexAny) {
    return X_ERROR_FUNCTION_FAILED;
  }

  UserProfile* user = nullptr;

  if (user_index == XUserIndexAny) {
    if (kernel_state()
            ->xam_state()
            ->profile_manager()
            ->IsAnyProfileSignedIn()) {
      user =
          kernel_state()->xam_state()->GetUserProfile(static_cast<uint32_t>(0));
    }
  } else {
    user = kernel_state()->xam_state()->GetUserProfile(user_index);
  }

  if (!user) {
    return X_ERROR_FUNCTION_FAILED;
  }

  auto* xam_state = kernel_state()->xam_state();
  bool ui_inactive = false;
  if (!xam_state->is_xam_dialog_present_.compare_exchange_strong(ui_inactive,
                                                                 true)) {
    return X_ERROR_ACCESS_DENIED;
  }

  const Emulator* emulator = kernel_state()->emulator();
  xe::ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  X_RESULT result = X_ERROR_FUNCTION_FAILED;
  if (!emulator->display_window()->app_context().CallInUIThreadSynchronous(
          [imgui_drawer, user, &result]() {
            auto close = [](ui::ShowCommunitySessionsUI* dialog) -> void {};
            result = xeXamDispatchDialogAsync<ui::ShowCommunitySessionsUI>(
                new ui::ShowCommunitySessionsUI(imgui_drawer, user), close);
          })) {
    xam_state->is_xam_dialog_present_.store(false);
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamShowCommunitySessionsUI, kUserProfiles, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(UI);
