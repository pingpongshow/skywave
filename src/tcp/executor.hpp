// Runs control commands on a thread of its own, in submission order.
//
// ModemWorker::control() takes the modem mutex, which the worker holds
// through whole decode attempts (0.5 s typical, >1 s measured). Called on
// the main thread it froze the GUI and stalled every TCP port for that
// long; through this executor the caller only queues the line and picks
// up the answer when it is ready. One thread per executor keeps one
// client's commands strictly ordered.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace skw {

class ControlExecutor {
 public:
  using Fn = std::function<std::string(const std::string&)>;
  struct Result {
    uint64_t tag;          // caller's label (e.g. the client generation)
    std::string line;      // the command
    std::string response;  // the dispatcher's answer
  };

  explicit ControlExecutor(Fn fn) : fn_(std::move(fn)), thread_([this] { run(); }) {}
  ~ControlExecutor() {
    {
      std::lock_guard lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  ControlExecutor(const ControlExecutor&) = delete;
  ControlExecutor& operator=(const ControlExecutor&) = delete;

  void post(std::string line, uint64_t tag = 0) {
    {
      std::lock_guard lock(mu_);
      queue_.push_back({tag, std::move(line), {}});
    }
    cv_.notify_one();
  }
  // Completed commands, oldest first.
  std::vector<Result> takeResults() {
    std::lock_guard lock(mu_);
    std::vector<Result> out(done_.begin(), done_.end());
    done_.clear();
    return out;
  }
  bool idle() {
    std::lock_guard lock(mu_);
    return queue_.empty() && !busy_;
  }

 private:
  void run() {
    std::unique_lock lock(mu_);
    while (true) {
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty()) return;  // stop_ with nothing left
      Result r = std::move(queue_.front());
      queue_.pop_front();
      busy_ = true;
      lock.unlock();
      r.response = fn_(r.line);
      lock.lock();
      busy_ = false;
      done_.push_back(std::move(r));
    }
  }

  Fn fn_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Result> queue_, done_;
  bool stop_ = false, busy_ = false;
  std::thread thread_;  // last: starts after the members it uses
};

}  // namespace skw
