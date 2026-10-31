/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <utility>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/stream.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/external_write_ranges.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

// TODO(benvanik): move xbox.h out
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_BOOL(protect_zero, true, "Memory", "Protect the zero page from reads and writes")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(protect_on_release, false, "Memory",
                    "Protect released memory to prevent accesses");

REXCVAR_DEFINE_BOOL(scribble_heap, false, "Memory", "Scribble 0xCD into all allocated heap memory");

namespace rex::memory {

uint32_t get_page_count(uint32_t value, uint32_t page_size, uint32_t page_size_shift) {
  return rex::round_up(value, page_size) >> page_size_shift;
}

/**
 * Memory map:
 * 0x00000000 - 0x3FFFFFFF (1024mb) - virtual 4k pages
 * 0x40000000 - 0x7FFFFFFF (1024mb) - virtual 64k pages
 * 0x80000000 - 0x8BFFFFFF ( 192mb) - xex 64k pages
 * 0x8C000000 - 0x8FFFFFFF (  64mb) - xex 64k pages (encrypted)
 * 0x90000000 - 0x9FFFFFFF ( 256mb) - xex 4k pages
 * 0xA0000000 - 0xBFFFFFFF ( 512mb) - physical 64k pages
 * 0xC0000000 - 0xDFFFFFFF          - physical 16mb pages
 * 0xE0000000 - 0xFFFFFFFF          - physical 4k pages
 *
 * We use the host OS to create an entire addressable range for this. That way
 * we don't have to emulate a TLB. It'd be really cool to pass through page
 * sizes or use madvice to let the OS know what to expect.
 *
 * We create our own heap of committed memory that lives at
 * memory_HEAP_LOW to memory_HEAP_HIGH - all normal user allocations
 * come from there. Since the Xbox has no paging, we know that the size of
 * this heap will never need to be larger than ~512MB (realistically, smaller
 * than that). We place it far away from the XEX data and keep the memory
 * around it uncommitted so that we have some warning if things go astray.
 *
 * For XEX/GPU/etc data we allow placement allocations (base_address != 0) and
 * commit the requested memory as needed. This bypasses the standard heap, but
 * XEXs should never be overwriting anything so that's fine. We can also query
 * for previous commits and assert that we really isn't committing twice.
 *
 * GPU memory is mapped onto the lower 512mb of the virtual 4k range (0).
 * So 0xA0000000 = 0x00000000. A more sophisticated allocator could handle
 * this.
 */

static memory::Memory* active_memory_ = nullptr;

void CrashDump() {
  static std::atomic<int> in_crash_dump(0);
  if (in_crash_dump.fetch_add(1)) {
    // rex::FatalError(
    //     "Hard crash: the memory system crashed while dumping a crash dump.");
    return;
  }
  active_memory_->DumpMap();
  --in_crash_dump;
}

Memory::Memory() {
  system_page_size_ = uint32_t(rex::memory::page_size());
  system_allocation_granularity_ = uint32_t(rex::memory::allocation_granularity());
  assert_zero(active_memory_);
  active_memory_ = this;
}

Memory::~Memory() {
  assert_true(active_memory_ == this);
  active_memory_ = nullptr;

  // All memory users have stopped. Retire tokens and restore certificate-only
  // restrictions while the fault handler still exists; failed repair ownership
  // remains in the tracker until the mappings are removed below.
  SetCertifiedPhysicalSnapshotsEnabled(false);

  // Uninstall the MMIO handler, as we won't be able to service more requests.
  mmio_handler_.reset();

  for (auto invalidation_callback : physical_memory_invalidation_callbacks_) {
    delete invalidation_callback;
  }

  heaps_.v00000000.Dispose();
  heaps_.v40000000.Dispose();
  heaps_.v80000000.Dispose();
  heaps_.v90000000.Dispose();
  heaps_.vA0000000.Dispose();
  heaps_.vC0000000.Dispose();
  heaps_.vE0000000.Dispose();
  heaps_.physical.Dispose();

  // Unmap all views and close mapping.
  if (mapping_ != rex::memory::kFileMappingHandleInvalid) {
    UnmapViews();
    rex::memory::CloseFileMappingHandle(mapping_, file_name_);
    mapping_base_ = nullptr;
    mapping_ = rex::memory::kFileMappingHandleInvalid;
  }

  virtual_membase_ = nullptr;
  physical_membase_ = nullptr;
}

std::unique_lock<std::recursive_mutex> Memory::AcquireSnapshotMutationLock() {
  // Establish global-before-physical-heap from initialization, even while
  // disabled. Enable may run after host audio/GPU workers already exist.
  return global_critical_region_.Acquire();
}

bool Memory::SetCertifiedPhysicalSnapshotsEnabled(bool enabled) {
  auto lock=global_critical_region_.Acquire();
  if(!enabled)return !certified_snapshots_ || certified_snapshots_->Disable();
  if(external_snapshot_writers_)return false;
#if REX_PLATFORM_LINUX || REX_PLATFORM_WIN32
  // The tracker protects individual 4 KiB pages of every alias. Windows maps
  // views on 64 KiB boundaries (the 0xE0000000 view at file offset 0 with a
  // 0x1000 host address offset, see PhysicalSnapshotAliases) but protects 4 KiB
  // pages, so only the page size must match.
  if(system_page_size_!=PhysicalSnapshotTracker::kPage ||
     system_allocation_granularity_%PhysicalSnapshotTracker::kPage ||
#if REX_PLATFORM_LINUX
     system_allocation_granularity_!=PhysicalSnapshotTracker::kPage ||
#endif
     !virtual_membase_ || !physical_membase_ || !mmio_handler_)return false;
  if(!certified_snapshots_) {
    PhysicalSnapshotTracker::Host host{};
    host.query=[](void*,uintptr_t address,size_t& extent,PhysicalSnapshotTracker::Access& access) {
      PageAccess actual=PageAccess::kNoAccess;
      if(!rex::memory::QueryProtect(reinterpret_cast<void*>(address),extent,actual))return false;
      using Access=PhysicalSnapshotTracker::Access;
      switch(actual) {
        case PageAccess::kNoAccess:access=Access::NoAccess;break;
        case PageAccess::kReadOnly:access=Access::ReadOnly;break;
        case PageAccess::kReadWrite:access=Access::ReadWrite;break;
        default:access=Access::Unsupported;break;
      }
      return true;
    };
    host.query_pages=[](void*,std::span<const PhysicalSnapshotTracker::PageQuery> requests) {
      if(requests.empty()||requests.size()>256)return false;
      size_t count=0;
      for(const auto& r:requests) {
        if(r.pages.size()>8704-count)return false;
        count+=r.pages.size();
      }
      std::vector<PageAccess> permissions(count,PageAccess::kNoAccess);
      // Keep the original four-alias scalar storage path allocation-free.
      std::array<PageProtectionQuery,4> scalar_queries;
      std::vector<PageProtectionQuery> batch_queries;
      if(requests.size()!=4)batch_queries.resize(requests.size());
      std::span<PageProtectionQuery> queries=requests.size()==4?
          std::span<PageProtectionQuery>(scalar_queries):std::span<PageProtectionQuery>(batch_queries);
      size_t offset=0;
      for(size_t i=0;i<requests.size();++i) {
        queries[i]={requests[i].address,std::span<PageAccess>(permissions).subspan(offset,requests[i].pages.size())};
        offset+=requests[i].pages.size();
      }
      // The existing portable OS query accepts at most16 ranges per call.
      // Chunk the union metadata without changing mappings or releasing the
      // SAME global lock. Warm token validation makes no OS query at all.
      for(size_t first=0;first<queries.size();first+=16)
        if(!QueryProtectPages(queries.subspan(first,std::min(size_t(16),queries.size()-first))))return false;
      offset=0;
      for(const auto& request:requests)for(auto& output:request.pages) {
        using Access=PhysicalSnapshotTracker::Access;
        switch(permissions[offset++]) {
          case PageAccess::kNoAccess:output=Access::NoAccess;break;
          case PageAccess::kReadOnly:output=Access::ReadOnly;break;
          case PageAccess::kReadWrite:output=Access::ReadWrite;break;
          default:output=Access::Unsupported;break;
        }
      }
      return true;
    };
    host.protect=[](void*,uintptr_t address,size_t bytes,PhysicalSnapshotTracker::Access access) noexcept {
      return rex::memory::Protect(reinterpret_cast<void*>(address),bytes,
          access==PhysicalSnapshotTracker::Access::ReadWrite?PageAccess::kReadWrite:PageAccess::kReadOnly);
    };
    try {
      certified_snapshots_=std::make_unique<PhysicalSnapshotTracker>(host,PhysicalSnapshotAliases());
    } catch(...) { return false; }
    snapshot_tracking_installed_.store(true,std::memory_order_release);
  }
  certified_snapshots_->SetBatchQueriesEnabled(certified_batch_queries_);
  certified_snapshots_->SetDiagnosticsEnabled(certified_diagnostics_);
  certified_snapshots_->Enable();
  return certified_snapshots_->enabled();
#else
  // Mac pages are 16 KiB, larger than the tracker's 4 KiB page.
  return false;
#endif
}

void Memory::SetCertifiedPhysicalFaultFastPath(bool enabled) {
  auto lock=global_critical_region_.Acquire();certified_fault_fastpath_=enabled;
}

void Memory::SetCertifiedPhysicalSnapshotBatchQueries(bool enabled) {
  auto lock=global_critical_region_.Acquire();certified_batch_queries_=enabled;
  if(certified_snapshots_)certified_snapshots_->SetBatchQueriesEnabled(enabled);
}
void Memory::SetCertifiedPhysicalSnapshotDiagnostics(bool enabled) {
  auto lock=global_critical_region_.Acquire();certified_diagnostics_=enabled;
  if(certified_snapshots_)certified_snapshots_->SetDiagnosticsEnabled(enabled);
}

CertifiedPhysicalSnapshotPtr Memory::TryGetCertifiedPhysicalSnapshot(
    uint32_t address,uint32_t length,const CertifiedPhysicalSnapshotPtr& previous) {
  if(!snapshot_tracking_installed_.load(std::memory_order_acquire))return {};
  auto lock=global_critical_region_.Acquire();
  if(!certified_snapshots_ || !certified_snapshots_->enabled() || !length ||
     address<PhysicalSnapshotTracker::kFirst ||
     length>PhysicalSnapshotTracker::kMaxSnapshot ||
     uint64_t(address)+length>PhysicalSnapshotTracker::kEnd)return {};
  // An exact live token already certifies canonical allocation readability as
  // well as all aliases. Every relevant metadata/permission mutation revokes
  // its page generations under this lock, so no heap scan is needed on a hit.
  if(auto reused=certified_snapshots_->TryReuse(address,length,previous))return reused;
  // New/invalidated tokens must independently prove a committed/readable range.
  // QueryRegionInfo scans to the end of a whole allocation; avoid that unrelated
  // work by inspecting only the requested pages, including unaligned edges.
  std::chrono::steady_clock::time_point metadata_start{};
  if(certified_diagnostics_)metadata_start=std::chrono::steady_clock::now();
  const bool readable=heaps_.physical.IsRangeCommittedReadable(address,address+length-1);
  if(certified_diagnostics_)certified_snapshots_->RecordMetadataNanoseconds(uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-metadata_start).count()));
  if(!readable)return {};
  try { return certified_snapshots_->Capture(address,length,previous); }
  catch(...) { return {}; } // Optional optimization; exact caller path remains.
}

CertifiedPhysicalSnapshotPtr Memory::ValidatePhysicalSnapshot(
    uint32_t address,uint32_t length,const CertifiedPhysicalSnapshotPtr& previous) {
  if(!previous||!snapshot_tracking_installed_.load(std::memory_order_acquire))return {};
  auto lock=global_critical_region_.Acquire();
  if(!certified_snapshots_||!certified_snapshots_->enabled()||!length||
     address<PhysicalSnapshotTracker::kFirst||length>PhysicalSnapshotTracker::kMaxSnapshot||
     uint64_t(address)+length>PhysicalSnapshotTracker::kEnd)return {};
  return certified_snapshots_->TryReuse(address,length,previous);
}

std::vector<CertifiedPhysicalSnapshotPtr> Memory::TryGetCertifiedPhysicalSnapshots(
    std::span<const PhysicalSnapshotBatchRequest> requests) {
  if(requests.empty()||requests.size()>64||!snapshot_tracking_installed_.load(std::memory_order_acquire))return {};
  uint64_t total=0;for(const auto& request:requests) {
    total+=request.length;if(total>PhysicalSnapshotTracker::kMaxSnapshot)return {};
  }
  auto lock=global_critical_region_.Acquire();
  if(!certified_snapshots_||!certified_snapshots_->enabled())return {};
  try {
    std::vector<PhysicalSnapshotBatchRequest> checked(requests.begin(),requests.end());
    for(auto& request:checked) {
      if(request.mode!=PhysicalSnapshotBatchRequest::Mode::Capture)continue;
      if(!request.length||request.address<PhysicalSnapshotTracker::kFirst||
         uint64_t(request.address)+request.length>PhysicalSnapshotTracker::kEnd||
         !heaps_.physical.IsRangeCommittedReadable(request.address,request.address+request.length-1)) {
        // Keep the output index, but refuse cold admission without affecting
        // validated neighbors. Never convert an invalid active token to Capture.
        request.length=0;request.previous={};request.mode=PhysicalSnapshotBatchRequest::Mode::ValidateOnly;
      }
    }
    return certified_snapshots_->CaptureBatch(checked);
  } catch(...) {return {};} // Optional batch failure retains exact caller fallback.
}

