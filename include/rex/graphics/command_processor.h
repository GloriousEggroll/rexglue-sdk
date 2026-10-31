/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/atomic_shared_ptr.h>
#include <rex/graphics/native_completion_markers.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/trace_writer.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>

#include <rex/graphics/shader_storage_progress.h>
#include <rex/graphics/frame_limiter.h>

namespace rex::stream {
class ByteStream;
}  // namespace rex::stream

namespace rex::graphics {
// Optional CPU API frame marker carried in the host-generated swap packet.
// Thread-local scope prevents another guest thread from changing its identity.
uint32_t ExchangeApiFrameMarker(uint32_t frame);
uint32_t ApiFrameMarker();

class GraphicsSystem;
class Shader;

enum class ReadbackResolveMode {
  kDisabled,
  kFast,
  kSome,
  kFull,
};

struct SwapState {
  // Lock must be held when changing data in this structure.
  std::mutex mutex;
  // Dimensions of the framebuffer textures. Should match window size.
  uint32_t width = 0;
  uint32_t height = 0;
  // Current front buffer, being drawn to the screen.
  uintptr_t front_buffer_texture = 0;
  // Current back buffer, being updated by the CP.
  uintptr_t back_buffer_texture = 0;
  // Backend data
  void* backend_data = nullptr;
  // Whether the back buffer is dirty and a swap is pending.
  bool pending = false;
};

enum class SwapMode {
  kNormal,
  kIgnored,
};

enum class GammaRampType {
  kUnknown = 0,
  kTable,
  kPWL,
};

class CommandProcessor {
  ShaderStorageProgress shader_storage_progress_;
  FrameLimiter frame_limiter_;
  // Set while the application paces frames on the guest thread instead.
  std::atomic<bool> guest_thread_frame_limiter_{false};

 public:
  enum class SwapPostEffect {
    kNone,
    kFxaa,
    kFxaaExtreme,
  };

  CommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  virtual ~CommandProcessor();

  uint32_t counter() const { return counter_; }
  void increment_counter() { counter_++; }

  ShaderStorageProgress& shader_storage_progress() { return shader_storage_progress_; }

  Shader* active_vertex_shader() const { return active_vertex_shader_; }
  Shader* active_pixel_shader() const { return active_pixel_shader_; }

  virtual bool Initialize();
  virtual void Shutdown();

  void CallInThread(std::function<void()> fn);

  // Direct execution (no PM4 ring): the guest's D3D runs GPU-less and the
  // native renderer posts its work here. Thread-safe; runs in order on the
  // worker thread and wakes it when idle.
  void PostDirectWork(std::function<void()> fn);
  // Installed once before direct work is posted. Called on the worker thread
  // while it has no work; returns true while GPU work is still outstanding,
  // so the worker keeps polling instead of sleeping.
  void SetDirectIdleHook(std::function<bool()> hook);
  bool direct_execution() const { return direct_idle_hook_.load() != nullptr; }
  // Direct execution: present a guest VdSwap without an XE_SWAP packet. Posts
  // the swap behind all previously posted work (frame limiter, IssueSwap).
  void PostDirectSwap(uint32_t frontbuffer_ptr, uint32_t width, uint32_t height, uint32_t api_frame);
  // Optional: receives each direct swap's work item instead of posting it, so
  // a renderer that plans asynchronously can post it after the frame's own
  // work. Returning false posts the item immediately as usual.
  using DirectSwapRouter = std::function<bool(std::function<void()>&)>;
  void SetDirectSwapRouter(DirectSwapRouter router);
  // Low-latency pacing: the application applies framerate_limit on the guest
  // thread before the next frame samples input, so the GPU thread does not
  // also hold finished frames back.
  // Anti-lag markers (VK_AMD_anti_lag on Vulkan): `present == false` before
  // the frame reads input, true before it is presented. No-op by default.
  virtual void UpdateAntiLag(bool present, uint64_t frame_index) {}
  void SetGuestThreadFrameLimiter(bool enabled) {
    guest_thread_frame_limiter_.store(enabled, std::memory_order_relaxed);
  }

  virtual void ClearCaches();
  virtual void InvalidateGpuMemory();

