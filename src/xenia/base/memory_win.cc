/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/memory.h"

#include <atomic>

#include "xenia/base/crash_report.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | \
                            WINAPI_PARTITION_SYSTEM | WINAPI_PARTITION_GAMES)
#define XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
#endif
/*
        these two dont bypass much ms garbage compared to the threading ones,
   but Protect is used by PhysicalHeap::EnableAccessCallbacks which eats a lot
   of cpu time, so every bit counts
*/
XE_NTDLL_IMPORT(NtProtectVirtualMemory, cls_NtProtectVirtualMemory,
                NtProtectVirtualMemoryPointer);
XE_NTDLL_IMPORT(NtQueryVirtualMemory, cls_NtQueryVirtualMemory,
                NtQueryVirtualMemoryPointer);
namespace xe {
namespace memory {

size_t page_size() {
#if XE_ARCH_AMD64 == 1
  return 4096;
#else
  static size_t value = 0;
  if (!value) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    value = si.dwPageSize;
  }
  return value;
#endif
}

size_t allocation_granularity() {
#if XE_ARCH_AMD64 == 1 && XE_PLATFORM_WIN32 == 1
  return 65536;
#else
  static size_t value = 0;
  if (!value) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    value = si.dwAllocationGranularity;
  }
  return value;
#endif
}

DWORD ToWin32ProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PAGE_NOACCESS;
    case PageAccess::kReadOnly:
      return PAGE_READONLY;
    case PageAccess::kReadWrite:
      return PAGE_READWRITE;
    case PageAccess::kExecuteReadOnly:
      return PAGE_EXECUTE_READ;
    case PageAccess::kExecuteReadWrite:
      return PAGE_EXECUTE_READWRITE;
    default:
      assert_unhandled_case(access);
      return PAGE_NOACCESS;
  }
}

PageAccess ToXeniaProtectFlags(DWORD access) {
  if (access & PAGE_GUARD) {
    // Strip the page guard flag for now...
    access &= ~PAGE_GUARD;
  }

  switch (access) {
    case PAGE_NOACCESS:
      return PageAccess::kNoAccess;
    case PAGE_READONLY:
      return PageAccess::kReadOnly;
    case PAGE_READWRITE:
      return PageAccess::kReadWrite;
    case PAGE_EXECUTE_READ:
      return PageAccess::kExecuteReadOnly;
    case PAGE_EXECUTE_READWRITE:
      return PageAccess::kExecuteReadWrite;
    default:
      return PageAccess::kNoAccess;
  }
}

bool IsWritableExecutableMemorySupported() {
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return true;
#else
  // To test FromApp functions on desktop, undefine
  // XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS and link to WindowsApp.lib.
  return false;
#endif
}