PhysicalSnapshotTracker::Stats Memory::certified_physical_snapshot_stats() {
  auto lock=global_critical_region_.Acquire();
  return certified_snapshots_?certified_snapshots_->stats():PhysicalSnapshotTracker::Stats{};
}

// Host address of physical address 0 in each view the tracker protects: the
// raw physical view, then the 0xA0000000, 0xC0000000 and 0xE0000000 heaps.
// The 0xE0000000 heap maps physical P at guest 0xE0000000+P-0x1000; with a
// host address offset the view itself starts at file offset 0 instead.
std::array<uintptr_t,4> Memory::PhysicalSnapshotAliases() const noexcept {
  const auto v=reinterpret_cast<uintptr_t>(virtual_membase_);
  return {reinterpret_cast<uintptr_t>(physical_membase_),v+0xA0000000ull,v+0xC0000000ull,
          v+0xE0000000ull+heaps_.vE0000000.host_address_offset()-0x1000ull};
}
bool Memory::IsSnapshotHostRange(void* base,size_t length) const noexcept {
  if(!virtual_membase_ || !physical_membase_ || !length)return false;
  const auto address=reinterpret_cast<uintptr_t>(base);
  if(length>UINTPTR_MAX-address)return true;
  for(auto alias:PhysicalSnapshotAliases())
    if(address<alias+PhysicalSnapshotTracker::kEnd && address+length>alias+PhysicalSnapshotTracker::kFirst)return true;
  return false;
}
bool Memory::ProtectHostMemory(void* base,size_t length,PageAccess access,PageAccess* previous) {
  if(!IsSnapshotHostRange(base,length))return rex::memory::Protect(base,length,access,previous);
  auto lock=AcquireSnapshotMutationLock();
  if(certified_snapshots_ && !certified_snapshots_->BeforeHostMutation(
      reinterpret_cast<uintptr_t>(base),length))return false;
  return rex::memory::Protect(base,length,access,previous);
}
void* Memory::AllocateHostMemory(void* base,size_t length,AllocationType type,PageAccess access) {
  if(!IsSnapshotHostRange(base,length))return rex::memory::AllocFixed(base,length,type,access);
  auto lock=AcquireSnapshotMutationLock();
  if(certified_snapshots_ && !certified_snapshots_->BeforeHostMutation(
      reinterpret_cast<uintptr_t>(base),length))return nullptr;
  return rex::memory::AllocFixed(base,length,type,access);
}
bool Memory::PrepareHostWrite(void* destination,size_t length) {
  if(!IsSnapshotHostRange(destination,length))return true;
  auto lock=AcquireSnapshotMutationLock();
  return !certified_snapshots_||certified_snapshots_->BeforeHostMutation(
      reinterpret_cast<uintptr_t>(destination),length);
}
void Memory::InvalidateHostSnapshot(void* base,size_t length) {
  if(!IsSnapshotHostRange(base,length))return;
  auto lock=AcquireSnapshotMutationLock();
  if(certified_snapshots_)certified_snapshots_->InvalidateHost(reinterpret_cast<uintptr_t>(base),length);
}
Memory::ExternalHostWrite Memory::BeginExternalHostWrite(void* destination,size_t length) {
  ExternalHostWrite result;
  if(!length)return result;
  result.host_=reinterpret_cast<uintptr_t>(destination);result.length_=length;
  if(length>UINTPTR_MAX-result.host_){result.ready_=false;return result;}
  auto lock=global_critical_region_.Acquire();
  if(external_host_writes_.size()>=kMaxExternalHostWrites){result.ready_=false;return result;}
  // Track all destinations: physical ranges below the certificate domain and
  // Windows' E-view offset still need exact OS-writer exclusion for coherence.
  try {external_host_writes_.emplace_back(result.host_,length);}
  catch(...) {result.ready_=false;return result;}
  result.snapshot_tracked_=IsSnapshotHostRange(destination,length);
  if(result.snapshot_tracked_) {
    try {
      result.ready_=external_snapshot_writers_!=UINT32_MAX &&
          (!certified_snapshots_ || certified_snapshots_->BeginExternalWrite(result.host_,length));
    } catch(...) {result.ready_=false;}
    if(!result.ready_){external_host_writes_.pop_back();return result;}
    ++external_snapshot_writers_;
  }
  result.memory_=this;
  return result;
}
void Memory::EndExternalHostWrite(uintptr_t host,size_t length,bool snapshot_tracked) {
  auto lock=global_critical_region_.Acquire();
  const auto it=std::find(external_host_writes_.begin(),external_host_writes_.end(),
                          std::pair<uintptr_t,size_t>{host,length});
  if(it==external_host_writes_.end())std::terminate();
  // The syscall may have written a prefix even when it returned an error.
  // Publish ownership invalidation before removing the active writer, in the
  // same global-lock transaction used by native snapshot capture. Notification
  // is independent of optional byte certificates and never changes protection.
  // Existing callbacks use the existing global -> cache/coherence lock order.
  const uintptr_t base=reinterpret_cast<uintptr_t>(virtual_membase_);
  const std::array<ExternalWriteAlias,5> aliases{{
      {reinterpret_cast<uintptr_t>(physical_membase_),0,0x20000000},
      {base+0xA0000000ull,0,0x20000000},
      {base+0xC0000000ull,0,0x20000000},
      {base+0xE0000000ull+heaps_.vE0000000.host_address_offset(),0x1000,0x1FD01000},
      {base+0x7F000000ull,0,0x01000000}}};
  const auto ranges=ExternalWritePhysicalRanges(host,length,system_page_size_,aliases);
  for(size_t range=0;range<ranges.count;++range) {
    const auto [address,bytes]=ranges.values[range];
    for(const auto* callback:physical_memory_invalidation_callbacks_)
      (void)callback->first(callback->second,address,bytes,true);
  }
  if(snapshot_tracked) {
    if(!external_snapshot_writers_)std::terminate();
    --external_snapshot_writers_;
    if(certified_snapshots_)certified_snapshots_->EndExternalWrite(host,length);
  }
  external_host_writes_.erase(it);
}
Memory::ExternalHostWrite::~ExternalHostWrite() {
  if(memory_)memory_->EndExternalHostWrite(host_,length_,snapshot_tracked_);
}

bool Memory::IsExternalHostWritePending(uint32_t address,uint32_t length) {
  if(!virtual_membase_||!physical_membase_||!length||uint64_t(address)+length>0x20000000ull)return true;
  auto lock=global_critical_region_.Acquire();
  const uint64_t end=uint64_t(address)+length;
  const uintptr_t base=reinterpret_cast<uintptr_t>(virtual_membase_);
  const auto overlaps=[&](uintptr_t alias,uint32_t first,uint32_t last) {
    const uint64_t lo=std::max<uint64_t>(address,first),hi=std::min<uint64_t>(end,last);
    if(lo>=hi)return false;
    const uintptr_t host_first=alias+lo-first,host_last=alias+hi-first;
    for(const auto& [writer,bytes]:external_host_writes_)
      if(writer<host_last&&writer+bytes>host_first)return true;
    return false;
  };
  return overlaps(reinterpret_cast<uintptr_t>(physical_membase_),0,0x20000000) ||
      overlaps(base+0xA0000000ull,0,0x20000000) ||
      overlaps(base+0xC0000000ull,0,0x20000000) ||
      overlaps(base+0xE0000000ull+heaps_.vE0000000.host_address_offset(),0x1000,0x1FD01000) ||
      overlaps(base+0x7F000000ull,0,0x01000000);
}

bool Memory::Initialize() {
  file_name_ = fmt::format("xenia_memory_{}", chrono::Clock::QueryHostTickCount());

  // Create main page file-backed mapping. This is all reserved but
  // uncommitted (so it shouldn't expand page file).
  mapping_ =
      rex::memory::CreateFileMappingHandle(file_name_,
                                           // entire 4gb space + 512mb physical:
                                           0x11FFFFFFF, rex::memory::PageAccess::kReadWrite, false);
  if (mapping_ == rex::memory::kFileMappingHandleInvalid) {
    REXSYS_ERROR("Unable to reserve the 4gb guest address space.");
    assert_always();
    return false;
  }

  // Attempt to create our views. This may fail at the first address
  // we pick, so try a few times.
  mapping_base_ = 0;
  for (size_t n = 32; n < 64; n++) {
    auto mapping_base = reinterpret_cast<uint8_t*>(1ull << n);
    if (!MapViews(mapping_base)) {
      mapping_base_ = mapping_base;
      break;
    }
  }
  if (!mapping_base_) {
    REXSYS_ERROR("Unable to find a continuous block in the 64bit address space.");
    assert_always();
    return false;
  }
  virtual_membase_ = mapping_base_;
  physical_membase_ = mapping_base_ + 0x100000000ull;

  // Prepare virtual heaps.
  heaps_.v00000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestVirtual, 0x00000000,
                              0x40000000, 4096);
  heaps_.v40000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestVirtual, 0x40000000,
                              0x40000000 - 0x01000000, 64 * 1024);
  heaps_.v80000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestXex, 0x80000000,
                              0x10000000, 64 * 1024);
  heaps_.v90000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestXex, 0x90000000,
                              0x10000000, 4096);

  // Prepare physical heaps.
  heaps_.physical.Initialize(this, physical_membase_, memory::HeapType::kGuestPhysical, 0x00000000,
                             0x20000000, 4096);
  heaps_.vA0000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestPhysical, 0xA0000000,
                              0x20000000, 64 * 1024, &heaps_.physical);
  heaps_.vC0000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestPhysical, 0xC0000000,
                              0x20000000, 16 * 1024 * 1024, &heaps_.physical);
  heaps_.vE0000000.Initialize(this, virtual_membase_, memory::HeapType::kGuestPhysical, 0xE0000000,
                              0x1FD00000, 4096, &heaps_.physical);
  // Recompiled code adds the 0xE0 heap's host offset at compile time; it
  // must match the offset this host's allocation granularity requires.
  if (heaps_.vE0000000.host_address_offset() !=
      memory::detail::PhysicalHostOffset(0xE0000000u)) {
    REXSYS_ERROR("0xE0000000 heap host offset {:#x} does not match the compiled offset {:#x}",
                 heaps_.vE0000000.host_address_offset(),
                 memory::detail::PhysicalHostOffset(0xE0000000u));
    return false;
  }

  // Protect the first and last 64kb of memory.
  heaps_.v00000000.AllocFixed(0x00000000, 0x10000, 0x10000,
                              memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                              !REXCVAR_GET(protect_zero)
                                  ? memory::kMemoryProtectRead | memory::kMemoryProtectWrite
                                  : memory::kMemoryProtectNoAccess);
  heaps_.physical.AllocFixed(0x1FFF0000, 0x10000, 0x10000, memory::kMemoryAllocationReserve,
                             memory::kMemoryProtectNoAccess);

  // GPU writeback.
  // 0xC... is physical, 0x7F... is virtual. We may need to overlay these.
  heaps_.vC0000000.AllocFixed(0xC0000000, 0x01000000, 32,
                              memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                              memory::kMemoryProtectRead | memory::kMemoryProtectWrite);

  // Pre-commit the physical memory range so the GPU can access it
  // without page faults. Reference: xenia-canary 5f5be0668.
  AllocateHostMemory(heaps_.physical.TranslateRelative(0), heaps_.physical.heap_size(),
                          rex::memory::AllocationType::kCommit,
                          rex::memory::PageAccess::kReadWrite);

  // Install MMIO handler for physical address translation and MMIO ranges
  mmio_handler_ = runtime::MMIOHandler::Install(
      virtual_membase_, physical_membase_, physical_membase_ + 0x1FFFFFFF, HostToGuestVirtualThunk,
      this, AccessViolationCallbackThunk, this,CertifiedAccessViolationOwnedThunk);
  if (!mmio_handler_) {
    REXSYS_ERROR("Unable to install MMIO handlers");
    assert_always();
    return false;
  }
  REXSYS_DEBUG("Installed MMIO handler for physical address translation");

  // ?
  uint32_t unk_phys_alloc;
  heaps_.vA0000000.Alloc(0x340000, 64 * 1024, memory::kMemoryAllocationReserve,
                         memory::kMemoryProtectNoAccess, true, &unk_phys_alloc);

  // Allocate region at start of XEX range. Title 544307D5 explicitly
  // accesses 0x8000001C and expects a specific constant value.
  // Reference: xenia-canary 78f97f8ff.
  uint32_t unknown_xex_range;
  heaps_.v80000000.Alloc(0x40000, 4 * 1024, memory::kMemoryAllocationCommit,
                         memory::kMemoryProtectRead | memory::kMemoryProtectWrite, false,
                         &unknown_xex_range);

  uint32_t value_to_write = rex::byte_swap(uint32_t(0x2a6e3f38));
  std::memcpy(TranslateVirtual(0x80000000 + 0x1C), &value_to_write, sizeof(uint32_t));

  return true;
}

