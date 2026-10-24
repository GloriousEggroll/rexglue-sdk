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

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/chrono/clock.h>
#include <rex/graphics/command_processor.h>
#include <rex/system/xam/network.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/sampler_info.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>

REXCVAR_DEFINE_BOOL(gpu_bulk_type0_constants, false, "GPU diagnostics",
    "Use the existing bulk constant loader for sequential Type0 shader constants")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gpu_native_submit_completion_markers, false, "GPU",
    "Submit owned Native input reuse markers without waiting for their GPU fence");

REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");
REXCVAR_DEFINE_DOUBLE(framerate_limit, 60.0, "GPU", "Maximum frame rate (0 = unlimited)")
    .range(0.0, 1000.0);

REXCVAR_DEFINE_BOOL(clear_memory_page_state, true, "GPU",
                    "Refresh page-valid state from GPU-written memory at frame end. "
                    "Disable for minor CPU overhead reduction, but may break memory coherency.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(native_direct_execution, false, "GPU",
                   "Run the guest's D3D without the PM4 command processor; the native renderer executes API calls directly")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(occlusion_query_enable, true, "GPU", "Enable host occlusion query handling")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    occlusion_query_zero_is_visible, false, "GPU",
    "Bias zero-sample host occlusion query results to one visible sample. "
    "This avoids premature culling in titles that cannot consume immediate "
    "zero results safely.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(zpd_trace, false, "GPU",
                    "Log a bounded trace of EVENT_WRITE_ZPD report activity")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(readback_resolve, "none", "GPU",
                      "Controls CPU readback of render-to-texture resolve results.\n"
                      " none: Disable readback (default)\n"
                      " fast: Read previous frame (delayed, copy every frame)\n"
                      " some: Read previous frame (delayed, copy on cache miss)\n"
                      " full: Immediate sync readback (accurate but stalls)")
    .allowed({"none", "fast", "some", "full"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_resolve_half_pixel_offset, false, "GPU",
                    "When draw resolution scaling is active, sample from the center of each "
                    "scaled block during resolve readback downscale")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport, true, "GPU",
                    "Enable CPU readback of shader memexport writes for guest memory "
                    "coherency (can reduce correctness issues, but may add GPU/CPU sync cost)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport_fast, true, "GPU",
                    "Use fast double-buffered memexport readback when possible, with "
                    "automatic fallback to full synchronous readback")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(query_occlusion_fake_sample_count, 1000, "GPU",
                     "Fake sample count for occlusion queries")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(async_shader_compilation, true, "GPU",
                    "Compile shaders and create pipelines asynchronously in background "
                    "threads. This reduces stutter but may cause brief visual artifacts while "
                    "pipelines are being prepared.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(gpu_command_summary, false, "GPU",
                    "Log one command processing summary at the end of each guest frame")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    gpu_no_effect_trace, false, "GPU",
    "Log bounded Xenos register state for draws skipped as having no effect")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    gpu_draw_trace_interval, 0, "GPU",
    "Log normalized state for every draw in each Nth guest frame; 0 disables")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    gpu_draw_trace_resources, false, "GPU",
    "Include shader texture fetch and EDRAM copy state in GPU draw traces")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    gpu_draw_trace_memory, false, "GPU",
    "Hash the first 4 KiB of each unique texture base in sampled GPU draw frames")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics {
namespace {thread_local uint32_t api_frame_marker=UINT32_MAX;}
uint32_t ExchangeApiFrameMarker(uint32_t frame) {return std::exchange(api_frame_marker,frame);}
uint32_t ApiFrameMarker() {return api_frame_marker;}

using namespace rex::graphics::xenos;

namespace {

ReadbackResolveMode ParseReadbackResolveMode(std::string_view value) {
  if (value == "fast") {
    return ReadbackResolveMode::kFast;
  }
  if (value == "some") {
    return ReadbackResolveMode::kSome;
  }
  if (value == "full") {
    return ReadbackResolveMode::kFull;
  }
  return ReadbackResolveMode::kDisabled;
}

}  // namespace

void CommandProcessor::TraceDrawState(
    xenos::PrimitiveType prim_type, uint32_t index_count,
    const IndexBufferInfo* index_buffer_info) {
  if (draw_trace_frame_ != counter_) {
    draw_trace_frame_ = counter_;
    draw_trace_index_ = 0;
    draw_trace_memory_addresses_.clear();
  }
  const uint64_t draw_index = draw_trace_index_++;

  const int32_t interval = REXCVAR_GET(gpu_draw_trace_interval);
  if (interval <= 0 || counter_ % uint32_t(interval)) {
    return;
  }

  const RegisterFile& regs = *register_file_;
  const Shader* vertex_shader = active_vertex_shader();
  const Shader* pixel_shader = active_pixel_shader();
  const auto rb_modecontrol = regs.Get<reg::RB_MODECONTROL>();
  const auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  const auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  const auto rb_color_mask = regs.Get<reg::RB_COLOR_MASK>();
  const auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  const auto rb_depthcontrol = regs.Get<reg::RB_DEPTHCONTROL>();
  const auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  const auto vgt_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>();
  const auto vgt_min_vtx_indx = regs.Get<reg::VGT_MIN_VTX_INDX>();
  const auto vgt_max_vtx_indx = regs.Get<reg::VGT_MAX_VTX_INDX>();
  const auto rb_color_info_0 = regs.Get<reg::RB_COLOR_INFO>(
      reg::RB_COLOR_INFO::rt_register_indices[0]);
  const auto rb_color_info_1 = regs.Get<reg::RB_COLOR_INFO>(
      reg::RB_COLOR_INFO::rt_register_indices[1]);
  const auto rb_color_info_2 = regs.Get<reg::RB_COLOR_INFO>(
      reg::RB_COLOR_INFO::rt_register_indices[2]);
  const auto rb_color_info_3 = regs.Get<reg::RB_COLOR_INFO>(
      reg::RB_COLOR_INFO::rt_register_indices[3]);

  REXGPU_INFO(
      "GPU draw state: frame={} draw={} prim={} indices={} indexed={} "
      "ib={:08X} ifmt={} iendian={} vs={:016X} ps={:016X} "
      "index_offset={:08X} index_min={:08X} index_max={:08X} "
      "mode={:08X} surface={:08X} program={:08X} sc={:08X} "
      "color_mask={:08X} color_control={:08X} depth_control={:08X} "
      "rt0={:08X} rt1={:08X} rt2={:08X} rt3={:08X} depth={:08X}",
      counter_, draw_index, uint32_t(prim_type), index_count,
      index_buffer_info != nullptr,
      index_buffer_info ? index_buffer_info->guest_base : 0,
      index_buffer_info ? uint32_t(index_buffer_info->format) : 0,
      index_buffer_info ? uint32_t(index_buffer_info->endianness) : 0,
      vertex_shader ? vertex_shader->ucode_data_hash() : 0,
      pixel_shader ? pixel_shader->ucode_data_hash() : 0,
      vgt_indx_offset.value, vgt_min_vtx_indx.value,
      vgt_max_vtx_indx.value,
      rb_modecontrol.value, rb_surface_info.value, sq_program_cntl.value,
      pa_su_sc_mode_cntl.value, rb_color_mask.value, rb_colorcontrol.value,
      rb_depthcontrol.value, rb_color_info_0.value, rb_color_info_1.value,
      rb_color_info_2.value, rb_color_info_3.value, rb_depth_info.value);

  if (!REXCVAR_GET(gpu_draw_trace_resources)) {
    return;
  }

  constexpr uint64_t kGow3TitleVertexShaderHash =
      UINT64_C(0xF9EBE3D4293EC775);
  constexpr uint64_t kGow3TitlePixelShaderHash =
      UINT64_C(0x19055C4D86C6C08C);
  static uint32_t gow3_title_details_frame = UINT32_MAX;
  if (vertex_shader &&
      vertex_shader->ucode_data_hash() == kGow3TitleVertexShaderHash &&
      pixel_shader &&
      pixel_shader->ucode_data_hash() == kGow3TitlePixelShaderHash &&
      gow3_title_details_frame != counter_) {
    gow3_title_details_frame = counter_;

    auto trace_memory = [&](const char* kind, uint32_t base, size_t size) {
      size = std::min<size_t>(size, 4096);
      const uint8_t* bytes = memory_->TranslatePhysical(base);
      uint64_t hash = UINT64_C(14695981039346656037);
      uint32_t nonzero = 0;
      for (size_t i = 0; i < size; ++i) {
        const uint8_t value = bytes[i];
        hash = (hash ^ value) * UINT64_C(1099511628211);
        nonzero += value != 0;
      }
      REXGPU_INFO(
          "GPU GOW3 title memory: frame={} draw={} kind={} base={:08X} "
          "bytes={} hash={:016X} nonzero={}",
          counter_, draw_index, kind, base, size, hash, nonzero);
    };

    const Shader::ConstantRegisterMap& constant_map =
        vertex_shader->constant_register_map();
    const auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(
        reg::RB_BLENDCONTROL::rt_register_indices[0]);
    REXGPU_INFO(
        "GPU GOW3 title blend: frame={} draw={} value={:08X} "
        "color_src={} color_op={} color_dst={} alpha_src={} alpha_op={} "
        "alpha_dst={}",
        counter_, draw_index, blend_control.value,
        uint32_t(blend_control.color_srcblend),
        uint32_t(blend_control.color_comb_fcn),
        uint32_t(blend_control.color_destblend),
        uint32_t(blend_control.alpha_srcblend),
        uint32_t(blend_control.alpha_comb_fcn),
        uint32_t(blend_control.alpha_destblend));
    for (uint32_t i = 0;
         i < rex::countof(constant_map.vertex_fetch_bitmap); ++i) {
      uint32_t vfetch_bits_remaining =
          constant_map.vertex_fetch_bitmap[i];
      uint32_t j;
      while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
        vfetch_bits_remaining &= ~(uint32_t(1) << j);
        const uint32_t vfetch_index = i * 32 + j;
        const xenos::xe_gpu_vertex_fetch_t vertex_fetch =
            regs.GetVertexFetch(vfetch_index);
        REXGPU_INFO(
            "GPU GOW3 title vertex fetch: frame={} draw={} fetch={} "
            "words={:08X},{:08X} type={} base={:08X} bytes={}",
            counter_, draw_index, vfetch_index, vertex_fetch.dword_0,
            vertex_fetch.dword_1, uint32_t(vertex_fetch.type),
            vertex_fetch.address << 2, size_t(vertex_fetch.size) << 2);
        trace_memory("vertex", vertex_fetch.address << 2,
                     size_t(vertex_fetch.size) << 2);
        const uint8_t* vertex_bytes =
            memory_->TranslatePhysical(vertex_fetch.address << 2);
        const size_t vertex_count =
            std::min<size_t>(size_t(vertex_fetch.size) / 3, 8);
        for (size_t vertex_index = 0; vertex_index < vertex_count;
             ++vertex_index) {
          uint32_t words[3];
          std::memcpy(words, vertex_bytes + vertex_index * sizeof(words),
                      sizeof(words));
          REXGPU_INFO(
              "GPU GOW3 title vertex: frame={} draw={} vertex={} "
              "words={:08X},{:08X},{:08X}",
              counter_, draw_index, vertex_index, words[0], words[1],
              words[2]);
        }
      }
    }
    if (index_buffer_info) {
      trace_memory(
          "index", index_buffer_info->guest_base,
          size_t(index_count) *
              (index_buffer_info->format == xenos::IndexFormat::kInt16
                   ? sizeof(uint16_t)
                   : sizeof(uint32_t)));
    }

    auto trace_float_constants = [&](const char* stage,
                                     uint32_t constant_base,
                                     uint32_t constant_count) {
      for (uint32_t i = 0; i < constant_count; ++i) {
        const uint32_t register_base =
            XE_GPU_REG_SHADER_CONSTANT_000_X +
            ((constant_base + i) << 2);
        REXGPU_INFO(
            "GPU GOW3 title constant: frame={} draw={} stage={} c{}="
            "{:08X},{:08X},{:08X},{:08X}",
            counter_, draw_index, stage, i, regs[register_base],
            regs[register_base + 1], regs[register_base + 2],
            regs[register_base + 3]);
      }
    };
    trace_float_constants("vs", 0, 8);
    trace_float_constants("ps", 256, 4);
  }

  auto trace_shader_textures = [&](const char* stage, const Shader* shader) {
    if (!shader) {
      return;
    }
    for (const Shader::TextureBinding& binding : shader->texture_bindings()) {
      const xenos::xe_gpu_texture_fetch_t fetch =
          regs.GetTextureFetch(binding.fetch_constant);
      REXGPU_INFO(
          "GPU draw texture: frame={} draw={} stage={} binding={} fetch={} "
          "words={:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
          counter_, draw_index, stage, binding.binding_index,
          binding.fetch_constant, fetch.dword_0, fetch.dword_1,
          fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
      if (REXCVAR_GET(gpu_draw_trace_memory) &&
          fetch.type == xenos::FetchConstantType::kTexture) {
        const uint32_t base =
            fetch.base_address << xenos::kTextureSubresourceAlignmentBytesLog2;
        if (std::find(draw_trace_memory_addresses_.begin(),
                      draw_trace_memory_addresses_.end(),
                      base) == draw_trace_memory_addresses_.end()) {
          draw_trace_memory_addresses_.push_back(base);
          constexpr size_t kHashSize = 4096;
          const uint8_t* bytes = memory_->TranslatePhysical(base);
          uint64_t hash = UINT64_C(14695981039346656037);
          uint32_t nonzero = 0;
          uint8_t min_value = UINT8_MAX;
          uint8_t max_value = 0;
          for (size_t i = 0; i < kHashSize; ++i) {
            const uint8_t value = bytes[i];
            hash = (hash ^ value) * UINT64_C(1099511628211);
            nonzero += value != 0;
            min_value = std::min(min_value, value);
            max_value = std::max(max_value, value);
          }
          REXGPU_INFO(
              "GPU draw memory: frame={} draw={} stage={} binding={} fetch={} "
              "base={:08X} bytes={} hash={:016X} nonzero={} min={:02X} max={:02X}",
              counter_, draw_index, stage, binding.binding_index,
              binding.fetch_constant, base, kHashSize, hash, nonzero,
              min_value, max_value);
        }
      }
    }
  };
  trace_shader_textures("vs", vertex_shader);
  trace_shader_textures("ps", pixel_shader);

  if (rb_modecontrol.edram_mode == xenos::EdramMode::kCopy) {
    REXGPU_INFO(
        "GPU draw copy: frame={} draw={} control={:08X} dest_base={:08X} "
        "dest_pitch={:08X} dest_info={:08X} hiz_clear={:08X} "
        "depth_clear={:08X} color_clear={:08X} color_clear_lo={:08X} "
        "func={:08X} ref={:08X} mask={:08X} slice={:08X}",
        counter_, draw_index, regs[XE_GPU_REG_RB_COPY_CONTROL],
        regs[XE_GPU_REG_RB_COPY_DEST_BASE],
        regs[XE_GPU_REG_RB_COPY_DEST_PITCH],
        regs[XE_GPU_REG_RB_COPY_DEST_INFO], regs[XE_GPU_REG_RB_HIZ_CLEAR],
        regs[XE_GPU_REG_RB_DEPTH_CLEAR], regs[XE_GPU_REG_RB_COLOR_CLEAR],
        regs[XE_GPU_REG_RB_COLOR_CLEAR_LO], regs[XE_GPU_REG_RB_COPY_FUNC],
        regs[XE_GPU_REG_RB_COPY_REF], regs[XE_GPU_REG_RB_COPY_MASK],
        regs[XE_GPU_REG_RB_COPY_SURFACE_SLICE]);
  }
}

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   system::KernelState* kernel_state)
    : memory_(graphics_system->memory()),
      kernel_state_(kernel_state),
      graphics_system_(graphics_system),
      register_file_(graphics_system_->register_file()),
      trace_writer_(graphics_system->memory()->physical_membase()),
      worker_running_(true),
      write_ptr_index_event_(rex::thread::Event::CreateAutoResetEvent(false)),
      write_ptr_index_(0) {
  assert_not_null(write_ptr_index_event_);
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  // Initialize the gamma ramps to their default (linear) values - taken from
  // what games set when starting with the sRGB (return value 1)
  // VdGetCurrentDisplayGamma.
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    reg::DC_LUT_30_COLOR& gamma_ramp_entry = gamma_ramp_256_entry_table_[i];
    gamma_ramp_entry.color_10_blue = value;
    gamma_ramp_entry.color_10_green = value;
    gamma_ramp_entry.color_10_red = value;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    reg::DC_LUT_PWL_DATA gamma_ramp_entry = {};
    gamma_ramp_entry.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
    gamma_ramp_entry.delta = i < 0x7F ? 0x200 : 0;
    for (uint32_t j = 0; j < 3; ++j) {
      gamma_ramp_pwl_rgb_[i][j] = gamma_ramp_entry;
    }
  }

  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));
  worker_thread_->set_name("GPU Commands");
  worker_thread_->Create();

  return true;
}