namespace {

void* CallVirtualAlloc(void* base_address, size_t length, DWORD alloc_type,
                       DWORD protect) {
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return VirtualAlloc(base_address, length, alloc_type, protect);
#else
  return VirtualAllocFromApp(base_address, length, ULONG(alloc_type),
                             ULONG(protect));
#endif
}

bool IsOutOfMemoryError(DWORD error) {
  return error == ERROR_COMMITMENT_LIMIT || error == ERROR_NOT_ENOUGH_MEMORY ||
         error == ERROR_NOT_ENOUGH_QUOTA;
}

// Commits a reserved range, in halves down to 64 KiB when the system refuses
// all of it at once for lack of memory, in case smaller requests still fit.
// Returns the error that stopped it, or ERROR_SUCCESS.
DWORD CommitInPieces(uint8_t* base, size_t length, DWORD protect,
                     uint32_t& calls) {
  constexpr size_t kSmallestPiece = 64 * 1024;
  ++calls;
  if (CallVirtualAlloc(base, length, MEM_COMMIT, protect)) {
    return ERROR_SUCCESS;
  }
  const DWORD error = GetLastError();
  if (!IsOutOfMemoryError(error) || length <= kSmallestPiece) {
    return error;
  }
  const size_t first_length =
      (length / 2 + kSmallestPiece - 1) & ~(kSmallestPiece - 1);
  const DWORD first_error = CommitInPieces(base, first_length, protect, calls);
  if (first_error != ERROR_SUCCESS) {
    return first_error;
  }
  return CommitInPieces(base + first_length, length - first_length, protect,
                        calls);
}

// Commit only: the range is reserved already, as every caller's is.
// Waiting and trying again does not help: on an Xbox Series S, refusals came
// from a limit that held for the rest of the run.
void* Commit(void* base_address, size_t length, DWORD protect) {
  uint32_t calls = 0;
  const DWORD error = CommitInPieces(static_cast<uint8_t*>(base_address),
                                     length, protect, calls);
  // What VirtualAlloc returns for a commit: the start of its first page.
  void* const committed_base = reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(base_address) & ~uintptr_t(page_size() - 1));
  if (error == ERROR_SUCCESS && calls == 1) {
    return committed_base;
  }
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 64) {
    char status_buffer[1024];
    crash_report::Writer status(status_buffer, sizeof(status_buffer));
    crash_report::DescribeStatus(status);
    if (error == ERROR_SUCCESS) {
      XELOGW(
          "Host memory: committing {} KiB at {} was refused whole, then got "
          "in {} requests. Memory: {}",
          length >> 10, base_address, calls, status.view());
    } else {
      XELOGE(
          "Host memory: committing {} KiB at {} (protection 0x{:X}) failed "
          "with system error {} ({}). Memory: {}",
          length >> 10, base_address, uint32_t(protect), uint32_t(error),
          DescribeAllocationError(error), status.view());
    }
  }
  SetLastError(error);
  return error == ERROR_SUCCESS ? committed_base : nullptr;
}

}  // namespace

void* AllocFixed(void* base_address, size_t length,
                 AllocationType allocation_type, PageAccess access) {
  DWORD alloc_type = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      alloc_type = MEM_RESERVE;
      break;
    case AllocationType::kCommit:
      alloc_type = MEM_COMMIT;
      break;
    case AllocationType::kReserveCommit:
      alloc_type = MEM_RESERVE | MEM_COMMIT;
      break;
    default:
      assert_unhandled_case(allocation_type);
      break;
  }
  DWORD protect = ToWin32ProtectFlags(access);
  if (alloc_type == MEM_COMMIT && base_address) {
    // Callers only see null, so Commit logs refusals. Reserving at a fixed
    // address is left out: failing there is how callers probe for free space
    // (each guest thread's context does).
    return Commit(base_address, length, protect);
  }
  return CallVirtualAlloc(base_address, length, alloc_type, protect);
}

uint32_t LastAllocationError() { return uint32_t(GetLastError()); }

const char* DescribeAllocationError(uint32_t error) {
  switch (error) {
    case ERROR_COMMITMENT_LIMIT:
      return "commit limit reached";
    case ERROR_NOT_ENOUGH_MEMORY:
      return "not enough memory";
    case ERROR_NOT_ENOUGH_QUOTA:
      return "quota exceeded";
    case ERROR_INVALID_ADDRESS:
      return "address not reserved, or already in use";
    case ERROR_INVALID_PARAMETER:
      return "invalid parameter";
    case ERROR_ACCESS_DENIED:
      return "access denied";
    case ERROR_SUCCESS:
      return "no error recorded";
    default:
      return "no description";
  }
}

bool DeallocFixed(void* base_address, size_t length,
                  DeallocationType deallocation_type) {
  DWORD free_type = 0;
  switch (deallocation_type) {
    case DeallocationType::kRelease:
      free_type = MEM_RELEASE;
      length = 0;
      break;
    case DeallocationType::kDecommit:
      free_type = MEM_DECOMMIT;
      break;
    default:
      assert_unhandled_case(deallocation_type);
      break;
  }
  return VirtualFree(base_address, length, free_type) ? true : false;
}

bool Protect(void* base_address, size_t length, PageAccess access,
             PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }
  DWORD new_protect = ToWin32ProtectFlags(access);

#if XE_USE_NTDLL_FUNCTIONS == 1

  DWORD old_protect = 0;
  SIZE_T MemoryLength = length;
  PVOID MemoryCache = base_address;

  BOOL result = NtProtectVirtualMemoryPointer.invoke<NTSTATUS>(
                    (HANDLE)0xFFFFFFFFFFFFFFFFLL, &MemoryCache, &MemoryLength,
                    new_protect, &old_protect) >= 0;

