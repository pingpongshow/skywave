// Monotonic + injectable clock (ARCHITECTURE.md: the modem worker is a pure
// function of samples, commands, and clock - tests inject time).
#pragma once

#include <chrono>
#include <cstdint>

namespace skw {

// Monotonic microseconds. The worker and all link timers consume this
// interface only; production uses SteadyClock, tests use FakeClock.
class Clock {
 public:
  virtual ~Clock() = default;
  virtual int64_t nowUs() = 0;
};

class SteadyClock final : public Clock {
 public:
  int64_t nowUs() override {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch())
        .count();
  }
};

class FakeClock final : public Clock {
 public:
  explicit FakeClock(int64_t startUs = 0) : now_(startUs) {}
  int64_t nowUs() override { return now_; }
  void advanceUs(int64_t us) { now_ += us; }
  void advanceSec(double s) { now_ += int64_t(s * 1e6); }

 private:
  int64_t now_;
};

}  // namespace skw
