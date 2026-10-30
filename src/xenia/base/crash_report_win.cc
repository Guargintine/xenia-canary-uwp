/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/crash_report.h"

#include <signal.h>
#include <stdlib.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>

#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"

namespace xe {
namespace crash_report {

namespace {

// Status codes from ntstatus.h, which clashes with windows.h.
constexpr DWORD kStatusBadStack = 0xC0000028;
constexpr DWORD kStatusInvalidUnwindTarget = 0xC0000029;
constexpr DWORD kStatusHeapCorruption = 0xC0000374;
constexpr DWORD kStatusStackBufferOverrun = 0xC0000409;
constexpr DWORD kStatusAssertionFailure = 0xC0000420;
// A Visual C++ throw ('msc' with the error and customer bits).
constexpr DWORD kCppExceptionCode = 0xE06D7363;
// winnt.h FAST_FAIL_* codes.
constexpr unsigned int kFastFailInvalidArg = 5;
constexpr unsigned int kFastFailFatalAppExit = 7;

constexpr int kMaxFrames = 48;
constexpr int kMaxStackScanEntries = 24;
constexpr size_t kStackScanBytes = 16 * 1024;
constexpr uint32_t kMaxReports = 8;
constexpr uint32_t kMaxReportedSites = 16;
constexpr DWORD kReporterTimeoutMs = 15000;

using VectoredHandler = LONG(CALLBACK*)(PEXCEPTION_POINTERS);
using AddVectoredHandlerFn = PVOID(NTAPI*)(ULONG, VectoredHandler);
using TopLevelFilter = LONG(WINAPI*)(PEXCEPTION_POINTERS);
using SetFilterFn = TopLevelFilter(WINAPI*)(TopLevelFilter);
using PcToFileHeaderFn = PVOID(NTAPI*)(PVOID, PVOID*);
using GetProcessMemoryInfoFn = BOOL(WINAPI*)(HANDLE, void*, DWORD);
using GlobalMemoryStatusExFn = BOOL(WINAPI*)(MEMORYSTATUSEX*);
using GetPerformanceInfoFn = BOOL(WINAPI*)(void*, DWORD);
using QueryJobInformationFn = BOOL(WINAPI*)(HANDLE, JOBOBJECTINFOCLASS, void*,
                                            DWORD, DWORD*);

// PROCESS_MEMORY_COUNTERS (psapi.h).
struct ProcessMemoryCounters {
  DWORD cb;
  DWORD page_fault_count;
  SIZE_T peak_working_set_size;
  SIZE_T working_set_size;
  SIZE_T quota_peak_paged_pool_usage;
  SIZE_T quota_paged_pool_usage;
  SIZE_T quota_peak_non_paged_pool_usage;
  SIZE_T quota_non_paged_pool_usage;
  SIZE_T pagefile_usage;
  SIZE_T peak_pagefile_usage;
};

// PERFORMANCE_INFORMATION (psapi.h). Counts are in pages.
struct PerformanceInformation {
  DWORD cb;
  SIZE_T commit_total;
  SIZE_T commit_limit;
  SIZE_T commit_peak;
  SIZE_T physical_total;
  SIZE_T physical_available;
  SIZE_T system_cache;
  SIZE_T kernel_total;
  SIZE_T kernel_paged;
  SIZE_T kernel_nonpaged;
  SIZE_T page_size;
  DWORD handle_count;
  DWORD process_count;
  DWORD thread_count;
};

// What a Visual C++ throw describes on x64, as 32-bit offsets from the image
// base that the exception record carries.
struct CxxThrowInfo {
  uint32_t attributes;
  int32_t unwind;
  int32_t forward_compat;
  int32_t catchable_type_array;
};
struct CxxCatchableTypeArray {
  int32_t count;
  int32_t types[1];
};
struct CxxCatchableType {
  uint32_t properties;
  int32_t type;
  int32_t mdisp;
  int32_t pdisp;
  int32_t vdisp;
  int32_t size_or_offset;
  int32_t copy_function;
};
struct CxxTypeDescriptor {
  const void* vftable;
  void* spare;
  char name[1];
};

struct CppExceptionInfo {
  char type[128];
  char what[256];
};

// A report the failing thread hands to the reporter thread, which has the
// stack to build it.
struct Request {
  const char* heading;
  bool has_record;
  bool is_crash;
  DWORD thread_id;
  void* guest_thread;
  EXCEPTION_RECORD record;
  CppExceptionInfo cpp;
  CONTEXT context;
};

PcToFileHeaderFn pc_to_file_header = nullptr;
GetProcessMemoryInfoFn get_process_memory_info = nullptr;
GlobalMemoryStatusExFn global_memory_status = nullptr;
GetPerformanceInfoFn get_performance_info = nullptr;
QueryJobInformationFn query_job_information = nullptr;
TopLevelFilter previous_filter = nullptr;

wchar_t report_path[2048] = {};
char build_description[256] = {};
HANDLE report_file = INVALID_HANDLE_VALUE;
uint64_t start_tick = 0;

GuestHooks guest_hooks;
std::atomic<bool> guest_hooks_set{false};

HANDLE request_event = nullptr;
HANDLE done_event = nullptr;
HANDLE reporter_thread = nullptr;
DWORD reporter_thread_id = 0;
bool reporter_stuck = false;

// Thread making a report; reports are made one at a time.
std::atomic<DWORD> report_owner{0};
uint32_t report_count = 0;
struct Site {
  DWORD code;
  uint64_t address;
};
Site reported_sites[kMaxReportedSites];
uint32_t reported_site_count = 0;

Request request;
char report_buffer[96 * 1024];

thread_local CppExceptionInfo last_cpp_exception;
std::atomic<uint64_t> logged_cpp_types[16];

// Status sources: a slot is in use while its source is set. Writers take the
// mutex; readers (heartbeats and crash reports) only load the atomics.
struct StatusSlot {
  std::atomic<StatusSource> source{nullptr};
  std::atomic<void*> context{nullptr};
};
constexpr size_t kMaxStatusSources = 8;
StatusSlot status_slots[kMaxStatusSources];
std::mutex status_slots_mutex;

GuestHooks LoadHooks() {
  if (!guest_hooks_set.load(std::memory_order_acquire)) {
    return GuestHooks();
  }
  return guest_hooks;
}

void CopyString(const char* source, char* out, size_t out_size) {
  size_t i = 0;
  for (; i + 1 < out_size && source[i]; ++i) {
    out[i] = source[i];
  }
  out[i] = '\0';
}

const char* FileNamePart(const char* path) {
  const char* name = path;
  for (const char* c = path; *c; ++c) {
    if (*c == '\\' || *c == '/') {
      name = c + 1;
    }
  }
  return name;
}

uint64_t HashString(const char* text) {
  uint64_t hash = 14695981039346656037ull;
  for (const char* c = text; *c; ++c) {
    hash = (hash ^ uint8_t(*c)) * 1099511628211ull;
  }
  return hash ? hash : 1;
}

const IMAGE_NT_HEADERS64* ImageHeaders(uint64_t base) {
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return nullptr;
  }
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
      base + uint64_t(uint32_t(dos->e_lfanew)));
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return nullptr;
  }
  return nt;
}