#else
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  DWORD old_protect = 0;
  BOOL result = VirtualProtect(base_address, length, new_protect, &old_protect);
#else
  ULONG old_protect = 0;
  BOOL result = VirtualProtectFromApp(base_address, length, ULONG(new_protect),
                                      &old_protect);
#endif
#endif
  if (!result) {
    return false;
  }
  if (out_old_access) {
    *out_old_access = ToXeniaProtectFlags(DWORD(old_protect));
  }
  return true;
}

bool IsCommitted(const void* address) {
  MEMORY_BASIC_INFORMATION info = {};
  return VirtualQuery(address, &info, sizeof(info)) != 0 &&
         info.State == MEM_COMMIT;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  access_out = PageAccess::kNoAccess;

  MEMORY_BASIC_INFORMATION info;
  ZeroMemory(&info, sizeof(info));
#if XE_USE_NTDLL_FUNCTIONS == 1
  ULONG_PTR ResultLength;

  NTSTATUS query_result = NtQueryVirtualMemoryPointer.invoke<NTSTATUS>(
      (HANDLE)0xFFFFFFFFFFFFFFFFLL, (PVOID)base_address,
      0 /* MemoryBasicInformation*/, &info, length, &ResultLength);
  SIZE_T result = query_result >= 0 ? ResultLength : 0;
#else
  SIZE_T result = VirtualQuery(base_address, &info, sizeof(info));
#endif
  if (!result) {
    return false;
  }

  length = info.RegionSize;
  access_out = ToXeniaProtectFlags(info.Protect);
  return true;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path,
                                          size_t length, PageAccess access,
                                          bool commit) {
  DWORD protect =
      ToWin32ProtectFlags(access) | (commit ? SEC_COMMIT : SEC_RESERVE);
  auto full_path = "Local" / path;
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, protect,
                            static_cast<DWORD>(length >> 32),
                            static_cast<DWORD>(length), full_path.c_str());
#else
  return CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, ULONG(protect),
                                  ULONG64(length), full_path.c_str());
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle,
                            const std::filesystem::path& path) {
  CloseHandle(handle);
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                  PageAccess access, size_t file_offset) {
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  DWORD target_address_low = static_cast<DWORD>(file_offset);
  DWORD target_address_high = static_cast<DWORD>(file_offset >> 32);
  DWORD file_access = 0;
  switch (access) {
    case PageAccess::kReadOnly:
      file_access = FILE_MAP_READ;
      break;
    case PageAccess::kReadWrite:
      file_access = FILE_MAP_ALL_ACCESS;
      break;
    case PageAccess::kExecuteReadOnly:
      file_access = FILE_MAP_READ | FILE_MAP_EXECUTE;
      break;
    case PageAccess::kExecuteReadWrite:
      file_access = FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE;
      break;
    case PageAccess::kNoAccess:
    default:
      assert_unhandled_case(access);
      return nullptr;
  }
  return MapViewOfFileEx(handle, file_access, target_address_high,
                         target_address_low, length, base_address);
#else
  // VirtualAlloc2FromApp and MapViewOfFile3FromApp were added in 10.0.17134.0.
  // https://docs.microsoft.com/en-us/uwp/win32-and-com/win32-apis
  HANDLE process = GetCurrentProcess();
  void* placeholder = VirtualAlloc2FromApp(
      process, base_address, length, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
      PAGE_NOACCESS, nullptr, 0);
  if (!placeholder) {
    return nullptr;
  }
  void* mapping = MapViewOfFile3FromApp(
      handle, process, placeholder, ULONG64(file_offset), length,
      MEM_REPLACE_PLACEHOLDER, ULONG(ToWin32ProtectFlags(access)), nullptr, 0);
  if (!mapping) {
    VirtualFree(placeholder, length, MEM_RELEASE);
    return nullptr;
  }
  return mapping;
#endif
}

bool UnmapFileView(FileMappingHandle handle, void* base_address,
                   size_t length) {
  return UnmapViewOfFile(base_address) ? true : false;
}

