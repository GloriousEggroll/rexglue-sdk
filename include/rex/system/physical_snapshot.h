#pragma once
// Byte certificates are separate from GPU ownership and legacy write watches.
// All tracker methods are serialized by Memory's global protection lock.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <span>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace rex::memory {
struct PhysicalSnapshotMetadata {
  static constexpr size_t kLimit=8*1024*1024;
  std::atomic<size_t> retained{0},peak{0};
};
class PhysicalSnapshotTracker;
class CertifiedPhysicalSnapshot {
 public:
  using Bytes = std::vector<uint8_t>;
  CertifiedPhysicalSnapshot() = default;
  CertifiedPhysicalSnapshot(const CertifiedPhysicalSnapshot&)=delete;
  CertifiedPhysicalSnapshot& operator=(const CertifiedPhysicalSnapshot&)=delete;
  CertifiedPhysicalSnapshot(CertifiedPhysicalSnapshot&&)=delete;
  CertifiedPhysicalSnapshot& operator=(CertifiedPhysicalSnapshot&&)=delete;
  const std::shared_ptr<const Bytes>& bytes() const noexcept { return bytes_; }
  ~CertifiedPhysicalSnapshot(){if(metadata_)metadata_->retained.fetch_sub(metadata_bytes_,std::memory_order_relaxed);}
 private:
  friend class PhysicalSnapshotTracker;
  std::shared_ptr<const Bytes> bytes_;
  std::shared_ptr<const uint8_t> identity_;
  uint64_t epoch_ = 0;
  uint32_t address_ = 0, length_ = 0;
  std::vector<uint64_t> generations_;
  std::shared_ptr<PhysicalSnapshotMetadata> metadata_;
  size_t metadata_bytes_=0;
};
using CertifiedPhysicalSnapshotPtr = std::shared_ptr<const CertifiedPhysicalSnapshot>;
// ValidateOnly never captures an invalid token, including in a mixed batch.
struct PhysicalSnapshotBatchRequest {
  enum class Mode { ValidateOnly, Capture };
  uint32_t address=0,length=0;
  CertifiedPhysicalSnapshotPtr previous;
  Mode mode=Mode::ValidateOnly;
};

// OS-independent state machine, also exercised against real shared mappings by
// the SDK platform fixture. The production adapter uses only existing SDK APIs.
class PhysicalSnapshotTracker {
 public:
  static constexpr uint32_t kPage = 4096;
  static constexpr uint32_t kPhysicalBytes = 0x20000000;
  static constexpr uint32_t kFirst = 0x01000000;  // exclude the extra 0x7F alias
  static constexpr uint32_t kEnd = 0x1FD01000;   // exclude E-view MMIO/tail
  static constexpr uint32_t kMaxSnapshot = 8 * 1024 * 1024;
  enum class Access { NoAccess, ReadOnly, ReadWrite, Unsupported };
  struct PageQuery {uintptr_t address=0;std::span<Access> pages;};
  struct Host {
    void* context = nullptr;
    // Query returns the covered suffix starting at address, never a whole
    // region length beginning before it. False supplies no permission proof.
    bool (*query)(void*, uintptr_t, size_t&, Access&) = nullptr;
    bool (*protect)(void*, uintptr_t, size_t, Access) noexcept = nullptr;
    bool (*query_pages)(void*,std::span<const PageQuery>) = nullptr;
  };
  struct Stats {
    uint64_t hits=0, avoided_bytes=0, captures=0, capture_bytes=0;
    uint64_t snapshot_batches=0,snapshot_batch_views=0,snapshot_batch_cold_views=0;
    uint64_t rejected=0, protect_calls=0, protect_failures=0;
    uint64_t owned_fault_dispatches=0;
    uint64_t query_calls=0,queried_pages=0,batch_queries=0,batch_query_failures=0;
    uint64_t cold_ns=0,metadata_ns=0,query_ns=0,capture_protect_ns=0,copy_ns=0;
    uint64_t maximum_cold_ns=0;
    bool batch_queries_enabled=false,diagnostics_enabled=false;
    uint64_t faults=0, external_writes=0, external_writer_pages=0, mutations=0;
    uint64_t armed_pages=0, peak_armed_pages=0, metadata_bytes=0, peak_metadata_bytes=0;
    std::array<uint64_t,4> alias_faults{};
  };
  enum class Fault { NotOwned, Handled, Failed };

