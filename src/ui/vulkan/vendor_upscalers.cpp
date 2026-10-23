#include <rex/ui/vulkan/vendor_upscalers.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <mutex>

#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

#if defined(REX_HAS_DLSS) && REX_HAS_DLSS
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>
#endif

#if defined(REX_HAS_XESS) && REX_HAS_XESS
#include <xess/xess_vk.h>
#if REX_PLATFORM_WIN32
#include <windows.h>
#endif
#endif

namespace rex::ui::vulkan {
namespace {

// Quality mode for an upscale ratio along one axis.
float Ratio(uint32_t render, uint32_t output) {
  return render ? float(output) / float(render) : 1.0f;
}

#if defined(REX_HAS_DLSS) && REX_HAS_DLSS
// Our own project identifier (NGX_ProjectID path; no NVIDIA application ID).
constexpr const char* kNgxProjectId = "4f3e2b8c-5d1a-4e07-9a6b-73c2d8e5f190";
const wchar_t* NgxDataPath() {
  static const std::wstring path = std::filesystem::temp_directory_path().wstring();
  return path.c_str();
}
NVSDK_NGX_FeatureDiscoveryInfo NgxDiscoveryInfo() {
  NVSDK_NGX_FeatureDiscoveryInfo info = {};
  info.SDKVersion = NVSDK_NGX_Version_API;
  info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
  info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
  info.Identifier.v.ProjectDesc.ProjectId = kNgxProjectId;
  info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
  info.Identifier.v.ProjectDesc.EngineVersion = "1.0";
  info.ApplicationDataPath = NgxDataPath();
  return info;
}
NVSDK_NGX_Resource_VK NgxImage(const VendorUpscaler::Image& image, bool read_write) {
  return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image,
                                                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                                                image.format, image.width, image.height,
                                                read_write);
}

class DlssUpscaler final : public VendorUpscaler {
 public:
  static std::unique_ptr<VendorUpscaler> Create(const VulkanDevice* device) {
    const VulkanInstance* instance = device->vulkan_instance();
    auto gipa = instance->functions().vkGetInstanceProcAddr;
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        gipa(instance->instance(), "vkGetDeviceProcAddr"));
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_Init_with_ProjectID(
            kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", NgxDataPath(),
            instance->instance(), device->physical_device(), device->device(), gipa, gdpa))) {
      REXLOG_INFO("DLSS unavailable: NGX did not initialize on this GPU or driver");
      return nullptr;
    }
    auto upscaler = std::unique_ptr<DlssUpscaler>(new DlssUpscaler(device));
    int available = 0;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&upscaler->parameters_)) ||
        NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(upscaler->parameters_,
                                                  NVSDK_NGX_Parameter_SuperSampling_Available,
                                                  &available)) ||
        !available) {
      REXLOG_INFO("DLSS unavailable: DLSS Super Resolution is not supported here");
      return nullptr;
    }
    REXLOG_INFO("DLSS Super Resolution available");
    return upscaler;
  }
  ~DlssUpscaler() override {
    if (feature_) NVSDK_NGX_VULKAN_ReleaseFeature(feature_);
    if (parameters_) NVSDK_NGX_VULKAN_DestroyParameters(parameters_);
    NVSDK_NGX_VULKAN_Shutdown1(device_->device());
  }
  const char* name() const override { return "DLSS"; }

  bool Evaluate(const Dispatch& d, const std::function<void()>& await_idle) override {
    const uint32_t rw = d.color.width, rh = d.color.height;
    const uint32_t ow = d.output.width, oh = d.output.height;
    if (!feature_ || rw != render_width_ || rh != render_height_ || ow != output_width_ ||
        oh != output_height_) {
      if (feature_) {
        await_idle();
        NVSDK_NGX_VULKAN_ReleaseFeature(feature_);
        feature_ = nullptr;
      }
      const float ratio = std::max(Ratio(rw, ow), Ratio(rh, oh));
      NVSDK_NGX_DLSS_Create_Params create = {};
      create.Feature.InWidth = rw;
      create.Feature.InHeight = rh;
      create.Feature.InTargetWidth = ow;
      create.Feature.InTargetHeight = oh;
      create.Feature.InPerfQualityValue =
          ratio >= 2.5f ? NVSDK_NGX_PerfQuality_Value_UltraPerformance
          : ratio >= 1.85f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
          : ratio >= 1.6f  ? NVSDK_NGX_PerfQuality_Value_Balanced
          : ratio >= 1.05f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                           : NVSDK_NGX_PerfQuality_Value_DLAA;
      create.InFeatureCreateFlags =
          NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
      if (NVSDK_NGX_FAILED(NGX_VULKAN_CREATE_DLSS_EXT1(device_->device(), d.command_buffer, 1, 1,
                                                       &feature_, parameters_, &create))) {
        REXLOG_WARN("DLSS feature creation failed for {}x{} -> {}x{}", rw, rh, ow, oh);
        feature_ = nullptr;
        return false;
      }
      render_width_ = rw;
      render_height_ = rh;
      output_width_ = ow;
      output_height_ = oh;
    }
    NVSDK_NGX_Resource_VK color = NgxImage(d.color, false), output = NgxImage(d.output, true);
    NVSDK_NGX_Resource_VK depth = NgxImage(d.depth, false);
    NVSDK_NGX_Resource_VK motion = NgxImage(d.motion_vectors, false);
    NVSDK_NGX_Resource_VK reactive = NgxImage(d.reactive, false);
    NVSDK_NGX_VK_DLSS_Eval_Params eval = {};
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    // HUD pixels: take the current frame instead of history.
    eval.pInBiasCurrentColorMask = d.reactive.image ? &reactive : nullptr;
    eval.InJitterOffsetX = d.jitter_x;
    eval.InJitterOffsetY = d.jitter_y;
    eval.InRenderSubrectDimensions = {rw, rh};
    eval.InReset = d.reset ? 1 : 0;
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;
    eval.InFrameTimeDeltaInMsec = d.frame_time_ms;
    return NVSDK_NGX_SUCCEED(
        NGX_VULKAN_EVALUATE_DLSS_EXT(d.command_buffer, feature_, parameters_, &eval));
  }

 private:
  explicit DlssUpscaler(const VulkanDevice* device) : device_(device) {}
  const VulkanDevice* device_;
  NVSDK_NGX_Parameter* parameters_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  uint32_t render_width_ = 0, render_height_ = 0, output_width_ = 0, output_height_ = 0;
};
#endif  // REX_HAS_DLSS