static const struct {
  uint64_t virtual_address_start;
  uint64_t virtual_address_end;
  uint64_t target_address;
} map_info[] = {
    // (1024mb) - virtual 4k pages
    {
        0x00000000,
        0x3FFFFFFF,
        0x0000000000000000ull,
    },
    // (1024mb) - virtual 64k pages (cont)
    {
        0x40000000,
        0x7EFFFFFF,
        0x0000000040000000ull,
    },
    //   (16mb) - GPU writeback + 15mb of XPS?
    {
        0x7F000000,
        0x7FFFFFFF,
        0x0000000100000000ull,
    },
    //  (256mb) - xex 64k pages
    {
        0x80000000,
        0x8FFFFFFF,
        0x0000000080000000ull,
    },
    //  (256mb) - xex 4k pages
    {
        0x90000000,
        0x9FFFFFFF,
        0x0000000080000000ull,
    },
    //  (512mb) - physical 64k pages
    {
        0xA0000000,
        0xBFFFFFFF,
        0x0000000100000000ull,
    },
    //          - physical 16mb pages
    {
        0xC0000000,
        0xDFFFFFFF,
        0x0000000100000000ull,
    },
    //          - physical 4k pages
    {
        0xE0000000,
        0xFFFFFFFF,
        0x0000000100001000ull,
    },
    //          - physical raw
    {
        0x100000000,
        0x11FFFFFFF,
        0x0000000100000000ull,
    },
};
int Memory::MapViews(uint8_t* mapping_base) {
  assert_true(rex::countof(map_info) == rex::countof(views_.all_views));
  // 0xE0000000 4 KB offset is emulated via host_address_offset and on the CPU
  // side if system allocation granularity is bigger than 4 KB.
  uint64_t granularity_mask = ~uint64_t(system_allocation_granularity_ - 1);
  for (size_t n = 0; n < rex::countof(map_info); n++) {
    views_.all_views[n] = reinterpret_cast<uint8_t*>(rex::memory::MapFileView(
        mapping_, mapping_base + map_info[n].virtual_address_start,
        map_info[n].virtual_address_end - map_info[n].virtual_address_start + 1,
        rex::memory::PageAccess::kReadWrite, map_info[n].target_address & granularity_mask));
    if (!views_.all_views[n]) {
      // Failed, so bail and try again.
      UnmapViews();
      return 1;
    }
  }
  return 0;
}

void Memory::UnmapViews() {
  for (size_t n = 0; n < rex::countof(views_.all_views); n++) {
    if (views_.all_views[n]) {
      size_t length = map_info[n].virtual_address_end - map_info[n].virtual_address_start + 1;
      rex::memory::UnmapFileView(mapping_, views_.all_views[n], length);
    }
  }
}

PhysicalAllocationObservations::Ptr Memory::ObservePhysicalAllocation(uint32_t address,uint32_t bytes) {
  if(!bytes||uint64_t(address)+bytes>0x20000000ull)return {};
  auto lock=global_critical_region_.Acquire();
  HeapAllocationInfo first{},last{};
  if(!heaps_.physical.QueryRegionInfo(address,&first)||
     !heaps_.physical.QueryRegionInfo(address+bytes-1,&last))return {};
  const auto valid=[](const HeapAllocationInfo& info) {
    return (info.state&kMemoryAllocationCommit)&&(info.protect&kMemoryProtectWrite)&&
        info.allocation_size&&uint64_t(info.allocation_base)+info.allocation_size<=0x20000000ull;
  };
  if(!valid(first)||!valid(last)||first.allocation_base!=last.allocation_base||
     first.allocation_size!=last.allocation_size||address<first.allocation_base||
     uint64_t(address)+bytes>uint64_t(first.allocation_base)+first.allocation_size)return {};
  return allocation_observations_.Observe(first.allocation_base,first.allocation_size);
}

void Memory::Reset() {
  heaps_.v00000000.Reset();
  heaps_.v40000000.Reset();
  heaps_.v80000000.Reset();
  heaps_.v90000000.Reset();
  heaps_.physical.Reset();
}

const BaseHeap* Memory::LookupHeap(uint32_t address) const {
  if (address < 0x40000000) {
    return &heaps_.v00000000;
  } else if (address < 0x7F000000) {
    return &heaps_.v40000000;
  } else if (address < 0x80000000) {
    return nullptr;
  } else if (address < 0x90000000) {
    return &heaps_.v80000000;
  } else if (address < 0xA0000000) {
    return &heaps_.v90000000;
  } else if (address < 0xC0000000) {
    return &heaps_.vA0000000;
  } else if (address < 0xE0000000) {
    return &heaps_.vC0000000;
  } else if (address < 0xFFD00000) {
    return &heaps_.vE0000000;
  } else {
    return nullptr;
  }
}

BaseHeap* Memory::LookupHeapByType(bool physical, uint32_t page_size) {
  if (physical) {
    if (page_size <= 4096) {
      return &heaps_.vE0000000;
    } else if (page_size <= 64 * 1024) {
      return &heaps_.vA0000000;
    } else {
      return &heaps_.vC0000000;
    }
  } else {
    if (page_size <= 4096) {
      return &heaps_.v00000000;
    } else {
      return &heaps_.v40000000;
    }
  }
}

VirtualHeap* Memory::GetPhysicalHeap() {
  return &heaps_.physical;
}

uint32_t Memory::HostToGuestVirtual(const void* host_address) const {
  size_t virtual_address =
      reinterpret_cast<size_t>(host_address) - reinterpret_cast<size_t>(virtual_membase_);
  uint32_t vE0000000_host_offset = heaps_.vE0000000.host_address_offset();
  size_t vE0000000_host_base = size_t(heaps_.vE0000000.heap_base()) + vE0000000_host_offset;
  if (virtual_address >= vE0000000_host_base &&
      virtual_address <= (vE0000000_host_base + (heaps_.vE0000000.heap_size() - 1))) {
    virtual_address -= vE0000000_host_offset;
  }
  return uint32_t(virtual_address);
}

uint32_t Memory::HostToGuestVirtualThunk(const void* context, const void* host_address) {
  return reinterpret_cast<const memory::Memory*>(context)->HostToGuestVirtual(host_address);
}

uint32_t Memory::GetPhysicalAddress(uint32_t address) const {
  const BaseHeap* heap = LookupHeap(address);
  if (!heap || heap->heap_type() != memory::HeapType::kGuestPhysical) {
    return UINT32_MAX;
  }
  return static_cast<const PhysicalHeap*>(heap)->GetPhysicalAddress(address);
}

void Memory::Zero(uint32_t address, uint32_t size) {
  std::memset(TranslateVirtual(address), 0, size);
}

void Memory::Fill(uint32_t address, uint32_t size, uint8_t value) {
  std::memset(TranslateVirtual(address), value, size);
}

void Memory::Copy(uint32_t dest, uint32_t src, uint32_t size) {
  uint8_t* pdest = TranslateVirtual(dest);
  const uint8_t* psrc = TranslateVirtual(src);
  std::memcpy(pdest, psrc, size);
}

uint32_t Memory::SearchAligned(uint32_t start, uint32_t end, const uint32_t* values,
                               size_t value_count) {
  assert_true(start <= end);
  auto p = TranslateVirtual<const uint32_t*>(start);
  auto pe = TranslateVirtual<const uint32_t*>(end);
  while (p != pe) {
    if (*p == values[0]) {
      const uint32_t* pc = p + 1;
      size_t matched = 1;
      for (size_t n = 1; n < value_count; n++, pc++) {
        if (*pc != values[n]) {
          break;
        }
        matched++;
      }
      if (matched == value_count) {
        return HostToGuestVirtual(p);
      }
    }
    p++;
  }
  return 0;
}

bool Memory::AddVirtualMappedRange(uint32_t virtual_address, uint32_t mask, uint32_t size,
                                   void* context, runtime::MMIOReadCallback read_callback,
                                   runtime::MMIOWriteCallback write_callback) {
  if (!AllocateHostMemory(TranslateVirtual(virtual_address), size,
                               rex::memory::AllocationType::kCommit,
                               rex::memory::PageAccess::kNoAccess)) {
    REXSYS_ERROR("Unable to map range; commit/protect failed");
    return false;
  }
  return mmio_handler_->RegisterRange(virtual_address, mask, size, context, read_callback,
                                      write_callback);
}

runtime::MMIORange* Memory::LookupVirtualMappedRange(uint32_t virtual_address) {
  return mmio_handler_->LookupRange(virtual_address);
}

bool Memory::AccessViolationCallback(std::unique_lock<std::recursive_mutex> global_lock_locked_once,
                                     void* host_address, bool is_write) {
  // Access via physical_membase_ is special, when need to bypass everything
  // (for instance, for a data provider to actually write the data) so only
  // triggering callbacks on virtual memory regions.
  if (reinterpret_cast<size_t>(host_address) < reinterpret_cast<size_t>(virtual_membase_) ||
      reinterpret_cast<size_t>(host_address) >= reinterpret_cast<size_t>(physical_membase_)) {
    return certified_snapshots_ && certified_snapshots_->HandleFault(
        reinterpret_cast<uintptr_t>(host_address),is_write)==PhysicalSnapshotTracker::Fault::Handled;
  }
  uint32_t virtual_address = HostToGuestVirtual(host_address);
  BaseHeap* heap = LookupHeap(virtual_address);
  if (!heap) {
    return false;
  }
  if (heap->heap_type() != memory::HeapType::kGuestPhysical) {
    return false;
  }

  // Access violation callbacks from the guest are triggered when the global
  // critical region mutex is locked once.
  //
  // Will be rounded to physical page boundaries internally, so just pass 1 as
  // the length - guranteed not to cross page boundaries also.
  auto physical_heap = static_cast<PhysicalHeap*>(heap);
  if (physical_heap->TriggerCallbacks(std::move(global_lock_locked_once), virtual_address, 1,
                                      is_write, false)) {
    return true;
  }

  auto snapshot_recovery_lock=AcquireSnapshotMutationLock();
  if(certified_snapshots_) {
    const auto result=certified_snapshots_->HandleFault(reinterpret_cast<uintptr_t>(host_address),is_write);
    if(result!=PhysicalSnapshotTracker::Fault::NotOwned)
      return result==PhysicalSnapshotTracker::Fault::Handled;
  }
  // Recovery path for stale host protection state in physical memory:
  // if guest metadata says the page is writable but host protection is still
  // read-only / no-access, restore write access and resume execution.
  if (is_write) {
    constexpr uint32_t kWriteProtectMask =
        memory::kMemoryProtectWrite | memory::kMemoryProtectWriteCombine;
    uint32_t guest_protect = 0;
    bool allow_write = false;
    if (heap->QueryProtect(virtual_address, &guest_protect) &&
        (guest_protect & kWriteProtectMask)) {
      allow_write = true;
    } else {
      // If write-watch left current protect stale, trust committed allocation
      // metadata first for this alias.
      HeapAllocationInfo guest_info{};
      if (heap->QueryRegionInfo(virtual_address, &guest_info) &&
          (guest_info.state & memory::kMemoryAllocationCommit) &&
          (guest_info.allocation_protect & kWriteProtectMask)) {
        guest_protect = guest_info.protect;
        allow_write = true;
      } else {
        // Alias-aware fallback: consult canonical 0x00000000 physical heap
        // metadata when this alias has stale tracking.
        uint32_t physical_address = GetPhysicalAddress(virtual_address);
        if (physical_address != UINT32_MAX) {
          uint32_t physical_protect = 0;
          if (heaps_.physical.QueryProtect(physical_address, &physical_protect) &&
              (physical_protect & kWriteProtectMask)) {
            guest_protect = physical_protect;
            allow_write = true;
          } else {
            HeapAllocationInfo physical_info{};
            if (heaps_.physical.QueryRegionInfo(physical_address, &physical_info) &&
                (physical_info.state & memory::kMemoryAllocationCommit) &&
                (physical_info.allocation_protect & kWriteProtectMask)) {
              guest_protect = physical_info.protect;
              allow_write = true;
            }
          }
        }
      }
    }
    if (allow_write) {
      size_t host_page_size = rex::memory::page_size();
      uintptr_t page_base =
          reinterpret_cast<uintptr_t>(host_address) & ~(uintptr_t(host_page_size - 1));
      if (ProtectHostMemory(reinterpret_cast<void*>(page_base), host_page_size,
                               rex::memory::PageAccess::kReadWrite, nullptr)) {
        REXSYS_WARN(
            "Recovered stale physical page protection for guest {:08X} (host {:016X}, "
            "guest_protect {:08X})",
            virtual_address, static_cast<uint64_t>(page_base), guest_protect);
        return true;
      }
    }

    uint32_t current_guest_protect = 0;
    heap->QueryProtect(virtual_address, &current_guest_protect);
    uint32_t physical_address = GetPhysicalAddress(virtual_address);
    uint32_t physical_protect = 0;
    if (physical_address != UINT32_MAX) {
      heaps_.physical.QueryProtect(physical_address, &physical_protect);
    }
    REXSYS_ERROR(
        "Unhandled guest physical write fault: guest={:08X} host={:016X} phys={:08X} "
        "guest_protect={:08X} physical_protect={:08X}",
        virtual_address, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(host_address)),
        physical_address, current_guest_protect, physical_protect);
  }

  return false;
}

bool Memory::CertifiedAccessViolationOwnedThunk(void* context,void* host_address,bool is_write) {
  // MMIOHandler holds the same global lock through this ownership query and
  // callback entry. Virtual aliases still run legacy GPU invalidation first;
  // AccessViolationCallback rechecks ownership after any lock release there.
  // Raw byte faults never gain GPU authority.
  auto* memory=static_cast<Memory*>(context);
  if(!memory->certified_fault_fastpath_ || !memory->certified_snapshots_ ||
     !memory->certified_snapshots_->OwnsFault(reinterpret_cast<uintptr_t>(host_address),is_write))return false;
  memory->certified_snapshots_->RecordOwnedFaultDispatch();return true;
}

bool Memory::AccessViolationCallbackThunk(
    std::unique_lock<std::recursive_mutex> global_lock_locked_once, void* context,
    void* host_address, bool is_write) {
  return reinterpret_cast<memory::Memory*>(context)->AccessViolationCallback(
      std::move(global_lock_locked_once), host_address, is_write);
}

