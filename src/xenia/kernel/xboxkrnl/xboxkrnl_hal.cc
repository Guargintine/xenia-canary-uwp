/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/emulator.h"
#include "xenia/kernel/smc.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

#if XE_PLATFORM_WINRT
#include "xenia-canary-uwp/XeniaUWP.h"
#endif

DECLARE_int32(avpack);

namespace xe {
namespace kernel {
namespace xboxkrnl {

constexpr std::array<std::string_view, 9> FirmwareReentryMessage = {
    "hard poweroff",
    "hard reset (video error)",
    "hard reset (used for dumpwritedump/frozen processor)",
    "hard reset",
    "power off (hard)",
    "power off (nice)",
    "Shut off (Lost Settings)",
    "Shut off (Frozen Console)",
    "Shut off",
};

void HalReturnToFirmware_entry(dword_t routine,
                               const ppc_context_t& ppc_context) {
  const std::string exitMessage = fmt::format(
      "Game requested a {} via HalReturnToFirmware",
      static_cast<size_t>(routine) < FirmwareReentryMessage.size()
          ? FirmwareReentryMessage[routine]
          : fmt::format("Reboot (Routine Code: {})", routine.value()));
  // Logged and flushed with what led to it: exit() can end the process before
  // the log writer gets to a line logged just before.
  shim::LogGuestEvent(exitMessage, ppc_context.value());
#if XE_PLATFORM_WINRT
  // Close Xenia the way XamLoaderTerminateTitle does.
  kernel_state()->emulator()->display_window()->app_context()
      .CallInUIThreadSynchronous([]() { UWP::RequestQuit(); });
  kernel_state()->TerminateTitle();
#else
  // TODO(benvank): diediedie much more gracefully
  // Not sure how to blast back up the stack in LLVM without exceptions, though.
  exit(0);
#endif
}
DECLARE_XBOXKRNL_EXPORT2(HalReturnToFirmware, kNone, kStub, kImportant);

dword_result_t HalGetCurrentAVPack_entry() { return cvars::avpack; }
DECLARE_XBOXKRNL_EXPORT1(HalGetCurrentAVPack, kNone, kImplemented);

void HalSendSMCMessage_entry(pointer_t<X_SMC_DATA> smc_message,
                             pointer_t<X_SMC_DATA> smc_response) {
  if (!smc_message) {
    return;
  }

  kernel_state()->smc()->CallCommand(smc_message, smc_response);
}
DECLARE_XBOXKRNL_EXPORT3(HalSendSMCMessage, kNone, kStub, kImportant,
                         kHighFrequency);

void HalOpenCloseODDTray_entry(dword_t open_close) {
  kernel_state()->smc()->SetTrayState(open_close ? X_DVD_TRAY_STATE::CLOSED
                                                 : X_DVD_TRAY_STATE::OPEN);
}
DECLARE_XBOXKRNL_EXPORT1(HalOpenCloseODDTray, kNone, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Hal);
