#pragma once
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

#include <cstdint>
#include <mutex>

#include <rex/graphics/async_shader_translation.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/device.h>

namespace rex::graphics::vulkan {

class VulkanShader : public SpirvShader {
 public:
  class VulkanTranslation : public SpirvTranslation {
   public:
    explicit VulkanTranslation(VulkanShader& shader, uint64_t modification)
        : SpirvTranslation(shader, modification) {}
    ~VulkanTranslation() override;

    VkShaderModule GetOrCreateShaderModule();
    VkShaderModule shader_module() const { return shader_module_; }
    AsyncShaderTranslation& translation_work() { return translation_work_; }

   private:
    VkShaderModule shader_module_ = VK_NULL_HANDLE;
    AsyncShaderTranslation translation_work_;
  };

  explicit VulkanShader(const ui::vulkan::VulkanDevice* vulkan_device,
                        xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                        const uint32_t* ucode_dwords, size_t ucode_dword_count,
                        std::endian ucode_source_endian = std::endian::big);

  // For owning subsystem like the pipeline cache, accessors for unique
  // identifiers (used instead of hashes to make sure collisions can't happen)
  // of binding layouts used by the shader, for invalidation if a shader with an
  // incompatible layout has been bound.
  size_t GetTextureBindingLayoutUserUID() const { return texture_binding_layout_user_uid_; }
  size_t GetSamplerBindingLayoutUserUID() const { return sampler_binding_layout_user_uid_; }
  // Layout IDs are installed on the GPU command thread after a translation's
  // completion has published the shared resource bindings.
  bool binding_layout_user_uids_ready() const {
    return binding_layout_user_uids_set_up_.test(std::memory_order_relaxed);
  }
  bool EnterBindingLayoutUserUIDSetup() { return !binding_layout_user_uids_set_up_.test_and_set(); }
  void SetTextureBindingLayoutUserUID(size_t uid) { texture_binding_layout_user_uid_ = uid; }
  void SetSamplerBindingLayoutUserUID(size_t uid) { sampler_binding_layout_user_uid_ = uid; }
  // Variants share the first successful translation's resource bindings.
  // Serialize writers so no variant can publish before those bindings exist.
  std::mutex& translation_mutex() { return translation_mutex_; }

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;

 private:
  const ui::vulkan::VulkanDevice* vulkan_device_;
  std::mutex translation_mutex_;

  std::atomic_flag binding_layout_user_uids_set_up_ = ATOMIC_FLAG_INIT;
  size_t texture_binding_layout_user_uid_ = 0;
  size_t sampler_binding_layout_user_uid_ = 0;
};

}  // namespace rex::graphics::vulkan
