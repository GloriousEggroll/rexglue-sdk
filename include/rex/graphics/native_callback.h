#pragma once

#include <functional>
#include <memory>
#include <utility>

namespace rex::graphics {

// CopyPerInvocation preserves std::function's existing callable-copy semantics.
// SharedImmutable is an explicit contract: the callable itself must not retain
// mutable invocation state. Its referenced external owner may still mutate,
// subject to the existing command-processor thread confinement. Copy constructor
// side effects are also excluded from this opt-in contract.
enum class NativeCallbackStorage { CopyPerInvocation, SharedImmutable };

// Installation, removal and snapshotting stay on the command-processor thread.
// Each Snapshot owns its callable independently of the installed callback, so a
// nested invocation, handback or terminal callback may remove/replace the slot
// without destroying a callable that is currently executing. There is no new
// mutex, worker or publication protocol.
template <class Function>
class NativeCallback {
 public:
  class Invocation {
   public:
    explicit operator bool() const noexcept {
      return bool(shared_) || bool(copied_);
    }

    template <class... Args>
    decltype(auto) operator()(Args&&... args) const {
      if (shared_) {
        return (*shared_)(std::forward<Args>(args)...);
      }
      // An empty snapshot preserves std::function's bad_function_call.
      return copied_(std::forward<Args>(args)...);
    }

   private:
    friend class NativeCallback;
    Function copied_;
    std::shared_ptr<const Function> shared_;
  };

  NativeCallback() = default;
  NativeCallback(const NativeCallback&) = delete;
  NativeCallback& operator=(const NativeCallback&) = delete;

  explicit operator bool() const noexcept {
    return bool(shared_) || bool(copied_);
  }

  void Set(Function function, NativeCallbackStorage storage =
                                  NativeCallbackStorage::CopyPerInvocation) {
    // Allocate before changing the installed owner. An admission failure leaves
    // the previous provider usable, and no in-progress Snapshot is modified.
    std::shared_ptr<const Function> shared;
    Function copied;
    if (function && storage == NativeCallbackStorage::SharedImmutable) {
      shared = std::make_shared<const Function>(std::move(function));
    } else {
      copied = std::move(function);
    }
    shared_.swap(shared);
    copied_.swap(copied);
  }

  void Reset() noexcept {
    shared_.reset();
    copied_ = {};
  }

  Invocation Snapshot() const {
    Invocation result;
    if (shared_) {
      result.shared_ = shared_;
    } else {
      result.copied_ = copied_;
    }
    return result;
  }

 private:
  Function copied_;
  std::shared_ptr<const Function> shared_;
};

}  // namespace rex::graphics