// The CodeView record (RSDS: GUID, age, PDB path) of a loaded image.
const uint8_t* ImageCodeView(uint64_t base, const IMAGE_NT_HEADERS64* nt) {
  const IMAGE_DATA_DIRECTORY& debug =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  if (!debug.VirtualAddress) {
    return nullptr;
  }
  auto entries =
      reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + debug.VirtualAddress);
  const size_t count = debug.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
  for (size_t i = 0; i < count; ++i) {
    if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
        !entries[i].AddressOfRawData) {
      continue;
    }
    auto codeview =
        reinterpret_cast<const uint8_t*>(base + entries[i].AddressOfRawData);
    if (*reinterpret_cast<const uint32_t*>(codeview) == 0x53445352) {
      return codeview;
    }
  }
  return nullptr;
}

// A loaded image's name: from its export directory, or its PDB file name.
bool ReadImageName(uint64_t base, char* out, size_t out_size) {
  __try {
    const IMAGE_NT_HEADERS64* nt = ImageHeaders(base);
    if (!nt) {
      return false;
    }
    const IMAGE_DATA_DIRECTORY& exports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exports.VirtualAddress && exports.Size) {
      auto directory = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
          base + exports.VirtualAddress);
      if (directory->Name) {
        CopyString(reinterpret_cast<const char*>(base + directory->Name), out,
                   out_size);
        return out[0] != '\0';
      }
    }
    const uint8_t* codeview = ImageCodeView(base, nt);
    if (codeview) {
      CopyString(FileNamePart(reinterpret_cast<const char*>(codeview + 24)),
                 out, out_size);
      return out[0] != '\0';
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}

