/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_CRASH_REPORT_H_
#define XENIA_BASE_CRASH_REPORT_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"

// Reports why the process is dying when nothing else would say: a host
// exception no handler takes, abort() (std::terminate), an invalid C runtime
// parameter or a pure virtual call. Each report goes to the log and to a crash
// file, with the host stack as module+offset (symbolize with the build's PDB)
// and, through the guest hooks, the guest thread, the guest code in the stack
// and what the guest called last.
namespace xe {
namespace crash_report {

// Formats into a fixed buffer without allocating: reports are written while
// the process is failing, possibly with its heap locked or broken.
class Writer {
 public:
  Writer(char* buffer, size_t capacity)
      : buffer_(buffer), capacity_(capacity) {}

  template <typename... Args>
  void Append(fmt::format_string<Args...> format, Args&&... args) {
    if (size_ >= capacity_) {
      return;
    }
    auto result = fmt::format_to_n(buffer_ + size_, capacity_ - size_, format,
                                   std::forward<Args>(args)...);
    size_ += std::min(result.size, capacity_ - size_);
  }

  void AppendText(std::string_view text) {
    size_t length = std::min(text.size(), capacity_ - size_);
    std::copy_n(text.data(), length, buffer_ + size_);
    size_ += length;
  }

  std::string_view view() const { return std::string_view(buffer_, size_); }
  size_t size() const { return size_; }

 private:
  char* buffer_;
  size_t capacity_;
  size_t size_ = 0;
};

// What the emulator can say about guest state. Every hook is optional. They
// run while the process is failing, so they must not allocate, and should not
// take locks the failing thread may hold. All but the first run on the
// reporter thread while the failing thread waits.
struct GuestHooks {
  // Runs on the failing thread, which may be out of stack: the guest thread
  // object of the calling thread, or null.
  void* (*current_guest_thread)() = nullptr;
  void (*describe_guest_thread)(void* guest_thread, Writer& out) = nullptr;
  // Whether a host address is in generated (JIT) code.
  bool (*is_generated_code)(uint64_t host_pc) = nullptr;
  // Unwind data of generated code: a RUNTIME_FUNCTION relative to
  // `*image_base`.
  void* (*lookup_unwind_info)(uint64_t host_pc, uint64_t* image_base) = nullptr;
  // The guest function and address that generated code came from.
  void (*describe_generated_code)(uint64_t host_pc, Writer& out) = nullptr;
  // Describes a host address in guest memory; false if it is not in it.
  bool (*describe_data_address)(uint64_t host_address, Writer& out) = nullptr;
  // What the guest did last on a host thread (its recent kernel calls).
  void (*describe_recent_activity)(uint32_t host_thread_id,
                                   Writer& out) = nullptr;
};

// Installs the handlers. Each report is also written to `report_path`,
// replacing the previous process's reports. Call once logging is up.
void Install(const std::filesystem::path& report_path,
             std::string_view build_description);
void SetGuestHooks(const GuestHooks& hooks);
void ClearGuestHooks();

// Appends "module+0xOFFSET" (or the guest code) for a host code address.
void DescribeHostAddress(uint64_t address, Writer& out);

// Logs the calling thread's host stack with `reason` as the heading, for exits
// that are not crashes but should say who asked for them.
void LogCurrentStack(std::string_view reason);

}  // namespace crash_report
}  // namespace xe

#endif  // XENIA_BASE_CRASH_REPORT_H_
