#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <limits>

namespace rex::memory {

// Classification only. This token neither pins backing nor permits delayed
// access. The caller holds the existing global memory lock while registering,
// invalidating, or using a current token to authorize synchronous effects.
class PhysicalAllocationObservations {
 public:
  class Token {
   public:
    bool current() const noexcept {return current_.load(std::memory_order_acquire);}
    const uint32_t address,bytes;
   private:
    friend class PhysicalAllocationObservations;
    Token(uint32_t a,uint32_t n):address(a),bytes(n){}
    std::atomic<bool> current_{true};
  };
  using Ptr=std::shared_ptr<const Token>;
  static constexpr size_t kCapacity=8;

  Ptr Observe(uint32_t address,uint32_t bytes) {
    if(!bytes||uint64_t(address)+bytes>0x20000000ull)return {};
    size_t vacant=kCapacity;
    for(size_t i=0;i<slots_.size();++i) {
      auto token=slots_[i].lock();
      if(!token||!token->current()) {if(vacant==kCapacity)vacant=i;continue;}
      if(token->address==address&&token->bytes==bytes)return token;
    }
    if(vacant==kCapacity)return {};
    auto token=std::shared_ptr<Token>(new Token(address,bytes));
    slots_[vacant]=token;used_=true;return token;
  }
  void Invalidate(uint32_t address,uint64_t bytes) noexcept {
    if(!used_||!bytes)return;
    const uint64_t end=bytes>std::numeric_limits<uint64_t>::max()-address?
        std::numeric_limits<uint64_t>::max():uint64_t(address)+bytes;
    for(auto& slot:slots_)if(auto token=slot.lock())
      if(uint64_t(address)<uint64_t(token->address)+token->bytes&&uint64_t(token->address)<end)
        token->current_.store(false,std::memory_order_release);
  }
 private:
  bool used_=false;
  std::array<std::weak_ptr<Token>,kCapacity> slots_{};
};

} // namespace rex::memory
