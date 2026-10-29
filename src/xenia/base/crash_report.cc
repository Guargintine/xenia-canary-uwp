/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/crash_report.h"

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

// The reporter is Windows only (crash_report_win.cc); elsewhere these do
// nothing beyond logging.
#if !XE_PLATFORM_WIN32
namespace xe {
namespace crash_report {

void Install(const std::filesystem::path&, std::string_view) {}
void SetGuestHooks(const GuestHooks&) {}
void ClearGuestHooks() {}

void DescribeHostAddress(uint64_t address, Writer& out) {
  out.Append("0x{:016X}", address);
}

void LogCurrentStack(std::string_view reason) { XELOGI("{}", reason); }

}  // namespace crash_report
}  // namespace xe
#endif  // !XE_PLATFORM_WIN32