void CommandProcessor::Shutdown() {
  EndTracing();

  worker_running_ = false;
  write_ptr_index_event_->Set();
  worker_thread_->Wait(0, 0, 0, nullptr);
  worker_thread_.reset();
}

void CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                               uint32_t title_id, bool blocking) {}

void CommandProcessor::RequestFrameTrace(const std::filesystem::path& root_path) {
  if (trace_state_ == TraceState::kStreaming) {
    REXGPU_ERROR("Streaming trace; cannot also trace frame.");
    return;
  }
  if (trace_state_ == TraceState::kSingleFrame) {
    REXGPU_ERROR("Frame trace already pending; ignoring.");
    return;
  }
  trace_state_ = TraceState::kSingleFrame;
  trace_frame_path_ = root_path;
}
void CommandProcessor::RequestFrameTraceAtFrame(const std::filesystem::path& root_path,uint32_t frame) {
  CallInThread([this,root_path,frame] {
    if((external_frame_number_!=UINT32_MAX&&frame<=external_frame_number_+1)||trace_state_!=TraceState::kDisabled||trace_writer_.is_open()||trace_requested_frame_!=UINT32_MAX) {
      REXGPU_ERROR("Cannot schedule GPU history at API frame {} (last marker {})",frame,external_frame_number_);
      return;
    }
    std::error_code error;
    std::filesystem::create_directories(root_path,error);
    if(error){REXGPU_ERROR("Cannot create GPU history directory: {}",error.message());return;}
    // Two adjacent initial states supply both prior history and an exact
    // reference frontbuffer for the first captured API frame.
    if(frame>=UINT32_MAX-1){REXGPU_ERROR("Invalid GPU history frame");return;}
    trace_frame_path_=root_path;trace_requested_frame_=frame;trace_history_snapshots_left_=2;
    REXGPU_INFO("Scheduled GPU history for frame {}",frame);
  });
}

