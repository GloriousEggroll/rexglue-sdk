#pragma once
#include <cstdint>
#include <utility>

namespace rex::graphics {
struct NativeCoherenceWait {
  uint32_t count, info, address, reference, mask, delay, status;
};
// Only cache actions whose GPU dependencies are owned by the native renderer.
// Destination-base, PGLB and unknown bits retain their completion boundary.
constexpr bool IsNativeCacheOnlyWait(const NativeCoherenceWait& wait) {
  constexpr uint32_t cache_actions = 0x03000000u;
  return wait.count == 5 && wait.info == 3 && wait.address == 0xa31 &&
      wait.reference == 0 && wait.mask == 0x80000000u &&
      (wait.status & cache_actions) != 0 &&
      (wait.status & ~(0x80000000u | cache_actions)) == 0;
}
struct NativeCoherenceOwnership {
  bool pending = false, native_authority = false, authority_retired = false;
  bool foreign_query = false, shutdown = false;
  bool draw_provider = false, query_provider = false;
  bool completion_provider = false, coherence_provider = false;
};
enum class NativeCoherenceDisposition { NoPending, Submitted, Completed };
// The native provider certifies immutable inputs, ordered GPU cache visibility
// and no pending CPU publication dependent on this status clear. It may submit /
// poll accepted work, but must not clear ownership or invent completion values.
// Missing or declined ownership retains the original full completion boundary.
template<class Submit, class Complete>
NativeCoherenceDisposition PrepareNativeCoherenceWait(
    const NativeCoherenceWait& wait, const NativeCoherenceOwnership& state,
    Submit&& submit, Complete&& complete) {
  if (!state.pending) return NativeCoherenceDisposition::NoPending;
  if (IsNativeCacheOnlyWait(wait) && state.native_authority && !state.authority_retired &&
      !state.foreign_query && !state.shutdown && state.draw_provider && state.query_provider &&
      state.completion_provider && state.coherence_provider && std::forward<Submit>(submit)()) {
    return NativeCoherenceDisposition::Submitted;
  }
  // Do not catch an exception after uncertain accepted work and retry it.
  std::forward<Complete>(complete)();
  return NativeCoherenceDisposition::Completed;
}
}  // namespace rex::graphics
