/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_devices/svod_container_file.h"
#include "xenia/base/logging.h"
#include "xenia/vfs/devices/xcontent_devices/svod_container_entry.h"

namespace xe {
namespace vfs {

SvodContainerFile::SvodContainerFile(uint32_t file_access,
                                     SvodContainerEntry* entry)
    : XContentContainerFile(file_access, entry), entry_(entry) {}

SvodContainerFile::~SvodContainerFile() = default;

void SvodContainerFile::Destroy() { delete this; }

size_t SvodContainerFile::Read(std::span<uint8_t> buffer, size_t offset,
                               size_t record_file) {
  auto file = entry_->files()->find(record_file);
  if (file == entry_->files()->end()) {
    XELOGE("SVOD read references missing fragment {} at offset 0x{:X}.",
           record_file, offset);
    return 0;
  }
  return file->second->Read(offset, buffer.data(), buffer.size());
}

}  // namespace vfs
}  // namespace xe