void CommandProcessor::BeginTracing(const std::filesystem::path& root_path) {
  if (trace_state_ == TraceState::kStreaming) {
    REXGPU_ERROR("Streaming already active; ignoring request.");
    return;
  }
  if (trace_state_ == TraceState::kSingleFrame) {
    REXGPU_ERROR("Frame trace pending; ignoring streaming request.");
    return;
  }
  // Streaming starts on the next primary buffer execute.
  trace_state_ = TraceState::kStreaming;
  trace_stream_path_ = root_path;
}

void CommandProcessor::EndTracing() {
  if (!trace_writer_.is_open()) {
    return;
  }
  assert_true(trace_state_ == TraceState::kStreaming);
  trace_state_ = TraceState::kDisabled;
  trace_writer_.Close();
}

bool CommandProcessor::ExportDiagnosticSnapshot(const std::filesystem::path& directory,uint32_t title_id) {
  if(trace_writer_.is_open()||trace_state_!=TraceState::kDisabled) {
    REXGPU_ERROR("Cannot export diagnostic state while tracing is active");return false;
  }
  // Atomic creation of a new containing directory prevents overwriting either
  // an original trace or a previous diagnostic artifact.
  std::error_code error;
  if(!std::filesystem::create_directory(directory,error)) {
    REXGPU_ERROR("Diagnostic snapshot requires a new directory: {}",directory.string());return false;
  }
  try {
    if(!trace_writer_.Open(directory/"state.xtr",title_id))return false;
    InitializeTrace();
    trace_writer_.Close();
    std::ofstream metadata(directory/"state.json");
    metadata<<"{\n  \"version\": 1,\n  \"provenance\": \"diagnostic_replayed_gpu_state\",\n"
        <<"  \"last_present_api_frame\": "<<external_frame_number_<<",\n  \"has_command_stream\": false\n}\n";
    if(!metadata)throw std::runtime_error("Cannot write diagnostic snapshot metadata");
    return true;
  } catch(const std::exception& e) {
    trace_writer_.Close();REXGPU_ERROR("Diagnostic state export failed: {}",e.what());return false;
  }
}

void CommandProcessor::RestoreRegisters(uint32_t first_register, const uint32_t* register_values,
                                        uint32_t register_count, bool execute_callbacks) {
  if (first_register > RegisterFile::kRegisterCount ||
      RegisterFile::kRegisterCount - first_register < register_count) {
    REXGPU_WARN(
        "CommandProcessor::RestoreRegisters out of bounds (0x{:X} registers "
        "starting with 0x{:X}, while a total of 0x{:X} registers are stored)",
        register_count, first_register, RegisterFile::kRegisterCount);
    if (first_register > RegisterFile::kRegisterCount) {
      return;
    }
    register_count =
        std::min(uint32_t(RegisterFile::kRegisterCount) - first_register, register_count);
  }
  if (execute_callbacks) {
    for (uint32_t i = 0; i < register_count; ++i) {
      WriteRegister(first_register + i, register_values[i]);
    }
  } else {
    std::memcpy(register_file_->values + first_register, register_values,
                sizeof(uint32_t) * register_count);
  }
}

void CommandProcessor::RestoreGammaRamp(const reg::DC_LUT_30_COLOR* new_gamma_ramp_256_entry_table,
                                        const reg::DC_LUT_PWL_DATA* new_gamma_ramp_pwl_rgb,
                                        uint32_t new_gamma_ramp_rw_component) {
  std::memcpy(gamma_ramp_256_entry_table_, new_gamma_ramp_256_entry_table,
              sizeof(reg::DC_LUT_30_COLOR) * 256);
  std::memcpy(gamma_ramp_pwl_rgb_, new_gamma_ramp_pwl_rgb, sizeof(reg::DC_LUT_PWL_DATA) * 3 * 128);
  gamma_ramp_rw_component_ = new_gamma_ramp_rw_component;
  OnGammaRamp256EntryTableValueWritten();
  OnGammaRampPWLValueWritten();
}

void CommandProcessor::CallInThread(std::function<void()> fn) {
  if (pending_fns_.empty() && system::XThread::IsInThread(worker_thread_.get())) {
    fn();
  } else {
    pending_fns_.push(std::move(fn));
  }
}

void CommandProcessor::PostDirectWork(std::function<void()> fn) {
  {
    std::lock_guard lock(direct_work_mutex_);
    direct_work_.push_back(std::move(fn));
    direct_work_pending_.store(true, std::memory_order_release);
  }
  write_ptr_index_event_->Set();
}

void CommandProcessor::SetDirectIdleHook(std::function<bool()> hook) {
  direct_idle_hook_.store(hook ? std::make_shared<std::function<bool()>>(std::move(hook)) : nullptr);
  write_ptr_index_event_->Set();
}

void CommandProcessor::SetDirectSwapRouter(DirectSwapRouter router) {
  direct_swap_router_.store(router ? std::make_shared<DirectSwapRouter>(std::move(router)) : nullptr);
}

void CommandProcessor::PostDirectSwap(uint32_t frontbuffer_ptr, uint32_t width, uint32_t height,
                                      uint32_t api_frame) {
  std::function<void()> swap = [this, frontbuffer_ptr, width, height, api_frame] {
    rex::perf::Profiler::Flip();
    external_frame_number_ = api_frame;
    if (!guest_thread_frame_limiter_.load(std::memory_order_relaxed))
      frame_limiter_.Wait(system::xam::Network::FrameRateLimit(REXCVAR_GET(framerate_limit)));
    IssueSwap(frontbuffer_ptr, width, height);
    command_summary_ = {};
    ++counter_;
  };
  if (const auto router = direct_swap_router_.load(); router && (*router)(swap)) return;
  PostDirectWork(std::move(swap));
}

bool CommandProcessor::RunDirectWork() {
  if (!direct_work_pending_.load(std::memory_order_acquire)) {
    return false;
  }
  std::vector<std::function<void()>> work;
  {
    std::lock_guard lock(direct_work_mutex_);
    work.swap(direct_work_);
    direct_work_pending_.store(false, std::memory_order_relaxed);
  }
  // Poll completion between items too: a guest blocked on a result must not
  // wait behind a long run of posted work.
  const auto hook = direct_idle_hook_.load();
  for (auto& fn : work) {
    fn();
    if (hook) {
      (*hook)();
    }
  }
  return !work.empty();
}

void CommandProcessor::ClearCaches() {}

void CommandProcessor::InvalidateGpuMemory() {}

ReadbackResolveMode CommandProcessor::GetReadbackResolveMode(
    bool legacy_readback_resolve_enabled) const {
  ReadbackResolveMode shared_mode = ParseReadbackResolveMode(REXCVAR_GET(readback_resolve));
  bool shared_mode_overrides_legacy = shared_mode != ReadbackResolveMode::kDisabled ||
                                      rex::cvar::HasNonDefaultValue("readback_resolve");
  if (shared_mode_overrides_legacy) {
    return shared_mode;
  }
  return legacy_readback_resolve_enabled ? ReadbackResolveMode::kFast
                                         : ReadbackResolveMode::kDisabled;
}

bool CommandProcessor::IsReadbackMemexportEnabled(bool legacy_backend_flag) const {
  if (legacy_readback_memexport_cvar_name_ &&
      rex::cvar::HasNonDefaultValue(legacy_readback_memexport_cvar_name_)) {
    return legacy_backend_flag;
  }
  return REXCVAR_GET(readback_memexport);
}

void CommandProcessor::SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect) {
  if (swap_post_effect_desired_ == swap_post_effect) {
    return;
  }
  swap_post_effect_desired_ = swap_post_effect;
  CallInThread([this, swap_post_effect]() { swap_post_effect_actual_ = swap_post_effect; });
}

void CommandProcessor::WorkerThreadMain() {
  if (!SetupContext()) {
    rex::FatalError("Unable to setup command processor internal state");
    return;
  }

  while (worker_running_) {
    while (!pending_fns_.empty()) {
      auto fn = std::move(pending_fns_.front());
      pending_fns_.pop();
      fn();
    }

    uint32_t write_ptr_index = write_ptr_index_.load();
    if ((write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) &&
        direct_idle_hook_.load()) {
      // Direct execution: the guest submits no packets. Run posted work, and
      // between items poll GPU completion without forcing a drain.
      if (RunDirectWork()) {
        continue;
      }
      const auto hook = direct_idle_hook_.load();
      if (hook && (*hook)()) {
        rex::thread::MaybeYield();
      } else {
        rex::thread::Wait(write_ptr_index_event_.get(), true, std::chrono::milliseconds(1));
      }
      continue;
    }
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      SCOPE_profile_cpu_i("gpu", "rex::graphics::CommandProcessor::Stall");
      // We've run out of commands to execute.
      // We spin here waiting for new ones, as the overhead of waiting on our
      // event is too high.
      PrepareForWait();
      uint32_t loop_count = 0;
      do {
        // If we spin around too much, revert to a "low-power" state.
        if (loop_count > 500) {
          const int wait_time_ms = 5;
          rex::thread::Wait(write_ptr_index_event_.get(), true,
                            std::chrono::milliseconds(wait_time_ms));
        }

        rex::thread::MaybeYield();
        loop_count++;
        write_ptr_index = write_ptr_index_.load();
      } while (worker_running_ && pending_fns_.empty() &&
               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));
      ReturnFromWait();
      if (!worker_running_ || !pending_fns_.empty()) {
        continue;
      }
    }
    assert_true(read_ptr_index_ != write_ptr_index);

    // Execute. Note that we handle wraparound transparently.
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);

    // TODO(benvanik): use reader->Read_update_freq_ and only issue after moving
    //     that many indices.
    if (read_ptr_writeback_ptr_) {
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),
                                       read_ptr_index_);
    }

    // FIXME: We're supposed to process the WAIT_UNTIL register at this point,
    // but no games seem to actually use it.
  }

  ShutdownContext();
}