bool Memory::TriggerPhysicalMemoryCallbacks(
    std::unique_lock<std::recursive_mutex> global_lock_locked_once, uint32_t virtual_address,
    uint32_t length, bool is_write, bool unwatch_exact_range, bool unprotect) {
  BaseHeap* heap = LookupHeap(virtual_address);
  if (!heap) {
    return false;
  }
  if (heap->heap_type() == memory::HeapType::kGuestPhysical) {
    auto physical_heap = static_cast<PhysicalHeap*>(heap);
    return physical_heap->TriggerCallbacks(std::move(global_lock_locked_once), virtual_address,
                                           length, is_write, unwatch_exact_range, unprotect);
  }
  return false;
}

void* Memory::RegisterPhysicalMemoryInvalidationCallback(
    PhysicalMemoryInvalidationCallback callback, void* callback_context) {
  auto entry = new std::pair<PhysicalMemoryInvalidationCallback, void*>(callback, callback_context);
  auto lock = global_critical_region_.Acquire();
  physical_memory_invalidation_callbacks_.push_back(entry);
  return entry;
}

void Memory::UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle) {
  auto entry =
      reinterpret_cast<std::pair<PhysicalMemoryInvalidationCallback, void*>*>(callback_handle);
  {
    auto lock = global_critical_region_.Acquire();
    auto it = std::find(physical_memory_invalidation_callbacks_.begin(),
                        physical_memory_invalidation_callbacks_.end(), entry);
    assert_true(it != physical_memory_invalidation_callbacks_.end());
    if (it != physical_memory_invalidation_callbacks_.end()) {
      physical_memory_invalidation_callbacks_.erase(it);
    }
  }
  delete entry;
}

void Memory::EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                                 bool enable_invalidation_notifications,
                                                 bool enable_data_providers) {
  heaps_.vA0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications, enable_data_providers);
  heaps_.vC0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications, enable_data_providers);
  heaps_.vE0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications, enable_data_providers);
}

bool Memory::TryEnablePhysicalMemoryAccessCallbacks(uint32_t address,uint32_t length,
                                                    bool notifications,bool providers) {
  if(!length||uint64_t(address)+length>0x20000000ull)return false;
  // Attempt every alias even after a failure, keeping already armed ranges
  // protected. Callers must reject the snapshot if any required arm failed.
  const bool a=heaps_.vA0000000.TryEnableAccessCallbacks(address,length,notifications,providers);
  const bool c=heaps_.vC0000000.TryEnableAccessCallbacks(address,length,notifications,providers);
  const bool e=heaps_.vE0000000.TryEnableAccessCallbacks(address,length,notifications,providers);
  return a&&c&&e;
}

uint32_t Memory::SystemHeapAlloc(uint32_t size, uint32_t alignment, uint32_t system_heap_flags) {
  // TODO(benvanik): lightweight pool.
  bool is_physical = !!(system_heap_flags & memory::kSystemHeapPhysical);
  auto heap = LookupHeapByType(is_physical, 4096);
  uint32_t address;
  if (!heap->AllocSystemHeap(
          size, alignment, memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
          memory::kMemoryProtectRead | memory::kMemoryProtectWrite, false, &address)) {
    return 0;
  }
  Zero(address, size);
  return address;
}

void Memory::SystemHeapFree(uint32_t address, uint32_t* out_region_size) {
  if (!address) {
    return;
  }
  // TODO(benvanik): lightweight pool.
  auto heap = LookupHeap(address);
  heap->Release(address, out_region_size);
}

void Memory::GetHeapsPageStatsSummary(const BaseHeap* const* provided_heaps, size_t heaps_count,
                                      uint32_t& unreserved_pages, uint32_t& reserved_pages,
                                      uint32_t& used_pages, uint32_t& reserved_bytes) {
  // Heap array is fixed after init; page counts are approximate statistics.
  for (size_t i = 0; i < heaps_count; i++) {
    const BaseHeap* heap = provided_heaps[i];
    uint32_t heap_unreserved = heap->unreserved_page_count();
    uint32_t heap_reserved = heap->reserved_page_count();

    unreserved_pages += heap_unreserved;
    reserved_pages += heap_reserved;
    used_pages += ((heap->total_page_count() - heap_unreserved) * heap->page_size()) / 4096;
    reserved_bytes += heap_reserved * heap->page_size();
  }
}

void Memory::DumpMap() {
  REXSYS_ERROR("==================================================================");
  REXSYS_ERROR("Memory Dump");
  REXSYS_ERROR("==================================================================");
  REXSYS_ERROR("               System Page Size: {0} ({0:08X})", system_page_size_);
  REXSYS_ERROR("  System Allocation Granularity: {0} ({0:08X})", system_allocation_granularity_);
  REXSYS_ERROR("                Virtual Membase: {}", fmt::ptr(virtual_membase_));
  REXSYS_ERROR("               Physical Membase: {}", fmt::ptr(physical_membase_));
  REXSYS_ERROR("");
  REXSYS_ERROR("------------------------------------------------------------------");
  REXSYS_ERROR("Virtual Heaps");
  REXSYS_ERROR("------------------------------------------------------------------");
  REXSYS_ERROR("");
  heaps_.v00000000.DumpMap();
  heaps_.v40000000.DumpMap();
  heaps_.v80000000.DumpMap();
  heaps_.v90000000.DumpMap();
  REXSYS_ERROR("");
  REXSYS_ERROR("------------------------------------------------------------------");
  REXSYS_ERROR("Physical Heaps");
  REXSYS_ERROR("------------------------------------------------------------------");
  REXSYS_ERROR("");
  heaps_.physical.DumpMap();
  heaps_.vA0000000.DumpMap();
  heaps_.vC0000000.DumpMap();
  heaps_.vE0000000.DumpMap();
  REXSYS_ERROR("");
}

bool Memory::Save(stream::ByteStream* stream) {
  REXSYS_DEBUG("Serializing memory...");
  heaps_.v00000000.Save(stream);
  heaps_.v40000000.Save(stream);
  heaps_.v80000000.Save(stream);
  heaps_.v90000000.Save(stream);
  heaps_.physical.Save(stream);

  return true;
}

bool Memory::Restore(stream::ByteStream* stream) {
  REXSYS_DEBUG("Restoring memory...");
  heaps_.v00000000.Restore(stream);
  heaps_.v40000000.Restore(stream);
  heaps_.v80000000.Restore(stream);
  heaps_.v90000000.Restore(stream);
  heaps_.physical.Restore(stream);

  return true;
}

//=============================================================================
// Recompiled Code Function Table
//=============================================================================

bool Memory::InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,
                                     uint32_t image_size) {
  constexpr uint32_t kThunkReserveSize = runtime::FunctionDispatcher::kThunkReserveSize;
  uint32_t table_base = image_base + image_size;
  uint32_t table_size = (code_size + kThunkReserveSize) * 2;

  REXSYS_DEBUG(
      "Initializing function table at {:08X}, size {:08X} for code {:08X}-{:08X} "
      "(+{:08X} thunk reserve)",
      table_base, table_size, code_base, code_base + code_size, kThunkReserveSize);

  if (!heaps_.v80000000.AllocFixed(
          table_base, table_size, 0x10000,
          memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
          memory::kMemoryProtectRead | memory::kMemoryProtectWrite)) {
    REXSYS_ERROR("Failed to allocate function table at {:08X}", table_base);
    return false;
  }

  Zero(table_base, table_size);

  std::lock_guard lock(function_tables_mutex_);
  function_tables_.push_back({
      .table_base = table_base,
      .code_base = code_base,
      .code_size = code_size,
      .thunk_reserve = kThunkReserveSize,
  });

  return true;
}

bool Memory::DestroyFunctionTable(uint32_t code_base) {
  uint32_t table_base = 0;
  uint32_t table_size = 0;
  uint32_t logged_code_base = 0;
  uint32_t logged_code_size = 0;
  {
    std::lock_guard lock(function_tables_mutex_);
    auto it = std::find_if(
        function_tables_.begin(), function_tables_.end(),
        [code_base](const FunctionTableEntry& entry) { return entry.code_base == code_base; });
    if (it == function_tables_.end()) {
      return false;
    }
    table_base = it->table_base;
    table_size = (it->code_size + it->thunk_reserve) * 2;
    logged_code_base = it->code_base;
    logged_code_size = it->code_size;
    function_tables_.erase(it);
  }

  REXSYS_DEBUG("Destroying function table at {:08X}, size {:08X} for code {:08X}-{:08X}",
               table_base, table_size, logged_code_base, logged_code_base + logged_code_size);

  heaps_.v80000000.Release(table_base);
  return true;
}

bool Memory::SetFunction(uint32_t guest_address, PPCFunc* host_function) {
  uint32_t table_address = 0;
  {
    std::lock_guard lock(function_tables_mutex_);
    for (const auto& entry : function_tables_) {
      uint32_t range_end = entry.code_base + entry.code_size + entry.thunk_reserve;
      if (guest_address >= entry.code_base && guest_address < range_end) {
        uint64_t offset = (uint64_t(guest_address) - entry.code_base) * 2;
        table_address = entry.table_base + uint32_t(offset);
        break;
      }
    }
  }
  if (!table_address) {
    return false;
  }
  auto* slot = TranslateVirtual<PPCFunc**>(table_address);
  *slot = host_function;
  return true;
}

bool Memory::HasAnyFunctionTable() const {
  std::lock_guard lock(function_tables_mutex_);
  return !function_tables_.empty();
}

rex::memory::PageAccess ToPageAccess(uint32_t protect) {
  bool is_writable =
      (protect & memory::kMemoryProtectWrite) || (protect & memory::kMemoryProtectWriteCombine);

  if ((protect & memory::kMemoryProtectRead) && !is_writable) {
    return rex::memory::PageAccess::kReadOnly;
  } else if ((protect & memory::kMemoryProtectRead) && is_writable) {
    return rex::memory::PageAccess::kReadWrite;
  } else {
    return rex::memory::PageAccess::kNoAccess;
  }
}

uint32_t FromPageAccess(rex::memory::PageAccess protect) {
  switch (protect) {
    case memory::PageAccess::kNoAccess:
      return memory::kMemoryProtectNoAccess;
    case memory::PageAccess::kReadOnly:
      return memory::kMemoryProtectRead;
    case memory::PageAccess::kReadWrite:
      return memory::kMemoryProtectRead | memory::kMemoryProtectWrite;
    case memory::PageAccess::kExecuteReadOnly:
      // Guest memory cannot be executable - this should never happen :)
      assert_always();
      return memory::kMemoryProtectRead;
    case memory::PageAccess::kExecuteReadWrite:
      // Guest memory cannot be executable - this should never happen :)
      assert_always();
      return memory::kMemoryProtectRead | memory::kMemoryProtectWrite;
  }

  return memory::kMemoryProtectNoAccess;
}

BaseHeap::BaseHeap() : memory_(nullptr), membase_(nullptr), heap_type_(HeapType::kGuestVirtual), heap_base_(0), heap_size_(0), page_size_(0) {}

BaseHeap::~BaseHeap() = default;

void BaseHeap::Initialize(memory::Memory* memory, uint8_t* membase, HeapType heap_type,
                          uint32_t heap_base, uint32_t heap_size, uint32_t page_size,
                          uint32_t host_address_offset) {
  memory_ = memory;
  membase_ = membase;
  heap_type_ = heap_type;
  heap_base_ = heap_base;
  heap_size_ = heap_size;
  page_size_ = page_size;
  assert_true(rex::is_pow2(page_size_));
  page_size_shift_ = rex::log2_floor(page_size_);
  host_address_offset_ = host_address_offset;
  page_table_.resize(heap_size / page_size);
  unreserved_page_count_ = uint32_t(page_table_.size());
}

uint64_t BaseHeap::NextAccessGeneration() noexcept {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1);
}

void BaseHeap::InvalidateAllocationObservations(uint32_t address,uint64_t bytes) noexcept {
  if(!memory_)return;
  if(heap_type_==HeapType::kHostPhysical)
    memory_->allocation_observations_.Invalidate(address,bytes);
  else if(heap_type_==HeapType::kGuestPhysical)
    memory_->allocation_observations_.Invalidate(static_cast<PhysicalHeap*>(this)->GetPhysicalAddress(address),bytes);
}

void BaseHeap::Dispose() {
  auto snapshot_mutation_lock =
      (memory_ && (heap_type_==HeapType::kGuestPhysical || heap_type_==HeapType::kHostPhysical))
          ? memory_->AcquireSnapshotMutationLock() : std::unique_lock<std::recursive_mutex>{};
  const AccessMutation access_mutation(*this);
  InvalidateAllocationObservations(heap_base_,heap_size_);
  if(memory_)memory_->InvalidateHostSnapshot(TranslateRelative(0),heap_size_);
  // Walk table and release all regions.
  for (uint32_t page_number = 0; page_number < page_table_.size(); ++page_number) {
    auto& page_entry = page_table_[page_number];
    if (page_entry.state) {
      rex::memory::DeallocFixed(TranslateRelative(page_number << page_size_shift_), 0,
                                rex::memory::DeallocationType::kRelease);
      page_number += page_entry.region_page_count;
    }
  }
}