  // Alias bases correspond to physical zero. The E base is vE - 0x1000.
  PhysicalSnapshotTracker(Host host, std::array<uintptr_t,4> aliases,
                          size_t metadata_limit=PhysicalSnapshotMetadata::kLimit)
      : host_(host), aliases_(aliases), pages_(kPhysicalBytes/kPage),
        identity_(std::make_shared<const uint8_t>(0)),metadata_(std::make_shared<PhysicalSnapshotMetadata>()),
        metadata_limit_(std::min(metadata_limit,PhysicalSnapshotMetadata::kLimit)) {}
  PhysicalSnapshotTracker(const PhysicalSnapshotTracker&)=delete;
  PhysicalSnapshotTracker& operator=(const PhysicalSnapshotTracker&)=delete;
  PhysicalSnapshotTracker(PhysicalSnapshotTracker&&)=delete;
  PhysicalSnapshotTracker& operator=(PhysicalSnapshotTracker&&)=delete;
  Stats stats() const noexcept {
    auto result=stats_;result.batch_queries_enabled=batch_queries_;result.diagnostics_enabled=diagnostics_;
    result.metadata_bytes=metadata_->retained.load(std::memory_order_relaxed);
    result.peak_metadata_bytes=metadata_->peak.load(std::memory_order_relaxed);
    return result;
  }
  bool enabled() const noexcept { return enabled_; }
  // Advances (release) on every page invalidation, revocation and enable
  // change, before any write it guards can land. Equal values read before a
  // successful TryReuse and later prove no certified byte changed in between.
  uint64_t mutation_generation() const noexcept { return mutations_.load(std::memory_order_acquire); }
  void SetBatchQueriesEnabled(bool value) noexcept {batch_queries_=value;}
  void SetDiagnosticsEnabled(bool value) noexcept {diagnostics_=value;}
  bool diagnostics_enabled()const noexcept{return diagnostics_;}
  void RecordMetadataNanoseconds(uint64_t value)noexcept{stats_.metadata_ns+=value;}
  void Enable() noexcept { if (!exhausted_) enabled_=true; Mutated(); }
  bool Disable() noexcept {
    enabled_=false; RevokeAll();
    return RestoreRange(kFirst/kPage,kEnd/kPage);
  }
  void RevokeAll() noexcept {
    if (epoch_==UINT64_MAX) { exhausted_=true; enabled_=false; }
    else ++epoch_;
    Mutated();
  }

  // Exact token validation only: no OS queries, heap scans, allocation, source
  // reads or permission changes. Memory may use this before canonical metadata
  // checks because every allocation/protection/lifecycle mutation revokes the
  // same page generations under the same global lock. Fresh captures still
  // require independent committed/readable metadata validation.
  CertifiedPhysicalSnapshotPtr TryReuse(uint32_t address,uint32_t length,
      const CertifiedPhysicalSnapshotPtr& previous) {
    if(!previous || !Eligible(address,length) || previous->identity_!=identity_ ||
       previous->epoch_!=epoch_ || previous->address_!=address || previous->length_!=length)
      return {};
    const auto first=address/kPage,end=uint32_t((uint64_t(address)+length+kPage-1)/kPage);
    if(previous->generations_.size()!=end-first)return {};
    for(auto p=first;p<end;++p)
      if(pages_[p].writers || previous->generations_[p-first]!=pages_[p].generation)return {};
    ++stats_.hits;stats_.avoided_bytes+=length;return previous;
  }