  // "Desired" is for the external thread managing the post-processing effect.
  SwapPostEffect GetDesiredSwapPostEffect() const { return swap_post_effect_desired_; }
  void SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect);
  // Implementations must not make assumptions that the front buffer will
  // necessarily be a resolve destination - it may be a texture generated by any
  // means like written to by the CPU or loaded from a file (the disclaimer
  // screen right in the beginning of 4D530AA4 is not a resolved render target,
  // for instance).
  virtual void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                         uint32_t frontbuffer_height) = 0;

  // May be called not only from the command processor thread when the command
  // processor is paused, and the termination of this function may be explicitly
  // awaited.
  virtual void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                                       bool blocking);

  virtual void RequestFrameTrace(const std::filesystem::path& root_path);
  // Zero-based swap sequence, for pairing a CPU API capture with the exact
  // preceding GPU state. A past request fails instead of capturing another frame.
  void RequestFrameTraceAtFrame(const std::filesystem::path& root_path,uint32_t frame);
  virtual void BeginTracing(const std::filesystem::path& root_path);
  virtual void EndTracing();
  // Diagnostic replay inspection only, called on the command processor thread.
  // Requires a new directory; writes a state-only trace with explicit provenance.
  bool ExportDiagnosticSnapshot(const std::filesystem::path& directory,uint32_t title_id);

  virtual void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) = 0;

  void RestoreRegisters(uint32_t first_register, const uint32_t* register_values,
                        uint32_t register_count, bool execute_callbacks);
  void RestoreGammaRamp(const reg::DC_LUT_30_COLOR* new_gamma_ramp_256_entry_table,
                        const reg::DC_LUT_PWL_DATA* new_gamma_ramp_pwl_rgb,
                        uint32_t new_gamma_ramp_rw_component);
  virtual void RestoreEdramSnapshot(const void* snapshot) = 0;

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);

  void UpdateWritePointer(uint32_t value);

  void ExecutePacket(uint32_t ptr, uint32_t count);

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

  bool Save(::rex::stream::ByteStream* stream);
  bool Restore(::rex::stream::ByteStream* stream);

 protected:
  // Opt-in immutable identity of the PM4 draw currently being decoded. The
  // derived backend can validate native ownership without trusting FIFO draw
  // counts or addresses that may have been reused by the guest.
  bool capture_native_draw_packets_ = false;
  uint32_t native_draw_packet_address_ = 0;
  std::vector<uint8_t> native_draw_packet_bytes_;
  struct IndexBufferInfo {
    xenos::IndexFormat format = xenos::IndexFormat::kInt16;
    xenos::Endian endianness = xenos::Endian::kNone;
    uint32_t count = 0;
    uint32_t guest_base = 0;
    size_t length = 0;
  };

  void WorkerThreadMain();
  virtual bool SetupContext() = 0;
  virtual void ShutdownContext() = 0;

  virtual void WriteRegister(uint32_t index, uint32_t value);
  uint32_t ReadRegisterValue(uint32_t index) const;
  virtual void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  virtual void WriteRegisterRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                          uint32_t num_registers);
  void WriteALURangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteFetchRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteBoolRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteLoopRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteALURangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  void WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  void WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  void WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  void WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);

  const reg::DC_LUT_30_COLOR* gamma_ramp_256_entry_table() const {
    return gamma_ramp_256_entry_table_;
  }
  const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl_rgb() const { return gamma_ramp_pwl_rgb_[0]; }
  virtual void OnGammaRamp256EntryTableValueWritten() {}
  virtual void OnGammaRampPWLValueWritten() {}

  virtual void MakeCoherent();
  virtual void PrepareForWait();
  virtual void ReturnFromWait();
  // Executed on the command thread after predicate checks, before a type-3
  // packet can observe or publish effects of preceding native GPU work.
  // Strict scene backends reject unsupported or failed work before the decoder
  // drops a draw, fabricates a query, or advances a failed packet boundary.
  virtual void OnPacketExecutionFailure(const char* reason) {}
  virtual void PrepareForPacket(uint32_t opcode, uint32_t count) {}
  // Narrow known title pair only; the original scalar decoder remains the
  // fallback. A returned guard is released before the next packet is decoded.
  bool native_immediate_marker_pairs_enabled_=false;
  virtual std::shared_ptr<void> PrepareNativeImmediateMarkerPair(
      const NativeCompletionMarkerPair&) {return {};}
  bool TryExecuteNativeImmediateMarkerPair(memory::RingBuffer* reader);
  void WriteEventShdValue(uint32_t initiator,uint32_t address,uint32_t value);

  // Observation after the original pre-packet completion and operand reads,
  // before the existing register/store effects. No callback can alter decoding.
  bool observe_decoded_event_write_shd_=false;
  virtual void ObserveDecodedEventWriteShd(uint32_t packet,uint32_t count,
      uint32_t initiator,uint32_t address,uint32_t value,uintptr_t command_buffer,
      uint32_t capacity,uint32_t end_offset) noexcept {}
  // Decoded operands are copied before this hook. It runs before the first
  // memory/register read, MakeCoherent effect or predicate evaluation.
  virtual void PrepareForRegisterWait(uint32_t count, uint32_t info, uint32_t address,
                                     uint32_t reference, uint32_t mask, uint32_t delay) {}

  uint32_t ExecutePrimaryBuffer(uint32_t start_index, uint32_t end_index);
  virtual void OnPrimaryBufferEnd() {}
  void ExecuteIndirectBuffer(uint32_t ptr, uint32_t length);
  bool ExecutePacket(memory::RingBuffer* reader);
  bool ExecutePacketType0(memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType1(memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType2(memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType3(memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_NOP(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count);
  bool ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_REG_RMW(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count);
  bool ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count);
  virtual bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count);
  bool ExecutePacketType3Draw(memory::RingBuffer* reader, uint32_t packet, const char* opcode_name,
                              uint32_t viz_query_condition, uint32_t count_remaining);
  bool ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader, uint32_t packet,
                                        uint32_t count);
  bool ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader, uint32_t packet,
                                            uint32_t count);
  bool ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader, uint32_t packet,
                                               uint32_t count);
  bool ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,

                                            uint32_t packet, uint32_t count);
  bool ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader, uint32_t packet,
                                           uint32_t count);
  bool ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, uint32_t packet, uint32_t count);

  virtual Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                             const uint32_t* host_address, uint32_t dword_count) = 0;

  virtual bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                         IndexBufferInfo* index_buffer_info, bool major_mode_explicit) = 0;
  virtual bool IssueCopy() = 0;

  void TraceDrawState(xenos::PrimitiveType prim_type, uint32_t index_count,
                      const IndexBufferInfo* index_buffer_info);

  // "Actual" is for the command processor thread, to be read by the
  // implementations.
  SwapPostEffect GetActualSwapPostEffect() const { return swap_post_effect_actual_; }

  virtual void InitializeTrace();

  // Shared readback resolve mode with backend legacy-flag alias support.
  ReadbackResolveMode GetReadbackResolveMode(bool legacy_readback_resolve_enabled) const;
  // Shared memexport readback enable state with backend legacy-flag override support.
  bool IsReadbackMemexportEnabled(bool legacy_backend_flag) const;

  void RecordDrawSubmitted() { ++command_summary_.draws_submitted; }
  void RecordDrawSkippedNoEffect() { ++command_summary_.draws_no_effect; }
  void RecordDrawSkippedSurfacePitchZero() {
    ++command_summary_.draws_no_effect;
    ++command_summary_.draws_no_effect_surface_pitch_zero;
  }
  void RecordDrawSkippedNoRasterizationOrMemexport(
      bool edram_mode, bool vertex_multipass, bool surface_pitch_zero,
      bool cull_both) {
    ++command_summary_.draws_no_effect;
    ++command_summary_.draws_no_effect_no_rasterization_or_memexport;
    command_summary_.draws_no_effect_edram_mode += edram_mode;
    command_summary_.draws_no_effect_vertex_multipass += vertex_multipass;
    command_summary_.draws_no_effect_surface_pitch_zero += surface_pitch_zero;
    command_summary_.draws_no_effect_cull_both += cull_both;
  }
  void RecordDrawSkippedZeroVertices() {
    ++command_summary_.draws_no_effect;
    ++command_summary_.draws_no_effect_zero_vertices;
  }
  void RecordDrawSkippedAsync() { ++command_summary_.draws_async_placeholder; }
  void RecordCopyAttempt() { ++command_summary_.copy_attempts; }

  memory::Memory* memory_ = nullptr;
  system::KernelState* kernel_state_ = nullptr;
  GraphicsSystem* graphics_system_ = nullptr;
  RegisterFile* register_file_ = nullptr;

  TraceWriter trace_writer_;
  enum class TraceState {
    kDisabled,
    kStreaming,
    kSingleFrame,
  };
  TraceState trace_state_ = TraceState::kDisabled;
  std::filesystem::path trace_stream_path_;
  std::filesystem::path trace_frame_path_;
  uint32_t trace_requested_frame_ = UINT32_MAX;
  uint32_t trace_history_snapshots_left_ = 0;
  uint32_t external_frame_number_ = UINT32_MAX;

  std::atomic<bool> worker_running_;
  system::object_ref<system::XHostThread> worker_thread_;

  std::queue<std::function<void()>> pending_fns_;

  bool RunDirectWork();
  std::mutex direct_work_mutex_;
  std::vector<std::function<void()>> direct_work_;
  std::atomic<bool> direct_work_pending_{false};
  rex::AtomicSharedPtr<std::function<bool()>> direct_idle_hook_;
  rex::AtomicSharedPtr<DirectSwapRouter> direct_swap_router_;

  // MicroEngine binary from PM4_ME_INIT
  std::vector<uint32_t> me_bin_;

  uint32_t counter_ = 0;

  uint32_t primary_buffer_ptr_ = 0;
  uint32_t primary_buffer_size_ = 0;

  uint32_t read_ptr_index_ = 0;
  uint32_t read_ptr_update_freq_ = 0;
  uint32_t read_ptr_writeback_ptr_ = 0;

  std::unique_ptr<rex::thread::Event> write_ptr_index_event_;
  std::atomic<uint32_t> write_ptr_index_;

  // Some titles submit writes beyond the emulated register file range in PM4
  // packets. Preserve these values so dependent packet logic can still observe
  // them instead of dropping the write entirely.
  std::unordered_map<uint32_t, uint32_t> extended_register_values_;

  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint64_t bin_mask_ = 0xFFFFFFFFull;

  struct CommandSummary {
    uint64_t type0_constant_packets = 0, type0_constant_words = 0;
    uint64_t type0_bulk_packets = 0, type0_bulk_words = 0;
    bool movie_yuv = false;
    uint64_t type3_packets = 0;
    uint64_t indirect_buffers = 0;
    uint64_t predicated_packets = 0;
    uint64_t predicated_packets_skipped = 0;
    uint64_t unsupported_packets = 0;
    uint64_t draw_packets = 0;
    uint64_t draw_issue_attempts = 0;
    uint64_t draw_issue_failures = 0;
    uint64_t draws_viz_query_skipped = 0;
    uint64_t draws_submitted = 0;
    uint64_t draws_no_effect = 0;
    uint64_t draws_no_effect_surface_pitch_zero = 0;
    uint64_t draws_no_effect_no_rasterization_or_memexport = 0;
    uint64_t draws_no_effect_edram_mode = 0;
    uint64_t draws_no_effect_vertex_multipass = 0;
    uint64_t draws_no_effect_cull_both = 0;
    uint64_t draws_no_effect_zero_vertices = 0;
    uint64_t draws_async_placeholder = 0;
    uint64_t copy_attempts = 0;
    uint64_t occlusion_queries = 0;
    uint64_t memexport_full_readbacks = 0;
    uint64_t submissions = 0;
    uint64_t submission_us = 0;
    uint64_t submission_replay_us = 0;
    uint64_t submission_queue_us = 0;
    uint64_t fence_waits = 0;
    uint64_t fence_wait_us = 0;
  };
  CommandSummary command_summary_;
  uint32_t draw_trace_frame_ = UINT32_MAX;
  uint64_t draw_trace_index_ = 0;
  std::vector<uint32_t> draw_trace_memory_addresses_;

  Shader* active_vertex_shader_ = nullptr;
  Shader* active_pixel_shader_ = nullptr;

  bool paused_ = false;

  // By default (such as for tools), post-processing is disabled.
  // "Desired" is for the external thread managing the post-processing effect.
  SwapPostEffect swap_post_effect_desired_ = SwapPostEffect::kNone;
  SwapPostEffect swap_post_effect_actual_ = SwapPostEffect::kNone;

  // Set by backend command processors to their legacy memexport readback cvar
  // name (for explicit-override compatibility).
  const char* legacy_readback_memexport_cvar_name_ = nullptr;

 private:
  reg::DC_LUT_30_COLOR gamma_ramp_256_entry_table_[256] = {};
  reg::DC_LUT_PWL_DATA gamma_ramp_pwl_rgb_[128][3] = {};
  uint32_t gamma_ramp_rw_component_ = 0;
};

}  // namespace rex::graphics
