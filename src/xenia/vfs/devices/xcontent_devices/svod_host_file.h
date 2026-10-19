/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_XCONTENT_DEVICES_SVOD_HOST_FILE_H_
#define XENIA_VFS_DEVICES_XCONTENT_DEVICES_SVOD_HOST_FILE_H_

#include <memory>
#include <mutex>

#include "xenia/base/filesystem.h"
#include "xenia/base/mapped_memory.h"

namespace xe::vfs {

class SvodHostFile {
 public:
  enum class Storage { kFile, kMapped };
  static constexpr Storage kDefaultStorage =
      XE_PLATFORM_WINRT ? Storage::kFile : Storage::kMapped;

  static std::unique_ptr<SvodHostFile> Open(const std::filesystem::path& path,
                                            Storage storage = kDefaultStorage);

  size_t size() const { return size_; }
  // Reads at an absolute fragment offset, clamping to the fragment's end.
  size_t Read(size_t offset, void* buffer, size_t length) const;

 private:
  explicit SvodHostFile(const std::filesystem::path& path) : path_(path) {}

  std::filesystem::path path_;
  std::unique_ptr<filesystem::FileHandle> file_handle_;
  std::unique_ptr<MappedMemory> mapping_;
  size_t size_ = 0;
  mutable std::mutex read_mutex_;
};

}  // namespace xe::vfs

#endif  // XENIA_VFS_DEVICES_XCONTENT_DEVICES_SVOD_HOST_FILE_H_
