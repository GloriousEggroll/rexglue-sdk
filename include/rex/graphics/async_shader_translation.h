#pragma once

#include <atomic>
#include <cstdint>

namespace rex::graphics {

// Publishes the translation binary, resource bindings, and shader module as
// one result. Shader::Translation's ordinary fields must not be read while a
// worker is pending, even if its intermediate is_translated flag is already set.
class AsyncShaderTranslation {
 public:
  enum class State : uint8_t { kNotStarted, kPending, kReady, kFailed };

  State state() const { return state_.load(std::memory_order_acquire); }
  bool TryQueue() {
    State expected = State::kNotStarted;
    return state_.compare_exchange_strong(expected, State::kPending,
                                          std::memory_order_acq_rel);
  }
  void Complete(bool success) {
    state_.store(success ? State::kReady : State::kFailed, std::memory_order_release);
    state_.notify_all();
  }
  State Wait() const {
    State result = state();
    while (result == State::kPending) {
      state_.wait(State::kPending, std::memory_order_acquire);
      result = state();
    }
    return result;
  }

 private:
  std::atomic<State> state_{State::kNotStarted};
};

}  // namespace rex::graphics
