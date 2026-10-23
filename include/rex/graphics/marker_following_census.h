#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gow3::diagnostic {

// Diagnostic only. No submission, waiting, polling, clock, guest memory,
// ownership or query publication operation is available to this class.
// One CP thread owns it. Snapshots must be taken outside queue retirement.
class MarkerFollowingCensus {
 public:
  static constexpr size_t kMaximumOpenMarkers = 64;
  static constexpr size_t kHistogramBuckets = 7;  // 0,1,2-3,4-15,16-63,64-255,256+.
  enum class Boundary : uint8_t {
    CommandPacket, ScratchWrite, Wait, PrimaryBufferEnd, Presentation,
    ReferenceDraw, ProviderRemoval, QueryPressure, Shutdown, Count
  };
  enum class Censor : uint8_t {
    Disabled, GenerationChanged, Unavailable, InvalidSnapshot,
    FrontierRegression, FailedCompletion, InvalidBoundary, Count
  };
  struct Snapshot {
    uint64_t generation = 0;
    uint64_t accepted_draws = 0, accepted_transfers = 0;
    uint64_t accepted_work = 0, completed_work = 0;
    bool available = false;
  };
  struct Summary {
    uint64_t successful_full_58_seen = 0, admitted = 0, overflow_dropped = 0;
    uint64_t rejected_marker_snapshots = 0, finalized = 0;
    uint64_t no_later_work = 0, draw_only = 0, transfer_only = 0, mixed = 0;
    uint64_t maximum_draw_distance = 0, maximum_transfer_distance = 0;
    uint64_t maximum_work_distance = 0;
    std::array<uint64_t, kHistogramBuckets> draw_distance{}, transfer_distance{}, work_distance{};
    std::array<uint64_t, size_t(Boundary::Count)> finalized_by_boundary{};
    std::array<uint64_t, size_t(Censor::Count)> censored{};
    bool counter_overflow = false;
  };
  MarkerFollowingCensus() = default;
  MarkerFollowingCensus(const MarkerFollowingCensus&) = delete;
  MarkerFollowingCensus& operator=(const MarkerFollowingCensus&) = delete;
  MarkerFollowingCensus(MarkerFollowingCensus&&) = delete;
  MarkerFollowingCensus& operator=(MarkerFollowingCensus&&) = delete;

