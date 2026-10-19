// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_APP_STREAM_OUTPUT_H_
#define XENIA_APP_STREAM_OUTPUT_H_

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace xe::app {
class StreamOutput {
 public:
  StreamOutput();
  ~StreamOutput();
  bool SetDestination(uint32_t receiver, const std::string& url);
  void RemoveDestination(uint32_t receiver);
  void Clear();
  bool Empty() const;
  std::string GetDestination(uint32_t receiver) const;
  int Write(std::span<const uint8_t> bytes);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace xe::app
#endif