  CertifiedPhysicalSnapshotPtr Capture(uint32_t address, uint32_t length,
      const CertifiedPhysicalSnapshotPtr& previous) {
    if(auto reused=TryReuse(address,length,previous))return reused;
    if (!Eligible(address,length)) { ++stats_.rejected; return {}; }
    const auto first=address/kPage, end=uint32_t((uint64_t(address)+length+kPage-1)/kPage);
    for(auto p=first;p<end;++p)if(pages_[p].writers){++stats_.rejected;return {};}
    PhaseTimer cold(diagnostics_,stats_.cold_ns,&stats_.maximum_cold_ns);
    // Allocate all published storage before changing any protection. Allocation
    // failure leaves existing watches/certificates intact and propagates to the
    // optional API's fallback catch.
    auto result=std::make_shared<CertifiedPhysicalSnapshot>();
    auto bytes=std::make_shared<CertifiedPhysicalSnapshot::Bytes>(length);
    result->generations_.resize(end-first);
    // Include a conservative allowance for the shared-owner control block.
    const size_t charge=sizeof(CertifiedPhysicalSnapshot)+64+result->generations_.capacity()*sizeof(uint64_t);
    const auto current=metadata_->retained.load(std::memory_order_relaxed);
    if(charge>metadata_limit_-current){++stats_.rejected;return {};}
    result->metadata_=metadata_;result->metadata_bytes_=charge;
    const auto total=metadata_->retained.fetch_add(charge,std::memory_order_relaxed)+charge;
    metadata_->peak.store(std::max(metadata_->peak.load(std::memory_order_relaxed),total),std::memory_order_relaxed);
    // Snapshot every alias permission before arming any page. This metadata is
    // local to this capture/global-lock scope; no OS mapping cache survives it.
    std::vector<Access> permissions;
    const auto page_count=end-first;
    if(batch_queries_) {
      permissions.assign(size_t(page_count)*aliases_.size(),Access::Unsupported);
      std::array<PageQuery,4> requests;
      for(unsigned a=0;a<aliases_.size();++a)
        requests[a]={aliases_[a]+uint64_t(first)*kPage,
                     std::span<Access>(permissions).subspan(size_t(a)*page_count,page_count)};
      ++stats_.batch_queries;stats_.queried_pages+=permissions.size();
      bool complete=false;
      try {PhaseTimer timing(diagnostics_,stats_.query_ns);
        if(host_.query_pages)complete=host_.query_pages(host_.context,requests);
      } catch(...) {++stats_.batch_query_failures;++stats_.rejected;throw;}
      if(complete)for(size_t i=0;i<permissions.size();++i) {
        const auto access=permissions[i];
        if((access!=Access::ReadWrite&&access!=Access::ReadOnly&&access!=Access::NoAccess) ||
           (i<page_count&&access==Access::NoAccess)){complete=false;break;}
      }
      // No permission was changed by this capture. Preserve any older owned
      // restrictions and certificates on failed preflight; caller falls back.
      if(!complete){++stats_.batch_query_failures;++stats_.rejected;return {};}
    }
    try {
      for(unsigned a=0;a<aliases_.size();++a) {
        for(auto p=first;p<end;) {
          Access access=Access::Unsupported;uint32_t count=0;
          if(batch_queries_) {
            const size_t offset=size_t(a)*page_count+p-first;
            access=permissions[offset];count=1;
            while(p+count<end&&permissions[offset+count]==access)++count;
          } else {
            size_t extent=0;bool complete=false;++stats_.query_calls;
            {PhaseTimer timing(diagnostics_,stats_.query_ns);
             complete=host_.query(host_.context,aliases_[a]+uint64_t(p)*kPage,extent,access);}
            if(!complete || extent<kPage || access==Access::Unsupported || (a==0&&access==Access::NoAccess))
              return FailedCapture(first,end);
            count=uint32_t(std::min<uint64_t>(extent/kPage,end-p));stats_.queried_pages+=count;
          }
          if(access==Access::ReadWrite) {
            // Mark repair ownership BEFORE the call. Even a failed/partially
            // applied OS protection must remain repairable by our fault path.
            for(auto q=p;q<p+count;++q) {
              if(!pages_[q].owned)++stats_.armed_pages;
              pages_[q].owned|=uint8_t(1u<<a);
            }
            stats_.peak_armed_pages=std::max(stats_.peak_armed_pages,stats_.armed_pages);
            bool protected_range=false;
            {PhaseTimer timing(diagnostics_,stats_.capture_protect_ns);
             protected_range=Protect(a,p,count,Access::ReadOnly);}
            if(!protected_range)return FailedCapture(first,end);
          }
          p+=count;
        }
      }
      {PhaseTimer timing(diagnostics_,stats_.copy_ns);
      std::memcpy(bytes->data(),reinterpret_cast<const void*>(aliases_[0]+address),length);
      result->bytes_=bytes;
      if(previous && previous->bytes_ && previous->bytes_->size()==length &&
         !std::memcmp(previous->bytes_->data(),bytes->data(),length))result->bytes_=previous->bytes_;
      }
      result->identity_=identity_; result->epoch_=epoch_;
      result->address_=address;result->length_=length;
      for(auto p=first;p<end;++p)result->generations_[p-first]=pages_[p].generation;
      ++stats_.captures;stats_.capture_bytes+=length;
      return result;
    } catch(...) { FailedCapture(first,end); throw; }
  }