void CommandProcessor::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  thread::Fence fence;
  CallInThread([&fence]() {
    fence.Signal();
    thread::Thread::GetCurrentThread()->Suspend();
  });

  fence.Wait();
}

void CommandProcessor::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  worker_thread_->thread()->Resume();
}

bool CommandProcessor::Save(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  stream->Write<uint32_t>(primary_buffer_ptr_);
  stream->Write<uint32_t>(primary_buffer_size_);
  stream->Write<uint32_t>(read_ptr_index_);
  stream->Write<uint32_t>(read_ptr_update_freq_);
  stream->Write<uint32_t>(read_ptr_writeback_ptr_);
  stream->Write<uint32_t>(write_ptr_index_.load());

  return true;
}

bool CommandProcessor::Restore(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  primary_buffer_ptr_ = stream->Read<uint32_t>();
  primary_buffer_size_ = stream->Read<uint32_t>();
  read_ptr_index_ = stream->Read<uint32_t>();
  read_ptr_update_freq_ = stream->Read<uint32_t>();
  read_ptr_writeback_ptr_ = stream->Read<uint32_t>();
  write_ptr_index_.store(stream->Read<uint32_t>());

  return true;
}

bool CommandProcessor::SetupContext() {
  return true;
}

void CommandProcessor::ShutdownContext() {}

void CommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  // CP_RB_RPTR_ADDR Ring Buffer Read Pointer Address 0x70C
  // ptr = RB_RPTR_ADDR, pointer to write back the address to.
  read_ptr_writeback_ptr_ = ptr;
  // CP_RB_CNTL Ring Buffer Control 0x704
  // block_size = RB_BLKSZ, log2 of number of quadwords read between updates of
  //              the read pointer.
  read_ptr_update_freq_ = uint32_t(1) << block_size_log2 >> 2;
}

void CommandProcessor::UpdateWritePointer(uint32_t value) {
  write_ptr_index_ = value;
  write_ptr_index_event_->Set();
}

uint32_t CommandProcessor::ReadRegisterValue(uint32_t index) const {
  if (index < RegisterFile::kRegisterCount) {
    return register_file_->values[index];
  }
  auto it = extended_register_values_.find(index);
  return it != extended_register_values_.end() ? it->second : 0;
}

void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  RegisterFile& regs = *register_file_;
  if (index >= RegisterFile::kRegisterCount) {
    auto [it, inserted] = extended_register_values_.insert_or_assign(index, value);
    (void)it;
    if (inserted) {
      REXGPU_WARN(
          "CommandProcessor::WriteRegister index out of bounds: {} (stored as extended register)",
          index);
    }
    return;
  }

  // Volatile for the WAIT_REG_MEM loop.
  const_cast<volatile uint32_t&>(regs.values[index]) = value;
  // Register metadata is only needed to describe debug logging. Preserve the
  // register write and every hardware side effect regardless of log level.
  if (auto* logger = rex::GetLoggerRaw(rex::log::gpu());
      logger && logger->should_log(spdlog::level::debug) && !regs.GetRegisterInfo(index)) {
    REXGPU_DEBUG("GPU: Write to unknown register ({:04X} = {:08X})", index, value);
  }

  // Scratch register writeback.
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1 << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      // Enabled - write to address.
      uint32_t scratch_addr = regs.values[XE_GPU_REG_SCRATCH_ADDR];
      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
    }
  } else {
    switch (index) {
      // If this is a COHER register, set the dirty flag.
      // This will block the command processor the next time it WAIT_REG_MEMs
      // and allow us to synchronize the memory.
      case XE_GPU_REG_COHER_STATUS_HOST: {
        const_cast<volatile uint32_t&>(regs.values[index]) |= UINT32_C(0x80000000);
      } break;

      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        // Reset the sequential read / write component index (see the M56
        // DC_LUT_SEQ_COLOR documentation).
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is in the red, green, blue order, but the write
        // enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          uint32_t gamma_ramp_seq_color = regs.Get<reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        // Should be in the PWL writing mode.
        assert_not_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // DC_LUT_PWL_DATA is likely in the red, green, blue order because
        // DC_LUT_SEQ_COLOR is, but the write enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl][gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          // TODO(Triang3l): Should this increase beyond 7 bits for PWL?
          // Direct3D 9 explicitly sets rw_index to 0x80 after writing the last
          // PWL entry. However, the DC_LUT_RW_INDEX documentation says that for
          // PWL, the bit 7 is ignored.
          new_gamma_ramp_rw_index.rw_index = (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
                                             ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        uint32_t gamma_ramp_write_enable_mask = regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        // TODO(Triang3l): Should this reset the component write index? If this
        // increase is assumed to behave like a full DC_LUT_RW_INDEX write, it
        // probably should. Currently this also calls WriteRegister for
        // DC_LUT_RW_INDEX, which resets gamma_ramp_rw_component_ as well.
        gamma_ramp_rw_component_ = 0;
        reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                      rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;
    }
  }
}

void CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = memory::load_and_swap<uint32_t>(base + i);
    WriteRegister(start_index + i, data);
  }
}

void CommandProcessor::WriteRegisterRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  memory::RingBuffer::ReadRange range = ring->BeginRead(size_t(num_registers) * sizeof(uint32_t));
  if (range.first_length != 0) {
    uint32_t first_count = uint32_t(range.first_length / sizeof(uint32_t));
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
                          first_count);
    base += first_count;
  }
  if (range.second_length != 0) {
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
                          uint32_t(range.second_length / sizeof(uint32_t)));
  }
  ring->EndRead(range);
}

void CommandProcessor::WriteALURangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                             uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_registers);
}

void CommandProcessor::WriteFetchRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                               uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_registers);
}

void CommandProcessor::WriteBoolRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_registers);
}

void CommandProcessor::WriteLoopRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                   uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_registers);
}

void CommandProcessor::WriteALURangeFromMem(uint32_t start_index, uint32_t* base,
                                            uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base,
                                              uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}

void CommandProcessor::MakeCoherent() {
  SCOPE_profile_cpu_f("gpu");

  // Status host often has 0x01000000 or 0x03000000.
  // This is likely toggling VC (vertex cache) or TC (texture cache).
  // Or, it also has a direction in here maybe - there is probably
  // some way to check for dest coherency (what all the COHER_DEST_BASE_*
  // registers are for).
  // Best docs I've found on this are here:
  // https://web.archive.org/web/20160711162346/https://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2013/10/R6xx_R7xx_3D.pdf
  // https://cgit.freedesktop.org/xorg/driver/xf86-video-radeonhd/tree/src/r6xx_accel.c?id=3f8b6eccd9dba116cc4801e7f80ce21a879c67d2#n454

  // Volatile because this may be called from the WAIT_REG_MEM loop.
  volatile uint32_t* regs_volatile = register_file_->values;
  auto status_host = rex::memory::Reinterpret<reg::COHER_STATUS_HOST>(
      uint32_t(regs_volatile[XE_GPU_REG_COHER_STATUS_HOST]));
  uint32_t base_host = regs_volatile[XE_GPU_REG_COHER_BASE_HOST];
  uint32_t size_host = regs_volatile[XE_GPU_REG_COHER_SIZE_HOST];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  // TODO(benvanik): notify resource cache of base->size and type.
  REXGPU_TRACE("Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
               base_host + size_host, size_host, action);

  // Mark coherent.
  regs_volatile[XE_GPU_REG_COHER_STATUS_HOST] = 0;
}

void CommandProcessor::PrepareForWait() {
  trace_writer_.Flush();
}

void CommandProcessor::ReturnFromWait() {}

