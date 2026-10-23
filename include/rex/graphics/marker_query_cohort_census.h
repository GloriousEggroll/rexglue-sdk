#pragma once
#include <rex/graphics/marker_following_census.h>

namespace gow3::diagnostic {
// CP-only opportunity measurement, never an allocation or async capability.
// Existing synchronous marker completion and stores remain authoritative.
class MarkerQueryCohortCensus {
 public:
  using Census=MarkerFollowingCensus;
  using Snapshot=Census::Snapshot;
  enum class Boundary : uint8_t { CommandPacket,ScratchWrite,Wait,PrimaryBufferEnd,
    Presentation,ReferenceDraw,ProviderRemoval,QueryPressure,Shutdown,
    QueryBegin,QueryEnd,CpuWrite,MemoryCopy,CanonicalWait,NoncanonicalWait,UnknownOperation,Count };
  enum class Tickets : uint8_t { Zero,AllReady,SomePending,Count };
  struct Readiness { Snapshot work{}; uint32_t closed=0,ready=0,unknown=0; bool active=false,publication_blocked=false,available=false; };
  struct Summary {
    uint64_t attempts=0,unknown_readiness=0,unknown_ticket_frontier=0,ticket_mismatch=0,changed_frontier=0;
    uint64_t capacity_refused=0,pre_gpu_pending=0,active_query=0,all_ready_publisher_blocked=0;
    uint64_t canonical_no_pending=0,canonical_submitted=0,canonical_fallback=0,noncanonical=0;
    std::array<std::array<uint64_t,size_t(Boundary::Count)>,size_t(Tickets::Count)> boundaries{};
    bool counter_overflow=false;
  };
  void SetEnabled(bool value) noexcept {
    enabled_=value;for(auto& c:bands_)c.SetEnabled(value);
  }
  size_t open_markers() const noexcept {size_t n=0;for(const auto& c:bands_)n+=c.open_markers();return n;}
  void AfterOriginalFull58(const Readiness& before,uint32_t sdk_closed,const Snapshot& after) noexcept {
    if(!enabled_)return;Add(summary_.attempts);
    if(!before.available||!before.work.available) {Add(summary_.unknown_readiness);return;}
    if(before.closed!=sdk_closed||before.ready>before.closed||before.unknown>before.closed||before.closed>128) {Add(summary_.ticket_mismatch);return;}
    if(before.unknown) {Add(summary_.unknown_ticket_frontier);return;}
    // Sealing/committed-state export during completion may append work. Report
    // that case as unknown rather than invent an earlier all-work frontier.
    const auto& b=before.work;
    if(!after.available||b.generation!=after.generation||b.accepted_work!=after.accepted_work||
       b.accepted_draws!=after.accepted_draws||b.accepted_transfers!=after.accepted_transfers||
       b.completed_work>b.accepted_work||after.completed_work!=after.accepted_work) {
      Add(summary_.changed_frontier);return;
    }
    if(open_markers()>=Census::kMaximumOpenMarkers) {Add(summary_.capacity_refused);return;}
    if(b.completed_work<b.accepted_work)Add(summary_.pre_gpu_pending);
    if(before.active)Add(summary_.active_query);
    if(before.closed&&before.ready==before.closed&&before.publication_blocked)Add(summary_.all_ready_publisher_blocked);
    const auto band=!before.closed?Tickets::Zero:before.ready==before.closed?Tickets::AllReady:Tickets::SomePending;
    bands_[size_t(band)].SuccessfulFull58(after);
  }
  void End(const Snapshot& snapshot,Boundary reason) noexcept {
    if(!enabled_)return;
    if(size_t(reason)>=size_t(Boundary::Count)) {FailedCompletion();return;}
    for(size_t i=0;i<bands_.size();++i) {
      const auto before=bands_[i].summary().finalized;
      bands_[i].MandatoryBoundary(snapshot,Census::Boundary::CommandPacket);
      Add(summary_.boundaries[i][size_t(reason)],bands_[i].summary().finalized-before);
    }
  }
  // Canonical empty waits are a counterfactual opportunity only: the current
  // synchronous marker has already cleared the pending latch, so no provider
  // certificate was invoked. Keep that population explicit, never call it legal.
  void Wait(bool canonical,bool submitted,bool no_pending) noexcept {
    if(!enabled_)return;
    Add(!canonical?summary_.noncanonical:submitted?summary_.canonical_submitted:
        no_pending?summary_.canonical_no_pending:summary_.canonical_fallback);
  }
  void FailedCompletion() noexcept {for(auto& c:bands_)c.FailedCompletion();}
  const Summary& summary() const noexcept {return summary_;}
  const Census& band(size_t i) const noexcept {return bands_[i];}
 private:
  void Add(uint64_t& value,uint64_t n=1) noexcept {
    if(n>std::numeric_limits<uint64_t>::max()-value) {value=std::numeric_limits<uint64_t>::max();summary_.counter_overflow=true;}
    else value+=n;
  }
  bool enabled_=false;
  std::array<Census,size_t(Tickets::Count)> bands_{};
  Summary summary_{};
};

// Optional decoded-operand view of the existing following-work measurement.
// These POD observations are never owner leases or permission to defer a store.
class MarkerOperandCensus {
 public:
  using Census=MarkerFollowingCensus;
  using Snapshot=Census::Snapshot;
  using Readiness=MarkerQueryCohortCensus::Readiness;
  using Boundary=MarkerQueryCohortCensus::Boundary;
  enum class Band : uint8_t { ZeroImmediate3,ZeroTimestamp3,ZeroOther,
    AllPair,ZeroPair,ZeroInactivePair,AllObservedAllocationPair,ZeroObservedAllocationPair,ZeroInactiveObservedAllocationPair,Count };
  struct Allocation {
    uint32_t device=0,virtual_counter=0,physical_counter=0,allocation_base=0,allocation_bytes=0,logical_bytes=0;
    uint64_t registration_generation=0,command_epoch=0;
    bool available=false,live=false;
  };
  struct Operands {
    uintptr_t command_buffer=0;
    uint32_t capacity=0,end_offset=0,packet=0,count=0,initiator=0,address=0,value=0;
  };
  struct Example {
    uint32_t target=0,position=0,serial=0,command_begin=0;
    uint64_t registration_generation=0,command_epoch=0;
    bool allocation_matched=false,zero_seed=false,inactive=false;
  };
  struct Summary {
    uint64_t decoded=0,immediate3=0,timestamp3=0,other=0,noncanonical_header=0;
    uint64_t wrong_endian=0,nonphysical_target=0,wrapped_command=0;
    uint64_t successful_full=0,unknown_completion=0,zero_seeds=0;
    uint64_t shape_pairs=0,pair_without_full_seed=0,pair_without_zero_seed=0,pair_frontier_changed=0;
    uint64_t allocation_unavailable=0,allocation_mismatch=0,allocation_matched=0;
    uint64_t lookup_failures=0,example_capacity=0;
    bool counter_overflow=false;
  };
  void SetEnabled(bool enabled) noexcept {
    if(enabled_==enabled)return;
    enabled_=enabled;for(auto& band:bands_)band.SetEnabled(enabled);
    completion_={};first_={};have_first_=false;
  }
  bool enabled() const noexcept {return enabled_;}
  // Called at the original pre-packet seam. Decoding later may consume only
  // this packet's actual successful completion, never an older marker's.
  void BeginPacket() noexcept {completion_={};}
  void OriginalFull58(const Readiness& before,uint32_t sdk_closed,const Snapshot& after) noexcept {
    if(!enabled_)return;Add(summary_.successful_full);
    const auto& b=before.work;
    if(!before.available||!b.available||before.closed!=sdk_closed||before.closed>128||
       before.ready>before.closed||before.unknown||!Valid(after)||!Valid(b)||
       !SameAccepted(b,after)||after.completed_work!=after.accepted_work) {
      Add(summary_.unknown_completion);return;
    }
    completion_={after,!before.closed,before.active,true};
  }
  void LookupFailed() noexcept {if(enabled_)Add(summary_.lookup_failures);}
  void Decoded(const Operands& op,const Snapshot& now) noexcept {Decoded(op,now,Allocation{});}
  void Decoded(const Operands& op,const Snapshot& now,const Allocation& allocation) noexcept {
    if(!enabled_)return;Add(summary_.decoded);
    const auto shape=op.initiator==3?0u:op.initiator==0x80000003u?1u:2u;
    Add(shape==0?summary_.immediate3:shape==1?summary_.timestamp3:summary_.other);
    const bool header=op.packet==0xC0025800u&&op.count==3;
    const bool endian=(op.address&3)==2;
    const uint32_t target=op.address&~3u;
    const bool physical=target<=0x1ffffffcu;
    if(!header)Add(summary_.noncanonical_header);
    if(!endian)Add(summary_.wrong_endian);
    if(!physical)Add(summary_.nonphysical_target);
    if(completion_.valid&&completion_.zero) {
      Add(summary_.zero_seeds);bands_[shape].SuccessfulFull58(completion_.after);
    }
    uint32_t begin=0;
    const bool position=op.command_buffer&&op.capacity>=16&&!(op.capacity&3)&&
        op.end_offset<op.capacity&&!(op.end_offset&3);
    if(position)begin=op.end_offset>=16?op.end_offset-16:op.capacity-16+op.end_offset;
    // Admit only two physically contiguous sixteen-byte commands. A pair
    // ending exactly at capacity is contiguous; crossing the wrap is not.
    const bool whole=position&&uint64_t(begin)+16<=op.capacity;
    if(!whole)Add(summary_.wrapped_command);
    const bool form=header&&shape==0&&endian&&physical&&whole;
    const bool pair=form&&have_first_&&first_.op.command_buffer==op.command_buffer&&
        first_.op.capacity==op.capacity&&uint64_t(first_.begin)+16==begin&&
        uint64_t(first_.begin)+32<=op.capacity&&uint64_t(target)+4==first_.target;
    if(pair) {
      Add(summary_.shape_pairs);
      const bool known=allocation.available&&first_.allocation.available;
      const bool matched=known&&MatchAllocation(first_.allocation,allocation,target,first_.target);
      if(!known)Add(summary_.allocation_unavailable);
      else if(!matched)Add(summary_.allocation_mismatch);
      else Add(summary_.allocation_matched);
      const bool full=first_.completion.valid;
      const bool zero=full&&first_.completion.zero;
      const bool frontier=full&&Valid(now)&&SameAccepted(first_.completion.after,now)&&
          now.completed_work==now.accepted_work;
      if(!zero)Add(summary_.pair_without_zero_seed);
      if(!full)Add(summary_.pair_without_full_seed);
      else if(!frontier)Add(summary_.pair_frontier_changed);
      else {
        bands_[size_t(Band::AllPair)].SuccessfulFull58(now);
        if(matched)bands_[size_t(Band::AllObservedAllocationPair)].SuccessfulFull58(now);
        if(zero) {
          bands_[size_t(Band::ZeroPair)].SuccessfulFull58(now);
          if(!first_.completion.active)bands_[size_t(Band::ZeroInactivePair)].SuccessfulFull58(now);
          if(matched) {
            bands_[size_t(Band::ZeroObservedAllocationPair)].SuccessfulFull58(now);
            if(!first_.completion.active)bands_[size_t(Band::ZeroInactiveObservedAllocationPair)].SuccessfulFull58(now);
          }
        }
      }
      bool seen=false;for(size_t i=0;i<example_count_;++i)if(examples_[i].target==target)seen=true;
      if(!seen) {
        if(example_count_<examples_.size())examples_[example_count_++]={target,first_.op.value,op.value,
            first_.begin,matched?allocation.registration_generation:0,matched?allocation.command_epoch:0,
            matched,zero&&frontier,full&&!first_.completion.active};
        else Add(summary_.example_capacity);
      }
      have_first_=false;
    } else {
      have_first_=form;
      if(form)first_={op,allocation,completion_,begin,target};
    }
    completion_={};
  }
  void End(const Snapshot& snapshot,Boundary reason) noexcept {
    if(!enabled_)return;
    if(size_t(reason)>=size_t(Boundary::Count)){FailedCompletion();return;}
    for(auto& band:bands_)band.MandatoryBoundary(snapshot,Census::Boundary::CommandPacket);
    completion_={};first_={};have_first_=false;
  }
  void FailedCompletion() noexcept {
    for(auto& band:bands_)band.FailedCompletion();completion_={};first_={};have_first_=false;
  }
  size_t open_markers() const noexcept {size_t n=0;for(const auto& band:bands_)n+=band.open_markers();return n;}
  const Summary& summary() const noexcept {return summary_;}
  const Census& band(size_t i) const noexcept {return bands_[i];}
  size_t example_count() const noexcept {return example_count_;}
  const Example& example(size_t i) const noexcept {return examples_[i];}
 private:
  struct Completion {Snapshot after{};bool zero=false,active=false,valid=false;};
  struct First {Operands op{};Allocation allocation{};Completion completion{};uint32_t begin=0,target=0;};
  static bool Valid(const Snapshot& s) noexcept {
    return s.available&&s.generation&&s.completed_work<=s.accepted_work&&
        s.accepted_draws<=s.accepted_work&&s.accepted_transfers==s.accepted_work-s.accepted_draws;
  }
  static bool SameAccepted(const Snapshot& a,const Snapshot& b) noexcept {
    return a.generation==b.generation&&a.accepted_work==b.accepted_work&&
        a.accepted_draws==b.accepted_draws&&a.accepted_transfers==b.accepted_transfers;
  }
  static bool MatchAllocation(const Allocation& a,const Allocation& b,uint32_t serial,uint32_t position) noexcept {
    return a.live&&b.live&&a.device&&a.device==b.device&&a.virtual_counter==b.virtual_counter&&
        a.physical_counter==b.physical_counter&&b.physical_counter==serial&&uint64_t(serial)+4==position&&
        a.registration_generation&&a.registration_generation==b.registration_generation&&a.command_epoch&&a.command_epoch==b.command_epoch&&
        a.logical_bytes==96&&b.logical_bytes==96&&
        a.allocation_base==b.allocation_base&&a.allocation_bytes==b.allocation_bytes&&
        b.allocation_base<=serial&&uint64_t(serial)+96<=uint64_t(b.allocation_base)+b.allocation_bytes&&
        uint64_t(b.allocation_base)+b.allocation_bytes<=0x20000000ull;
  }
  void Add(uint64_t& value) noexcept {
    if(value==std::numeric_limits<uint64_t>::max())summary_.counter_overflow=true;else ++value;
  }
  bool enabled_=false,have_first_=false;
  Completion completion_{};First first_{};
  Summary summary_{};
  std::array<Census,size_t(Band::Count)> bands_{};
  std::array<Example,8> examples_{};size_t example_count_=0;
};

}  // namespace gow3::diagnostic
