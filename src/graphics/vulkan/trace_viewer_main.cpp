/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Recompilation Toolkit                                  *
 ******************************************************************************
 * Released under the BSD license. See LICENSE in the project root.           *
 ******************************************************************************
 */

#include <memory>

#include <rex/cvar.h>
#include <rex/graphics/trace_viewer.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app.h>

REXCVAR_DEFINE_STRING(trace_host_capture_path, "", "GPU",
                      "File path template for diagnostic RenderDoc captures.");

namespace rex::graphics::vulkan {

class VulkanTraceViewer final : public TraceViewer {
 public:
  static std::unique_ptr<ui::WindowedApp> Create(ui::WindowedAppContext& app_context) {
    return std::unique_ptr<ui::WindowedApp>(new VulkanTraceViewer(app_context));
  }

  std::unique_ptr<GraphicsSystem> CreateGraphicsSystem() override {
    return std::make_unique<VulkanGraphicsSystem>();
  }

  void BeginHostCapture() override {
    auto* renderdoc_api =
        static_cast<const ui::vulkan::VulkanProvider*>(graphics_system()->provider())
            ->vulkan_instance()
            ->renderdoc_api();
    if (renderdoc_api && !renderdoc_api->api_1_0_0()->IsFrameCapturing()) {
      const auto path = REXCVAR_GET(trace_host_capture_path);
      if (!path.empty()) {
        renderdoc_api->api_1_0_0()->SetCaptureFilePathTemplate(path.c_str());
      }
      renderdoc_api->api_1_0_0()->StartFrameCapture(nullptr, nullptr);
    }
  }

  void EndHostCapture() override {
    auto* renderdoc_api =
        static_cast<const ui::vulkan::VulkanProvider*>(graphics_system()->provider())
            ->vulkan_instance()
            ->renderdoc_api();
    if (renderdoc_api && renderdoc_api->api_1_0_0()->IsFrameCapturing()) {
      renderdoc_api->api_1_0_0()->EndFrameCapture(nullptr, nullptr);
    }
  }

  uintptr_t GetColorRenderTarget(uint32_t pitch, xenos::MsaaSamples samples, uint32_t base,
                                 xenos::ColorRenderTargetFormat format) override {
    return 0;
  }

  uintptr_t GetDepthRenderTarget(uint32_t pitch, xenos::MsaaSamples samples, uint32_t base,
                                 xenos::DepthRenderTargetFormat format) override {
    return 0;
  }

  uintptr_t GetTextureEntry(const TextureInfo& texture_info,
                            const SamplerInfo& sampler_info) override {
    return 0;
  }

 private:
  explicit VulkanTraceViewer(ui::WindowedAppContext& app_context)
      : TraceViewer(app_context, "rex-gpu-vulkan-trace-viewer") {}
};

}  // namespace rex::graphics::vulkan

REX_DEFINE_APP(rex_gpu_vulkan_trace_viewer,
               rex::graphics::vulkan::VulkanTraceViewer::Create)