uint32_t CommandProcessor::ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index) {
  SCOPE_profile_cpu_f("gpu");

  // If we have a pending trace stream open it now. That way we ensure we get
  // all commands.
  if (!trace_writer_.is_open() && trace_state_ == TraceState::kStreaming) {
    uint32_t title_id =
        kernel_state_->GetExecutableModule() ? kernel_state_->GetExecutableModule()->title_id() : 0;
    auto file_name = fmt::format("{:08X}_stream.xtr", title_id);
    auto path = trace_stream_path_ / file_name;
    trace_writer_.Open(path, title_id);
    InitializeTrace();
  }

  // Adjust pointer base.
  uint32_t start_ptr = primary_buffer_ptr_ + read_index * sizeof(uint32_t);
  start_ptr = (primary_buffer_ptr_ & ~0x1FFFFFFF) | (start_ptr & 0x1FFFFFFF);
  uint32_t end_ptr = primary_buffer_ptr_ + write_index * sizeof(uint32_t);
  end_ptr = (primary_buffer_ptr_ & ~0x1FFFFFFF) | (end_ptr & 0x1FFFFFFF);

  trace_writer_.WritePrimaryBufferStart(start_ptr, write_index - read_index);

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(primary_buffer_ptr_), primary_buffer_size_);
  reader.set_read_offset(read_index * sizeof(uint32_t));
  reader.set_write_offset(write_index * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      OnPacketExecutionFailure("GPU packet decoding or execution failed");
      // This probably should be fatal - but we're going to continue anyways.
      REXGPU_ERROR("**** PRIMARY RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());

  OnPrimaryBufferEnd();

  trace_writer_.WritePrimaryBufferEnd();

  return write_index;
}

void CommandProcessor::ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  trace_writer_.WriteIndirectBufferStart(ptr, count * sizeof(uint32_t));

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      OnPacketExecutionFailure("GPU packet decoding or execution failed");
      // Return up a level if we encounter a bad packet.
      REXGPU_ERROR("**** INDIRECT RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());

  trace_writer_.WriteIndirectBufferEnd();
}

void CommandProcessor::ExecutePacket(uint32_t ptr, uint32_t count) {
  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      OnPacketExecutionFailure("GPU packet decoding or execution failed");
      REXGPU_ERROR("**** ExecutePacket: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
}

bool CommandProcessor::ExecutePacket(memory::RingBuffer* reader) {
  if(native_immediate_marker_pairs_enabled_&&TryExecuteNativeImmediateMarkerPair(reader))return true;
  const uintptr_t packet_pointer=reader->read_ptr();
  const uint32_t packet = reader->ReadAndSwap<uint32_t>();
  const uint32_t packet_type = packet >> 30;
  if(capture_native_draw_packets_&&packet_type==3) {
    const uint32_t opcode=(packet>>8)&127;
    if(opcode==0x22||opcode==0x36) {
      const size_t payload_bytes=(((packet>>16)&0x3fff)+1)*sizeof(uint32_t);
      const uintptr_t physical_base=reinterpret_cast<uintptr_t>(memory_->physical_membase());
      if(packet_pointer<physical_base||packet_pointer-physical_base>=0x20000000ull||payload_bytes>reader->read_count())
        return false;
      native_draw_packet_address_=uint32_t(packet_pointer-physical_base);
      native_draw_packet_bytes_.resize(payload_bytes+4);
      for(unsigned byte=0;byte<4;++byte)native_draw_packet_bytes_[byte]=uint8_t(packet>>(24-byte*8));
      const auto range=reader->BeginRead(payload_bytes);
      std::memcpy(native_draw_packet_bytes_.data()+4,range.first,range.first_length);
      if(range.second_length)std::memcpy(native_draw_packet_bytes_.data()+4+range.first_length,range.second,range.second_length);
      // BeginRead only describes the memory; the normal decoder owns AdvanceRead.
    }
  }
  if (packet == 0) {
    trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 1);
    trace_writer_.WritePacketEnd();
    return true;
  }

  if (packet == 0xCDCDCDCD) {
    REXGPU_WARN("GPU packet is CDCDCDCD - probably read uninitialized memory!");
  }

  switch (packet_type) {
    case 0x00:
      return ExecutePacketType0(reader, packet);
    case 0x01:
      return ExecutePacketType1(reader, packet);
    case 0x02:
      return ExecutePacketType2(reader, packet);
    case 0x03:
      return ExecutePacketType3(reader, packet);
    default:
      assert_unhandled_case(packet_type);
      return false;
  }
}

bool CommandProcessor::ExecutePacketType0(memory::RingBuffer* reader, uint32_t packet) {
  // Type-0 packet.
  // Write count registers in sequence to the registers starting at
  // (base_index << 2).

  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType0 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 1 + count);

  uint32_t base_index = (packet & 0x7FFF);
  uint32_t write_one_reg = (packet >> 15) & 0x1;
  const auto within = [&](uint32_t first, uint32_t last) {
    return base_index >= first && base_index <= last && count <= last - base_index + 1;
  };
  const bool constants = !write_one_reg &&
      (within(XE_GPU_REG_SHADER_CONSTANT_000_X, XE_GPU_REG_SHADER_CONSTANT_511_W) ||
       within(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) ||
       within(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031, XE_GPU_REG_SHADER_CONSTANT_LOOP_31));
  if (constants) {
    ++command_summary_.type0_constant_packets;
    command_summary_.type0_constant_words += count;
  }
  if (constants && REXCVAR_GET(gpu_bulk_type0_constants)) {
    // Reuse the same endian conversion, ring splitting and backend dirty-state
    // notifications as SET_CONSTANT2. Only side-effect-free constant families
    // qualify. Scratch fences, coherence, gamma and repeated writes retain
    // their original per-word virtual dispatch and completion boundaries.
    WriteRegisterRangeFromRing(reader, base_index, count);
    ++command_summary_.type0_bulk_packets;
    command_summary_.type0_bulk_words += count;
  } else {
    for (uint32_t m = 0; m < count; m++) {
      uint32_t reg_data = reader->ReadAndSwap<uint32_t>();
      uint32_t target_index = write_one_reg ? base_index : base_index + m;
      WriteRegister(target_index, reg_data);
    }
  }

  trace_writer_.WritePacketEnd();
  return true;
}

bool CommandProcessor::ExecutePacketType1(memory::RingBuffer* reader, uint32_t packet) {
  // Type-1 packet.
  // Contains two registers of data. Type-0 should be more common.
  trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 3);
  uint32_t reg_index_1 = packet & 0x7FF;
  uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
  uint32_t reg_data_1 = reader->ReadAndSwap<uint32_t>();
  uint32_t reg_data_2 = reader->ReadAndSwap<uint32_t>();
  WriteRegister(reg_index_1, reg_data_1);
  WriteRegister(reg_index_2, reg_data_2);
  trace_writer_.WritePacketEnd();
  return true;
}

bool CommandProcessor::ExecutePacketType2(memory::RingBuffer* reader, uint32_t packet) {
  // Type-2 packet.
  // No-op. Do nothing.
  trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 1);
  trace_writer_.WritePacketEnd();
  return true;
}