void BaseHeap::DumpMap() {
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  REXSYS_ERROR("------------------------------------------------------------------");
  REXSYS_ERROR("Heap: {:08X}-{:08X}", heap_base_, heap_base_ + (heap_size_ - 1));
  REXSYS_ERROR("------------------------------------------------------------------");
  REXSYS_ERROR("            Heap Base: {:08X}", heap_base_);
  REXSYS_ERROR("            Heap Size: {0} ({0:08X})", heap_size_);
  REXSYS_ERROR("            Page Size: {0} ({0:08X})", page_size_);
  REXSYS_ERROR("           Page Count: {}", page_table_.size());
  REXSYS_ERROR("  Host Address Offset: {0} ({0:08X})", host_address_offset_);
  bool is_empty_span = false;
  uint32_t empty_span_start = 0;
  for (uint32_t i = 0; i < uint32_t(page_table_.size()); ++i) {
    auto& page = page_table_[i];
    if (!page.state) {
      if (!is_empty_span) {
        is_empty_span = true;
        empty_span_start = i;
      }
      continue;
    }
    if (is_empty_span) {
      REXSYS_ERROR("  {:08X}-{:08X} {:6d}p {:10d}b unreserved",
                   heap_base_ + (empty_span_start << page_size_shift_),
                   heap_base_ + (i << page_size_shift_), i - empty_span_start,
                   (i - empty_span_start) << page_size_shift_);
      is_empty_span = false;
    }
    const char* state_name = "   ";
    if (page.state & memory::kMemoryAllocationCommit) {
      state_name = "COM";
    } else if (page.state & memory::kMemoryAllocationReserve) {
      state_name = "RES";
    }
    char access_r = (page.current_protect & memory::kMemoryProtectRead) ? 'R' : ' ';
    char access_w = (page.current_protect & memory::kMemoryProtectWrite) ? 'W' : ' ';
    uint32_t region_pages = page.region_page_count;
    REXSYS_ERROR("  {:08X}-{:08X} {:6d}p {:10d}b {} {}{}", heap_base_ + (i << page_size_shift_),
                 heap_base_ + ((i + region_pages) << page_size_shift_), region_pages,
                 region_pages << page_size_shift_, state_name, access_r, access_w);
    i += region_pages - 1;
  }
  if (is_empty_span) {
    REXSYS_ERROR("  {:08X}-{:08X} - {} unreserved pages)",
                 heap_base_ + (empty_span_start << page_size_shift_), heap_base_ + (heap_size_ - 1),
                 page_table_.size() - empty_span_start);
  }
}

uint32_t BaseHeap::GetTotalPageCount() {
  return uint32_t(page_table_.size());
}

uint32_t BaseHeap::GetUnreservedPageCount() {
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  uint32_t count = 0;
  bool is_empty_span = false;
  uint32_t empty_span_start = 0;
  uint32_t size = uint32_t(page_table_.size());
  for (uint32_t i = 0; i < size; ++i) {
    auto& page = page_table_[i];
    if (!page.state) {
      if (!is_empty_span) {
        is_empty_span = true;
        empty_span_start = i;
      }
      continue;
    }
    if (is_empty_span) {
      is_empty_span = false;
      count += i - empty_span_start;
    }
    i += page.region_page_count - 1;
  }
  if (is_empty_span) {
    count += size - empty_span_start;
  }
  return count;
}

bool BaseHeap::Save(stream::ByteStream* stream) {
  auto snapshot_mutation_lock =
      (memory_ && (heap_type_==HeapType::kGuestPhysical || heap_type_==HeapType::kHostPhysical))
          ? memory_->AcquireSnapshotMutationLock() : std::unique_lock<std::recursive_mutex>{};
  REXSYS_DEBUG("Heap {:08X}-{:08X}", heap_base_, heap_base_ + (heap_size_ - 1));

  for (size_t i = 0; i < page_table_.size(); i++) {
    auto& page = page_table_[i];
    stream->Write(page.qword);
    if (!page.state) {
      // Unallocated.
      continue;
    }

    // TODO(DrChat): write compressed with snappy.
    if (page.state & memory::kMemoryAllocationCommit) {
      void* addr = TranslateRelative(i << page_size_shift_);

      memory::PageAccess old_access;
      if(!memory_->ProtectHostMemory(addr,page_size_,memory::PageAccess::kReadWrite,&old_access))return false;

      stream->Write(addr, page_size_);

      if(!memory_->ProtectHostMemory(addr,page_size_,old_access,nullptr))return false;
    }
  }

  return true;
}

bool BaseHeap::Restore(stream::ByteStream* stream) {
  auto snapshot_mutation_lock =
      (memory_ && (heap_type_==HeapType::kGuestPhysical || heap_type_==HeapType::kHostPhysical))
          ? memory_->AcquireSnapshotMutationLock() : std::unique_lock<std::recursive_mutex>{};
  const AccessMutation access_mutation(*this);
  InvalidateAllocationObservations(heap_base_,heap_size_);
  if(memory_)memory_->InvalidateHostSnapshot(TranslateRelative(0),heap_size_);
  REXSYS_DEBUG("Heap {:08X}-{:08X}", heap_base_, heap_base_ + (heap_size_ - 1));

  for (size_t i = 0; i < page_table_.size(); i++) {
    auto& page = page_table_[i];
    page.qword = stream->Read<uint64_t>();
    if (!page.state) {
      // Unallocated.
      continue;
    }

    memory::PageAccess page_access = memory::PageAccess::kNoAccess;
    if ((page.current_protect & memory::kMemoryProtectRead) &&
        (page.current_protect & memory::kMemoryProtectWrite)) {
      page_access = memory::PageAccess::kReadWrite;
    } else if (page.current_protect & memory::kMemoryProtectRead) {
      page_access = memory::PageAccess::kReadOnly;
    }

    // Commit the memory if it isn't already. We do not need to reserve any
    // memory, as the mapping has already taken care of that.
    if (page.state & memory::kMemoryAllocationCommit) {
      if(!memory_->AllocateHostMemory(TranslateRelative(i << page_size_shift_), page_size_,
                              memory::AllocationType::kCommit, memory::PageAccess::kReadWrite))return false;
    }

    // Now read into memory. We'll set R/W protection first, then set the
    // protection back to its previous state.
    // TODO(DrChat): read compressed with snappy.
    if (page.state & memory::kMemoryAllocationCommit) {
      void* addr = TranslateRelative(i << page_size_shift_);
      if(!memory_->ProtectHostMemory(addr,page_size_,memory::PageAccess::kReadWrite,nullptr))return false;

      stream->Read(addr, page_size_);

      if(!memory_->ProtectHostMemory(addr,page_size_,page_access,nullptr))return false;
    }
  }

  return true;
}

void BaseHeap::Reset() {
  auto snapshot_mutation_lock =
      (memory_ && (heap_type_==HeapType::kGuestPhysical || heap_type_==HeapType::kHostPhysical))
          ? memory_->AcquireSnapshotMutationLock() : std::unique_lock<std::recursive_mutex>{};
  const AccessMutation access_mutation(*this);
  InvalidateAllocationObservations(heap_base_,heap_size_);
  if(memory_)memory_->InvalidateHostSnapshot(TranslateRelative(0),heap_size_);
  // TODO(DrChat): protect pages.
  std::memset(page_table_.data(), 0, sizeof(PageEntry) * page_table_.size());
  // TODO(Triang3l): Remove access callbacks from pages if this is a physical
  // memory heap.
}

bool BaseHeap::Alloc(uint32_t size, uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                     bool top_down, uint32_t* out_address) {
  *out_address = 0;
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  // Reserve address space at the top for thread stacks.
  // 0x3XXXXXXX is for system threads, 0x7XXXXXXX is for title threads.
  uint32_t heap_virtual_guest_offset = 0;
  if (heap_type_ == memory::HeapType::kGuestVirtual) {
    heap_virtual_guest_offset = 0x10000000;
    if (page_size_ == 0x10000) {
      heap_virtual_guest_offset = 0x0F000000;
    }
  }

  uint32_t low_address = heap_base_;
  uint32_t high_address = heap_base_ + (heap_size_ - 1) - heap_virtual_guest_offset;
  return AllocRange(low_address, high_address, size, alignment, allocation_type, protect, top_down,
                    out_address);
}

bool BaseHeap::AllocSystemHeap(uint32_t size, uint32_t alignment, uint32_t allocation_type,
                               uint32_t protect, bool top_down, uint32_t* out_address) {
  *out_address = 0;
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  uint32_t low_address = heap_base_;
  if (heap_type_ == memory::HeapType::kGuestVirtual) {
    low_address = heap_base_ + heap_size_ - 0x10000000;
  }
  uint32_t high_address = heap_base_ + (heap_size_ - 1);
  return AllocRange(low_address, high_address, size, alignment, allocation_type, protect, top_down,
                    out_address);
}

bool BaseHeap::AllocFixed(uint32_t base_address, uint32_t size, uint32_t alignment,
                          uint32_t allocation_type, uint32_t protect) {
  auto snapshot_mutation_lock =
      (memory_ && (heap_type_==HeapType::kGuestPhysical || heap_type_==HeapType::kHostPhysical))
          ? memory_->AcquireSnapshotMutationLock() : std::unique_lock<std::recursive_mutex>{};
  alignment = rex::round_up(alignment, page_size_);
  if (base_address % alignment != 0) {
    if (base_address % page_size_ != 0) {
      REXSYS_ERROR(
          "BaseHeap::AllocFixed invalid base alignment: base={:08X} page_size={:08X} "
          "requested_alignment={:08X} heap={:08X}-{:08X}",
          base_address, page_size_, alignment, heap_base_, heap_base_ + (heap_size_ - 1));
      return false;
    }
    // Fixed allocations can only be guaranteed page-aligned. If callers provide
    // a stricter alignment for an already-fixed address, fall back to page size.
    REXSYS_WARN(
        "BaseHeap::AllocFixed clamping alignment from {:08X} to page size {:08X} for "
        "base={:08X}",
        alignment, page_size_, base_address);
    alignment = page_size_;
  }
  size = rex::align(size, alignment);
  uint32_t page_count = get_page_count(size, page_size_, page_size_shift_);
  uint32_t start_page_number = (base_address - heap_base_) >> page_size_shift_;
  uint32_t end_page_number = start_page_number + page_count - 1;
  if (start_page_number >= page_table_.size() || end_page_number > page_table_.size()) {
    REXSYS_ERROR("BaseHeap::AllocFixed passed out of range address range");
    return false;
  }

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // - If we are reserving the entire range requested must not be already
  //   reserved.
  // - If we are committing it's ok for pages within the range to already be
  //   committed.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    uint32_t state = page_table_[page_number].state;
    if ((allocation_type == memory::kMemoryAllocationReserve) && state) {
      // Already reserved.
      REXSYS_ERROR(
          "BaseHeap::AllocFixed attempting to reserve an already reserved "
          "range");
      return false;
    }
    if ((allocation_type == memory::kMemoryAllocationCommit) &&
        !(state & memory::kMemoryAllocationReserve)) {
      // Attempting a commit-only op on an unreserved page.
      // This may be OK.
      REXSYS_WARN("BaseHeap::AllocFixed attempting commit on unreserved page");
      allocation_type |= memory::kMemoryAllocationReserve;
      break;
    }
  }

  const AccessMutation access_mutation(*this);
  InvalidateAllocationObservations(heap_base_+(start_page_number<<page_size_shift_),
                                   uint64_t(page_count)<<page_size_shift_);
  // Allocate from host.
  if (allocation_type == memory::kMemoryAllocationReserve) {
    // Reserve is not needed, as we are mapped already.
  } else {
    auto alloc_type = (allocation_type & memory::kMemoryAllocationCommit)
                          ? rex::memory::AllocationType::kCommit
                          : rex::memory::AllocationType::kReserve;
    void* result =
        memory_->AllocateHostMemory(TranslateRelative(start_page_number << page_size_shift_),
                                page_count << page_size_shift_, alloc_type, ToPageAccess(protect));
    if (!result) {
      REXSYS_ERROR("BaseHeap::AllocFixed failed to alloc range from host");
      return false;
    }

    if (REXCVAR_GET(scribble_heap) && protect & memory::kMemoryProtectWrite) {
      std::memset(result, 0xCD, page_count << page_size_shift_);
    }
  }

  memory_->InvalidateHostSnapshot(TranslateRelative(start_page_number << page_size_shift_),
                                  page_count << page_size_shift_);
  // Set page state.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    if (allocation_type & memory::kMemoryAllocationReserve) {
      if (!page_entry.state) {
        unreserved_page_count_--;
      }
      // Region is based on reservation.
      page_entry.base_address = start_page_number;
      page_entry.region_page_count = page_count;
    }
    page_entry.allocation_protect = protect;
    page_entry.current_protect = protect;
    page_entry.state = memory::kMemoryAllocationReserve | allocation_type;
  }

  return true;
}