  void SetEnabled(bool enabled) noexcept {
    if (!enabled) { Discard(Censor::Disabled); have_last_ = false; }
    enabled_ = enabled;
  }
  // Invoke only after an actual pending Native full completion for opcode58
  // returned successfully. An early-return/no-pending packet is not a seed.
  // Consecutive58 packets do not end windows: a following paired marker is not
  // the non-marker boundary whose deferral opportunity is being measured.
  void SuccessfulFull58(const Snapshot& snapshot) noexcept {
    if (!enabled_) return;
    Increment(summary_.successful_full_58_seen);
    if (!Observe(snapshot)) {
      Increment(summary_.rejected_marker_snapshots);
      return;
    }
    if (snapshot.completed_work != snapshot.accepted_work) {
      Discard(Censor::InvalidSnapshot); have_last_ = false;
      Increment(summary_.rejected_marker_snapshots);
      return;
    }
    if (open_ == kMaximumOpenMarkers) { Increment(summary_.overflow_dropped); return; }
    markers_[open_++] = snapshot;
    Increment(summary_.admitted);
  }
  // Invoke before each attempted NON58 existing completion/publication
  // boundary selected by the adapter, including primary/idle/removal/shutdown
  // when the original pending latch is false. Current SDK QueryPressure is
  // full completion; the Runtime logical-prefix pressure path is separate.
  // Report adapter coverage; never force a diagnostic wait.
  void MandatoryBoundary(const Snapshot& snapshot, Boundary boundary) noexcept {
    if (!enabled_) return;
    if (size_t(boundary) >= size_t(Boundary::Count)) {
      Discard(Censor::InvalidBoundary); have_last_ = false; return;
    }
    if (!Observe(snapshot)) return;
    for (size_t i = 0; i < open_; ++i) {
      const auto& marker = markers_[i];
      const uint64_t draws = snapshot.accepted_draws - marker.accepted_draws;
      const uint64_t transfers = snapshot.accepted_transfers - marker.accepted_transfers;
      const uint64_t work = snapshot.accepted_work - marker.accepted_work;
      Increment(summary_.finalized);
      Increment(summary_.finalized_by_boundary[size_t(boundary)]);
      Increment(summary_.draw_distance[Bucket(draws)]);
      Increment(summary_.transfer_distance[Bucket(transfers)]);
      Increment(summary_.work_distance[Bucket(work)]);
      if (!work) Increment(summary_.no_later_work);
      else if (!transfers) Increment(summary_.draw_only);
      else if (!draws) Increment(summary_.transfer_only);
      else Increment(summary_.mixed);
      if (draws > summary_.maximum_draw_distance) summary_.maximum_draw_distance = draws;
      if (transfers > summary_.maximum_transfer_distance) summary_.maximum_transfer_distance = transfers;
      if (work > summary_.maximum_work_distance) summary_.maximum_work_distance = work;
    }
    open_ = 0;
  }
  // Configuration/lifetime/failure changes censor open measurements rather
  // than misclassifying their unknown tail as zero following work.
  void Discard(Censor reason) noexcept {
    if (size_t(reason) >= size_t(Censor::Count)) reason = Censor::InvalidSnapshot;
    Add(summary_.censored[size_t(reason)], uint64_t(open_));
    open_ = 0;
  }
  void FailedCompletion() noexcept { Discard(Censor::FailedCompletion); have_last_ = false; }
  size_t open_markers() const noexcept { return open_; }
  const Summary& summary() const noexcept { return summary_; }

 private:
  static size_t Bucket(uint64_t value) noexcept {
    return value == 0 ? 0 : value == 1 ? 1 : value < 4 ? 2 : value < 16 ? 3 :
           value < 64 ? 4 : value < 256 ? 5 : 6;
  }
  void Increment(uint64_t& counter) noexcept { Add(counter, 1); }
  void Add(uint64_t& counter, uint64_t amount) noexcept {
    if (amount > std::numeric_limits<uint64_t>::max() - counter) {
      counter = std::numeric_limits<uint64_t>::max(); summary_.counter_overflow = true;
    } else counter += amount;
  }
  bool Observe(const Snapshot& snapshot) noexcept {
    if (!snapshot.available) { Discard(Censor::Unavailable); have_last_ = false; return false; }
    // Use subtraction to validate the sum without uint64 overflow.
    if (!snapshot.generation || snapshot.completed_work > snapshot.accepted_work ||
        snapshot.accepted_draws > snapshot.accepted_work ||
        snapshot.accepted_transfers != snapshot.accepted_work - snapshot.accepted_draws) {
      Discard(Censor::InvalidSnapshot); have_last_ = false; return false;
    }
    if (have_last_ && snapshot.generation != last_.generation) {
      Discard(Censor::GenerationChanged); have_last_ = false;
    }
    if (have_last_ && (snapshot.accepted_work < last_.accepted_work ||
        snapshot.completed_work < last_.completed_work ||
        snapshot.accepted_draws < last_.accepted_draws ||
        snapshot.accepted_transfers < last_.accepted_transfers)) {
      Discard(Censor::FrontierRegression); have_last_ = false; return false;
    }
    last_ = snapshot; have_last_ = true; return true;
  }
  bool enabled_ = false, have_last_ = false;
  size_t open_ = 0;
  Snapshot last_{};
  std::array<Snapshot, kMaximumOpenMarkers> markers_{};
  Summary summary_{};
};
}  // namespace gow3::diagnostic
