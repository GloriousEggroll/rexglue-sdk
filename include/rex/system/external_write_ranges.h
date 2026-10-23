#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>

namespace rex::memory {

// The first host byte maps to physical_first, including the E alias's platform
// offset. Half-open physical bounds exclude its MMIO tail. This is mapping
// metadata only: it never proves readable/writable guest or host permissions.
struct ExternalWriteAlias {
  uintptr_t host_first;
  uint32_t physical_first, physical_end;
};
struct ExternalWriteRanges {
  std::array<std::pair<uint32_t, uint32_t>, 5> values{}; // physical address, bytes
  size_t count = 0;
};

// Called only for an admitted finite guard. A fixed five-range scratch array
// bounds stack use; rounding can add neighboring bytes in the same system page,
// exactly as existing physical invalidation callbacks require. No heap storage,
// guest reads, permission changes, or retained host addresses are involved.
inline ExternalWriteRanges ExternalWritePhysicalRanges(
    uintptr_t host, size_t length, uint32_t page_size,
    const std::array<ExternalWriteAlias, 5>& aliases) noexcept {
  if (!page_size || length > UINTPTR_MAX - host) std::terminate();
  ExternalWriteRanges result;
  if (!length) return result;
  for (const auto& alias : aliases) {
    if (alias.physical_first >= alias.physical_end ||
        alias.physical_end > 0x20000000u ||
        alias.physical_end - alias.physical_first > UINTPTR_MAX - alias.host_first)
      std::terminate();
    const auto begin = std::max(host, alias.host_first);
    const auto end = std::min(host + length,
        alias.host_first + (alias.physical_end - alias.physical_first));
    if (begin >= end) continue;
    const uint64_t physical_begin = alias.physical_first + (begin - alias.host_first);
    const uint64_t physical_end = alias.physical_first + (end - alias.host_first);
    const auto first = uint32_t(physical_begin / page_size * page_size);
    const auto last = uint32_t(std::min<uint64_t>(0x20000000u,
        (physical_end + page_size - 1) / page_size * page_size));
    result.values[result.count++] = {first, last}; // exclusive ends until merged
  }
  std::sort(result.values.begin(), result.values.begin() + result.count);
  size_t count = 0;
  for (size_t i = 0; i < result.count; ++i) {
    const auto range = result.values[i];
    if (count && range.first <= result.values[count - 1].second)
      result.values[count - 1].second = std::max(result.values[count - 1].second, range.second);
    else result.values[count++] = range;
  }
  result.count = count;
  for (size_t i = 0; i < count; ++i) result.values[i].second -= result.values[i].first;
  return result;
}

} // namespace rex::memory