#if defined(REX_HAS_XESS) && REX_HAS_XESS
// libxess.dll is loaded at runtime so a missing library only disables XeSS.
struct XessLibrary {
  decltype(&xessVKGetRequiredInstanceExtensions) get_instance_extensions = nullptr;
  decltype(&xessVKGetRequiredDeviceExtensions) get_device_extensions = nullptr;
  decltype(&xessVKGetRequiredDeviceFeatures) get_device_features = nullptr;
  decltype(&xessVKCreateContext) create_context = nullptr;
  decltype(&xessVKInit) init = nullptr;
  decltype(&xessVKExecute) execute = nullptr;
  decltype(&xessDestroyContext) destroy_context = nullptr;
  bool loaded = false;
};
const XessLibrary& Xess() {
  static XessLibrary library;
  static std::once_flag once;
  std::call_once(once, [] {
#if REX_PLATFORM_WIN32
    HMODULE module = LoadLibraryW(L"libxess.dll");
    if (!module) return;
    auto load = [&](auto& function, const char* name) {
      function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
          reinterpret_cast<void*>(GetProcAddress(module, name)));
      return function != nullptr;
    };
    library.loaded = load(library.get_instance_extensions, "xessVKGetRequiredInstanceExtensions") &&
                     load(library.get_device_extensions, "xessVKGetRequiredDeviceExtensions") &&
                     load(library.get_device_features, "xessVKGetRequiredDeviceFeatures") &&
                     load(library.create_context, "xessVKCreateContext") &&
                     load(library.init, "xessVKInit") && load(library.execute, "xessVKExecute") &&
                     load(library.destroy_context, "xessDestroyContext");
#endif
  });
  return library;
}
xess_vk_image_view_info XessImage(const VendorUpscaler::Image& image) {
  xess_vk_image_view_info info = {};
  info.imageView = image.view;
  info.image = image.image;
  info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  info.format = image.format;
  info.width = image.width;
  info.height = image.height;
  return info;
}

class XessUpscaler final : public VendorUpscaler {
 public:
  static std::unique_ptr<VendorUpscaler> Create(const VulkanDevice* device) {
    const XessLibrary& xess = Xess();
    if (!xess.loaded) {
      REXLOG_INFO("XeSS unavailable: libxess.dll was not found");
      return nullptr;
    }
    xess_context_handle_t context = nullptr;
    if (xess.create_context(device->vulkan_instance()->instance(), device->physical_device(),
                            device->device(), &context) != XESS_RESULT_SUCCESS) {
      REXLOG_INFO("XeSS unavailable: context creation failed on this device");
      return nullptr;
    }
    REXLOG_INFO("XeSS Super Resolution available");
    return std::unique_ptr<VendorUpscaler>(new XessUpscaler(context));
  }
  ~XessUpscaler() override { Xess().destroy_context(context_); }
  const char* name() const override { return "XeSS"; }

