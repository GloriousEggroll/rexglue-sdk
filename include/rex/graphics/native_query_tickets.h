#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace rex::graphics {

struct NativeQueryIdentity {
  uint32_t end_address = 0;
  uint64_t generation = 0;
  uint64_t reference_frame = 0;
  bool native_authority = false;
  bool operator==(const NativeQueryIdentity&) const = default;
};

// CPU-only publication capabilities. A closed query keeps its exact identity
// until its GPU work has completed, even after its packet callback has returned.
// Superseding a report prevents an old write, but does not retire GPU ownership.
class NativeQueryTickets {
 public:
  static constexpr size_t kCapacity = 128;
  enum class EndStatus { Outstanding, Deferred, Published };
  struct Statistics {
    uint64_t deferred = 0;
    uint64_t retired = 0;
    uint64_t guest_written = 0;
    uint64_t superseded = 0;
    size_t pending = 0;
    size_t peak = 0;
  };
  NativeQueryTickets() = default;
  NativeQueryTickets(const NativeQueryTickets&) = delete;
  NativeQueryTickets& operator=(const NativeQueryTickets&) = delete;

  class EndScope {
   public:
    EndScope(NativeQueryTickets& owner, const NativeQueryIdentity& identity)
        : owner_(owner) {
      if (owner_.current_end_)
        throw std::runtime_error("Overlapping native query end callbacks");
      if (!identity.generation)
        throw std::runtime_error("Native query end has no generation");
      owner_.current_end_ = identity;
      owner_.end_status_ = EndStatus::Outstanding;
    }
    EndScope(const EndScope&) = delete;
    EndScope& operator=(const EndScope&) = delete;
    ~EndScope() { owner_.current_end_.reset(); }
   private:
    NativeQueryTickets& owner_;
  };

  [[nodiscard]] EndScope BeginEndCallback(const NativeQueryIdentity& identity) {
    return EndScope(*this, identity);
  }
  const std::optional<NativeQueryIdentity>& current_end() const { return current_end_; }
  EndStatus end_status() const { return end_status_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  bool full() const { return size_ == kCapacity; }
  Statistics statistics() const {
    return {deferred_, retired_, guest_written_, superseded_, size_, peak_};
  }

  void Defer(const NativeQueryIdentity& identity) {
    if (!IsCurrent(identity) || end_status_ != EndStatus::Outstanding ||
        !identity.native_authority)
      throw std::runtime_error("Native query deferral has no exact end capability");
    if (full()) throw std::runtime_error("Native query ticket capacity exhausted");
    if (Find(identity)) throw std::runtime_error("Duplicate native query ticket");
    for (auto& ticket : tickets_) {
      if (ticket.occupied) continue;
      ticket = {identity, true, true};
      ++size_;
      ++deferred_;
      if (size_ > peak_) peak_ = size_;
      end_status_ = EndStatus::Deferred;
      return;
    }
    throw std::runtime_error("Native query ticket count mismatch");
  }

  // Only called when a newer guest begin reuses this report. Old tickets still
  // require their real GPU completion and exact retirement capability.
  void SupersedeAddress(uint32_t address) {
    for (auto& ticket : tickets_)
      if (ticket.occupied && ticket.identity.end_address == address)
        ticket.publishable = false;
  }

  template<class Publisher>
  bool Publish(const NativeQueryIdentity& identity, Publisher&& publisher) {
    const bool current = IsCurrent(identity);
    Ticket* ticket = Find(identity);
    if (!ticket && !(current && end_status_ == EndStatus::Outstanding))
      throw std::runtime_error("Native query publication has no exact capability");
    const bool write = !ticket || ticket->publishable;
    // A failing publisher retains both the ticket and callback state. Neither
    // the caller nor the completion latch may report this work as retired.
    if (write) std::forward<Publisher>(publisher)();
    if (ticket) {
      ticket->occupied = false;
      --size_;
      ++retired_;
      if (write) ++guest_written_;
      else ++superseded_;
    }
    if (current) end_status_ = EndStatus::Published;
    return write;
  }

 private:
  struct Ticket {
    NativeQueryIdentity identity;
    bool occupied = false;
    bool publishable = false;
  };
  bool IsCurrent(const NativeQueryIdentity& identity) const {
    return current_end_ && *current_end_ == identity;
  }
  Ticket* Find(const NativeQueryIdentity& identity) {
    for (auto& ticket : tickets_)
      if (ticket.occupied && ticket.identity == identity) return &ticket;
    return nullptr;
  }
  std::array<Ticket, kCapacity> tickets_{};
  size_t size_ = 0, peak_ = 0;
  uint64_t deferred_ = 0, retired_ = 0, guest_written_ = 0, superseded_ = 0;
  std::optional<NativeQueryIdentity> current_end_;
  EndStatus end_status_ = EndStatus::Outstanding;
};

}  // namespace rex::graphics
