/**
 * @file        rex/atomic_shared_ptr.h
 * @brief       std::atomic<std::shared_ptr<T>>, or an equivalent where the
 *              standard library lacks it (libc++).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <atomic>
#include <memory>
#include <mutex>

namespace rex {

#if defined(_LIBCPP_VERSION)
template <typename T>
class AtomicSharedPtr {
 public:
  std::shared_ptr<T> load() const {
    std::lock_guard lock(mutex_);
    return value_;
  }
  void store(std::shared_ptr<T> value) {
    {
      std::lock_guard lock(mutex_);
      value_.swap(value);
    }
    // The previous value is released here, outside the lock.
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<T> value_;
};
#else
template <typename T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;
#endif

}  // namespace rex