  bool Evaluate(const Dispatch& d, const std::function<void()>& await_idle) override {
    const XessLibrary& xess = Xess();
    const uint32_t ow = d.output.width, oh = d.output.height;
    if (!initialized_ || ow != output_width_ || oh != output_height_) {
      if (initialized_) await_idle();
      const float ratio = std::max(Ratio(d.color.width, ow), Ratio(d.color.height, oh));
      xess_vk_init_params_t init = {};
      init.outputResolution = {ow, oh};
      // The input may be smaller than a mode's optimum, never larger.
      init.qualitySetting = ratio >= 2.5f   ? XESS_QUALITY_SETTING_ULTRA_PERFORMANCE
                            : ratio >= 1.85f ? XESS_QUALITY_SETTING_PERFORMANCE
                            : ratio >= 1.6f  ? XESS_QUALITY_SETTING_BALANCED
                            : ratio >= 1.4f  ? XESS_QUALITY_SETTING_QUALITY
                            : ratio >= 1.05f ? XESS_QUALITY_SETTING_ULTRA_QUALITY
                                             : XESS_QUALITY_SETTING_AA;
      init.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_LDR_INPUT_COLOR |
                       XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
      init.creationNodeMask = 1;
      init.visibleNodeMask = 1;
      if (xess.init(context_, &init) != XESS_RESULT_SUCCESS) {
        REXLOG_WARN("XeSS initialization failed for {}x{}", ow, oh);
        return false;
      }
      initialized_ = true;
      output_width_ = ow;
      output_height_ = oh;
    }
    xess_vk_execute_params_t execute = {};
    execute.colorTexture = XessImage(d.color);
    execute.velocityTexture = XessImage(d.motion_vectors);
    execute.depthTexture = XessImage(d.depth);
    if (d.reactive.image) execute.responsivePixelMaskTexture = XessImage(d.reactive);
    execute.outputTexture = XessImage(d.output);
    execute.jitterOffsetX = d.jitter_x;
    execute.jitterOffsetY = d.jitter_y;
    execute.exposureScale = 1.0f;
    execute.resetHistory = d.reset ? 1 : 0;
    execute.inputWidth = d.color.width;
    execute.inputHeight = d.color.height;
    return xess.execute(context_, d.command_buffer, &execute) == XESS_RESULT_SUCCESS;
  }

 private:
  explicit XessUpscaler(xess_context_handle_t context) : context_(context) {}
  xess_context_handle_t context_;
  bool initialized_ = false;
  uint32_t output_width_ = 0, output_height_ = 0;
};

// ORs requested VkBool32 features into an enabled structure where supported.
// Feature structures are sType, pNext, then only VkBool32 members.
template <typename T>
void MergeBool32(T* enabled, const T* supported, const void* requested, size_t first) {
  if (!enabled || !supported) return;
  auto* e = reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(enabled) + first);
  auto* s = reinterpret_cast<const VkBool32*>(reinterpret_cast<const uint8_t*>(supported) + first);
  auto* r = reinterpret_cast<const VkBool32*>(static_cast<const uint8_t*>(requested) + first);
  for (size_t i = 0; i < (sizeof(T) - first) / sizeof(VkBool32); ++i) {
    if (r[i] && s[i]) e[i] = VK_TRUE;
  }
}
#endif  // REX_HAS_XESS

}  // namespace

std::vector<std::string> GetVendorUpscalerInstanceExtensions() {
  std::vector<std::string> names;
#if defined(REX_HAS_DLSS) && REX_HAS_DLSS
  {
    NVSDK_NGX_FeatureDiscoveryInfo info = NgxDiscoveryInfo();
    uint32_t count = 0;
    VkExtensionProperties* extensions = nullptr;
    if (NVSDK_NGX_SUCCEED(
            NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &extensions))) {
      for (uint32_t i = 0; i < count; ++i) names.emplace_back(extensions[i].extensionName);
    }
  }
#endif
#if defined(REX_HAS_XESS) && REX_HAS_XESS
  if (const XessLibrary& xess = Xess(); xess.loaded) {
    uint32_t count = 0, min_api = 0;
    const char* const* extensions = nullptr;
    if (xess.get_instance_extensions(&count, &extensions, &min_api) == XESS_RESULT_SUCCESS) {
      for (uint32_t i = 0; i < count; ++i) names.emplace_back(extensions[i]);
    }
  }
#endif
  return names;
}

std::vector<std::string> GetVendorUpscalerDeviceExtensions(
    [[maybe_unused]] VkInstance instance, [[maybe_unused]] VkPhysicalDevice physical_device) {
  std::vector<std::string> names;
#if defined(REX_HAS_DLSS) && REX_HAS_DLSS
  {
    NVSDK_NGX_FeatureDiscoveryInfo info = NgxDiscoveryInfo();
    uint32_t count = 0;
    VkExtensionProperties* extensions = nullptr;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
            instance, physical_device, &info, &count, &extensions))) {
      for (uint32_t i = 0; i < count; ++i) names.emplace_back(extensions[i].extensionName);
    }
  }
