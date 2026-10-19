/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_AUDIO_DRIVER_H_
#define XENIA_APU_AUDIO_DRIVER_H_

#include <functional>
#include <span>

#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace apu {

class AudioDriver {
 public:
  using CaptureCallback = std::function<void(
      const AudioDriver*, std::span<const float>, uint32_t, uint32_t)>;
  // Replacement waits for any current callback. Clear before destroying its
  // target. Capture callbacks must not call SetCaptureCallback recursively.
  static void SetCaptureCallback(CaptureCallback callback);
  static constexpr uint32_t kFrameFrequencyDefault = 48000;
  static constexpr uint32_t kFrameChannelsDefault = 6;
  static constexpr uint32_t kChannelSamplesDefault = 256;
  static constexpr uint32_t kFrameSamplesMax =
      kFrameChannelsDefault * kChannelSamplesDefault;
  static constexpr uint32_t kFrameSizeMax = sizeof(float) * kFrameSamplesMax;

  virtual ~AudioDriver();

  virtual bool Initialize() = 0;
  virtual void Shutdown() = 0;

  virtual void SubmitFrame(float* samples) = 0;
  virtual void Pause() = 0;
  virtual void Resume() = 0;
  virtual void SetVolume(float volume) = 0;

 protected:
  // Post-conversion, interleaved host float samples. An empty span removes this
  // source. Uses try_lock so an audio callback never waits for registration.
  void CaptureSamples(std::span<const float> samples, uint32_t channels,
                      uint32_t sample_rate);
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_AUDIO_DRIVER_H_