bool CommandProcessor::ExecutePacketType3(memory::RingBuffer* reader, uint32_t packet) {
  // Type-3 packet.
  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  auto data_start_offset = reader->read_offset();
  ++command_summary_.type3_packets;

  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType3 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  // To handle nesting behavior when tracing we special case indirect buffers.
  if (opcode == PM4_INDIRECT_BUFFER) {
    trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 2);
  } else {
    trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 1 + count);
  }

  // & 1 == predicate - when set, we do bin check to see if we should execute
  // the packet. Only type 3 packets are affected.
  // We also skip predicated swaps, as they are never valid (probably?).
  if (packet & 1) {
    ++command_summary_.predicated_packets;
    bool any_pass = (bin_select_ & bin_mask_) != 0;
    if (!any_pass || opcode == PM4_XE_SWAP) {
      ++command_summary_.predicated_packets_skipped;
      reader->AdvanceRead(count * sizeof(uint32_t));
      trace_writer_.WritePacketEnd();
      return true;
    }
  }

  PrepareForPacket(opcode, count);
  bool result = false;
  switch (opcode) {
    case PM4_ME_INIT:
      result = ExecutePacketType3_ME_INIT(reader, packet, count);
      break;
    case PM4_NOP:
      result = ExecutePacketType3_NOP(reader, packet, count);
      break;
    case PM4_INTERRUPT:
      result = ExecutePacketType3_INTERRUPT(reader, packet, count);
      break;
    case PM4_XE_SWAP:
      result = ExecutePacketType3_XE_SWAP(reader, packet, count);
      break;
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
      ++command_summary_.indirect_buffers;
      result = ExecutePacketType3_INDIRECT_BUFFER(reader, packet, count);
      break;
    case PM4_WAIT_REG_MEM:
      result = ExecutePacketType3_WAIT_REG_MEM(reader, packet, count);
      break;
    case PM4_REG_RMW:
      result = ExecutePacketType3_REG_RMW(reader, packet, count);
      break;
    case PM4_REG_TO_MEM:
      result = ExecutePacketType3_REG_TO_MEM(reader, packet, count);
      break;
    case PM4_MEM_WRITE:
      result = ExecutePacketType3_MEM_WRITE(reader, packet, count);
      break;
    case PM4_COND_WRITE:
      result = ExecutePacketType3_COND_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE:
      result = ExecutePacketType3_EVENT_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_SHD:
      result = ExecutePacketType3_EVENT_WRITE_SHD(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_EXT:
      result = ExecutePacketType3_EVENT_WRITE_EXT(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_ZPD:
      result = ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
      break;
    case PM4_DRAW_INDX:
      result = ExecutePacketType3_DRAW_INDX(reader, packet, count);
      break;
    case PM4_DRAW_INDX_2:
      result = ExecutePacketType3_DRAW_INDX_2(reader, packet, count);
      break;
    case PM4_SET_CONSTANT:
      result = ExecutePacketType3_SET_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_CONSTANT2:
      result = ExecutePacketType3_SET_CONSTANT2(reader, packet, count);
      break;
    case PM4_LOAD_ALU_CONSTANT:
      result = ExecutePacketType3_LOAD_ALU_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_SHADER_CONSTANTS:
      result = ExecutePacketType3_SET_SHADER_CONSTANTS(reader, packet, count);
      break;
    case PM4_IM_LOAD:
      result = ExecutePacketType3_IM_LOAD(reader, packet, count);
      break;
    case PM4_IM_LOAD_IMMEDIATE:
      result = ExecutePacketType3_IM_LOAD_IMMEDIATE(reader, packet, count);
      break;
    case PM4_INVALIDATE_STATE:
      result = ExecutePacketType3_INVALIDATE_STATE(reader, packet, count);
      break;
    case PM4_VIZ_QUERY:
      result = ExecutePacketType3_VIZ_QUERY(reader, packet, count);
      break;

    case PM4_SET_BIN_MASK_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_MASK_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_MASK: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_CONTEXT_UPDATE: {
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU context update = {:08X}", value);
      assert_true(value == 0);
      result = true;
      break;
    }
    case PM4_WAIT_FOR_IDLE: {
      // This opcode is used by 5454084E while going / being ingame.
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU wait for idle = {:08X}", value);
      result = true;
      break;
    }

    default:
      OnPacketExecutionFailure("Unimplemented GPU opcode");
      ++command_summary_.unsupported_packets;
      REXGPU_INFO("Unimplemented GPU OPCODE: 0x{:02X}\t\tCOUNT: {}\n", opcode, count);
      assert_always();
      reader->AdvanceRead(count * sizeof(uint32_t));
      break;
  }

  trace_writer_.WritePacketEnd();
  if (opcode == PM4_XE_SWAP) {
    const bool scheduled_history=external_frame_number_!=UINT32_MAX&&external_frame_number_+1==trace_requested_frame_;
    // End the trace writer frame.
    if (trace_writer_.is_open()) {
      trace_writer_.WriteEvent(EventCommand::Type::kSwap);
      trace_writer_.Flush();
      if (trace_state_ == TraceState::kSingleFrame) {
        trace_state_ = TraceState::kDisabled;
        trace_writer_.Close();
      }
    }
    if(scheduled_history) {
      trace_requested_frame_=UINT32_MAX;
      if(trace_state_==TraceState::kDisabled&&!trace_writer_.is_open())trace_state_=TraceState::kSingleFrame;
      else {trace_history_snapshots_left_=0;REXGPU_ERROR("Scheduled GPU history conflicted with another trace");}
    }
    if (!trace_writer_.is_open()&&trace_state_ == TraceState::kSingleFrame) {
      // New trace request - we only start tracing at the beginning of a frame.
      uint32_t title_id = kernel_state_->GetExecutableModule()->title_id();
      auto file_name = scheduled_history?fmt::format("{:08X}_api_{}.xtr",title_id,external_frame_number_+1):
          fmt::format("{:08X}_{}.xtr", title_id, counter_ - 1);
      auto path = trace_frame_path_ / file_name;
      trace_writer_.Open(path, title_id);
      InitializeTrace();
      if(scheduled_history&&trace_writer_.is_open()) {
        trace_writer_.Flush();
        std::ofstream metadata(path.string()+".initial.json");
        metadata << "{\"version\":1,\"frame\":" << external_frame_number_+1
                 << ",\"provenance\":\"reference_gpu_frame_boundary\",\"trace\":\"" << file_name << "\"}\n";
        REXGPU_INFO("Captured GPU history immediately before API frame {}: {}",external_frame_number_+1,path.string());
        if(trace_history_snapshots_left_&&--trace_history_snapshots_left_)
          trace_requested_frame_=external_frame_number_+2;
      }
    }
  }

  assert_true(reader->read_offset() ==
              (data_start_offset + (count * sizeof(uint32_t))) % reader->capacity());
  return result;
}

bool CommandProcessor::ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // initialize CP's micro-engine
  me_bin_.clear();
  for (uint32_t i = 0; i < count; i++) {
    me_bin_.push_back(reader->ReadAndSwap<uint32_t>());
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_NOP(memory::RingBuffer* reader, uint32_t packet,
                                              uint32_t count) {
  // skip N 32-bit words to get to the next packet
  // No-op, ignore some data.
  reader->AdvanceRead(count * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // generate interrupt from the command stream
  uint32_t cpu_mask = reader->ReadAndSwap<uint32_t>();
  for (int n = 0; n < 6; n++) {
    if (cpu_mask & (1 << n)) {
      if (graphics_system_) {
        graphics_system_->DispatchInterruptCallback(1, n);
      }
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  {
    static uint64_t last_frame_tick = 0;
    uint64_t now = rex::chrono::Clock::QueryHostTickCount();
    if (last_frame_tick) {
      uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();
      int64_t dt_us = static_cast<int64_t>((now - last_frame_tick) * 1000000 / freq);
      PROFILE_FRAME_TIME_US(dt_us);
      PROFILE_FPS(freq / (now - last_frame_tick));
    }
    last_frame_tick = now;
  }
#endif
  rex::perf::Profiler::Flip();

  // Xenia-specific VdSwap hook.
  // VdSwap will post this to tell us we need to swap the screen/fire an
  // interrupt.
  // 63 words here, but only the first has any data.
  uint32_t magic = reader->ReadAndSwap<memory::fourcc_t>();
  assert_true(magic == kSwapSignature);

  // TODO(benvanik): only swap frontbuffer ptr.
  uint32_t frontbuffer_ptr = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_width = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_height = reader->ReadAndSwap<uint32_t>();
  external_frame_number_=count>=5?reader->ReadAndSwap<uint32_t>():UINT32_MAX;
  reader->AdvanceRead((count - (count>=5?5:4)) * sizeof(uint32_t));

  const bool timing_enabled = REXCVAR_GET(gpu_command_summary);
  const auto limiter_start = timing_enabled ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
  if (!guest_thread_frame_limiter_.load(std::memory_order_relaxed))
      frame_limiter_.Wait(system::xam::Network::FrameRateLimit(REXCVAR_GET(framerate_limit)));
  const auto swap_start = timing_enabled ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

  if (timing_enabled) {
    const auto swap_end = std::chrono::steady_clock::now();
    REXGPU_INFO(
        "GPU frame {}: type3={} indirect={} draw_packets={} issue={} submitted={} "
        "failed={} no_effect={} no_effect_pitch={} no_effect_raster_memexport={} "
        "no_effect_edram={} no_effect_multipass={} no_effect_cull_both={} "
        "no_effect_vertices={} async_placeholder={} viz_skipped={} "
        "predicated_skipped={}/{} copies={} unsupported={} movie_yuv={} "
        "queries={} memexport_full={} submissions={} submit_us={} replay_us={} queue_us={} "
        "fence_waits={} fence_us={} limiter_us={} swap_us={} "
        "type0_constant_packets={} type0_constant_words={} type0_bulk_packets={} type0_bulk_words={}",
        counter_, command_summary_.type3_packets, command_summary_.indirect_buffers,
        command_summary_.draw_packets, command_summary_.draw_issue_attempts,
        command_summary_.draws_submitted, command_summary_.draw_issue_failures,
        command_summary_.draws_no_effect,
        command_summary_.draws_no_effect_surface_pitch_zero,
        command_summary_.draws_no_effect_no_rasterization_or_memexport,
        command_summary_.draws_no_effect_edram_mode,
        command_summary_.draws_no_effect_vertex_multipass,
        command_summary_.draws_no_effect_cull_both,
        command_summary_.draws_no_effect_zero_vertices,
        command_summary_.draws_async_placeholder, command_summary_.draws_viz_query_skipped,
        command_summary_.predicated_packets_skipped, command_summary_.predicated_packets,
        command_summary_.copy_attempts, command_summary_.unsupported_packets,
        command_summary_.movie_yuv, command_summary_.occlusion_queries,
        command_summary_.memexport_full_readbacks, command_summary_.submissions,
        command_summary_.submission_us, command_summary_.submission_replay_us,
        command_summary_.submission_queue_us, command_summary_.fence_waits,
        command_summary_.fence_wait_us,
        std::chrono::duration_cast<std::chrono::microseconds>(swap_start - limiter_start).count(),
        std::chrono::duration_cast<std::chrono::microseconds>(swap_end - swap_start).count(),
        command_summary_.type0_constant_packets, command_summary_.type0_constant_words,
        command_summary_.type0_bulk_packets, command_summary_.type0_bulk_words);
  }
  command_summary_ = {};

  ++counter_;
  return true;
}

bool CommandProcessor::ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // indirect buffer dispatch
  uint32_t list_ptr = CpuToGpu(reader->ReadAndSwap<uint32_t>());
  uint32_t list_length = reader->ReadAndSwap<uint32_t>();
  assert_zero(list_length & ~0xFFFFF);
  list_length &= 0xFFFFF;
  ExecuteIndirectBuffer(GpuToCpu(list_ptr), list_length);
  return true;
}

bool CommandProcessor::ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // wait until a register or memory location is a specific value

  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t wait = reader->ReadAndSwap<uint32_t>();

  PrepareForRegisterWait(count, wait_info, poll_reg_addr, ref, mask, wait);
  bool is_memory = (wait_info & 0x10) != 0;

  bool matched = false;
  do {
    uint32_t value = 0;
    if (is_memory) {
      value =
          *reinterpret_cast<uint32_t*>(memory_->TranslatePhysical(poll_reg_addr & ~uint32_t(0x3)));
      trace_writer_.WriteMemoryRead(CpuToGpu(poll_reg_addr & ~uint32_t(0x3)), sizeof(uint32_t));
      value = xenos::GpuSwap(value, static_cast<xenos::Endian>(poll_reg_addr & 0x3));
    } else {
      value = ReadRegisterValue(poll_reg_addr);
      if (poll_reg_addr == XE_GPU_REG_COHER_STATUS_HOST) {
        MakeCoherent();
        value = ReadRegisterValue(poll_reg_addr);
      }
    }
    switch (wait_info & 0x7) {
      case 0x0:  // Never.
        matched = false;
        break;
      case 0x1:  // Less than reference.
        matched = (value & mask) < ref;
        break;
      case 0x2:  // Less than or equal to reference.
        matched = (value & mask) <= ref;
        break;
      case 0x3:  // Equal to reference.
        matched = (value & mask) == ref;
        break;
      case 0x4:  // Not equal to reference.
        matched = (value & mask) != ref;
        break;
      case 0x5:  // Greater than or equal to reference.
        matched = (value & mask) >= ref;
        break;
      case 0x6:  // Greater than reference.
        matched = (value & mask) > ref;
        break;
      case 0x7:  // Always
        matched = true;
        break;
    }
    if (!matched) {
      // Wait.
      if (wait >= 0x100) {
        PrepareForWait();
        if (!REXCVAR_GET(vsync)) {
          // User wants it fast and dangerous.
          rex::thread::MaybeYield();
        } else {
          rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));
        }
        rex::thread::SyncMemory();
        ReturnFromWait();

        if (!worker_running_) {
          // Short-circuited exit.
          return false;
        }
      } else {
        PrepareForWait();
        rex::thread::MaybeYield();
        ReturnFromWait();
      }
    }
  } while (!matched);

  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_RMW(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // register read/modify/write
  // ? (used during shader upload and edram setup)
  uint32_t rmw_info = reader->ReadAndSwap<uint32_t>();
  uint32_t and_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t or_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t value = register_file_->values[rmw_info & 0x1FFF];
  if ((rmw_info >> 31) & 0x1) {
    // & reg
    value &= register_file_->values[and_mask & 0x1FFF];
  } else {
    // & imm
    value &= and_mask;
  }
  if ((rmw_info >> 30) & 0x1) {
    // | reg
    value |= register_file_->values[or_mask & 0x1FFF];
  } else {
    // | imm
    value |= or_mask;
  }
  WriteRegister(rmw_info & 0x1FFF, value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // Copy Register to Memory (?)
  // Count is 2, assuming a Register Addr and a Memory Addr.

  uint32_t reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t mem_addr = reader->ReadAndSwap<uint32_t>();

  uint32_t reg_val = ReadRegisterValue(reg_addr);

  auto endianness = static_cast<xenos::Endian>(mem_addr & 0x3);
  mem_addr &= ~0x3;
  reg_val = GpuSwap(reg_val, endianness);
  memory::store(memory_->TranslatePhysical(mem_addr), reg_val);
  trace_writer_.WriteMemoryWrite(CpuToGpu(mem_addr), 4);

  return true;
}

bool CommandProcessor::ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  uint32_t write_addr = reader->ReadAndSwap<uint32_t>();
  for (uint32_t i = 0; i < count - 1; i++) {
    uint32_t write_data = reader->ReadAndSwap<uint32_t>();

    auto endianness = static_cast<xenos::Endian>(write_addr & 0x3);
    auto addr = write_addr & ~0x3;
    write_data = GpuSwap(write_data, endianness);
    memory::store(memory_->TranslatePhysical(addr), write_data);
    trace_writer_.WriteMemoryWrite(CpuToGpu(addr), 4);
    write_addr += 4;
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // conditional write to memory or register
  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t write_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t write_data = reader->ReadAndSwap<uint32_t>();
  uint32_t value;
  if (wait_info & 0x10) {
    // Memory.
    auto endianness = static_cast<xenos::Endian>(poll_reg_addr & 0x3);
    poll_reg_addr &= ~0x3;
    trace_writer_.WriteMemoryRead(CpuToGpu(poll_reg_addr), 4);
    value = memory::load<uint32_t>(memory_->TranslatePhysical(poll_reg_addr));
    value = GpuSwap(value, endianness);
  } else {
    // Register.
    value = ReadRegisterValue(poll_reg_addr);
  }
  bool matched = false;
  switch (wait_info & 0x7) {
    case 0x0:  // Never.
      matched = false;
      break;
    case 0x1:  // Less than reference.
      matched = (value & mask) < ref;
      break;
    case 0x2:  // Less than or equal to reference.
      matched = (value & mask) <= ref;
      break;
    case 0x3:  // Equal to reference.
      matched = (value & mask) == ref;
      break;
    case 0x4:  // Not equal to reference.
      matched = (value & mask) != ref;
      break;
    case 0x5:  // Greater than or equal to reference.
      matched = (value & mask) >= ref;
      break;
    case 0x6:  // Greater than reference.
      matched = (value & mask) > ref;
      break;
    case 0x7:  // Always
      matched = true;
      break;
  }
  if (matched) {
    // Write.
    if (wait_info & 0x100) {
      // Memory.
      auto endianness = static_cast<xenos::Endian>(write_reg_addr & 0x3);
      write_reg_addr &= ~0x3;
      write_data = GpuSwap(write_data, endianness);
      memory::store(memory_->TranslatePhysical(write_reg_addr), write_data);
      trace_writer_.WriteMemoryWrite(CpuToGpu(write_reg_addr), 4);
    } else {
      // Register.
      WriteRegister(write_reg_addr, write_data);
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // generate an event that creates a write to memory when completed
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  if (count == 1) {
    // Just an event flag? Where does this write?
  } else {
    // Write to an address.
    OnPacketExecutionFailure("Addressed EVENT_WRITE is not implemented");
    assert_always();
    reader->AdvanceRead((count - 1) * sizeof(uint32_t));
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a VS|PS_done event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  uint32_t value = reader->ReadAndSwap<uint32_t>();

  if(observe_decoded_event_write_shd_)ObserveDecodedEventWriteShd(packet,count,initiator,address,value,
      reinterpret_cast<uintptr_t>(reader->buffer()),reader->capacity(),reader->read_offset());

  WriteEventShdValue(initiator,address,value);
  return true;
}

void CommandProcessor::WriteEventShdValue(uint32_t initiator,uint32_t address,uint32_t value) {
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  uint32_t data_value;
  if ((initiator >> 31) & 0x1) {
    // Write counter (GPU vblank counter?).
    data_value = counter_;
  } else {
    // Write value.
    data_value = value;
  }
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;
  data_value = GpuSwap(data_value, endianness);
  memory::store(memory_->TranslatePhysical(address), data_value);
  trace_writer_.WriteMemoryWrite(CpuToGpu(address), 4);
}

bool CommandProcessor::TryExecuteNativeImmediateMarkerPair(memory::RingBuffer* reader) {
  // Trace/debug callbacks and partial/wrapped publications retain the original
  // scalar path. Never inspect bytes beyond this decoder's published extent.
  if(trace_writer_.is_open()||trace_state_!=TraceState::kDisabled||
     reader->capacity()<32||reader->read_count()<32||reader->read_offset()>reader->capacity()-32)return false;
  const uintptr_t physical=reinterpret_cast<uintptr_t>(memory_->physical_membase());
  const uintptr_t buffer=reinterpret_cast<uintptr_t>(reader->buffer());
  if(buffer<physical||buffer-physical>=0x20000000ull||
     uint64_t(buffer-physical)+reader->read_offset()+32>0x20000000ull)return false;
  auto copy=*reader;
  NativeCompletionMarkerPair pair;
  pair.words[0]=copy.ReadAndSwap<uint32_t>();
  if(pair.words[0]!=0xc0025800u)return false;
  pair.command_buffer=uint32_t(buffer-physical);pair.command_capacity=reader->capacity();
  pair.command_address=pair.command_buffer+reader->read_offset();
  for(size_t i=1;i<pair.words.size();++i)pair.words[i]=copy.ReadAndSwap<uint32_t>();
  pair.counter=pair.words[6]&~3u;
  if(!IsNativeCompletionMarkerPair(pair))return false;
  auto guard=PrepareNativeImmediateMarkerPair(pair);
  if(!guard)return false;
  // No queue/provider/capture callback may run while the target guard is held.
  // Both original stores remain immediate; only their redundant GPU wait was
  // removed. The real pending-work latch and future mandatory fences survive.
  for(size_t part=0;part<2;++part) {
    const auto begin=reader->read_ptr();
    reader->AdvanceRead(16);
    ++command_summary_.type3_packets;
    trace_writer_.WritePacketStart(uint32_t(begin),4);
    WriteEventShdValue(pair.words[part*4+1],pair.words[part*4+2],pair.words[part*4+3]);
    trace_writer_.WritePacketEnd();
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  OnPacketExecutionFailure("Screen-extent query results are not implemented");
  // generate a screen extent event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;

  // Let us hope we can fake this.
  // This callback tells the driver the xy coordinates affected by a previous
  // drawcall.
  // https://www.google.com/patents/US20060055701
  uint16_t extents[] = {
      0 >> 3,                                    // min x
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max x
      0 >> 3,                                    // min y
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max y
      0,                                         // min z
      1,                                         // max z
  };
  assert_true(endianness == xenos::Endian::k8in16);
  memory::copy_and_swap_16_unaligned(memory_->TranslatePhysical(address), extents,
                                     rex::countof(extents));
  trace_writer_.WriteMemoryWrite(CpuToGpu(address), sizeof(extents));
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  // Occlusion queries:
  // This command is send on query begin and end.
  // As a workaround report some fixed amount of passed samples.
  auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
  if (fake_sample_count >= 0) {
    const uint32_t report_address =
        register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
    uint32_t sample_count_address =
        xenos::XenosZPDReport::GetRecordBase(report_address);
    auto* pSampleCounts = memory_->TranslatePhysical<xe_gpu_depth_sample_counts*>(
        sample_count_address);
    if (!pSampleCounts) {
      return true;
    }
    bool is_end = xenos::XenosZPDReport::IsEndRecord(report_address);
    if (!xenos::XenosZPDReport::IsBeginRecord(report_address) && !is_end) {
      is_end = xenos::XenosZPDReport::HasPendingSentinel(pSampleCounts);
    }
    if (is_end) {
      xenos::XenosZPDReport::WriteSampleCount(pSampleCounts,
                                              uint32_t(fake_sample_count));
    } else {
      std::memset(pSampleCounts, 0, sizeof(xe_gpu_depth_sample_counts));
    }
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3Draw(memory::RingBuffer* reader, uint32_t packet,
                                              const char* opcode_name, uint32_t viz_query_condition,
                                              uint32_t count_remaining) {
  // if viz_query_condition != 0, this is a conditional draw based on viz query.
  // This ID matches the one issued in PM4_VIZ_QUERY
  // uint32_t viz_id = viz_query_condition & 0x3F;
  // when true, render conditionally based on query result
  // uint32_t viz_use = viz_query_condition & 0x100;

  if (viz_query_condition & 0x100)
    OnPacketExecutionFailure("Conditional visualization-query draws are not implemented");
  ++command_summary_.draw_packets;
  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("{}: Packet too small, can't read VGT_DRAW_INITIATOR", opcode_name);
    return false;
  }
  reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
  vgt_draw_initiator.value = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  WriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, vgt_draw_initiator.value);

  bool draw_succeeded = true;
  // TODO(Triang3l): Remove IndexBufferInfo and replace handling of all this
  // with PrimitiveProcessor when the old Vulkan renderer is removed.
  bool is_indexed = false;
  IndexBufferInfo index_buffer_info;
  switch (vgt_draw_initiator.source_select) {
    case xenos::SourceSelect::kDMA: {
      // Indexed draw.
      is_indexed = true;

      // Two separate bounds checks so if there's only one missing register
      // value out of two, one uint32_t will be skipped in the command buffer,
      // not two.
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_BASE", opcode_name);
        return false;
      }
      uint32_t vgt_dma_base = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      WriteRegister(XE_GPU_REG_VGT_DMA_BASE, vgt_dma_base);
      reg::VGT_DMA_SIZE vgt_dma_size;
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_SIZE", opcode_name);
        return false;
      }
      vgt_dma_size.value = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      WriteRegister(XE_GPU_REG_VGT_DMA_SIZE, vgt_dma_size.value);

      uint32_t index_size_bytes = vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16
                                      ? sizeof(uint16_t)
                                      : sizeof(uint32_t);
      // The base address must already be word-aligned according to the R6xx
      // documentation, but for safety.
      index_buffer_info.guest_base = vgt_dma_base & ~(index_size_bytes - 1);
      index_buffer_info.endianness = vgt_dma_size.swap_mode;
      index_buffer_info.format = vgt_draw_initiator.index_size;
      index_buffer_info.length = vgt_dma_size.num_words * index_size_bytes;
      index_buffer_info.count = vgt_draw_initiator.num_indices;
    } break;
    case xenos::SourceSelect::kImmediate: {
      OnPacketExecutionFailure("Immediate vertex indices are not implemented");
      // TODO(Triang3l): VGT_IMMED_DATA.
      REXGPU_ERROR(
          "{}: Using immediate vertex indices, which are not supported yet. "
          "Report the game to Xenia developers!",
          opcode_name, uint32_t(vgt_draw_initiator.source_select));
      draw_succeeded = false;
      assert_always();
    } break;
    case xenos::SourceSelect::kAutoIndex: {
      // Auto draw.
      index_buffer_info.guest_base = 0;
      index_buffer_info.length = 0;
    } break;
    default: {
      OnPacketExecutionFailure("Invalid draw source selection");
      // Invalid source selection.
      draw_succeeded = false;
      assert_unhandled_case(vgt_draw_initiator.source_select);
    } break;
  }

  // Skip to the next command, for example, if there are immediate indexes that
  // we don't support yet.
  reader->AdvanceRead(count_remaining * sizeof(uint32_t));

  if (draw_succeeded) {
    auto viz_query = register_file_->Get<reg::PA_SC_VIZ_QUERY>();
    if (!(viz_query.viz_query_ena && viz_query.kill_pix_post_hi_z)) {
      // TODO(Triang3l): Don't drop the draw call completely if the vertex
      // shader has memexport.
      // TODO(Triang3l || JoelLinn): Handle this properly in the render
      // backends.

      bool major_mode_explicit =
          xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type);
      ++command_summary_.draw_issue_attempts;
      TraceDrawState(vgt_draw_initiator.prim_type,
                     vgt_draw_initiator.num_indices,
                     is_indexed ? &index_buffer_info : nullptr);
      draw_succeeded = IssueDraw(vgt_draw_initiator.prim_type, vgt_draw_initiator.num_indices,
                                 is_indexed ? &index_buffer_info : nullptr, major_mode_explicit);
      if (!draw_succeeded) {
        OnPacketExecutionFailure("Draw backend rejected an owned packet");
        ++command_summary_.draw_issue_failures;
        auto vgt_output_path_cntl = register_file_->Get<reg::VGT_OUTPUT_PATH_CNTL>();
        auto vgt_hos_cntl = register_file_->Get<reg::VGT_HOS_CNTL>();
        auto rb_modecontrol = register_file_->Get<reg::RB_MODECONTROL>();
        REXGPU_ERROR(
            "{}({}, {}, {}): Failed in backend "
            "(major_mode={}, explicit_major={}, path_select={}, tess_mode={}, edram_mode={})",
            opcode_name, static_cast<uint32_t>(vgt_draw_initiator.num_indices),
            uint32_t(vgt_draw_initiator.prim_type), uint32_t(vgt_draw_initiator.source_select),
            uint32_t(vgt_draw_initiator.major_mode), uint32_t(major_mode_explicit),
            uint32_t(vgt_output_path_cntl.path_select), uint32_t(vgt_hos_cntl.tess_mode),
            uint32_t(rb_modecontrol.edram_mode));
      }
    } else {
      OnPacketExecutionFailure("Post-HiZ visualization-query draw suppression is not implemented");
      ++command_summary_.draws_viz_query_skipped;
    }
  }

  // If read the packed correctly, but merely couldn't execute it (because of,
  // for instance, features not supported by the host), don't terminate command
  // buffer processing as that would leave rendering in a way more inconsistent
  // state than just a single dropped draw command.
  return true;
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // "initiate fetch of index buffer and draw"
  // Generally used by Xbox 360 Direct3D 9 for kDMA and kAutoIndex sources.
  // With a viz query token as the first one.
  uint32_t count_remaining = count;
  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("PM4_DRAW_INDX: Packet too small, can't read the viz query token");
    return false;
  }
  uint32_t viz_query_condition = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX", viz_query_condition,
                                count_remaining);
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // "draw using supplied indices in packet"
  // Generally used by Xbox 360 Direct3D 9 for kAutoIndex source.
  // No viz query token.
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX_2", 0, count);
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  // load constant into chip and to memory
  // PM4_REG(reg) ((0x4 << 16) | (GSL_HAL_SUBBLOCK_OFFSET(reg)))
  //                                     reg - 0x2000
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t count_registers = count - 1;
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromRing(reader, index, count_registers);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromRing(reader, index, count_registers);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromRing(reader, index, count_registers);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromRing(reader, index, count_registers);
      break;
    case 4:  // REGISTERS
      WriteREGISTERSRangeFromRing(reader, index, count_registers);
      break;
    default:
      assert_always();
      reader->AdvanceRead((count - 1) * sizeof(uint32_t));
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader, uint32_t packet,
                                                        uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  // load constants from memory
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  address &= 0x3FFFFFFF;
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t size_dwords = reader->ReadAndSwap<uint32_t>();
  size_dwords &= 0xFFF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t* xlat_address = memory_->TranslatePhysical<uint32_t*>(address);
  switch (type) {
    case 0:  // ALU
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteALURangeFromMem(index, xlat_address, size_dwords);
      break;
    case 1:  // FETCH
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteFetchRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 2:  // BOOL
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteBoolRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 3:  // LOOP
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteLoopRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 4:  // REGISTERS
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteREGISTERSRangeFromMem(index, xlat_address, size_dwords);
      break;
    default:
      assert_always();
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (pointer-based)
  uint32_t addr_type = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(addr_type & 0x3);
  uint32_t addr = addr_type & ~0x3;
  uint32_t start_size = reader->ReadAndSwap<uint32_t>();
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);

  trace_writer_.WriteMemoryRead(CpuToGpu(addr), size_dwords * 4);
  auto shader =
      LoadShader(shader_type, addr, memory_->TranslatePhysical<uint32_t*>(addr), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (code embedded in packet)
  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();
  uint32_t dword1 = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(dword0);
  uint32_t start_size = dword1;
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);
  assert_true(reader->read_count() >= size_dwords * 4);
  assert_true(count - 2 >= size_dwords);
  auto shader = LoadShader(shader_type, uint32_t(reader->read_ptr()),
                           reinterpret_cast<uint32_t*>(reader->read_ptr()), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  reader->AdvanceRead(size_dwords * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader,
                                                           uint32_t packet, uint32_t count) {
  // selective invalidation of state pointers
  /*uint32_t mask =*/reader->ReadAndSwap<uint32_t>();
  // driver_->InvalidateState(mask);
  return true;
}

bool CommandProcessor::ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  OnPacketExecutionFailure("Visualization-query results are not implemented");
  // begin/end initiator for viz query extent processing
  // https://www.google.com/patents/US20050195186
  assert_true(count == 1);

  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();

  uint32_t id = dword0 & 0x3F;
  uint32_t end = dword0 & 0x100;
  if (!end) {
    // begin a new viz query @ id
    // On hardware this clears the internal state of the scan converter (which
    // is different to the register)
    WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_START);
    REXGPU_INFO("Begin viz query ID {:02X}", id);
  } else {
    // end the viz query
    WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_END);
    REXGPU_INFO("End viz query ID {:02X}", id);
    // The scan converter writes the internal result back to the register here.
    // We just fake it and say it was visible in case it is read back.
    if (id < 32) {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0] |= uint32_t(1) << id;
    } else {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1] |= uint32_t(1) << (id - 32);
    }
  }

  return true;
}

void CommandProcessor::InitializeTrace() {
  // Write the initial register values, to be loaded directly into the
  // RegisterFile since all registers, including those that may have side
  // effects on setting, will be saved.
  trace_writer_.WriteRegisters(0, register_file_->values, RegisterFile::kRegisterCount, false);

  trace_writer_.WriteGammaRamp(gamma_ramp_256_entry_table(), gamma_ramp_pwl_rgb(),
                               gamma_ramp_rw_component_);
}

}  // namespace rex::graphics
