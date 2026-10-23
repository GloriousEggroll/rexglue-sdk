#pragma once
// Per-frame inputs for temporal upscaling and frame generation, produced by a
// renderer that knows the scene (depth, motion, jitter) and consumed by the
// presenter. All images live on the presenter's VkDevice and are in
// VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL once the producing submission has
// executed; `keep_alive` owns them until the presenter's use retires.

#include <atomic>
#include <cstdint>
#include <memory>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {

struct TemporalUpscaleImage {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
};

struct TemporalUpscaleInputs {
  uint32_t width = 0, height = 0;  // Render size; the guest output size.
  // Scene depth, one channel, larger is nearer when `inverted_depth`.
  TemporalUpscaleImage depth;
  // Two channels in pixels: (previous position - current position).
  TemporalUpscaleImage motion_vectors;
  // One channel 0..1; 1 where the image must not accumulate history (HUD).
  TemporalUpscaleImage reactive;
  // Optional: the frame before the HUD, same format as the guest output.
  TemporalUpscaleImage hudless_color;
  // Sub-pixel camera jitter in pixels, FidelityFX sign convention.
  float jitter_x = 0, jitter_y = 0;
  float frame_time_ms = 16.667f;
  float camera_near = 1, camera_far = 1e9f, fov_y_radians = 1;
  float view_space_to_meters = 1;
  bool inverted_depth = true, infinite_depth = true;
  // History is invalid (camera cut, load, first frame).
  bool reset = false;
  uint64_t frame_id = 0;
  std::shared_ptr<const void> keep_alive;
};

// What the presenter asks of the producer. The producer jitters and captures
// inputs only while `active`, and uses `jitter_phase_count` jitter positions.
struct TemporalUpscaleRequest {
  bool active = false;
  uint32_t jitter_phase_count = 8;
};

void SetTemporalUpscaleRequest(const TemporalUpscaleRequest& request);
TemporalUpscaleRequest GetTemporalUpscaleRequest();

}  // namespace rex::ui::vulkan