#endif
#if defined(REX_HAS_XESS) && REX_HAS_XESS
  if (const XessLibrary& xess = Xess(); xess.loaded) {
    uint32_t count = 0;
    const char* const* extensions = nullptr;
    if (xess.get_device_extensions(instance, physical_device, &count, &extensions) ==
        XESS_RESULT_SUCCESS) {
      for (uint32_t i = 0; i < count; ++i) names.emplace_back(extensions[i]);
    }
  }
#endif
  return names;
}

void MergeVendorUpscalerFeatures([[maybe_unused]] VkInstance instance,
                                 [[maybe_unused]] VkPhysicalDevice physical_device,
                                 [[maybe_unused]] const VendorUpscalerFeatureTargets& t) {
#if defined(REX_HAS_XESS) && REX_HAS_XESS
  const XessLibrary& xess = Xess();
  if (!xess.loaded) return;
  void* chain = nullptr;
  if (xess.get_device_features(instance, physical_device, &chain) != XESS_RESULT_SUCCESS) return;
  // XeSS builds a fresh chain; fold it into the device's own structures so no
  // structure (or promoted alias of one) appears twice in VkDeviceCreateInfo.
  for (auto* node = static_cast<const VkBaseInStructure*>(chain); node; node = node->pNext) {
    switch (node->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: {
        const auto& requested = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->features;
        if (t.core_enabled && t.core_supported) {
          auto* e = reinterpret_cast<VkBool32*>(t.core_enabled);
          auto* s = reinterpret_cast<const VkBool32*>(t.core_supported);
          auto* r = reinterpret_cast<const VkBool32*>(&requested);
          for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); ++i)
            if (r[i] && s[i]) e[i] = VK_TRUE;
        }
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
        MergeBool32(t.enabled_1_1, t.supported_1_1, node,
                    offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess));
        break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
        MergeBool32(t.enabled_1_2, t.supported_1_2, node,
                    offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge));
        break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
        MergeBool32(t.enabled_1_3, t.supported_1_3, node,
                    offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess));
        break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: {
        auto* r = reinterpret_cast<const VkPhysicalDeviceShaderFloat16Int8Features*>(node);
        if (t.enabled_1_2 && t.supported_1_2) {
          if (r->shaderFloat16 && t.supported_1_2->shaderFloat16) t.enabled_1_2->shaderFloat16 = VK_TRUE;
          if (r->shaderInt8 && t.supported_1_2->shaderInt8) t.enabled_1_2->shaderInt8 = VK_TRUE;
        }
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES: {
        auto* r = reinterpret_cast<const VkPhysicalDeviceSubgroupSizeControlFeatures*>(node);
        if (t.enabled_1_3 && t.supported_1_3) {
          if (r->subgroupSizeControl && t.supported_1_3->subgroupSizeControl)
            t.enabled_1_3->subgroupSizeControl = VK_TRUE;
          if (r->computeFullSubgroups && t.supported_1_3->computeFullSubgroups)
            t.enabled_1_3->computeFullSubgroups = VK_TRUE;
        }
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES: {
        auto* r = reinterpret_cast<const VkPhysicalDeviceShaderIntegerDotProductFeatures*>(node);
        if (t.enabled_1_3 && t.supported_1_3 && r->shaderIntegerDotProduct &&
            t.supported_1_3->shaderIntegerDotProduct)
          t.enabled_1_3->shaderIntegerDotProduct = VK_TRUE;
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT: {
        auto* r = reinterpret_cast<const VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT*>(node);
        if (t.enabled_mutable_descriptor_type && t.supported_mutable_descriptor_type &&
            r->mutableDescriptorType && t.supported_mutable_descriptor_type->mutableDescriptorType)
          t.enabled_mutable_descriptor_type->mutableDescriptorType = VK_TRUE;
        break;
      }
      default:
        REXLOG_INFO("XeSS requests feature structure {} (not merged)", uint32_t(node->sType));
        break;
    }
  }
#endif
}

std::unique_ptr<VendorUpscaler> VendorUpscaler::Create(VendorUpscalerKind kind,
                                                       [[maybe_unused]] const VulkanDevice* device) {
  switch (kind) {
    case VendorUpscalerKind::kDlss:
#if defined(REX_HAS_DLSS) && REX_HAS_DLSS
      return DlssUpscaler::Create(device);
#else
      REXLOG_INFO("DLSS unavailable: this build has no DLSS SDK");
      return nullptr;
#endif
    case VendorUpscalerKind::kXess:
#if defined(REX_HAS_XESS) && REX_HAS_XESS
      return XessUpscaler::Create(device);
#else
      REXLOG_INFO("XeSS unavailable: this build has no XeSS SDK (Windows only)");
      return nullptr;
#endif
  }
  return nullptr;
}

}  // namespace rex::ui::vulkan
