#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

namespace rex::ui::vulkan {

// Optional observation of the existing paint command buffer. This helper never
// submits, waits, or inserts memory barriers. Only an existing submission fence
// may authorize query readback and slot reuse. Timestamps are not clock-calibrated.
class PresenterGpuTiming final {
 public:
  static constexpr uint32_t kSlots = 3;
  struct Shape {
    uint32_t width = 0, height = 0, source_width = 0, source_height = 0;
    uint32_t effects = 0, first_effect = UINT32_MAX;
    bool hdr = false, ui = false;
    bool operator==(const Shape& b) const {
      return width == b.width && height == b.height && source_width == b.source_width &&
             source_height == b.source_height && effects == b.effects &&
             first_effect == b.first_effect && hdr == b.hdr && ui == b.ui;
    }
  };

  static std::unique_ptr<PresenterGpuTiming> Create(const VulkanDevice* device) {
    auto result = std::unique_ptr<PresenterGpuTiming>(new PresenterGpuTiming(device));
    return result->Initialize() ? std::move(result) : nullptr;
  }

  ~PresenterGpuTiming() {
    Flush();
    if (pool_ == VK_NULL_HANDLE) return;
    for (const auto& slot : slots_) {
      if (slot.pending) {
        // Failed/uncertain GPU completion must not free a possibly referenced
        // query pool. No diagnostic-only blocking recovery is allowed. Leave
        // this one pool to terminal device cleanup after reporting the failure.
        REXLOG_ERROR("Presenter GPU timing: query pool retained after unproven completion; samples invalid");
        return;
      }
    }
    device_->functions().vkDestroyQueryPool(device_->device(), pool_, nullptr);
  }

  bool Begin(VkCommandBuffer command, uint64_t submission, Shape shape) {
    auto& slot = slots_[submission % kSlots];
    slot.recorded = false;
    if (!enabled_) return false;
    if (slot.pending) {
      enabled_ = false;
      REXLOG_ERROR("Presenter GPU timing: slot not fence-retired; observation disabled");
      return false;
    }
    slot = {};
    slot.submission = submission;
    slot.shape = shape;
    slot.begin_steady_ns = NowNs();
    slot.recorded = true;
    const uint32_t first = uint32_t(submission % kSlots) * 2;
    device_->functions().vkCmdResetQueryPool(command, pool_, first, 2);
    write_timestamp_(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_, first);
    return true;
  }

  void SetGuestFlow(uint64_t submission, uint32_t width, uint32_t height,
                    uint32_t effects, uint32_t first_effect) {
    auto& slot = slots_[submission % kSlots];
    if (!slot.recorded || slot.submission != submission) return;
    slot.shape.source_width = width;
    slot.shape.source_height = height;
    slot.shape.effects = effects;
    slot.shape.first_effect = first_effect;
  }

  void End(VkCommandBuffer command, uint64_t submission) {
    const auto& slot = slots_[submission % kSlots];
    if (!slot.recorded || slot.submission != submission) return;
    write_timestamp_(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_,
                     uint32_t(submission % kSlots) * 2 + 1);
  }

  void Submitted(uint64_t submission, bool success) {
    auto& slot = slots_[submission % kSlots];
    if (!slot.recorded || slot.submission != submission) return;
    slot.recorded = false;
    // On failed submission, conservatively retain ownership until a later real
    // fence proves completion. Never reuse uncertain slots or report their data.
    slot.pending = true;
    slot.valid = success;
    if (!success) {
      enabled_ = false;
      REXLOG_ERROR("Presenter GPU timing: submit failed; observation disabled");
    }
  }

