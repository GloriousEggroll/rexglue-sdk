#pragma once
// Vendor temporal upscalers (NVIDIA DLSS through NGX, Intel XeSS) behind one
// interface. They are compiled in only when their SDKs are configured
// (REX_HAS_DLSS, REX_HAS_XESS); at runtime Create returns nullptr when the
// GPU, driver or runtime library does not support them, and the presenter
// falls back to FSR 3.1.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {

class VulkanDevice;

enum class VendorUpscalerKind { kDlss, kXess };

// Extensions the compiled-in upscalers want. The instance and device enable
// those the implementation supports; missing ones only disable the upscaler.
std::vector<std::string> GetVendorUpscalerInstanceExtensions();
std::vector<std::string> GetVendorUpscalerDeviceExtensions(VkInstance instance,
                                                           VkPhysicalDevice physical_device);

// Device features the upscalers need, merged into the features the device is
// created with. Only features the device supports are enabled.
struct VendorUpscalerFeatureTargets {
  VkPhysicalDeviceFeatures* core_enabled = nullptr;
  const VkPhysicalDeviceFeatures* core_supported = nullptr;
  VkPhysicalDeviceVulkan11Features* enabled_1_1 = nullptr;
  const VkPhysicalDeviceVulkan11Features* supported_1_1 = nullptr;
  VkPhysicalDeviceVulkan12Features* enabled_1_2 = nullptr;
  const VkPhysicalDeviceVulkan12Features* supported_1_2 = nullptr;
  VkPhysicalDeviceVulkan13Features* enabled_1_3 = nullptr;
  const VkPhysicalDeviceVulkan13Features* supported_1_3 = nullptr;
  // Linked by the device when VK_EXT_mutable_descriptor_type is enabled.
  VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT* enabled_mutable_descriptor_type = nullptr;
  const VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT* supported_mutable_descriptor_type =
      nullptr;
};
void MergeVendorUpscalerFeatures(VkInstance instance, VkPhysicalDevice physical_device,
                                 const VendorUpscalerFeatureTargets& targets);

class VendorUpscaler {
 public:
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
  };
  // Inputs are in SHADER_READ_ONLY_OPTIMAL and the output in GENERAL; both
  // are left in those layouts. Motion vectors are in render pixels, previous
  // minus current; depth is reverse-Z; `reactive` (optional) marks pixels
  // that must not use history.
  struct Dispatch {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    Image color, depth, motion_vectors, reactive, output;
    float jitter_x = 0, jitter_y = 0;
    bool reset = false;
    float frame_time_ms = 16.667f;
  };

  static std::unique_ptr<VendorUpscaler> Create(VendorUpscalerKind kind,
                                                const VulkanDevice* device);
  virtual ~VendorUpscaler() = default;
  virtual const char* name() const = 0;
  // Records the upscale. Recreating internal state for a new size requires
  // the GPU to be done with earlier dispatches; `await_idle` provides that.
  virtual bool Evaluate(const Dispatch& dispatch, const std::function<void()>& await_idle) = 0;
};

}  // namespace rex::ui::vulkan