bool BaseHeap::AllocRange(uint32_t low_address, uint32_t high_address, uint32_t size,
                          uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                          bool top_down, uint32_t* out_address) {
  auto snapshot_mutation_lock =
      (memory_ && (heap_type_==HeapType::kGuestPhysical || heap_type_==HeapType::kHostPhysical))
          ? memory_->AcquireSnapshotMutationLock() : std::unique_lock<std::recursive_mutex>{};
  *out_address = 0;

  alignment = rex::round_up(alignment, page_size_);
  uint32_t page_count = get_page_count(size, page_size_, page_size_shift_);
  low_address = std::max(heap_base_, rex::align(low_address, alignment));
  high_address = std::min(heap_base_ + (heap_size_ - 1), rex::align(high_address, alignment));
  if (high_address < low_address) {
    REXSYS_ERROR("BaseHeap::Alloc invalid requested range");
    return false;
  }
  uint32_t low_page_number = (low_address - heap_base_) >> page_size_shift_;
  uint32_t high_page_number = (high_address - heap_base_) >> page_size_shift_;
  low_page_number = std::min(uint32_t(page_table_.size()) - 1, low_page_number);
  high_page_number = std::min(uint32_t(page_table_.size()) - 1, high_page_number);
  if (high_page_number < low_page_number) {
    REXSYS_ERROR("BaseHeap::Alloc invalid requested page range");
    return false;
  }

  uint32_t available_page_count = high_page_number - low_page_number + 1;
  if (!page_count || page_count > available_page_count) {
    REXSYS_ERROR("BaseHeap::Alloc page count too big for requested range");
    return false;
  }

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Find a free page range.
  // The base page must match the requested alignment, so we first scan for
  // a free aligned page and only then check for continuous free pages.
  // TODO(benvanik): optimized searching (free list buckets, bitmap, etc).
  uint32_t start_page_number = UINT_MAX;
  uint32_t end_page_number = UINT_MAX;
  uint32_t page_scan_stride = alignment >> page_size_shift_;
  uint32_t max_base_page_number = high_page_number + 1 - page_count;
  if (top_down) {
    max_base_page_number -= max_base_page_number % page_scan_stride;
    for (int64_t base_page_number = max_base_page_number; base_page_number >= low_page_number;
         base_page_number -= page_scan_stride) {
      if (page_table_[base_page_number].state != 0) {
        // Base page not free, skip to next usable page.
        continue;
      }
      // Check requested range to ensure free.
      start_page_number = uint32_t(base_page_number);
      end_page_number = uint32_t(base_page_number) + page_count - 1;
      assert_true(end_page_number < page_table_.size());
      bool any_taken = false;
      for (uint32_t page_number = uint32_t(base_page_number);
           !any_taken && page_number <= end_page_number; ++page_number) {
        bool is_free = page_table_[page_number].state == 0;
        if (!is_free) {
          // At least one page in the range is used, skip to next.
          // We know we'll be starting at least before this page.
          any_taken = true;
          if (page_count > page_number) {
            // Not enough space left to fit entire page range. Breaks outer
            // loop.
            base_page_number = -1;
          } else {
            base_page_number = page_number - page_count;
            base_page_number -= base_page_number % page_scan_stride;
            base_page_number += page_scan_stride;  // cancel out loop logic
          }
          break;
        }
      }
      if (!any_taken) {
        // Found our place.
        break;
      }
      // Retry.
      start_page_number = end_page_number = UINT_MAX;
    }
  } else {
    for (uint32_t base_page_number = low_page_number; base_page_number <= max_base_page_number;
         base_page_number += page_scan_stride) {
      if (page_table_[base_page_number].state != 0) {
        // Base page not free, skip to next usable page.
        continue;
      }
      // Check requested range to ensure free.
      start_page_number = base_page_number;
      end_page_number = base_page_number + page_count - 1;
      bool any_taken = false;
      for (uint32_t page_number = base_page_number; !any_taken && page_number <= end_page_number;
           ++page_number) {
        bool is_free = page_table_[page_number].state == 0;
        if (!is_free) {
          // At least one page in the range is used, skip to next.
          // We know we'll be starting at least after this page.
          any_taken = true;
          base_page_number = rex::round_up(page_number + 1, page_scan_stride);
          base_page_number -= page_scan_stride;  // cancel out loop logic
          break;
        }
      }
      if (!any_taken) {
        // Found our place.
        break;
      }
      // Retry.
      start_page_number = end_page_number = UINT_MAX;
    }
  }
  if (start_page_number == UINT_MAX || end_page_number == UINT_MAX) {
    // Out of memory.
    REXSYS_ERROR("BaseHeap::Alloc failed to find contiguous range");
    assert_always("Heap exhausted!");
    return false;
  }

  const AccessMutation access_mutation(*this);
  InvalidateAllocationObservations(heap_base_+(start_page_number<<page_size_shift_),
                                   uint64_t(page_count)<<page_size_shift_);
  // Allocate from host.
  if (allocation_type == memory::kMemoryAllocationReserve) {
    // Reserve is not needed, as we are mapped already.
  } else {
    auto alloc_type = (allocation_type & memory::kMemoryAllocationCommit)
                          ? rex::memory::AllocationType::kCommit
                          : rex::memory::AllocationType::kReserve;
    void* result =
        memory_->AllocateHostMemory(TranslateRelative(start_page_number << page_size_shift_),
                                page_count << page_size_shift_, alloc_type, ToPageAccess(protect));
    if (!result) {
      REXSYS_ERROR("BaseHeap::Alloc failed to alloc range from host");
      return false;
    }

    if (REXCVAR_GET(scribble_heap) && (protect & memory::kMemoryProtectWrite)) {
      std::memset(result, 0xCD, page_count << page_size_shift_);
    }
  }

  memory_->InvalidateHostSnapshot(TranslateRelative(start_page_number << page_size_shift_),
                                  page_count << page_size_shift_);
  // Set page state.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    if (!page_entry.state) {
      unreserved_page_count_--;
    }
    page_entry.base_address = start_page_number;
    page_entry.region_page_count = page_count;
    page_entry.allocation_protect = protect;
    page_entry.current_protect = protect;
    page_entry.state = memory::kMemoryAllocationReserve | allocation_type;
  }

  *out_address = heap_base_ + (start_page_number << page_size_shift_);
  return true;
}

bool BaseHeap::Decommit(uint32_t address, uint32_t size) {
  auto snapshot_mutation_lock =
      (memory_ && (heap_type_==HeapType::kGuestPhysical || heap_type_==HeapType::kHostPhysical))
          ? memory_->AcquireSnapshotMutationLock() : std::unique_lock<std::recursive_mutex>{};
  uint32_t page_count = get_page_count(size, page_size_, page_size_shift_);
  uint32_t start_page_number = (address - heap_base_) >> page_size_shift_;
  uint32_t end_page_number = start_page_number + page_count - 1;
  start_page_number = std::min(uint32_t(page_table_.size()) - 1, start_page_number);
  end_page_number = std::min(uint32_t(page_table_.size()) - 1, end_page_number);

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  const AccessMutation access_mutation(*this);
  InvalidateAllocationObservations(heap_base_+(start_page_number<<page_size_shift_),
                                   uint64_t(end_page_number-start_page_number+1)<<page_size_shift_);
  // Release from host.
  // TODO(benvanik): find a way to actually decommit memory;
  //     mapped memory cannot be decommitted.
  /*BOOL result =
      VirtualFree(TranslateRelative(start_page_number << page_size_shift_),
                  page_count << page_size_shift_, MEM_DECOMMIT);
  if (!result) {
    PLOGW("BaseHeap::Decommit failed due to host VirtualFree failure");
    return false;
  }*/

  memory_->InvalidateHostSnapshot(TranslateRelative(start_page_number << page_size_shift_),
                                  page_count << page_size_shift_);
  // Perform table change.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    page_entry.state &= ~memory::kMemoryAllocationCommit;
  }

  return true;
}

bool BaseHeap::Release(uint32_t base_address, uint32_t* out_region_size) {
  auto snapshot_mutation_lock =
      (memory_ && (heap_type_==HeapType::kGuestPhysical || heap_type_==HeapType::kHostPhysical))
          ? memory_->AcquireSnapshotMutationLock() : std::unique_lock<std::recursive_mutex>{};
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Given address must be a region base address.
  uint32_t base_page_number = (base_address - heap_base_) >> page_size_shift_;
  auto base_page_entry = page_table_[base_page_number];
  if (base_page_entry.base_address != base_page_number) {
    REXSYS_ERROR("BaseHeap::Release failed because address is not a region start");
    return false;
  }

  if (heap_base_ == 0x00000000 && base_page_number == 0) {
    REXSYS_ERROR("BaseHeap::Release: Attempt to free 0!");
    return false;
  }

  if (out_region_size) {
    *out_region_size = (base_page_entry.region_page_count << page_size_shift_);
  }

  const AccessMutation access_mutation(*this);
  InvalidateAllocationObservations(base_address,uint64_t(base_page_entry.region_page_count)<<page_size_shift_);
  // Release from host not needed as mapping reserves the range for us.
  // TODO(benvanik): protect with NOACCESS?
  /*BOOL result = VirtualFree(
      TranslateRelative(base_page_number << page_size_shift_), 0, MEM_RELEASE);
  if (!result) {
    PLOGE("BaseHeap::Release failed due to host VirtualFree failure");
    return false;
  }*/
  // Instead, we just protect it, if we can.
  if (page_size_ == rex::memory::page_size() ||
      ((base_page_entry.region_page_count << page_size_shift_) % rex::memory::page_size() == 0 &&
       ((base_page_number << page_size_shift_) % rex::memory::page_size() == 0))) {
    // TODO(benvanik): figure out why games are using memory after releasing
    // it. It's possible this is some virtual/physical stuff where the GPU
    // still can access it.
    if (REXCVAR_GET(protect_on_release)) {
      if (!memory_->ProtectHostMemory(TranslateRelative(base_page_number << page_size_shift_),
                                base_page_entry.region_page_count << page_size_shift_,
                                rex::memory::PageAccess::kNoAccess, nullptr)) {
        REXSYS_WARN("BaseHeap::Release failed due to host VirtualProtect failure");
      }
    }
  }

  memory_->InvalidateHostSnapshot(TranslateRelative(base_page_number << page_size_shift_),
                                  base_page_entry.region_page_count << page_size_shift_);
  // Perform table change.
  uint32_t end_page_number = base_page_number + base_page_entry.region_page_count - 1;
  for (uint32_t page_number = base_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    page_entry.qword = 0;
    unreserved_page_count_++;
  }

  return true;
}

bool BaseHeap::Protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t* old_protect) {
  auto snapshot_mutation_lock =
      (memory_ && (heap_type_==HeapType::kGuestPhysical || heap_type_==HeapType::kHostPhysical))
          ? memory_->AcquireSnapshotMutationLock() : std::unique_lock<std::recursive_mutex>{};
  if (!size) {
    REXSYS_ERROR("BaseHeap::Protect failed due to zero size");
    return false;
  }

  // From the VirtualProtect MSDN page:
  //
  // "The region of affected pages includes all pages containing one or more
  //  bytes in the range from the lpAddress parameter to (lpAddress+dwSize).
  //  This means that a 2-byte range straddling a page boundary causes the
  //  protection attributes of both pages to be changed."
  //
  // "The access protection value can be set only on committed pages. If the
  //  state of any page in the specified region is not committed, the function
  //  fails and returns without modifying the access protection of any pages in
  //  the specified region."

  uint32_t start_page_number = (address - heap_base_) >> page_size_shift_;
  if (start_page_number >= page_table_.size()) {
    REXSYS_ERROR("BaseHeap::Protect failed due to out-of-bounds base address {:08X}", address);
    return false;
  }
  uint32_t end_page_number =
      uint32_t((uint64_t(address) + size - 1 - heap_base_) >> page_size_shift_);
  if (end_page_number >= page_table_.size()) {
    REXSYS_ERROR(
        "BaseHeap::Protect failed due to out-of-bounds range ({:08X} bytes "
        "from {:08x})",
        size, address);
    return false;
  }

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Ensure all pages are in the same reserved region and all are committed.
  uint32_t first_base_address = UINT_MAX;
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto page_entry = page_table_[page_number];
    if (first_base_address == UINT_MAX) {
      first_base_address = page_entry.base_address;
    } else if (first_base_address != page_entry.base_address) {
      REXSYS_ERROR("BaseHeap::Protect failed due to request spanning regions");
      return false;
    }
    if (!(page_entry.state & memory::kMemoryAllocationCommit)) {
      REXSYS_ERROR("BaseHeap::Protect failed due to uncommitted page");
      return false;
    }
  }

  const AccessMutation access_mutation(*this);
  InvalidateAllocationObservations(heap_base_+(start_page_number<<page_size_shift_),
                                   uint64_t(end_page_number-start_page_number+1)<<page_size_shift_);
  // Attempt host change (hopefully won't fail).
  // We can only do this if our size matches system page granularity.
  uint32_t page_count = end_page_number - start_page_number + 1;
  if (page_size_ == rex::memory::page_size() ||
      (((page_count << page_size_shift_) % rex::memory::page_size() == 0) &&
       ((start_page_number << page_size_shift_) % rex::memory::page_size() == 0))) {
    memory::PageAccess old_protect_access;
    if (!memory_->ProtectHostMemory(TranslateRelative(start_page_number << page_size_shift_),
                              page_count << page_size_shift_, ToPageAccess(protect),
                              old_protect ? &old_protect_access : nullptr)) {
      REXSYS_ERROR("BaseHeap::Protect failed due to host VirtualProtect failure");
      return false;
    }

    if (old_protect) {
      *old_protect = FromPageAccess(old_protect_access);
    }
  } else if (rex::memory::page_size() > page_size_) {
    // Host pages are larger than this heap's (16 KB on Apple silicon) and may
    // hold other allocations: the host keeps the range accessible, and only
    // the page table records the guest protection.
    if (old_protect) {
      *old_protect = page_table_[start_page_number].current_protect;
    }
  } else {
    REXSYS_WARN("BaseHeap::Protect: ignoring request as not 4k page aligned");
    return false;
  }

  // Perform table change.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    page_entry.current_protect = protect;
  }

  return true;
}