// Placeholder APIs (Windows 10 1803+), looked up at runtime so older SDKs and
// systems still build and run without view ranges.
namespace {
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE process, PVOID base_address,
                                       SIZE_T size, ULONG allocation_type,
                                       ULONG page_protection,
                                       void* extended_parameters,
                                       ULONG parameter_count);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE file_mapping, HANDLE process,
                                        PVOID base_address, ULONG64 offset,
                                        SIZE_T view_size, ULONG allocation_type,
                                        ULONG page_protection,
                                        void* extended_parameters,
                                        ULONG parameter_count);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE process, PVOID base_address,
                                         ULONG unmap_flags);
struct PlaceholderFunctions {
  VirtualAlloc2Fn virtual_alloc2 = nullptr;
  MapViewOfFile3Fn map_view_of_file3 = nullptr;
  UnmapViewOfFile2Fn unmap_view_of_file2 = nullptr;
  PlaceholderFunctions() {
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (!kernelbase) {
      return;
    }
#ifndef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
    // App container (UWP / Xbox Developer Mode): prefer the FromApp variants,
    // the same functions MapFileView uses there. They take the same arguments
    // as the desktop ones.
    virtual_alloc2 = reinterpret_cast<VirtualAlloc2Fn>(
        GetProcAddress(kernelbase, "VirtualAlloc2FromApp"));
    map_view_of_file3 = reinterpret_cast<MapViewOfFile3Fn>(
        GetProcAddress(kernelbase, "MapViewOfFile3FromApp"));
#endif
    if (!virtual_alloc2) {
      virtual_alloc2 = reinterpret_cast<VirtualAlloc2Fn>(
          GetProcAddress(kernelbase, "VirtualAlloc2"));
    }
    if (!map_view_of_file3) {
      map_view_of_file3 = reinterpret_cast<MapViewOfFile3Fn>(
          GetProcAddress(kernelbase, "MapViewOfFile3"));
    }
    unmap_view_of_file2 = reinterpret_cast<UnmapViewOfFile2Fn>(
        GetProcAddress(kernelbase, "UnmapViewOfFile2"));
  }
  bool available() const {
    return virtual_alloc2 && map_view_of_file3 && unmap_view_of_file2;
  }
};
const PlaceholderFunctions& GetPlaceholderFunctions() {
  static const PlaceholderFunctions functions;
  return functions;
}
}  // namespace

void* ReserveViewRange(void* base_address, size_t length) {
  const auto& fns = GetPlaceholderFunctions();
  if (!fns.available()) {
    return nullptr;
  }
  return fns.virtual_alloc2(GetCurrentProcess(), base_address, length,
                            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                            PAGE_NOACCESS, nullptr, 0);
}

bool MapViewInRange(FileMappingHandle handle, void* address, size_t length,
                    PageAccess access, size_t file_offset) {
  const auto& fns = GetPlaceholderFunctions();
  if (!fns.available()) {
    return false;
  }
  // Split the placeholder so [address, address + length) is one of its own.
  // This fails harmlessly when it already is (e.g. after UnmapViewInRange).
  VirtualFree(address, length, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  void* view = fns.map_view_of_file3(
      handle, GetCurrentProcess(), address, ULONG64(file_offset), length,
      MEM_REPLACE_PLACEHOLDER, ULONG(ToWin32ProtectFlags(access)), nullptr, 0);
  if (view && view != address) {
    UnmapViewOfFile(view);
    return false;
  }
  return view != nullptr;
}

bool UnmapViewInRange(FileMappingHandle handle, void* address, size_t length) {
  const auto& fns = GetPlaceholderFunctions();
  if (!fns.available()) {
    return false;
  }
  return fns.unmap_view_of_file2(GetCurrentProcess(), address,
                                 MEM_PRESERVE_PLACEHOLDER)
             ? true
             : false;
}

void ReleaseViewRange(void* base_address, size_t length) {
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
  // Merge the placeholders left by splitting, then release the whole range.
  VirtualFree(base_address, length, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
  VirtualFree(base_address, 0, MEM_RELEASE);
}

}  // namespace memory
}  // namespace xe
