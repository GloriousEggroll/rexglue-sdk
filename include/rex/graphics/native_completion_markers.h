#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>

namespace rex::graphics {
// Exact title-emitted position/serial pair. 32 command bytes, eight target
// bytes. No GPU completion receipt, delayed store, or borrowed target pointer.
struct NativeCompletionMarkerPair {
  std::array<uint32_t,8> words{};
  uint32_t counter=0,command_address=0,command_buffer=0,command_capacity=0;
};
inline bool IsNativeCompletionMarkerPair(const NativeCompletionMarkerPair& pair) noexcept {
  const auto& w=pair.words;
  return !(pair.counter&3)&&!(pair.command_address&3)&&!(pair.command_buffer&3)&&
      w[0]==0xc0025800u&&w[1]==3&&w[4]==0xc0025800u&&w[5]==3&&
      (w[2]&3)==2&&(w[6]&3)==2&&pair.counter==(w[6]&~3u)&&
      uint64_t(pair.counter)+96<=0x20000000ull&&(w[2]&~3u)==pair.counter+4&&
      pair.command_capacity>=32&&pair.command_address>=pair.command_buffer&&
      uint64_t(pair.command_address)+32<=uint64_t(pair.command_buffer)+pair.command_capacity&&
      uint64_t(pair.command_buffer)+pair.command_capacity<=0x20000000ull&&
      (uint64_t(pair.command_address)+32<=pair.counter||
       uint64_t(pair.counter)+96<=pair.command_address);
}
struct NativeImmediateMarkerProviders {
  // Full-prefix immutable ownership and ZERO active/current/unpublished
  // queries. Seals/submits original work and leaves its real fence pending.
  std::function<bool()> prepare;
  // Called only after prepare: exact emitted-source/title-epoch/allocator
  // validation and a short guard over the original two immediate stores.
  // This guard may not wait for rendering or survive the pair's decoder call.
  std::function<std::shared_ptr<void>(const NativeCompletionMarkerPair&)> acquire;
  explicit operator bool()const noexcept{return prepare&&acquire;}
};
} // namespace rex::graphics
