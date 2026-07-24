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
#include <bit>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_STRING(guest_function_trace_path, "", "Debug",
                      "Write first-seen guest function addresses to CSV")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(guest_function_state_trace_path, "", "Debug",
                      "Write selected guest function register and memory state to CSV")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(guest_function_state_trace_address, 0, "Debug",
                      "Guest function address to trace state for")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(guest_function_state_trace_lr, 0, "Debug",
                      "Optional guest link register filter for function state tracing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(guest_function_state_trace_memory, false, "Debug",
                    "Include 64-byte memory snapshots for r3, r4, and r5")
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

uint32_t LoadGuestU32(const uint8_t* memory_base, uint32_t address) {
  uint32_t value;
  size_t host_offset = address;
#if REX_PLATFORM_WIN32
  if (address >= 0xE0000000u) {
    host_offset += 0x1000;
  }
#endif
  std::memcpy(&value, memory_base + host_offset, sizeof(value));
  if constexpr (std::endian::native == std::endian::little) {
    value = std::byteswap(value);
  }
  return value;
}

void WriteGuestWords(std::FILE* file, const uint8_t* memory_base,
                     uint32_t address) {
  std::fputc('"', file);
  for (uint32_t offset = 0; offset < 64; offset += sizeof(uint32_t)) {
    std::fprintf(file, "%s%08X", offset ? ":" : "",
                 LoadGuestU32(memory_base, address + offset));
  }
  std::fputc('"', file);
}

class GuestFunctionStateTrace {
 public:
  ~GuestFunctionStateTrace() {
    std::lock_guard lock(file_mutex_);
    if (file_) {
      std::fclose(file_);
    }
  }

  void Record(uint32_t guest_address, const PPCContext& context,
              const uint8_t* memory_base) {
    const uint32_t lr = uint32_t(context.lr);
    const uint32_t lr_filter = REXCVAR_GET(guest_function_state_trace_lr);
    if (lr_filter && lr_filter != lr) {
      return;
    }

    std::lock_guard lock(file_mutex_);
    if (!OpenFile()) {
      return;
    }

    std::fprintf(file_,
                 "0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,"
                 "0x%08X,0x%08X,0x%08X,0x%08X,",
                 guest_address, lr, context.r3.u32, context.r4.u32,
                 context.r5.u32, context.r6.u32, context.r7.u32,
                 context.r8.u32, context.r9.u32, context.r10.u32);
    std::fflush(file_);
    if (!REXCVAR_GET(guest_function_state_trace_memory)) {
      std::fputs(",,\n", file_);
      std::fflush(file_);
      return;
    }
    WriteGuestWords(file_, memory_base, context.r3.u32);
    std::fputc(',', file_);
    std::fflush(file_);
    WriteGuestWords(file_, memory_base, context.r4.u32);
    std::fputc(',', file_);
    std::fflush(file_);
    WriteGuestWords(file_, memory_base, context.r5.u32);
    std::fputc('\n', file_);
    std::fflush(file_);
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

    const std::string& path =
        REXCVAR_GET(guest_function_state_trace_path);
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
      REXLOG_ERROR("Unable to open guest function state trace '{}'", path);
      return false;
    }
    std::fputs(
        "address,lr,r3,r4,r5,r6,r7,r8,r9,r10,r3mem,r4mem,r5mem\n",
        file_);
    std::fflush(file_);
    REXLOG_INFO("Guest function state trace enabled: {}", path);
    return true;
  }

  std::mutex file_mutex_;
  std::FILE* file_ = nullptr;
  bool open_attempted_ = false;
};

GuestFunctionStateTrace& GetStateTrace() {
  static GuestFunctionStateTrace trace;
  return trace;
}

}  // namespace

bool IsGuestFunctionTraceEnabled() {
  return !REXCVAR_GET(guest_function_trace_path).empty();
}

void TraceGuestFunction(uint32_t guest_address) {
  GetTrace().Record(guest_address);
}

bool IsGuestFunctionStateTraceEnabled(uint32_t guest_address) {
  return !REXCVAR_GET(guest_function_state_trace_path).empty() &&
         REXCVAR_GET(guest_function_state_trace_address) == guest_address;
}

void TraceGuestFunctionState(uint32_t guest_address,
                             const PPCContext& context,
                             const uint8_t* memory_base) {
  GetStateTrace().Record(guest_address, context, memory_base);
}

}  // namespace rex::debug
