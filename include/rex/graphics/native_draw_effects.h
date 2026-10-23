#pragma once
#include <cstdint>

namespace rex::graphics {
// Restricted proof for driver synchronization draws with all framebuffer
// writes disabled. Query visibility and either shader's memory exports remain
// observable even when color/depth/stencil masks are zero.
struct NativeDrawEffects {
  uint32_t edram_mode=0,color_mask=0,depth_control=0;
  bool vertex_shader_present=false;
  bool vertex_memory_export=false,pixel_memory_export=false;
  bool host_query=false,native_query=false,resuming_query=false,publishing_query=false;
};
inline bool IsNativeDrawWithoutOutputs(const NativeDrawEffects& state) {
  return (state.edram_mode==4||state.edram_mode==5)&&state.vertex_shader_present&&
      !state.color_mask&&!state.depth_control&&
      !state.vertex_memory_export&&!state.pixel_memory_export&&
      !state.host_query&&!state.native_query&&!state.resuming_query&&!state.publishing_query;
}
} // namespace rex::graphics