bool BaseHeap::QueryRegionInfo(uint32_t base_address, HeapAllocationInfo* out_info) {
  uint32_t start_page_number = (base_address - heap_base_) >> page_size_shift_;
  if (start_page_number > page_table_.size()) {
    REXSYS_ERROR("BaseHeap::QueryRegionInfo base page out of range");
    return false;
  }

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  auto start_page_entry = page_table_[start_page_number];
  out_info->base_address = base_address;
  out_info->allocation_base = 0;
  out_info->allocation_protect = 0;
  out_info->region_size = 0;
  out_info->state = 0;
  out_info->protect = 0;
  if (start_page_entry.state) {
    // Committed/reserved region.
    out_info->allocation_base = heap_base_ + (start_page_entry.base_address << page_size_shift_);
    out_info->allocation_protect = start_page_entry.allocation_protect;
    out_info->allocation_size = start_page_entry.region_page_count << page_size_shift_;
    out_info->state = start_page_entry.state;
    out_info->protect = start_page_entry.current_protect;

    // Scan forward and report the size of the region matching the initial
    // base address's attributes.
    for (uint32_t page_number = start_page_number;
         page_number < start_page_entry.base_address + start_page_entry.region_page_count;
         ++page_number) {
      auto page_entry = page_table_[page_number];
      if (page_entry.base_address != start_page_entry.base_address ||
          page_entry.state != start_page_entry.state ||
          page_entry.current_protect != start_page_entry.current_protect) {
        // Different region or different properties within the region; done.
        break;
      }
      out_info->region_size += page_size_;
    }
  } else {
    // Free region.
    for (uint32_t page_number = start_page_number; page_number < page_table_.size();
         ++page_number) {
      auto page_entry = page_table_[page_number];
      if (page_entry.state) {
        // First non-free page; done with region.
        break;
      }
      out_info->region_size += page_size_;
    }
  }
  return true;
}

bool BaseHeap::QuerySize(uint32_t address, uint32_t* out_size) {
  uint32_t page_number = (address - heap_base_) >> page_size_shift_;
  if (page_number > page_table_.size()) {
    REXSYS_ERROR("BaseHeap::QuerySize base page out of range");
    *out_size = 0;
    return false;
  }
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  auto page_entry = page_table_[page_number];
  *out_size = (page_entry.region_page_count << page_size_shift_);
  return true;
}

bool BaseHeap::QueryBaseAndSize(uint32_t* in_out_address, uint32_t* out_size) {
  uint32_t page_number = (*in_out_address - heap_base_) >> page_size_shift_;
  if (page_number > page_table_.size()) {
    REXSYS_ERROR("BaseHeap::QuerySize base page out of range");
    *out_size = 0;
    return false;
  }
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  auto page_entry = page_table_[page_number];
  *in_out_address = (page_entry.base_address << page_size_shift_);
  *out_size = (page_entry.region_page_count << page_size_shift_);
  return true;
}

bool BaseHeap::QueryProtect(uint32_t address, uint32_t* out_protect) {
  uint32_t page_number = (address - heap_base_) >> page_size_shift_;
  if (page_number > page_table_.size()) {
    REXSYS_ERROR("BaseHeap::QueryProtect base page out of range");
    *out_protect = 0;
    return false;
  }
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  auto page_entry = page_table_[page_number];
  *out_protect = page_entry.current_protect;
  return true;
}

bool BaseHeap::IsRangeCommittedReadable(uint32_t low_address,uint32_t high_address) {
  if(low_address>high_address || low_address<heap_base_ ||
     (high_address-heap_base_)>=heap_size_)return false;
  const uint32_t first=(low_address-heap_base_)>>page_size_shift_;
  const uint32_t last=(high_address-heap_base_)>>page_size_shift_;
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  for(auto page=first;page<=last;++page) {
    const auto& entry=page_table_[page];
    if(!(entry.state&kMemoryAllocationCommit) || !(entry.current_protect&kMemoryProtectRead))return false;
  }
  return true;
}

rex::memory::PageAccess BaseHeap::QueryRangeAccess(uint32_t low_address, uint32_t high_address) {
  if (low_address > high_address || low_address < heap_base_ ||
      (high_address - heap_base_) >= heap_size_) {
    return rex::memory::PageAccess::kNoAccess;
  }
  uint32_t low_page_number = (low_address - heap_base_) >> page_size_shift_;
  uint32_t high_page_number = (high_address - heap_base_) >> page_size_shift_;
  bool all_readable = true;
  bool all_writable = true;
  // Lock-free snapshot when current_protect lies in the entry's first aligned
  // word (checked once, independent of compiler bitfield layout). The table is
  // never reallocated after Initialize. Holding heap_mutex_ gave no stronger
  // guarantee: protection may change as soon as this returns. The renderer
  // queries this for every captured block, so the lock dominated its cost.
  static const bool protect_in_qword = [] {
    PageEntry written{};
    written.current_protect = 0xF;
    PageEntry read{};
    read.qword = written.qword;
    return read.current_protect == 0xF;
  }();
  // Renderer captures repeat the same texture and stream ranges every draw;
  // a multi-megabyte texture is a thousand-page walk. Results are reused only
  // while no mutation has started or finished since they were computed.
  struct CachedAccess {
    const BaseHeap* heap;
    uint32_t low, high;
    uint64_t generation;
    rex::memory::PageAccess access;
  };
  static thread_local std::array<CachedAccess, 512> cached_access{};
  const uint64_t generation = access_generation_.load();
  const bool stable = protect_in_qword && !access_mutations_.load();
  auto& cached = cached_access[uint32_t(((uint64_t(low_address) << 32 | high_address) * 0x9E3779B97F4A7C15ull) >> 55)];
  if (stable && cached.heap == this && cached.low == low_address &&
      cached.high == high_address && cached.generation == generation) {
    return cached.access;
  }
  std::unique_lock<std::recursive_mutex> heap_lock(heap_mutex_, std::defer_lock);
  if (!protect_in_qword) {
    heap_lock.lock();
  }
  for (uint32_t i = low_page_number; i <= high_page_number; ++i) {
    PageEntry entry{};
    if (protect_in_qword) {
      entry.qword = std::atomic_ref<uint64_t>(page_table_[i].qword).load(std::memory_order_acquire);
    } else {
      entry = page_table_[i];
    }
    uint32_t page_protect = entry.current_protect;
    if (!(page_protect & memory::kMemoryProtectRead)) {
      all_readable = false;
    }
    if (!(page_protect & memory::kMemoryProtectWrite) &&
        !(page_protect & memory::kMemoryProtectWriteCombine)) {
      all_writable = false;
    }
  }
  const auto access = all_readable && all_writable ? rex::memory::PageAccess::kReadWrite
                      : all_readable                ? rex::memory::PageAccess::kReadOnly
                                                    : rex::memory::PageAccess::kNoAccess;
  if (stable && !access_mutations_.load() && access_generation_.load() == generation) {
    cached = {this, low_address, high_address, generation, access};
  }
  return access;
}

VirtualHeap::VirtualHeap() = default;

VirtualHeap::~VirtualHeap() = default;

void VirtualHeap::Initialize(memory::Memory* memory, uint8_t* membase, HeapType heap_type,
                             uint32_t heap_base, uint32_t heap_size, uint32_t page_size) {
  BaseHeap::Initialize(memory, membase, heap_type, heap_base, heap_size, page_size);
}

PhysicalHeap::PhysicalHeap() : parent_heap_(nullptr) {}

PhysicalHeap::~PhysicalHeap() = default;

void PhysicalHeap::Initialize(memory::Memory* memory, uint8_t* membase, HeapType heap_type,
                              uint32_t heap_base, uint32_t heap_size, uint32_t page_size,
                              VirtualHeap* parent_heap) {
  uint32_t host_address_offset;
  if (heap_base >= 0xE0000000 && rex::memory::allocation_granularity() > 0x1000) {
    host_address_offset = 0x1000;
  } else {
    host_address_offset = 0;
  }

  BaseHeap::Initialize(memory, membase, heap_type, heap_base, heap_size, page_size,
                       host_address_offset);
  parent_heap_ = parent_heap;
  system_page_size_ = uint32_t(rex::memory::page_size());

  system_page_count_ =
      (size_t(heap_size_) + host_address_offset + (system_page_size_ - 1)) / system_page_size_;
  system_page_flags_.resize((system_page_count_ + 63) / 64);
}

bool PhysicalHeap::Alloc(uint32_t size, uint32_t alignment, uint32_t allocation_type,
                         uint32_t protect, bool top_down, uint32_t* out_address) {
  auto snapshot_mutation_lock = memory_->AcquireSnapshotMutationLock();
  *out_address = 0;

  // Default top-down. Since parent heap is bottom-up this prevents
  // collisions.
  top_down = true;

  // Adjust alignment size our page size differs from the parent.
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Allocate from parent heap (gets our physical address in 0-512mb).
  uint32_t parent_heap_start = GetPhysicalAddress(heap_base_);
  uint32_t parent_heap_end = GetPhysicalAddress(heap_base_ + (heap_size_ - 1));
  uint32_t parent_address;
  if (!parent_heap_->AllocRange(parent_heap_start, parent_heap_end, size, alignment,
                                allocation_type, protect, top_down, &parent_address)) {
    REXSYS_ERROR("PhysicalHeap::Alloc unable to alloc physical memory in parent heap");
    return false;
  }

  // Given the address we've reserved in the parent heap, pin that here.
  // Shouldn't be possible for it to be allocated already.
  uint32_t address = heap_base_ + parent_address - parent_heap_start;
  if (!BaseHeap::AllocFixed(address, size, alignment, allocation_type, protect)) {
    REXSYS_ERROR("PhysicalHeap::Alloc unable to pin physical memory in physical heap");
    // TODO(benvanik): don't leak parent memory.
    return false;
  }
  *out_address = address;
  return true;
}

bool PhysicalHeap::AllocFixed(uint32_t base_address, uint32_t size, uint32_t alignment,
                              uint32_t allocation_type, uint32_t protect) {
  auto snapshot_mutation_lock = memory_->AcquireSnapshotMutationLock();
  // Adjust alignment size our page size differs from the parent.
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Allocate from parent heap (gets our physical address in 0-512mb).
  // NOTE: this can potentially overwrite heap contents if there are already
  // committed pages in the requested physical range.
  // TODO(benvanik): flag for ensure-not-committed?
  uint32_t parent_base_address = GetPhysicalAddress(base_address);
  if (!parent_heap_->AllocFixed(parent_base_address, size, alignment, allocation_type, protect)) {
    REXSYS_ERROR("PhysicalHeap::Alloc unable to alloc physical memory in parent heap");
    return false;
  }

  // Given the address we've reserved in the parent heap, pin that here.
  // Shouldn't be possible for it to be allocated already.
  uint32_t address = heap_base_ + parent_base_address - GetPhysicalAddress(heap_base_);
  if (!BaseHeap::AllocFixed(address, size, page_size_, allocation_type, protect)) {
    REXSYS_ERROR("PhysicalHeap::Alloc unable to pin physical memory in physical heap");
    // TODO(benvanik): don't leak parent memory.
    return false;
  }

  return true;
}

bool PhysicalHeap::AllocRange(uint32_t low_address, uint32_t high_address, uint32_t size,
                              uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                              bool top_down, uint32_t* out_address) {
  auto snapshot_mutation_lock = memory_->AcquireSnapshotMutationLock();
  *out_address = 0;

  // Adjust alignment size our page size differs from the parent.
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Allocate from parent heap (gets our physical address in 0-512mb).
  low_address = std::max(heap_base_, low_address);
  high_address = std::min(heap_base_ + (heap_size_ - 1), high_address);
  uint32_t parent_low_address = GetPhysicalAddress(low_address);
  uint32_t parent_high_address = GetPhysicalAddress(high_address);
  uint32_t parent_address;
  if (!parent_heap_->AllocRange(parent_low_address, parent_high_address, size, alignment,
                                allocation_type, protect, top_down, &parent_address)) {
    REXSYS_ERROR("PhysicalHeap::Alloc unable to alloc physical memory in parent heap");
    return false;
  }

  // Given the address we've reserved in the parent heap, pin that here.
  // Shouldn't be possible for it to be allocated already.
  uint32_t address = heap_base_ + parent_address - GetPhysicalAddress(heap_base_);
  if (!BaseHeap::AllocFixed(address, size, page_size_, allocation_type, protect)) {
    REXSYS_ERROR("PhysicalHeap::Alloc unable to pin physical memory in physical heap");
    // TODO(benvanik): don't leak parent memory.
    return false;
  }
  *out_address = address;
  return true;
}

bool PhysicalHeap::AllocSystemHeap(uint32_t size, uint32_t alignment, uint32_t allocation_type,
                                   uint32_t protect, bool top_down, uint32_t* out_address) {
  return Alloc(size, alignment, allocation_type, protect, top_down, out_address);
}

bool PhysicalHeap::Decommit(uint32_t address, uint32_t size) {
  auto global_lock = global_critical_region_.Acquire();

  uint32_t parent_address = GetPhysicalAddress(address);
  if (!parent_heap_->Decommit(parent_address, size)) {
    REXSYS_ERROR("PhysicalHeap::Decommit failed due to parent heap failure");
    return false;
  }

  // Not caring about the contents anymore.
  TriggerCallbacks(std::move(global_lock), address, size, true, true);

  return BaseHeap::Decommit(address, size);
}

bool PhysicalHeap::Release(uint32_t base_address, uint32_t* out_region_size) {
  auto global_lock = global_critical_region_.Acquire();

  uint32_t parent_base_address = GetPhysicalAddress(base_address);
  if (!parent_heap_->Release(parent_base_address, out_region_size)) {
    REXSYS_ERROR("PhysicalHeap::Release failed due to parent heap failure");
    return false;
  }

  // Must invalidate here because the range being released may be reused in
  // another mapping of physical memory - but callback flags are set in each
  // heap separately (https://github.com/xenia-project/xenia/issues/1559 -
  // dynamic vertices in 4D5307F2 start screen and menu allocated in 0xA0000000
  // at addresses that overlap intro video textures in 0xE0000000, with the
  // state of the allocator as of February 24th, 2020). If memory is invalidated
  // in Alloc instead, Alloc won't be aware of callbacks enabled in other heaps,
  // thus callback handlers will keep considering this range valid forever.
  uint32_t region_size;
  if (QuerySize(base_address, &region_size)) {
    TriggerCallbacks(std::move(global_lock), base_address, region_size, true, true);
  }

  return BaseHeap::Release(base_address, out_region_size);
}