  // One lock is supplied by Memory. Results keep independent original-sized
  // owners/tokens. Warming fallbacks outside this method remain per-view reads;
  // this API makes no simultaneous-snapshot claim for those unprotected bytes.
  std::vector<CertifiedPhysicalSnapshotPtr> CaptureBatch(
      std::span<const PhysicalSnapshotBatchRequest> views) {
    if(views.empty()||views.size()>64)return {};
    uint64_t requested=0;
    for(const auto& v:views) {
      requested+=v.length;
      if(requested>kMaxSnapshot)return {};
    }
    ++stats_.snapshot_batches;stats_.snapshot_batch_views+=views.size();
    std::vector<CertifiedPhysicalSnapshotPtr> output(views.size());
    struct Prepared {
      size_t index;uint32_t first,end;
      std::shared_ptr<CertifiedPhysicalSnapshot> token;
      std::shared_ptr<CertifiedPhysicalSnapshot::Bytes> bytes;
    };
    std::vector<Prepared> cold;
    std::vector<uint32_t> union_pages;
    // Warm validate-only batches allocate only the returned owner array.
    if(std::any_of(views.begin(),views.end(),[](const auto& v){return v.mode==PhysicalSnapshotBatchRequest::Mode::Capture;})) {
      cold.reserve(views.size());
      // At most 2048 full pages plus two unaligned edges per bounded view.
      union_pages.reserve(size_t(requested/kPage)+2*views.size());
    }
    for(size_t i=0;i<views.size();++i) {
      const auto& v=views[i];
      if(auto hit=TryReuse(v.address,v.length,v.previous)){output[i]=std::move(hit);continue;}
      if(v.mode!=PhysicalSnapshotBatchRequest::Mode::Capture)continue;
      if(!Eligible(v.address,v.length)){++stats_.rejected;continue;}
      const auto first=v.address/kPage,end=uint32_t((uint64_t(v.address)+v.length+kPage-1)/kPage);
      bool writing=false;
      for(auto page=first;page<end;++page)writing|=bool(pages_[page].writers);
      if(writing){++stats_.rejected;continue;}
      auto token=std::make_shared<CertifiedPhysicalSnapshot>();
      auto bytes=std::make_shared<CertifiedPhysicalSnapshot::Bytes>(v.length);
      token->generations_.resize(end-first);
      const size_t charge=sizeof(CertifiedPhysicalSnapshot)+64+token->generations_.capacity()*sizeof(uint64_t);
      const auto current=metadata_->retained.load(std::memory_order_relaxed);
      if(current>metadata_limit_||charge>metadata_limit_-current){++stats_.rejected;continue;}
      token->metadata_=metadata_;token->metadata_bytes_=charge;
      const auto total=metadata_->retained.fetch_add(charge,std::memory_order_relaxed)+charge;
      metadata_->peak.store(std::max(metadata_->peak.load(std::memory_order_relaxed),total),std::memory_order_relaxed);
      cold.push_back({i,first,end,std::move(token),std::move(bytes)});
      for(auto page=first;page<end;++page)union_pages.push_back(page);
    }
    if(cold.empty())return output;
    stats_.snapshot_batch_cold_views+=cold.size();
    PhaseTimer cold_timer(diagnostics_,stats_.cold_ns,&stats_.maximum_cold_ns);
    std::sort(union_pages.begin(),union_pages.end());
    union_pages.erase(std::unique(union_pages.begin(),union_pages.end()),union_pages.end());
    struct Run {uint32_t first,end;size_t offset;};
    std::vector<Run> runs;runs.reserve(union_pages.size());
    for(size_t i=0;i<union_pages.size();) {
      const auto start=i++;while(i<union_pages.size()&&union_pages[i]==union_pages[i-1]+1)++i;
      runs.push_back({union_pages[start],union_pages[i-1]+1,start});
    }
    std::vector<Access> permissions(union_pages.size()*aliases_.size(),Access::Unsupported);
    std::vector<PageQuery> queries;queries.reserve(runs.size()*aliases_.size());
    for(unsigned a=0;a<aliases_.size();++a)for(const auto& run:runs)
      queries.push_back({aliases_[a]+uint64_t(run.first)*kPage,
        std::span<Access>(permissions).subspan(size_t(a)*union_pages.size()+run.offset,run.end-run.first)});
    // Every allocation and permission query finishes BEFORE the first protect.
    // Noncontiguous gaps are not queried, protected, copied or owned.
    bool complete=true;
    {PhaseTimer timing(diagnostics_,stats_.query_ns);
    if(batch_queries_) {
      ++stats_.batch_queries;stats_.queried_pages+=permissions.size();
      try {complete=host_.query_pages&&host_.query_pages(host_.context,queries);}
      catch(...) {++stats_.batch_query_failures;throw;}
    } else for(auto& query:queries) {
      size_t index=0;
      while(index<query.pages.size()) {
        size_t extent=0;Access access=Access::Unsupported;++stats_.query_calls;
        if(!host_.query||!host_.query(host_.context,query.address+index*kPage,extent,access)||extent<kPage) {
          complete=false;break;
        }
        const auto count=std::min(extent/kPage,query.pages.size()-index);
        std::fill_n(query.pages.begin()+index,count,access);index+=count;stats_.queried_pages+=count;
      }
      if(!complete)break;
    }}
    if(complete)for(size_t i=0;i<permissions.size();++i) {
      const auto access=permissions[i];
      if((access!=Access::ReadWrite&&access!=Access::ReadOnly&&access!=Access::NoAccess)||
         (i<union_pages.size()&&access==Access::NoAccess)){complete=false;break;}
    }
    if(!complete) {
      if(batch_queries_)++stats_.batch_query_failures;
      stats_.rejected+=cold.size();return output; // Existing validated neighbors survive preflight failure.
    }
    for(unsigned a=0;a<aliases_.size();++a)for(const auto& run:runs) {
      auto page=run.first;
      while(page<run.end) {
        const auto offset=size_t(a)*union_pages.size()+run.offset+page-run.first;
        const auto access=permissions[offset];uint32_t count=1;
        while(page+count<run.end&&permissions[offset+count]==access)++count;
        if(access==Access::ReadWrite) {
          for(auto q=page;q<page+count;++q) {
            if(!pages_[q].owned)++stats_.armed_pages;
            pages_[q].owned|=uint8_t(1u<<a);
          }
          stats_.peak_armed_pages=std::max(stats_.peak_armed_pages,stats_.armed_pages);
          bool armed=false;
          {PhaseTimer timing(diagnostics_,stats_.capture_protect_ns);armed=Protect(a,page,count,Access::ReadOnly);}
          if(!armed) {
            // A partially changed union revokes every affected token. Conservatively
            // discard even previously validated neighbors; exact caller fallback follows.
            for(const auto& repair:runs){Invalidate(repair.first,repair.end);RestoreRange(repair.first,repair.end);}
            stats_.rejected+=cold.size();std::fill(output.begin(),output.end(),nullptr);return output;
          }
        }
        page+=count;
      }
    }
    // No throwing operation remains after protection. All selected cold pages
    // are armed across all writable aliases before reading any selected view.
    for(auto& prepared:cold) {
      const auto& v=views[prepared.index];auto& token=*prepared.token;
      {PhaseTimer timing(diagnostics_,stats_.copy_ns);
      std::memcpy(prepared.bytes->data(),reinterpret_cast<const void*>(aliases_[0]+v.address),v.length);
      token.bytes_=prepared.bytes;
      if(v.previous&&v.previous->bytes_&&v.previous->bytes_->size()==v.length&&
         !std::memcmp(v.previous->bytes_->data(),prepared.bytes->data(),v.length))token.bytes_=v.previous->bytes_;}
      token.identity_=identity_;token.epoch_=epoch_;token.address_=v.address;token.length_=v.length;
      for(auto page=prepared.first;page<prepared.end;++page)token.generations_[page-prepared.first]=pages_[page].generation;
      output[prepared.index]=std::move(prepared.token);++stats_.captures;stats_.capture_bytes+=v.length;
    }
    return output;
  }

