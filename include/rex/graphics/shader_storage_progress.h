#pragma once

#include <cstddef>
#include <mutex>

namespace rex::graphics {

// Startup-only progress, shared by the GPU preload workers and the UI thread.
// A locked snapshot keeps counts and phase consistent during phase changes.
class ShaderStorageProgress {
 public:
  enum class Phase { kReading, kShaders, kPipelines, kComplete };
  enum class Outcome { kPrepared, kSkipped, kFailed };
  struct Snapshot {
    Phase phase = Phase::kReading;
    size_t completed = 0;
    size_t total = 0;
    size_t failed = 0;
    size_t skipped = 0;
  };

  void Begin(Phase phase, size_t total = 0) {
    std::lock_guard lock(mutex_);
    snapshot_ = {phase, 0, total, 0, 0};
  }
  void Advance(bool success = true) {
    Advance(success ? Outcome::kPrepared : Outcome::kFailed);
  }
  // Expected optional admission refusals consume work without reporting a
  // compilation/load failure. Callers must still use kFailed for real errors.
  void Advance(Outcome outcome) {
    std::lock_guard lock(mutex_);
    ++snapshot_.completed;
    snapshot_.failed += outcome == Outcome::kFailed;
    snapshot_.skipped += outcome == Outcome::kSkipped;
  }
  Snapshot Read() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
  }
  void Finish() {
    std::lock_guard lock(mutex_);
    snapshot_.phase = Phase::kComplete;
  }

 private:
  mutable std::mutex mutex_;
  Snapshot snapshot_;
};

}  // namespace rex::graphics