bool PhysicalHeap::Protect(uint32_t address, uint32_t size, uint32_t protect,
                           uint32_t* old_protect) {
  auto global_lock = global_critical_region_.Acquire();

  // Only invalidate if making writable again, for simplicity - not when simply
  // marking some range as immutable, for instance.
  if (protect & memory::kMemoryProtectWrite) {
    TriggerCallbacks(std::move(global_lock), address, size, true, true, false);
  }

  if (!parent_heap_->Protect(GetPhysicalAddress(address), size, protect, old_protect)) {
    REXSYS_ERROR("PhysicalHeap::Protect failed due to parent heap failure");
    return false;
  }

  return BaseHeap::Protect(address, size, protect);
}

void PhysicalHeap::EnableAccessCallbacks(uint32_t address,uint32_t length,
                                         bool notifications,bool providers) {
  (void)TryEnableAccessCallbacks(address,length,notifications,providers);
}

bool PhysicalHeap::TryEnableAccessCallbacks(uint32_t physical_address,uint32_t length,
                                            bool enable_invalidation_notifications,
                                            bool enable_data_providers) {
  // TODO(Triang3l): Implement data providers.
  assert_false(enable_data_providers);
  if(enable_data_providers)return false;
  if (!enable_invalidation_notifications && !enable_data_providers) {
    return true;
  }
  uint32_t physical_address_offset = GetPhysicalAddress(heap_base_);
  if (physical_address < physical_address_offset) {
    if (physical_address_offset - physical_address >= length) {
      return true;
    }
    length -= physical_address_offset - physical_address;
    physical_address = physical_address_offset;
  }
  uint32_t heap_relative_address = physical_address - physical_address_offset;
  if (heap_relative_address >= heap_size_) {
    return true;
  }
  length = std::min(length, heap_size_ - heap_relative_address);
  if (length == 0) {
    return true;
  }

  uint32_t system_page_first = (heap_relative_address + host_address_offset()) / system_page_size_;
  uint32_t system_page_last =
      (heap_relative_address + length - 1 + host_address_offset()) / system_page_size_;
  system_page_last = std::min(system_page_last, system_page_count_ - 1);
  assert_true(system_page_first <= system_page_last);

  // Update callback flags for system pages and make their protection stricter
  // if needed.
  rex::memory::PageAccess protect_access = enable_data_providers
                                               ? rex::memory::PageAccess::kNoAccess
                                               : rex::memory::PageAccess::kReadOnly;
  uint8_t* protect_base = membase_ + heap_base_;
  uint32_t protect_system_page_first = UINT32_MAX;
  auto global_lock = global_critical_region_.Acquire();
  const auto protect_range=[&](uint32_t first,uint32_t end) {
    const bool armed=memory_->ProtectHostMemory(protect_base+first*system_page_size_,
                                                (end-first)*system_page_size_,protect_access);
    // Failed OS protection may have made an earlier part of the run RO. Retain
    // notification ownership so those stores can recover, while preventing the
    // uncertain bits from satisfying any later arm without fresh OS protection.
    for(uint32_t page=first;page<end;++page) {
      auto& unconfirmed=system_page_flags_[page>>6].unconfirmed_write_protection;
      const auto mask=uint64_t{1}<<(page&63);
      if(armed)unconfirmed&=~mask;else unconfirmed|=mask;
    }
    return armed;
  };
  for (uint32_t i = system_page_first; i <= system_page_last; ++i) {
    // Check if need to enable callbacks for the page and raise its protection.
    //
    // If enabling invalidation notifications:
    // - Page writable and not watched for changes yet - protect and enable
    //   invalidation notifications.
    // - Page seen as writable by the guest, but only needs data providers -
    //   just set the bits to enable invalidation notifications (already has
    //   even stricter protection than needed).
    // - Page not writable as requested by the game - don't do anything (need
    //   real access violations here).
    // If enabling data providers:
    // - Page accessible (either read/write or read-only) and didn't need data
    //   providers initially - protect and enable data providers.
    // - Otherwise - do nothing.
    //
    // It's safe not to await data provider completion here before protecting as
    // this never makes protection lighter, so it can't interfere with page
    // faults that await data providers.
    //
    // Enabling data providers doesn't need to be deferred - providers will be
    // polled for the last time without releasing the lock.
    SystemPageFlagsBlock& page_flags_block = system_page_flags_[i >> 6];
    uint64_t page_flags_bit = uint64_t(1) << (i & 63);
    uint32_t guest_page_number =
        rex::sat_sub(i * system_page_size_, host_address_offset()) >> page_size_shift_;
    rex::memory::PageAccess current_page_access =
        ToPageAccess(page_table_[guest_page_number].current_protect);
    bool protect_system_page = false;
    // Don't do anything with inaccessible pages - don't protect, don't enable
    // callbacks - because real access violations are needed there. And don't
    // enable invalidation notifications for read-only pages for the same
    // reason.
    if (current_page_access != rex::memory::PageAccess::kNoAccess) {
      // TODO(Triang3l): Enable data providers.
      if (enable_invalidation_notifications) {
        if (current_page_access != rex::memory::PageAccess::kReadOnly &&
            ((page_flags_block.notify_on_invalidation & page_flags_bit) == 0 ||
             (page_flags_block.unconfirmed_write_protection & page_flags_bit) != 0)) {
          // TODO(Triang3l): Check if data providers are already enabled.
          // If data providers are already enabled for the page, it has even
          // stricter protection.
          protect_system_page = true;
          page_flags_block.notify_on_invalidation |= page_flags_bit;
        }
      }
    }
    if (protect_system_page) {
      if (protect_system_page_first == UINT32_MAX) {
        protect_system_page_first = i;
      }
    } else {
      if (protect_system_page_first != UINT32_MAX) {
        if(!protect_range(protect_system_page_first,i))return false;
        protect_system_page_first = UINT32_MAX;
      }
    }
  }
  if (protect_system_page_first != UINT32_MAX) {
    if(!protect_range(protect_system_page_first,system_page_last+1))return false;
  }
  return true;
}

bool PhysicalHeap::TriggerCallbacks(std::unique_lock<std::recursive_mutex> global_lock_locked_once,
                                    uint32_t virtual_address, uint32_t length, bool is_write,
                                    bool unwatch_exact_range, bool unprotect) {
  // TODO(Triang3l): Support read watches.
  assert_true(is_write);
  if (!is_write) {
    return false;
  }

  if (virtual_address < heap_base_) {
    if (heap_base_ - virtual_address >= length) {
      return false;
    }
    length -= heap_base_ - virtual_address;
    virtual_address = heap_base_;
  }
  uint32_t heap_relative_address = virtual_address - heap_base_;
  if (heap_relative_address >= heap_size_) {
    return false;
  }
  length = std::min(length, heap_size_ - heap_relative_address);
  if (length == 0) {
    return false;
  }

  uint32_t system_page_first = (heap_relative_address + host_address_offset()) / system_page_size_;
  uint32_t system_page_last =
      (heap_relative_address + length - 1 + host_address_offset()) / system_page_size_;
  system_page_last = std::min(system_page_last, system_page_count_ - 1);
  assert_true(system_page_first <= system_page_last);
  uint32_t block_index_first = system_page_first >> 6;
  uint32_t block_index_last = system_page_last >> 6;

  // Check if watching any page, whether need to call the callback at all.
  bool any_watched = false;
  for (uint32_t i = block_index_first; i <= block_index_last; ++i) {
    uint64_t block = system_page_flags_[i].notify_on_invalidation;
    if (i == block_index_first) {
      block &= ~((uint64_t(1) << (system_page_first & 63)) - 1);
    }
    if (i == block_index_last && (system_page_last & 63) != 63) {
      block &= (uint64_t(1) << ((system_page_last & 63) + 1)) - 1;
    }
    if (block) {
      any_watched = true;
      break;
    }
  }
  if (!any_watched) {
    return false;
  }

  // Trigger callbacks.
  if (!unprotect) {
    // If not doing anything with protection, no point in unwatching excess
    // pages.
    unwatch_exact_range = true;
  }
  uint32_t physical_address_offset = GetPhysicalAddress(heap_base_);
  uint32_t physical_address_start =
      rex::sat_sub(system_page_first * system_page_size_, host_address_offset()) +
      physical_address_offset;
  uint32_t physical_length =
      std::min(rex::sat_sub(system_page_last * system_page_size_ + system_page_size_,
                            host_address_offset()) +
                   physical_address_offset - physical_address_start,
               heap_size_ - (physical_address_start - physical_address_offset));
  uint32_t unwatch_first = 0;
  uint32_t unwatch_last = UINT32_MAX;
  for (auto invalidation_callback : memory_->physical_memory_invalidation_callbacks_) {
    std::pair<uint32_t, uint32_t> callback_unwatch_range =
        invalidation_callback->first(invalidation_callback->second, physical_address_start,
                                     physical_length, unwatch_exact_range);
    if (!unwatch_exact_range) {
      unwatch_first = std::max(unwatch_first, callback_unwatch_range.first);
      unwatch_last = std::min(
          unwatch_last, rex::sat_add(callback_unwatch_range.first,
                                     std::max(callback_unwatch_range.second, uint32_t(1)) - 1));
    }
  }
  if (!unwatch_exact_range) {
    // Always unwatch at least the requested pages.
    unwatch_first = std::min(unwatch_first, physical_address_start);
    unwatch_last = std::max(unwatch_last, physical_address_start + physical_length - 1);
    // Don't unprotect too much if not caring much about the region (limit to
    // 4 MB - somewhat random, but max 1024 iterations of the page loop).
    const uint32_t kMaxUnwatchExcess = 4 * 1024 * 1024;
    unwatch_first = std::max(unwatch_first, physical_address_start & ~(kMaxUnwatchExcess - 1));
    unwatch_last = std::min(
        unwatch_last, (physical_address_start + physical_length - 1) | (kMaxUnwatchExcess - 1));
    // Convert to heap-relative addresses.
    unwatch_first = rex::sat_sub(unwatch_first, physical_address_offset);
    unwatch_last = rex::sat_sub(unwatch_last, physical_address_offset);
    // Clamp to the heap upper bound.
    unwatch_first = std::min(unwatch_first, heap_size_ - 1);
    unwatch_last = std::min(unwatch_last, heap_size_ - 1);
    // Convert to system pages and update the range.
    unwatch_first += host_address_offset();
    unwatch_last += host_address_offset();
    assert_true(unwatch_first <= unwatch_last);
    system_page_first = unwatch_first / system_page_size_;
    system_page_last = unwatch_last / system_page_size_;
    block_index_first = system_page_first >> 6;
    block_index_last = system_page_last >> 6;
  }

  const auto unprotect_failed=[&] {
    // Earlier subranges may already be RW and later ones may still be RO.
    // Keep fault-repair ownership, but never let these bits certify a later
    // snapshot arm without actually re-protecting every uncertain page.
    for(uint32_t page=system_page_first;page<=system_page_last;++page) {
      auto& flags=system_page_flags_[page>>6];
      flags.unconfirmed_write_protection|=flags.notify_on_invalidation&(uint64_t{1}<<(page&63));
    }
    return false;
  };

  // Unprotect ranges that need unprotection.
  if (unprotect) {
    uint8_t* protect_base = membase_ + heap_base_;
    uint32_t unprotect_system_page_first = UINT32_MAX;
    for (uint32_t i = system_page_first; i <= system_page_last; ++i) {
      // Check if need to allow writing to this page.
      bool unprotect_page =
          (system_page_flags_[i >> 6].notify_on_invalidation & (uint64_t(1) << (i & 63))) != 0;
      if (unprotect_page) {
        uint32_t guest_page_number =
            rex::sat_sub(i * system_page_size_, host_address_offset()) >> page_size_shift_;
        if (ToPageAccess(page_table_[guest_page_number].current_protect) !=
            rex::memory::PageAccess::kReadWrite) {
          unprotect_page = false;
        }
      }
      if (unprotect_page) {
        if (unprotect_system_page_first == UINT32_MAX) {
          unprotect_system_page_first = i;
        }
      } else {
        if (unprotect_system_page_first != UINT32_MAX) {
          if(!memory_->ProtectHostMemory(protect_base + unprotect_system_page_first * system_page_size_,
                               (i - unprotect_system_page_first) * system_page_size_,
                               rex::memory::PageAccess::kReadWrite))return unprotect_failed();
          unprotect_system_page_first = UINT32_MAX;
        }
      }
    }
    if (unprotect_system_page_first != UINT32_MAX) {
      if(!memory_->ProtectHostMemory(protect_base + unprotect_system_page_first * system_page_size_,
                           (system_page_last + 1 - unprotect_system_page_first) * system_page_size_,
                           rex::memory::PageAccess::kReadWrite))return unprotect_failed();
    }
  }

  // Mark pages as not write-watched.
  for (uint32_t i = block_index_first; i <= block_index_last; ++i) {
    uint64_t mask = 0;
    if (i == block_index_first) {
      mask |= (uint64_t(1) << (system_page_first & 63)) - 1;
    }
    if (i == block_index_last && (system_page_last & 63) != 63) {
      mask |= ~((uint64_t(1) << ((system_page_last & 63) + 1)) - 1);
    }
    system_page_flags_[i].notify_on_invalidation &= mask;
    system_page_flags_[i].unconfirmed_write_protection &= mask;
  }

  return true;
}

uint32_t PhysicalHeap::GetPhysicalAddress(uint32_t address) const {
  assert_true(address >= heap_base_);
  address -= heap_base_;
  assert_true(address < heap_size_);
  if (heap_base_ >= 0xE0000000) {
    address += 0x1000;
  }
  return address;
}

}  // namespace rex::memory
