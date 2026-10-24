#include <rex/ui/hdr_display.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wayland/color-management-client.h"

namespace rex::ui {
namespace {
struct Output {
  HdrDisplay result;
  wl_output* output = nullptr;
  wp_color_management_output_v1* color = nullptr;
  wp_image_description_v1* image = nullptr;
  wp_image_description_info_v1* info = nullptr;
  bool own_output = false;
  bool complete = false;
  bool pq = false;
  ~Output() {
    if (info) wp_image_description_info_v1_destroy(info);
    if (image) wp_image_description_v1_destroy(image);
    if (color) wp_color_management_output_v1_destroy(color);
    if (own_output) wl_output_destroy(output);
  }
};
const wp_image_description_info_v1_listener info_listener = {
  .done = [](void* p, wp_image_description_info_v1* info) {
    auto& o = *static_cast<Output*>(p);
    o.complete = true;
    o.info = nullptr;
    wp_image_description_info_v1_destroy(info);
  },
  .icc_file = [](void*, wp_image_description_info_v1*, int32_t fd, uint32_t) { close(fd); },
  .primaries = [](void*, wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {},
  .primaries_named = [](void*, wp_image_description_info_v1*, uint32_t) {},
  .tf_power = [](void*, wp_image_description_info_v1*, uint32_t) {},
  .tf_named = [](void* p, wp_image_description_info_v1*, uint32_t tf) {
    static_cast<Output*>(p)->pq = tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
  },
  .luminances = [](void*, wp_image_description_info_v1*, uint32_t, uint32_t, uint32_t) {},
  .target_primaries = [](void*, wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {},
  .target_luminance = [](void* p, wp_image_description_info_v1*, uint32_t, uint32_t max) {
    static_cast<Output*>(p)->result.peak_nits = float(max);
  },
  .target_max_cll = [](void*, wp_image_description_info_v1*, uint32_t) {},
  .target_max_fall = [](void*, wp_image_description_info_v1*, uint32_t) {},
};
const wp_image_description_v1_listener image_listener = {
  .failed = [](void* p, wp_image_description_v1*, uint32_t, const char*) { static_cast<Output*>(p)->complete = true; },
  .ready = [](void* p, wp_image_description_v1* image, uint32_t) {
    auto& o = *static_cast<Output*>(p);
    o.info = wp_image_description_v1_get_information(image);
    wp_image_description_info_v1_add_listener(o.info, &info_listener, p);
  },
};
// wl_output_listener as of version 4. Declared here because headers older than
// libwayland 1.20 lack the name and description events; libwayland dispatches
// by index into this table either way.
struct OutputListener {
  void (*geometry)(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*,
                   const char*, int32_t);
  void (*mode)(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t);
  void (*done)(void*, wl_output*);
  void (*scale)(void*, wl_output*, int32_t);
  void (*name)(void*, wl_output*, const char*);
  void (*description)(void*, wl_output*, const char*);
};
const OutputListener output_listener = {
  .geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {},
  .mode = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
  .done = [](void*, wl_output*) {},
  .scale = [](void*, wl_output*, int32_t) {},
  .name = [](void* p, wl_output*, const char* name) { static_cast<Output*>(p)->result.name = name; },
  .description = [](void* p, wl_output*, const char* text) { static_cast<Output*>(p)->result.description = text; },
};
struct Probe {
  wp_color_manager_v1* manager = nullptr;
  bool supports_pq = false;
  bool specific_output = false;
  std::vector<std::unique_ptr<Output>> outputs;
};
const wp_color_manager_v1_listener manager_listener = {
  .supported_intent = [](void*, wp_color_manager_v1*, uint32_t) {},
  .supported_feature = [](void*, wp_color_manager_v1*, uint32_t) {},
  .supported_tf_named = [](void* p, wp_color_manager_v1*, uint32_t tf) {
    if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) static_cast<Probe*>(p)->supports_pq = true;
  },
  .supported_primaries_named = [](void*, wp_color_manager_v1*, uint32_t) {},
  .done = [](void*, wp_color_manager_v1*) {},
};
const wl_registry_listener registry_listener = {
  .global = [](void* p, wl_registry* registry, uint32_t name, const char* type, uint32_t version) {
    auto& probe = *static_cast<Probe*>(p);
    if (!std::strcmp(type, "wp_color_manager_v1")) {
      // v1 is sufficient and avoids relying on newer ready2 events.
      probe.manager = static_cast<wp_color_manager_v1*>(wl_registry_bind(registry, name, &wp_color_manager_v1_interface, 1));
      wp_color_manager_v1_add_listener(probe.manager, &manager_listener, p);
    } else if (!probe.specific_output && !std::strcmp(type, "wl_output")) {
      auto o = std::make_unique<Output>();
      // Never above what the loaded libwayland knows: events beyond its
      // interface are a fatal protocol error.
      const uint32_t bind_version =
          std::min({version, 4u, uint32_t(wl_output_interface.version)});
      o->output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, bind_version));
      o->own_output = true;
      wl_output_add_listener(o->output, reinterpret_cast<const wl_output_listener*>(&output_listener), o.get());
      probe.outputs.push_back(std::move(o));
    }
  },
  .global_remove = [](void*, wl_registry*, uint32_t) {},
};
std::vector<HdrDisplay> ProbeDisplay(wl_display* display, wl_output* selected) {
  std::vector<HdrDisplay> results;
  if (!display) return results;
  auto* queue = wl_display_create_queue(display);
  if (!queue) return results;
  auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
  auto* registry = wl_display_get_registry(wrapper);
  wl_proxy_wrapper_destroy(wrapper);
  Probe probe;
  probe.specific_output = selected != nullptr;
  if (selected) {
    probe.outputs.push_back(std::make_unique<Output>());
    probe.outputs.back()->output = selected;
  }
  wl_registry_add_listener(registry, &registry_listener, &probe);
  bool ok = WaylandRoundtrip(display, queue) && WaylandRoundtrip(display, queue);
  if (ok && probe.manager && probe.supports_pq) {
    for (auto& o : probe.outputs) {
      o->color = wp_color_manager_v1_get_output(probe.manager, o->output);
      static const wp_color_management_output_v1_listener changed = {[](void*, wp_color_management_output_v1*) {}};
      wp_color_management_output_v1_add_listener(o->color, &changed, o.get());
      o->image = wp_color_management_output_v1_get_image_description(o->color);
      wp_image_description_v1_add_listener(o->image, &image_listener, o.get());
    }
    ok = WaylandRoundtrip(display, queue) && WaylandRoundtrip(display, queue);
  }
  for (auto& o : probe.outputs) {
    o->result.hdr = ok && probe.supports_pq && o->complete && o->pq;
    results.push_back(o->result);
  }
  probe.outputs.clear();
  if (probe.manager) wp_color_manager_v1_destroy(probe.manager);
  wl_registry_destroy(registry);
  wl_event_queue_destroy(queue);
  return results;
}
}
bool WaylandRoundtrip(wl_display* display, wl_event_queue* queue, int timeout_ms) {
  auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
  auto* callback = wl_display_sync(wrapper);
  wl_proxy_wrapper_destroy(wrapper);
  bool done = false;
  static const wl_callback_listener listener = {[](void* p, wl_callback*, uint32_t) { *static_cast<bool*>(p) = true; }};
  wl_callback_add_listener(callback, &listener, &done);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool ok = true;
  while (!done && ok) {
    if ((queue ? wl_display_dispatch_queue_pending(display, queue) : wl_display_dispatch_pending(display)) < 0) { ok = false; break; }
    if (done) break;
    if (std::chrono::steady_clock::now() >= deadline) { ok = false; break; }
    if ((queue ? wl_display_prepare_read_queue(display, queue) : wl_display_prepare_read(display)) != 0) continue;
    wl_display_flush(display);
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    pollfd fd{wl_display_get_fd(display), POLLIN, 0};
    if (poll(&fd, 1, int(std::max<int64_t>(1, remaining))) <= 0 || !(fd.revents & POLLIN)) {
      wl_display_cancel_read(display); ok = false; break;
    }
    if (wl_display_read_events(display) < 0) { ok = false; break; }
  }
  wl_callback_destroy(callback);
  return ok && done;
}
std::vector<HdrDisplay> ProbeHdrDisplays() {
  auto* display = wl_display_connect(nullptr);
  auto result = ProbeDisplay(display, nullptr);
  if (display) wl_display_disconnect(display);
  return result;
}
HdrDisplay ProbeHdrOutput(wl_display* display, wl_output* output) {
  if (!output) return {};
  auto result = ProbeDisplay(display, output);
  return result.empty() ? HdrDisplay{} : result.front();
}
}
