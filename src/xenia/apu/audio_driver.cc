/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_driver.h"

#include <mutex>
#include <utility>

namespace xe {
namespace apu {

namespace {
std::mutex capture_mutex;
AudioDriver::CaptureCallback capture_callback;
}  // namespace

void AudioDriver::SetCaptureCallback(CaptureCallback callback) {
  std::lock_guard lock(capture_mutex);
  capture_callback = std::move(callback);
}

void AudioDriver::CaptureSamples(std::span<const float> samples,
                                 uint32_t channels, uint32_t sample_rate) {
  std::unique_lock lock(capture_mutex, std::try_to_lock);
  if (!lock || !capture_callback) {
    return;
  }
  // A diagnostic capture failure must never interrupt emulator audio playback.
  try {
    capture_callback(this, samples, channels, sample_rate);
  } catch (...) {
  }
}

AudioDriver::~AudioDriver() { CaptureSamples({}, 0, 0); }

}  // namespace apu
}  // namespace xe
