#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace rex::graphics {

// A query may switch from native to reference exactly once. Reference command
// submissions may split the remaining host query; accumulate raw samples before
// resolution normalization so rounding is independent of submission boundaries.
struct NativeQueryAccumulator {
  uint32_t begin_address = 0;
  uint64_t generation = 0;
  uint64_t reference_segments = 0;
  uint64_t native_samples = 0;
  uint64_t reference_samples = 0;
  bool final_recorded = false;
  bool published = false;

  static uint64_t SaturatingAdd(uint64_t a, uint64_t b) {
    return a > std::numeric_limits<uint64_t>::max() - b ?
        std::numeric_limits<uint64_t>::max() : a + b;
  }
  void RetireSegment(uint64_t samples, bool final) {
    if (published || (final && !final_recorded))
      throw std::runtime_error("Invalid native/reference query segment retirement");
    reference_samples = SaturatingAdd(reference_samples, samples);
    ++reference_segments;
    published = final;
  }
  uint64_t Total(uint64_t normalized_reference) const {
    if (!published) throw std::runtime_error("Query read before final segment retirement");
    return SaturatingAdd(native_samples, normalized_reference);
  }
};

}  // namespace rex::graphics