  // Call before ANY non-certificate host protection/allocation/lifecycle change,
  // including stricter protection and legacy expanded unprotect spans. Restore
  // only our own restriction on this alias so old-access reports remain exact.
  // A failed repair keeps ownership and refuses the requested host mutation.
  bool BeforeHostMutation(uintptr_t host, size_t length) noexcept {
    bool success=true;
    ForHost(host,length,[&](unsigned a,uint32_t first,uint32_t end) {
      ++stats_.mutations;Invalidate(first,end);
      success=RestoreAlias(a,first,end)&&success;
    });
    return success;
  }
  // Metadata-only release/decommit/reset still revokes old range identities.
  void InvalidateHost(uintptr_t host,size_t length) noexcept {
    ForHost(host,length,[&](unsigned,uint32_t first,uint32_t end){Invalidate(first,end);});
  }
  // Ownership proves permission to restore the original writable mapping,
  // even when an earlier Protect failed and left repair-only ownership. It
  // does NOT prove current RO protection for capture/certificate construction.
  // Called only under the same SDK global protection lock as HandleFault.
  bool OwnsFault(uintptr_t host,bool write)const noexcept {
    if(!write)return false;
    bool owned=false;
    ForHost(host,1,[&](unsigned a,uint32_t first,uint32_t){owned|=bool(pages_[first].owned&(1u<<a));});
    return owned;
  }
  void RecordOwnedFaultDispatch()noexcept{++stats_.owned_fault_dispatches;}
  Fault HandleFault(uintptr_t host,bool write) noexcept {
    if(!write)return Fault::NotOwned;
    Fault result=Fault::NotOwned;
    ForHost(host,1,[&](unsigned a,uint32_t first,uint32_t end) {
      if(!(pages_[first].owned&(1u<<a)))return;
      Invalidate(first,end);
      if(RestoreAlias(a,first,end)) {
        ++stats_.faults;++stats_.alias_faults[a];result=Fault::Handled;
      } else result=Fault::Failed;
    });
    return result;
  }
  bool BeginExternalWrite(uintptr_t host,size_t length) noexcept {
    if(length>UINTPTR_MAX-host)return false;
    bool possible=true;
    ForHost(host,length,[&](unsigned,uint32_t first,uint32_t end){
      for(auto p=first;p<end;++p)if(pages_[p].writers==UINT32_MAX)possible=false;
    });
    if(!possible)return false;
    ForHost(host,length,[&](unsigned a,uint32_t first,uint32_t end){
      Invalidate(first,end);stats_.external_writer_pages+=end-first;
      for(auto p=first;p<end;++p)++pages_[p].writers;
      possible=RestoreAlias(a,first,end)&&possible;
    });
    if(!possible){EndExternalWrite(host,length);return false;}
    ++stats_.external_writes;return true;
  }
  void EndExternalWrite(uintptr_t host,size_t length) noexcept {
    ForHost(host,length,[&](unsigned,uint32_t first,uint32_t end){
      Invalidate(first,end);
      for(auto p=first;p<end;++p){if(!pages_[p].writers)std::terminate();--pages_[p].writers;}
    });
  }
 private:
  class PhaseTimer {
   public:
    PhaseTimer(bool enabled,uint64_t& total,uint64_t* maximum=nullptr)noexcept
        :enabled_(enabled),total_(total),maximum_(maximum) {
      if(enabled_)start_=std::chrono::steady_clock::now();
    }
    ~PhaseTimer(){if(enabled_) {
      const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now()-start_).count());
      total_+=ns;if(maximum_)*maximum_=std::max(*maximum_,ns);
    }}
   private:
    bool enabled_;uint64_t& total_;uint64_t* maximum_;
    std::chrono::steady_clock::time_point start_{};
  };
  struct Page {uint64_t generation=1;uint32_t writers=0;uint8_t owned=0;};
  bool Eligible(uint32_t address,uint32_t length)const noexcept {
    return enabled_ && length && length<=kMaxSnapshot && address>=kFirst &&
           uint64_t(address)+length<=kEnd;
  }
  template<class F> void ForHost(uintptr_t host,size_t length,F&& f)const noexcept {
    if(!length || length>UINTPTR_MAX-host)return;
    for(unsigned a=0;a<aliases_.size();++a) {
      const auto lo=std::max(host,aliases_[a]+kFirst);
      const auto hi=std::min(host+length,aliases_[a]+kEnd);
      if(lo<hi)f(a,uint32_t((lo-aliases_[a])/kPage),
                  uint32_t((hi-aliases_[a]+kPage-1)/kPage));
    }
  }
  void Mutated() noexcept { mutations_.fetch_add(1,std::memory_order_acq_rel); }
  void Invalidate(uint32_t first,uint32_t end) noexcept {
    if(first<end)Mutated();
    for(auto p=first;p<end;++p) {
      if(pages_[p].generation==UINT64_MAX){RevokeAll();pages_[p].generation=1;}
      else ++pages_[p].generation;
    }
  }
  bool Protect(unsigned alias,uint32_t first,uint32_t count,Access access) noexcept {
    ++stats_.protect_calls;
    if(host_.protect(host_.context,aliases_[alias]+uint64_t(first)*kPage,
                     size_t(count)*kPage,access))return true;
    ++stats_.protect_failures;return false;
  }
  bool RestoreAlias(unsigned a,uint32_t first,uint32_t end) noexcept {
    bool success=true;
    for(auto p=first;p<end;) {
      if(!(pages_[p].owned&(1u<<a))){++p;continue;}
      auto next=p+1;
      while(next<end && (pages_[next].owned&(1u<<a)))++next;
      if(Protect(a,p,next-p,Access::ReadWrite))
        for(auto q=p;q<next;++q) {
          pages_[q].owned&=uint8_t(~(1u<<a));
          if(!pages_[q].owned)--stats_.armed_pages;
        }
      else success=false;
      p=next;
    }
    return success;
  }
  bool RestoreRange(uint32_t first,uint32_t end) noexcept {
    bool success=true;
    for(unsigned a=0;a<aliases_.size();++a)success=RestoreAlias(a,first,end)&&success;
    return success;
  }
  CertifiedPhysicalSnapshotPtr FailedCapture(uint32_t first,uint32_t end) noexcept {
    Invalidate(first,end);RestoreRange(first,end);++stats_.rejected;return {};
  }
  Host host_;std::array<uintptr_t,4> aliases_;std::vector<Page> pages_;
  std::shared_ptr<const uint8_t> identity_;
  std::shared_ptr<PhysicalSnapshotMetadata> metadata_;
  size_t metadata_limit_;
  uint64_t epoch_=1;bool enabled_=false,exhausted_=false,batch_queries_=false,diagnostics_=false;Stats stats_{};
  std::atomic<uint64_t> mutations_{1};
};
} // namespace rex::memory
