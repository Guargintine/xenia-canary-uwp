/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"

namespace xe::vfs::test {
namespace {

constexpr size_t kSectorSize = 2048;

class TestDiscImage {
 public:
  TestDiscImage() : bytes(36 * kSectorSize) {
    path = std::filesystem::temp_directory_path() /
           ("xenia-disc-test-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".iso");
    std::memcpy(bytes.data() + 32 * kSectorSize, "MICROSOFT*XBOX*MEDIA", 20);
    Put32(32 * kSectorSize + 20, 33);
    Put32(32 * kSectorSize + 24, kSectorSize);
    Put32(33 * kSectorSize + 4, 34);
    Put32(33 * kSectorSize + 8, 5);
    bytes[33 * kSectorSize + 12] = kFileAttributeNormal;
    bytes[33 * kSectorSize + 13] = 8;
    std::memcpy(bytes.data() + 33 * kSectorSize + 14, "test.bin", 8);
    std::memcpy(bytes.data() + 34 * kSectorSize, "hello", 5);
  }

  ~TestDiscImage() {
    std::error_code error;
    std::filesystem::remove(path, error);
  }

  void Put32(size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
      bytes[offset + i] = uint8_t(value >> (8 * i));
    }
  }

  void Write() {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    REQUIRE(file.good());
  }

  std::filesystem::path path;
  std::vector<uint8_t> bytes;
};

}  // namespace

TEST_CASE("Disc image reads use the device backing store", "[disc_image]") {
  TestDiscImage image;
  image.Write();
  DiscImageDevice device("", image.path);
  REQUIRE(device.Initialize());
  Entry* entry = device.ResolvePath("test.bin");
  REQUIRE(entry != nullptr);
  File* raw_file = nullptr;
  REQUIRE(entry->Open(filesystem::FileAccess::kGenericRead, &raw_file) ==
          X_STATUS_SUCCESS);
  std::unique_ptr<File> file(raw_file);
  std::array<uint8_t, 8> buffer{};
  size_t bytes_read = 999;

  REQUIRE(file->ReadSync(buffer, 0, &bytes_read) == X_STATUS_SUCCESS);
  REQUIRE(bytes_read == 5);
  REQUIRE(std::memcmp(buffer.data(), "hello", 5) == 0);
  REQUIRE(file->ReadSync(buffer, 3, &bytes_read) == X_STATUS_SUCCESS);
  REQUIRE(bytes_read == 2);
  REQUIRE(std::memcmp(buffer.data(), "lo", 2) == 0);
  REQUIRE(file->ReadSync(buffer, 5, &bytes_read) == X_STATUS_END_OF_FILE);
  REQUIRE(bytes_read == 0);

  if (entry->can_map()) {
    auto mapping = entry->OpenMapped(MappedMemory::Mode::kRead, 3, 8);
    REQUIRE(mapping != nullptr);
    REQUIRE(mapping->size() == 2);
    REQUIRE(std::memcmp(mapping->data(), "lo", 2) == 0);
    REQUIRE(entry->OpenMapped(MappedMemory::Mode::kRead, 6, 1) == nullptr);
  }

  bytes_read = 999;
  REQUIRE_FALSE(device.ReadImageData(image.bytes.size() - 1, buffer.data(), 2,
                                     &bytes_read));
  REQUIRE(bytes_read == 0);
}

TEST_CASE("Disc image rejects a root directory beyond the image",
          "[disc_image]") {
  TestDiscImage image;
  image.Put32(32 * kSectorSize + 20, 36);
  image.Write();
  DiscImageDevice device("", image.path);
  REQUIRE_FALSE(device.Initialize());
}

TEST_CASE("Disc image file reads reject data beyond the image",
          "[disc_image]") {
  TestDiscImage image;
  image.Put32(33 * kSectorSize + 4, 36);
  image.Write();
  DiscImageDevice device("", image.path);
  REQUIRE(device.Initialize());
  Entry* entry = device.ResolvePath("test.bin");
  REQUIRE(entry != nullptr);
  File* raw_file = nullptr;
  REQUIRE(entry->Open(filesystem::FileAccess::kGenericRead, &raw_file) ==
          X_STATUS_SUCCESS);
  std::unique_ptr<File> file(raw_file);
  std::array<uint8_t, 8> buffer{};
  size_t bytes_read = 999;
  REQUIRE(file->ReadSync(buffer, 0, &bytes_read) == X_STATUS_UNSUCCESSFUL);
  REQUIRE(bytes_read == 0);
}

}  // namespace xe::vfs::test
