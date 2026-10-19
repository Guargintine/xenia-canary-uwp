// Copyright 2026 Xenia Canary. Released under the BSD license.

#ifndef XENIA_KERNEL_XAM_XAM_LOADER_DATA_H_
#define XENIA_KERNEL_XAM_XAM_LOADER_DATA_H_

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace xe::kernel::xam {

struct LoaderData {
  std::string host_path;
  std::string launch_path;
  uint32_t launch_flags = 0;
  std::vector<uint8_t> launch_data;
};

inline std::filesystem::path PersistentLoaderDataPath(
    const std::filesystem::path& storage_root) {
  return storage_root / "launch_data.bin";
}

// Preserve the upstream Windows wire format, including its 16-bit lengths.
inline bool SaveLoaderDataFile(const std::filesystem::path& path,
                               const LoaderData& data) {
  constexpr auto max_size = std::numeric_limits<uint16_t>::max();
  if (data.host_path.size() > max_size || data.launch_path.size() > max_size ||
      data.launch_data.size() > max_size) {
    return false;
  }

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  auto write = [&file](const void* value, size_t size) {
    if (size) {
      file.write(static_cast<const char*>(value), size);
    }
  };
  auto write_sized = [&write](const auto& value) {
    const uint16_t size = static_cast<uint16_t>(value.size());
    write(&size, sizeof(size));
    write(value.data(), size);
  };
  write_sized(data.host_path);
  write_sized(data.launch_path);
  write(&data.launch_flags, sizeof(data.launch_flags));
  write_sized(data.launch_data);
  file.flush();
  file.close();
  if (!file) {
    std::error_code error;
    std::filesystem::remove(path, error);
    return false;
  }
  return true;
}

inline bool LoadLoaderDataFile(const std::filesystem::path& path,
                               LoaderData& data) {
  data = {};
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  auto read = [&file](void* value, size_t size) {
    if (size) {
      file.read(static_cast<char*>(value), size);
    }
    return bool(file);
  };
  auto read_sized = [&read](auto& value) {
    uint16_t size = 0;
    if (!read(&size, sizeof(size))) {
      return false;
    }
    value.resize(size);
    return read(value.data(), size);
  };
  LoaderData loaded;
  const bool valid = read_sized(loaded.host_path) &&
                     read_sized(loaded.launch_path) &&
                     read(&loaded.launch_flags, sizeof(loaded.launch_flags)) &&
                     read_sized(loaded.launch_data);
  file.close();

  // A partial record cannot become a launch request. Consume valid data once.
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  if (!valid || !removed || error) {
    return false;
  }
  data = std::move(loaded);
  return true;
}

}  // namespace xe::kernel::xam

#endif  // XENIA_KERNEL_XAM_XAM_LOADER_DATA_H_