bool ReadPdbSignature(uint64_t base, GUID* guid, uint32_t* age) {
  __try {
    const IMAGE_NT_HEADERS64* nt = ImageHeaders(base);
    const uint8_t* codeview = nt ? ImageCodeView(base, nt) : nullptr;
    if (!codeview) {
      return false;
    }
    std::memcpy(guid, codeview + 4, sizeof(GUID));
    std::memcpy(age, codeview + 20, sizeof(uint32_t));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}

// Whether an address in a loaded image is in an executable section.
bool IsImageCode(uint64_t base, uint64_t address) {
  __try {
    const IMAGE_NT_HEADERS64* nt = ImageHeaders(base);
    if (!nt) {
      return false;
    }
    const uint64_t rva = address - base;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
      const uint64_t size =
          std::max<uint64_t>(section->Misc.VirtualSize, section->SizeOfRawData);
      if (rva >= section->VirtualAddress && rva < section->VirtualAddress + size) {
        return (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}

uint64_t ImageBaseOf(uint64_t address) {
  PVOID base = nullptr;
  if (pc_to_file_header) {
    pc_to_file_header(reinterpret_cast<PVOID>(address), &base);
  }
  return reinterpret_cast<uint64_t>(base);
}

bool ReadQword(uint64_t address, uint64_t* value) {
  __try {
    *value = *reinterpret_cast<const volatile uint64_t*>(address);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}

PRUNTIME_FUNCTION LookupFunctionEntry(DWORD64 pc, DWORD64* image_base) {
  __try {
    return RtlLookupFunctionEntry(pc, image_base, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return nullptr;
}

bool UnwindFrame(CONTEXT* context, DWORD64 image_base,
                 PRUNTIME_FUNCTION function) {
  __try {
    if (function) {
      PVOID handler_data = nullptr;
      DWORD64 establisher_frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context->Rip, function,
                       context, &handler_data, &establisher_frame, nullptr);
    } else {
      // A leaf function: the return address is on top of the stack.
      context->Rip = *reinterpret_cast<const DWORD64*>(context->Rsp);
      context->Rsp += sizeof(DWORD64);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}

void DecodeCppException(uint64_t object, uint64_t throw_info_address,
                        uint64_t image_base, CppExceptionInfo* info) {
  if (!throw_info_address || !image_base) {
    return;
  }
  __try {
    auto throw_info = reinterpret_cast<const CxxThrowInfo*>(throw_info_address);
    if (!throw_info->catchable_type_array) {
      return;
    }
    auto types = reinterpret_cast<const CxxCatchableTypeArray*>(
        image_base + uint32_t(throw_info->catchable_type_array));
    for (int32_t i = 0; i < types->count && i < 16; ++i) {
      auto type = reinterpret_cast<const CxxCatchableType*>(
          image_base + uint32_t(types->types[i]));
      auto descriptor = reinterpret_cast<const CxxTypeDescriptor*>(
          image_base + uint32_t(type->type));
      if (i == 0) {
        CopyString(descriptor->name, info->type, sizeof(info->type));
      }
      if (object && type->pdisp < 0 &&
          std::strcmp(descriptor->name, ".?AVexception@std@@") == 0) {
        auto exception = reinterpret_cast<const std::exception*>(
            object + uint64_t(int64_t(type->mdisp)));
        const char* what = exception->what();
        if (what) {
          CopyString(what, info->what, sizeof(info->what));
        }
        break;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Remembers the C++ exception a thread throws, for a report if nothing catches
// it, and logs each exception type the first time it is thrown.
void NoteCppException(const EXCEPTION_RECORD& record) {
  CppExceptionInfo& info = last_cpp_exception;
  info.type[0] = '\0';
  info.what[0] = '\0';
  if (record.NumberParameters < 4) {
    return;
  }
  const uint64_t magic = record.ExceptionInformation[0];
  if (magic != 0x19930520 && magic != 0x19930521 && magic != 0x19930522) {
    return;
  }
  DecodeCppException(record.ExceptionInformation[1],
                     record.ExceptionInformation[2],
                     record.ExceptionInformation[3], &info);
  if (!info.type[0]) {
    return;
  }
  const uint64_t hash = HashString(info.type);
  for (auto& slot : logged_cpp_types) {
    uint64_t value = slot.load(std::memory_order_relaxed);
    if (value == 0 && slot.compare_exchange_strong(value, hash)) {
      XELOGW("C++ exception thrown: {} {}", static_cast<const char*>(info.type),
             static_cast<const char*>(info.what));
      return;
    }
    if (value == hash) {
      return;
    }
  }
}

bool IsReportable(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_INVALID_DISPOSITION:
    case kStatusBadStack:
    case kStatusInvalidUnwindTarget:
    case kStatusHeapCorruption:
    case kStatusStackBufferOverrun:
    case kStatusAssertionFailure:
      return true;
    default:
      return false;
  }
}

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION:
      return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "integer division by zero";
    case EXCEPTION_INT_OVERFLOW:
      return "integer overflow";
    case EXCEPTION_STACK_OVERFLOW:
      return "stack overflow";
    case EXCEPTION_BREAKPOINT:
      return "breakpoint (a debug break with no debugger attached)";
    case EXCEPTION_IN_PAGE_ERROR:
      return "page read error";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      return "misaligned data";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
      return "continued a noncontinuable exception";
    case EXCEPTION_INVALID_DISPOSITION:
      return "invalid exception disposition";
    case kStatusBadStack:
      return "bad stack while unwinding";
    case kStatusInvalidUnwindTarget:
      return "invalid unwind target";
    case kStatusHeapCorruption:
      return "heap corruption";
    case kStatusStackBufferOverrun:
      return "stack buffer overrun";
    case kStatusAssertionFailure:
      return "assertion failure";
    case kCppExceptionCode:
      return "C++ exception";
    default:
      return "exception";
  }
}

void DescribeException(const EXCEPTION_RECORD& record, const GuestHooks& hooks,
                       Writer& out) {
  const DWORD code = record.ExceptionCode;
  out.Append("{} (0x{:08X}) at ", ExceptionName(code), code);
  DescribeHostAddress(reinterpret_cast<uint64_t>(record.ExceptionAddress), out);
  out.AppendText("\n");
  if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
      record.NumberParameters >= 2) {
    const ULONG_PTR operation = record.ExceptionInformation[0];
    const uint64_t target = record.ExceptionInformation[1];
    out.Append("Fault {} 0x{:016X}",
               operation == 0   ? "reading"
               : operation == 1 ? "writing"
               : operation == 8 ? "executing"
                                : "accessing",
               target);
    char text[192];
    Writer description(text, sizeof(text));
    if (hooks.describe_data_address &&
        hooks.describe_data_address(target, description)) {
      out.AppendText(" (");
      out.AppendText(description.view());
      out.AppendText(")");
    } else if (target < 0x10000) {
      out.AppendText(" (a null pointer)");
    }
    out.AppendText("\n");
  }
}

void DescribeRegisters(const CONTEXT& c, Writer& out) {
  out.Append("rax {:016X} rbx {:016X} rcx {:016X} rdx {:016X}\n", c.Rax, c.Rbx,
             c.Rcx, c.Rdx);
  out.Append("rsi {:016X} rdi {:016X} rbp {:016X} rsp {:016X}\n", c.Rsi, c.Rdi,
             c.Rbp, c.Rsp);
  out.Append("r8  {:016X} r9  {:016X} r10 {:016X} r11 {:016X}\n", c.R8, c.R9,
             c.R10, c.R11);
  out.Append("r12 {:016X} r13 {:016X} r14 {:016X} r15 {:016X}\n", c.R12, c.R13,
             c.R14, c.R15);
  out.Append("rip {:016X} eflags {:08X}\n", c.Rip, uint32_t(c.EFlags));
}

void WalkStack(const CONTEXT& start, const GuestHooks& hooks, Writer& out) {
  CONTEXT context = start;
  for (int frame = 0; frame < kMaxFrames && context.Rip; ++frame) {
    const DWORD64 pc = context.Rip;
    out.Append("  #{:02} ", frame);
    DescribeHostAddress(pc, out);
    out.AppendText("\n");
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = nullptr;
    if (hooks.is_generated_code && hooks.is_generated_code(pc)) {
      if (hooks.lookup_unwind_info) {
        uint64_t base = 0;
        function = static_cast<PRUNTIME_FUNCTION>(
            hooks.lookup_unwind_info(pc, &base));
        image_base = base;
      }
    } else {
      function = LookupFunctionEntry(pc, &image_base);
      if (!function && frame) {
        // A return address just past a call that ends its function.
        function = LookupFunctionEntry(pc - 1, &image_base);
      }
    }
    const DWORD64 sp = context.Rsp;
    if (!UnwindFrame(&context, image_base, function) || context.Rsp <= sp) {
      out.AppendText("  (unwinding stopped here)\n");
      break;
    }
  }
}

// Code addresses on the stack, which show the calls even where unwinding
// fails (generated code and thunks without unwind data).
void ScanStack(uint64_t sp, const GuestHooks& hooks, Writer& out) {
  out.AppendText("Code addresses on the stack:\n");
  int found = 0;
  for (size_t offset = 0; offset < kStackScanBytes && found < kMaxStackScanEntries;
       offset += sizeof(uint64_t)) {
    uint64_t value = 0;
    if (!ReadQword(sp + offset, &value)) {
      break;
    }
    if (value < 0x10000) {
      continue;
    }
    bool is_code = hooks.is_generated_code && hooks.is_generated_code(value);
    if (!is_code) {
      const uint64_t base = ImageBaseOf(value);
      is_code = base && IsImageCode(base, value);
    }
    if (!is_code) {
      continue;
    }
    out.Append("  [rsp+0x{:04X}] ", offset);
    DescribeHostAddress(value, out);
    out.AppendText("\n");
    ++found;
  }
}

void DescribeProcessMemory(Writer& out) {
  if (!get_process_memory_info) {
    return;
  }
  ProcessMemoryCounters counters = {};
  counters.cb = sizeof(counters);
  if (get_process_memory_info(GetCurrentProcess(), &counters,
                              sizeof(counters))) {
    out.Append(
        "Process memory: {} MiB committed (peak {} MiB), {} MiB working set "
        "(peak {} MiB)\n",
        uint64_t(counters.pagefile_usage >> 20),
        uint64_t(counters.peak_pagefile_usage >> 20),
        uint64_t(counters.working_set_size >> 20),
        uint64_t(counters.peak_working_set_size >> 20));
  }
}

void WriteToReportFile(std::string_view text) {
  DWORD written = 0;
  WriteFile(report_file, text.data(), DWORD(text.size()), &written, nullptr);
}

void AppendReportFile(std::string_view text) {
  if (report_file == INVALID_HANDLE_VALUE) {
    if (!report_path[0]) {
      return;
    }
    CREATEFILE2_EXTENDED_PARAMETERS parameters = {};
    parameters.dwSize = sizeof(parameters);
    parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    report_file = CreateFile2(report_path, GENERIC_WRITE, FILE_SHARE_READ,
                              CREATE_ALWAYS, &parameters);
    if (report_file == INVALID_HANDLE_VALUE) {
      return;
    }
    WriteToReportFile(build_description);
    WriteToReportFile("\n\n");
  }
  WriteToReportFile(text);
  WriteToReportFile("\n\n");
  FlushFileBuffers(report_file);
}

void FlushLogAfterReport() {
  xe::FlushLog();
  // The log writer thread sleeps up to 50 ms when it has been idle.
  Sleep(150);
  xe::FlushLog();
}

// Runs on the reporter thread, or on the failing thread if there is none.
void WriteRequestedReport() {
  const Request& r = request;
  const GuestHooks hooks = LoadHooks();
  Writer out(report_buffer, sizeof(report_buffer));
  out.Append("==== {} ====\n", r.heading);
  out.Append("{:.1f} s after start, host thread {:X}",
             double(GetTickCount64() - start_tick) / 1000.0,
             uint32_t(r.thread_id));
  if (r.guest_thread && hooks.describe_guest_thread) {
    out.AppendText(", ");
    hooks.describe_guest_thread(r.guest_thread, out);
  }
  out.AppendText("\n");
  if (r.has_record) {
    DescribeException(r.record, hooks, out);
  }
  if (r.cpp.type[0]) {
    out.Append("Last C++ exception on this thread: {} {}\n",
               static_cast<const char*>(r.cpp.type),
               static_cast<const char*>(r.cpp.what));
  }
  if (r.is_crash) {
    DescribeRegisters(r.context, out);
  }
  out.AppendText("Host stack:\n");
  WalkStack(r.context, hooks, out);
  if (r.is_crash) {
    ScanStack(r.context.Rsp, hooks, out);
  }
  if (hooks.describe_recent_activity) {
    hooks.describe_recent_activity(uint32_t(r.thread_id), out);
  }
  DescribeProcessMemory(out);
  out.AppendText("Status: ");
  DescribeStatus(out);
  out.AppendText("\n==== end of report ====");

  xe::logging::AppendLogLine(r.is_crash ? xe::LogLevel::Error
                                        : xe::LogLevel::Info,
                             r.is_crash ? xe::logging::kPrefixCharError
                                        : xe::logging::kPrefixCharInfo,
                             out.view());
  if (r.is_crash) {
    AppendReportFile(out.view());
  }
  FlushLogAfterReport();
}

DWORD WINAPI ReporterThreadMain(LPVOID) {
  while (WaitForSingleObjectEx(request_event, INFINITE, FALSE) ==
         WAIT_OBJECT_0) {
    WriteRequestedReport();
    SetEvent(done_event);
  }
  return 0;
}

bool AcquireReportLock() {
  const DWORD self = GetCurrentThreadId();
  if (report_owner.load(std::memory_order_relaxed) == self) {
    // Failed again while making a report.
    return false;
  }
  for (int attempt = 0; attempt < 30000; ++attempt) {
    DWORD expected = 0;
    if (report_owner.compare_exchange_strong(expected, self,
                                             std::memory_order_acquire)) {
      return true;
    }
    Sleep(1);
  }
  return false;
}

void ReleaseReportLock() { report_owner.store(0, std::memory_order_release); }

bool IsSiteReported(DWORD code, uint64_t address) {
  for (uint32_t i = 0; i < reported_site_count; ++i) {
    if (reported_sites[i].code == code && reported_sites[i].address == address) {
      return true;
    }
  }
  return false;
}

// Describes the calling thread at `context`. Crash reports also go to the
// crash file; each exception site is reported once.
void Report(const char* heading, const EXCEPTION_RECORD* record,
            const CONTEXT& context, bool is_crash) {
  if (!AcquireReportLock()) {
    return;
  }
  if (is_crash) {
    if (record) {
      const DWORD code = record->ExceptionCode;
      const uint64_t address =
          reinterpret_cast<uint64_t>(record->ExceptionAddress);
      if (IsSiteReported(code, address)) {
        ReleaseReportLock();
        return;
      }
      if (reported_site_count < kMaxReportedSites) {
        reported_sites[reported_site_count++] = {code, address};
      }
    }
    if (report_count >= kMaxReports) {
      ReleaseReportLock();
      return;
    }
    ++report_count;
  }
  const GuestHooks hooks = LoadHooks();
  request.heading = heading;
  request.has_record = record != nullptr;
  if (record) {
    request.record = *record;
  }
  request.is_crash = is_crash;
  request.thread_id = GetCurrentThreadId();
  request.guest_thread =
      hooks.current_guest_thread ? hooks.current_guest_thread() : nullptr;
  request.cpp = last_cpp_exception;
  request.context = context;
  if (reporter_thread && !reporter_stuck &&
      WaitForSingleObjectEx(reporter_thread, 0, FALSE) == WAIT_TIMEOUT) {
    SetEvent(request_event);
    if (WaitForSingleObjectEx(done_event, kReporterTimeoutMs, FALSE) !=
        WAIT_OBJECT_0) {
      reporter_stuck = true;
    }
  } else {
    WriteRequestedReport();
  }
  ReleaseReportLock();
}

// After every handler Xenia installs (they come first), for what none took.
LONG CALLBACK LastChanceHandler(PEXCEPTION_POINTERS info) {
  if (GetCurrentThreadId() == reporter_thread_id) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  if (record->ExceptionCode == kCppExceptionCode) {
    NoteCppException(*record);
  } else if (IsReportable(record->ExceptionCode)) {
    Report(
        "Host exception none of Xenia's handlers took (fatal unless the code "
        "that raised it catches it)",
        record, *info->ContextRecord, true);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI UnhandledExceptionReporter(PEXCEPTION_POINTERS info) {
  if (GetCurrentThreadId() != reporter_thread_id) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    bool reported = false;
    if (AcquireReportLock()) {
      reported = IsSiteReported(
          record->ExceptionCode,
          reinterpret_cast<uint64_t>(record->ExceptionAddress));
      ReleaseReportLock();
    }
    if (reported) {
      XELOGE("Nothing handled the host exception reported above: Xenia is "
             "closing");
      FlushLogAfterReport();
    } else {
      Report("Unhandled host exception: Xenia is closing", record,
             *info->ContextRecord, true);
    }
  }
  return previous_filter ? previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void __cdecl AbortReporter(int) {
  CONTEXT context;
  RtlCaptureContext(&context);
  Report(
      "abort() was called: std::terminate (a C++ exception nothing caught, or "
      "one thrown out of noexcept code) or a failed runtime check",
      nullptr, context, true);
}

void __cdecl InvalidParameterReporter(const wchar_t*, const wchar_t*,
                                      const wchar_t*, unsigned int,
                                      uintptr_t) {
  CONTEXT context;
  RtlCaptureContext(&context);
  Report("A C runtime function got an invalid parameter: Xenia is closing",
         nullptr, context, true);
  __fastfail(kFastFailInvalidArg);
}

void __cdecl PurecallReporter() {
  CONTEXT context;
  RtlCaptureContext(&context);
  Report(
      "A pure virtual function was called (an object used while or after it "
      "was destroyed): Xenia is closing",
      nullptr, context, true);
  __fastfail(kFastFailFatalAppExit);
}

}  // namespace

void Install(const std::filesystem::path& path,
             std::string_view build) {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  start_tick = GetTickCount64();

  const std::wstring wide_path = path.wstring();
  const size_t path_length =
      std::min(wide_path.size(), std::size(report_path) - 1);
  std::copy_n(wide_path.c_str(), path_length, report_path);
  report_path[path_length] = L'\0';
  const size_t build_length =
      std::min(build.size(), sizeof(build_description) - 1);
  std::copy_n(build.data(), build_length, build_description);
  build_description[build_length] = '\0';

  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (ntdll) {
    pc_to_file_header = reinterpret_cast<PcToFileHeaderFn>(
        GetProcAddress(ntdll, "RtlPcToFileHeader"));
  }
  for (HMODULE module : {kernelbase, kernel32}) {
    if (!module) {
      continue;
    }
    if (!get_process_memory_info) {
      get_process_memory_info = reinterpret_cast<GetProcessMemoryInfoFn>(
          GetProcAddress(module, "K32GetProcessMemoryInfo"));
    }
    if (!global_memory_status) {
      global_memory_status = reinterpret_cast<GlobalMemoryStatusExFn>(
          GetProcAddress(module, "GlobalMemoryStatusEx"));
    }
    if (!get_performance_info) {
      get_performance_info = reinterpret_cast<GetPerformanceInfoFn>(
          GetProcAddress(module, "K32GetPerformanceInfo"));
    }
    if (!query_job_information) {
      query_job_information = reinterpret_cast<QueryJobInformationFn>(
          GetProcAddress(module, "QueryInformationJobObject"));
    }
  }

  request_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
  done_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
  if (request_event && done_event) {
    reporter_thread = CreateThread(nullptr, 512 * 1024, ReporterThreadMain,
                                   nullptr, 0, &reporter_thread_id);
  }

  bool vectored = false;
  auto add_handler =
      ntdll ? reinterpret_cast<AddVectoredHandlerFn>(
                  GetProcAddress(ntdll, "RtlAddVectoredExceptionHandler"))
            : nullptr;
  if (add_handler) {
    // 0: after the handlers already installed and any added later with 1, as
    // Xenia's are.
    vectored = add_handler(0, LastChanceHandler) != nullptr;
  }
  SetFilterFn set_filter = nullptr;
  for (HMODULE module : {kernelbase, kernel32}) {
    if (module && !set_filter) {
      set_filter = reinterpret_cast<SetFilterFn>(
          GetProcAddress(module, "SetUnhandledExceptionFilter"));
    }
  }
  if (set_filter) {
    previous_filter = set_filter(UnhandledExceptionReporter);
  }
  signal(SIGABRT, AbortReporter);
  _set_invalid_parameter_handler(InvalidParameterReporter);
  _set_purecall_handler(PurecallReporter);

  // Identify the symbols a report's offsets need.
  const uint64_t base = ImageBaseOf(reinterpret_cast<uint64_t>(&Install));
  char name[96] = "?";
  GUID guid = {};
  uint32_t age = 0;
  if (base) {
    ReadImageName(base, name, sizeof(name));
    ReadPdbSignature(base, &guid, &age);
  }
  XELOGI(
      "Crash reporter: vectored handler {}, unhandled filter {}, reporter "
      "thread {}; {} at 0x{:X}, PDB {:08X}{:04X}{:04X}{:02X}{:02X}{:02X}{:02X}"
      "{:02X}{:02X}{:02X}{:02X} age {}",
      vectored, set_filter != nullptr, reporter_thread != nullptr,
      static_cast<const char*>(name), base, uint32_t(guid.Data1),
      uint32_t(guid.Data2), uint32_t(guid.Data3), guid.Data4[0], guid.Data4[1],
      guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
      guid.Data4[6], guid.Data4[7], age);

  char limits_buffer[512];
  Writer limits(limits_buffer, sizeof(limits_buffer));
  DescribeMemoryLimits(limits);
  XELOGI("Memory limits: {}", limits.view());
}

void SetGuestHooks(const GuestHooks& hooks) {
  guest_hooks_set.store(false, std::memory_order_release);
  guest_hooks = hooks;
  guest_hooks_set.store(true, std::memory_order_release);
}

void ClearGuestHooks() {
  guest_hooks_set.store(false, std::memory_order_release);
}

void DescribeHostAddress(uint64_t address, Writer& out) {
  const GuestHooks hooks = LoadHooks();
  if (hooks.is_generated_code && hooks.is_generated_code(address)) {
    if (hooks.describe_generated_code) {
      hooks.describe_generated_code(address, out);
    } else {
      out.Append("generated code 0x{:X}", address);
    }
    return;
  }
  const uint64_t base = ImageBaseOf(address);
  if (base) {
    char name[96];
    if (!ReadImageName(base, name, sizeof(name))) {
      CopyString("image", name, sizeof(name));
    }
    out.Append("{}+0x{:X}", static_cast<const char*>(name), address - base);
    return;
  }
  out.Append("0x{:016X}", address);
}

void AddStatusSource(StatusSource source, void* context) {
  std::lock_guard<std::mutex> lock(status_slots_mutex);
  for (StatusSlot& slot : status_slots) {
    if (!slot.source.load(std::memory_order_relaxed)) {
      slot.context.store(context, std::memory_order_relaxed);
      slot.source.store(source, std::memory_order_release);
      return;
    }
  }
}

void RemoveStatusSource(StatusSource source, void* context) {
  std::lock_guard<std::mutex> lock(status_slots_mutex);
  for (StatusSlot& slot : status_slots) {
    if (slot.source.load(std::memory_order_relaxed) == source &&
        slot.context.load(std::memory_order_relaxed) == context) {
      slot.source.store(nullptr, std::memory_order_release);
      slot.context.store(nullptr, std::memory_order_relaxed);
    }
  }
}

void DescribeMemoryLimits(Writer& out) {
  bool first = true;
  const auto separate = [&]() {
    if (!first) {
      out.AppendText("; ");
    }
    first = false;
  };
  const auto append_limit = [&](const char* name, bool set, SIZE_T bytes) {
    if (set) {
      out.Append("{} {} MiB", name, uint64_t(bytes >> 20));
    } else {
      out.Append("{} none", name);
    }
  };
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job = {};
  separate();
  if (!query_job_information) {
    out.AppendText("job limits unavailable");
  } else if (query_job_information(nullptr, JobObjectExtendedLimitInformation,
                                   &job, sizeof(job), nullptr)) {
    const DWORD flags = job.BasicLimitInformation.LimitFlags;
    append_limit("job memory limit", (flags & JOB_OBJECT_LIMIT_JOB_MEMORY) != 0,
                 job.JobMemoryLimit);
    out.AppendText(", ");
    append_limit("process memory limit",
                 (flags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) != 0,
                 job.ProcessMemoryLimit);
  } else {
    out.Append("job limits unreadable (system error {})",
               uint32_t(GetLastError()));
  }
  MEMORYSTATUSEX status = {};
  status.dwLength = sizeof(status);
  if (global_memory_status && global_memory_status(&status)) {
    separate();
    out.Append("commit limit for this process {} MiB, RAM {} MiB",
               uint64_t(status.ullTotalPageFile >> 20),
               uint64_t(status.ullTotalPhys >> 20));
  }
  PerformanceInformation performance = {};
  performance.cb = sizeof(performance);
  if (get_performance_info &&
      get_performance_info(&performance, sizeof(performance))) {
    separate();
    out.Append("system commit limit {} MiB",
               uint64_t(performance.commit_limit) * performance.page_size >>
                   20);
  }
}

void DescribeStatus(Writer& out) {
  bool first = true;
  if (get_process_memory_info) {
    ProcessMemoryCounters counters = {};
    counters.cb = sizeof(counters);
    if (get_process_memory_info(GetCurrentProcess(), &counters,
                                sizeof(counters))) {
      out.Append("process commit {} MiB (peak {} MiB)",
                 uint64_t(counters.pagefile_usage >> 20),
                 uint64_t(counters.peak_pagefile_usage >> 20));
      first = false;
    }
  }
  // Whose limit a refused commit hit: this process's (the job's) or the
  // whole system's.
  MEMORYSTATUSEX memory_status = {};
  memory_status.dwLength = sizeof(memory_status);
  if (global_memory_status && global_memory_status(&memory_status)) {
    out.Append("{}{} MiB more may be committed, {} MiB RAM free",
               first ? "" : ", ",
               uint64_t(memory_status.ullAvailPageFile >> 20),
               uint64_t(memory_status.ullAvailPhys >> 20));
    first = false;
  }
  PerformanceInformation performance = {};
  performance.cb = sizeof(performance);
  if (get_performance_info &&
      get_performance_info(&performance, sizeof(performance))) {
    out.Append("{}system commit {} of {} MiB", first ? "" : "; ",
               uint64_t(performance.commit_total) * performance.page_size >> 20,
               uint64_t(performance.commit_limit) * performance.page_size >>
                   20);
    first = false;
  }
  for (StatusSlot& slot : status_slots) {
    StatusSource source = slot.source.load(std::memory_order_acquire);
    if (!source) {
      continue;
    }
    if (!first) {
      out.AppendText("; ");
    }
    first = false;
    source(slot.context.load(std::memory_order_relaxed), out);
  }
}

void LogCurrentStack(std::string_view reason) {
  char heading[256];
  CopyString(std::string(reason).c_str(), heading, sizeof(heading));
  CONTEXT context;
  RtlCaptureContext(&context);
  Report(heading, nullptr, context, false);
}

}  // namespace crash_report
}  // namespace xe