  void Retire(uint64_t completed_submission) {
    // Three fixed slots, sorted by submission to keep resize/flow groups and
    // chronological summary endpoints distinct even during a shutdown drain.
    std::array<Slot*, kSlots> ordered{{&slots_[0], &slots_[1], &slots_[2]}};
    std::sort(ordered.begin(), ordered.end(), [](const Slot* a, const Slot* b) {
      return a->submission < b->submission;
    });
    for (Slot* slot : ordered) {
      if (!slot->pending || slot->submission > completed_submission) continue;
      slot->pending = false;  // Lifetime proven by the original paint fence.
      if (!slot->valid) continue;
      struct Result { uint64_t value, available; } values[2]{};
      const VkResult result = get_results_(device_->device(), pool_,
          uint32_t(slot->submission % kSlots) * 2, 2, sizeof(values), values,
          sizeof(Result), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      const uint64_t retired_ns = NowNs();
      const double host_age_ns = double(retired_ns - slot->begin_steady_ns);
      const double wrap_ns = std::ldexp(double(period_ns_), int(valid_bits_));
      if (result != VK_SUCCESS || !values[0].available || !values[1].available ||
          !(host_age_ns < wrap_ns)) {
        enabled_ = false;
        ++invalid_samples_;
        REXLOG_ERROR("Presenter GPU timing: unavailable or ambiguous retired query; result={} observation disabled", int(result));
        continue;
      }
      const uint64_t mask = valid_bits_ == 64 ? UINT64_MAX : (uint64_t(1) << valid_bits_) - 1;
      const uint64_t ticks = (values[1].value - values[0].value) & mask;
      Add(*slot, double(ticks) * double(period_ns_) / 1000000.0, retired_ns);
    }
  }

  void Flush() {
    if (!sample_count_) return;
    auto sorted = samples_ms_;
    std::sort(sorted.begin(), sorted.begin() + sample_count_);
    REXLOG_INFO("Presenter GPU timing: samples={} first_submission={} last_submission={} output={}x{} source={}x{} hdr={} ui={} effects={} first_effect={} mean_ms={:.6f} p50_ms={:.6f} p95_ms={:.6f} max_ms={:.6f} first_record_steady_ns={} last_retired_steady_ns={} invalid_samples={} scope=paint_inclusive_uncalibrated",
        sample_count_, first_submission_, last_submission_, shape_.width, shape_.height,
        shape_.source_width, shape_.source_height, shape_.hdr, shape_.ui,
        shape_.effects, shape_.first_effect, sum_ms_ / double(sample_count_),
        sorted[(sample_count_ - 1) / 2], sorted[(sample_count_ * 95 + 99) / 100 - 1],
        sorted[sample_count_ - 1], first_record_ns_, last_retired_ns_, invalid_samples_);
    sample_count_ = 0;
    sum_ms_ = 0;
  }

 private:
  struct Slot {
    uint64_t submission = 0, begin_steady_ns = 0;
    Shape shape;
    bool recorded = false, pending = false, valid = false;
  };
  explicit PresenterGpuTiming(const VulkanDevice* device) : device_(device) {}
  static uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  bool Initialize() {
    const auto& ifn = device_->vulkan_instance()->functions();
    VkPhysicalDeviceProperties properties{};
    ifn.vkGetPhysicalDeviceProperties(device_->physical_device(), &properties);
    period_ns_ = properties.limits.timestampPeriod;
    uint32_t count = 0;
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(device_->physical_device(), &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(device_->physical_device(), &count, families.data());
    const uint32_t family = device_->queue_family_graphics_compute();
    if (family < count) valid_bits_ = families[family].timestampValidBits;
    write_timestamp_ = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
        ifn.vkGetDeviceProcAddr(device_->device(), "vkCmdWriteTimestamp"));
    get_results_ = reinterpret_cast<PFN_vkGetQueryPoolResults>(
        ifn.vkGetDeviceProcAddr(device_->device(), "vkGetQueryPoolResults"));
    if (!valid_bits_ || valid_bits_ > 64 || !std::isfinite(period_ns_) ||
        period_ns_ <= 0 || !write_timestamp_ || !get_results_) {
      REXLOG_WARN("Presenter GPU timing: timestamp support unavailable; observation disabled");
      return false;
    }
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kSlots * 2;
    if (device_->functions().vkCreateQueryPool(device_->device(), &info, nullptr, &pool_) != VK_SUCCESS) {
      REXLOG_WARN("Presenter GPU timing: query pool creation failed; observation disabled");
      return false;
    }
    enabled_ = true;
    REXLOG_INFO("Presenter GPU timing enabled: slots={} period_ns={} valid_bits={} scope=scaler_UI_HDR_inclusive; includes GPU dependencies/acquire-semaphore wait; excludes CPU acquire/present, SDK gamma, compositor; no added waits", kSlots, period_ns_, valid_bits_);
    return true;
  }
  void Add(const Slot& slot, double milliseconds, uint64_t retired_ns) {
    if (sample_count_ && !(shape_ == slot.shape)) Flush();
    if (!sample_count_) {
      shape_ = slot.shape;
      first_submission_ = slot.submission;
      first_record_ns_ = slot.begin_steady_ns;
    }
    last_submission_ = slot.submission;
    last_retired_ns_ = retired_ns;
    samples_ms_[sample_count_++] = milliseconds;
    sum_ms_ += milliseconds;
    if (sample_count_ == samples_ms_.size()) Flush();
  }

  const VulkanDevice* device_;
  VkQueryPool pool_ = VK_NULL_HANDLE;
  PFN_vkCmdWriteTimestamp write_timestamp_ = nullptr;
  PFN_vkGetQueryPoolResults get_results_ = nullptr;
  float period_ns_ = 0;
  uint32_t valid_bits_ = 0;
  bool enabled_ = false;
  std::array<Slot, kSlots> slots_{};
  std::array<double, 120> samples_ms_{};
  size_t sample_count_ = 0;
  double sum_ms_ = 0;
  Shape shape_;
  uint64_t first_submission_ = 0, last_submission_ = 0;
  uint64_t first_record_ns_ = 0, last_retired_ns_ = 0, invalid_samples_ = 0;
};

}  // namespace rex::ui::vulkan
