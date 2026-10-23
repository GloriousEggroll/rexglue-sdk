#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rex::graphics {

// CP-thread-only sticky policy. Shared host resources and final presentation
// are permitted; reference scene raster, copy, query and source lookup are not.
// Shutdown releases resources without downgrading this policy.
class NativeSceneAuthority {
 public:
  enum class ReferenceOperation { Draw, Copy, Query, SwapSource, StateImport, StateRestore, Count };
  using Counts = std::array<uint64_t, size_t(ReferenceOperation::Count)>;
  using FailureObserver = void(*)(void*,std::string_view) noexcept;
  void SetFailureObserver(void* context,FailureObserver observer) noexcept {
    observer_context_ = context; observer_ = observer;
  }
  bool strict() const noexcept { return strict_; }
  const Counts& reference_counts() const noexcept { return reference_counts_; }
  uint64_t rejected() const noexcept { return rejected_; }

  void EnableStrict() {
    if (strict_) Reject("native authority was installed twice");
    for (const auto count : reference_counts_)
      if (count) Reject("native authority cannot follow Xenia scene work");
    strict_ = true;
  }
  void Reference(ReferenceOperation operation, std::string_view context) {
    if (strict_) Reject(context);
    ++reference_counts_.at(size_t(operation));
  }
  void Require(bool condition, std::string_view context) {
    if (strict_ && !condition) Reject(context);
  }
  void ProviderChange(std::string_view context) {
    if (strict_) Reject(context);
  }
  [[noreturn]] void Reject(std::string_view context) {
    ++rejected_;
    if(observer_)observer_(observer_context_,context);
    throw std::runtime_error("Native renderer stopped: " + std::string(context) +
        ". Automatic fallback is disabled; select Xenia and restart to use it.");
  }
 private:
  void* observer_context_ = nullptr;
  FailureObserver observer_ = nullptr;
  bool strict_ = false;
  Counts reference_counts_{};
  uint64_t rejected_ = 0;
};

}  // namespace rex::graphics
