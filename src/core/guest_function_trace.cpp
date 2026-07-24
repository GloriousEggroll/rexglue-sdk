/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Recompilation Toolkit                                  *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <rex/trace/guest_function.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(guest_function_trace_path, "", "Debug",
                      "Write first-seen guest function addresses to CSV")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::debug {

namespace {

constexpr uint32_t kGuestPageShift = 20;
constexpr uint32_t kGuestPageCount = 1u << (32 - kGuestPageShift);
constexpr uint32_t kFunctionsPerPage = (1u << kGuestPageShift) / sizeof(uint32_t);
constexpr uint32_t kBitsPerWord = 64;
constexpr uint32_t kWordsPerPage = kFunctionsPerPage / kBitsPerWord;

struct TracePage {
  std::array<std::atomic<uint64_t>, kWordsPerPage> seen{};
};

class GuestFunctionTrace {
 public:
  ~GuestFunctionTrace() {
    std::lock_guard lock(file_mutex_);
    if (file_) {
      std::fclose(file_);
    }
    for (auto& page : pages_) {
      delete page.load(std::memory_order_relaxed);
    }
  }

  void Record(uint32_t guest_address) {
    const uint32_t page_index = guest_address >> kGuestPageShift;
    TracePage* page = pages_[page_index].load(std::memory_order_acquire);
    if (!page) {
      auto* new_page = new (std::nothrow) TracePage();
      if (!new_page) {
        return;
      }
      if (!pages_[page_index].compare_exchange_strong(page, new_page, std::memory_order_release,
                                                      std::memory_order_acquire)) {
        delete new_page;
      } else {
        page = new_page;
      }
    }

    const uint32_t function_index =
        (guest_address & ((1u << kGuestPageShift) - 1)) / sizeof(uint32_t);
    const uint32_t word_index = function_index / kBitsPerWord;
    const uint64_t bit = uint64_t{1} << (function_index % kBitsPerWord);
    if (page->seen[word_index].fetch_or(bit, std::memory_order_relaxed) & bit) {
      return;
    }

    std::lock_guard lock(file_mutex_);
    if (!OpenFile()) {
      return;
    }
    std::fprintf(file_, "0x%08X\n", guest_address);
    if ((++pending_records_ & 0xFF) == 0) {
      std::fflush(file_);
    }
  }

 private:
  bool OpenFile() {
    if (file_) {
      return true;
    }
    if (open_attempted_) {
      return false;
    }
    open_attempted_ = true;

    const std::string& path = REXCVAR_GET(guest_function_trace_path);
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
      REXLOG_ERROR("Unable to open guest function trace '{}'", path);
      return false;
    }
    std::fputs("address\n", file_);
    REXLOG_INFO("Guest function coverage enabled: {}", path);
    return true;
  }

  std::array<std::atomic<TracePage*>, kGuestPageCount> pages_{};
  std::mutex file_mutex_;
  std::FILE* file_ = nullptr;
  uint32_t pending_records_ = 0;
  bool open_attempted_ = false;
};

GuestFunctionTrace& GetTrace() {
  static GuestFunctionTrace trace;
  return trace;
}

}  // namespace

bool IsGuestFunctionTraceEnabled() {
  return !REXCVAR_GET(guest_function_trace_path).empty();
}

void TraceGuestFunction(uint32_t guest_address) {
  GetTrace().Record(guest_address);
}

}  // namespace rex::debug
