#pragma once

#include <cstdint>
#include <deque>

namespace rex::graphics {

// GPU command-thread owned. Results stay private until their submission fence
// completes. Guest report memory may be reused before an older result retires.
class PendingQueryResults {
 public:
  struct Entry {
    uint64_t submission;
    uint32_t host_index;
    uint32_t guest_address;
    bool publish = true;
  };

  void Push(uint64_t submission, uint32_t host_index, uint32_t guest_address) {
    entries_.push_back({submission, host_index, guest_address});
  }
  void InvalidateAddress(uint32_t guest_address) {
    for (auto& entry : entries_) {
      if (entry.guest_address == guest_address) entry.publish = false;
    }
  }
  bool empty() const { return entries_.empty(); }
  uint64_t first_submission() const { return empty() ? 0 : entries_.front().submission; }
  uint64_t last_submission() const { return empty() ? 0 : entries_.back().submission; }
  void Clear() { entries_.clear(); }

  template <typename Publish>
  void Drain(uint64_t completed_submission, Publish publish) {
    while (!entries_.empty() && entries_.front().submission <= completed_submission) {
      Entry entry = entries_.front();
      entries_.pop_front();
      if (entry.publish) publish(entry);
    }
  }

 private:
  std::deque<Entry> entries_;
};

}  // namespace rex::graphics
