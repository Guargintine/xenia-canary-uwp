// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_KERNEL_ASYNC_WORK_H_
#define XENIA_KERNEL_ASYNC_WORK_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace xe::kernel {

class AsyncWorkGroup {
 public:
  ~AsyncWorkGroup() { Stop(); }

  std::stop_token stop_token() const { return shutdown_.get_token(); }

  std::stop_source Start(std::function<void(std::stop_token)> function) {
    std::lock_guard lock(mutex_);
    if (!accepting_) {
      return std::stop_source(std::nostopstate);
    }
    std::erase_if(work_, [](const auto& work) {
      return work->finished->load();
    });
    auto work = std::make_shared<Work>();
    work->thread = std::jthread(
        [finished = work->finished, function = std::move(function)](
            std::stop_token stop) {
          function(stop);
          finished->store(true);
        });
    work->stop = work->thread.get_stop_source();
    work_.push_back(work);
    return work->stop;
  }

  void Wait(const std::stop_source& source) {
    std::shared_ptr<Work> found;
    {
      std::lock_guard lock(mutex_);
      auto item = std::find_if(work_.begin(), work_.end(), [&](const auto& work) {
        return work->stop.get_token() == source.get_token();
      });
      if (item != work_.end()) {
        found = *item;
      }
    }
    if (found) {
      found->Join();
    }
  }

  void CancelAndWait(std::stop_source source) {
    source.request_stop();
    Wait(source);
  }

  void Stop() {
    std::lock_guard stopping_lock(stopping_mutex_);
    std::vector<std::shared_ptr<Work>> work;
    {
      std::lock_guard lock(mutex_);
      accepting_ = false;
      work = work_;
    }
    shutdown_.request_stop();
    for (const auto& item : work) {
      item->stop.request_stop();
    }
    for (const auto& item : work) {
      item->Join();
    }
    std::lock_guard lock(mutex_);
    work_.clear();
  }

  static bool Delay(std::stop_token stop, std::chrono::milliseconds duration) {
    std::mutex mutex;
    std::condition_variable_any condition;
    std::unique_lock lock(mutex);
    condition.wait_for(lock, stop, duration, [] { return false; });
    return !stop.stop_requested();
  }

 private:
  struct Work {
    void Join() {
      std::lock_guard lock(mutex);
      if (thread.joinable()) {
        thread.join();
      }
    }
    std::mutex mutex;
    std::jthread thread;
    std::stop_source stop;
    std::shared_ptr<std::atomic<bool>> finished =
        std::make_shared<std::atomic<bool>>(false);
  };

  std::mutex mutex_;
  std::mutex stopping_mutex_;
  std::stop_source shutdown_;
  bool accepting_ = true;
  std::vector<std::shared_ptr<Work>> work_;
};

}  // namespace xe::kernel
#endif  // XENIA_KERNEL_ASYNC_WORK_H_
