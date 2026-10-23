#pragma once

#include <algorithm>
#include <chrono>
#include <thread>

namespace rex::graphics {

// A ceiling only: late frames reset the schedule instead of causing a burst
// of catch-up frames. Zero disables the limiter. Uses real time, not guest time.
class FrameLimiter {
 public:
  using Clock = std::chrono::steady_clock;
  Clock::time_point Deadline(Clock::time_point now, double limit) const {
    if (!started_ || limit <= 0) return now;
    auto interval = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / limit));
    return std::max(now, last_ + interval);
  }
  void Presented(Clock::time_point now) { last_ = now; started_ = true; }
  void Wait(double limit) {
    auto now = Clock::now();
    auto deadline = Deadline(now, limit);
    if (deadline > now) std::this_thread::sleep_until(deadline);
    Presented(Clock::now());
  }

 private:
  bool started_ = false;
  Clock::time_point last_{};
};

}  // namespace rex::graphics
