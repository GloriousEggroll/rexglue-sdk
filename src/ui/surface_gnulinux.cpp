/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdlib>

#include <rex/ui/surface_gnulinux.h>
#include <cstring>
#include <algorithm>
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include <wayland-client.h>
#include "wayland/viewporter-client.h"
#include "wayland/fractional-scale-client.h"
#include "wayland/color-management-client.h"

namespace rex {
namespace ui {

std::unique_ptr<WaylandWindowSurface> WaylandWindowSurface::Create(
    GtkWidget* widget, std::function<void()> changed) {
  auto result = std::make_unique<WaylandWindowSurface>();
  result->changed_ = std::move(changed);
  result->widget_ = widget;
  result->display_ = gdk_wayland_display_get_wl_display(gtk_widget_get_display(widget));
  auto* top = gtk_widget_get_toplevel(widget);
  auto* parent = gdk_wayland_window_get_wl_surface(gtk_widget_get_window(top));
  if (!result->display_ || !parent) return nullptr;
  struct Globals {
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_fractional_scale_manager_v1* fractional = nullptr;
    wp_color_manager_v1* color = nullptr;
  } globals;
  auto* queue = wl_display_create_queue(result->display_);
  auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(result->display_));
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
  auto* registry = wl_display_get_registry(wrapper);
  wl_proxy_wrapper_destroy(wrapper);
  static const wl_registry_listener listener = {
    [](void* p, wl_registry* r, uint32_t id, const char* type, uint32_t version) {
      auto& g = *static_cast<Globals*>(p);
      if (!std::strcmp(type, "wl_compositor")) g.compositor = static_cast<wl_compositor*>(wl_registry_bind(r, id, &wl_compositor_interface, std::min(version, 4u)));
      if (!std::strcmp(type, "wl_subcompositor")) g.subcompositor = static_cast<wl_subcompositor*>(wl_registry_bind(r, id, &wl_subcompositor_interface, 1));
      if (!std::strcmp(type, "wp_viewporter")) g.viewporter = static_cast<wp_viewporter*>(wl_registry_bind(r, id, &wp_viewporter_interface, 1));
      if (!std::strcmp(type, "wp_fractional_scale_manager_v1")) g.fractional = static_cast<wp_fractional_scale_manager_v1*>(wl_registry_bind(r, id, &wp_fractional_scale_manager_v1_interface, 1));
      if (!std::strcmp(type, "wp_color_manager_v1")) g.color = static_cast<wp_color_manager_v1*>(wl_registry_bind(r, id, &wp_color_manager_v1_interface, 1));
    }, [](void*, wl_registry*, uint32_t) {}};
  wl_registry_add_listener(registry, &listener, &globals);
  if (WaylandRoundtrip(result->display_, queue) && globals.compositor && globals.subcompositor) {
    result->surface_ = wl_compositor_create_surface(globals.compositor);
    // GTK owns the parent and all input; the child only carries Vulkan pixels.
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(result->surface_), nullptr);
    static const wl_surface_listener surface_listener = {
      [](void* p, wl_surface*, wl_output* output) { static_cast<WaylandWindowSurface*>(p)->EnterOutput(output); },
      [](void* p, wl_surface*, wl_output*) { static_cast<WaylandWindowSurface*>(p)->ScheduleUpdate(); }};
    wl_surface_add_listener(result->surface_, &surface_listener, result.get());
    if (globals.color) {
      result->color_manager_ = globals.color;
      globals.color = nullptr;
      wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(result->color_manager_), nullptr);
      static const wp_color_manager_v1_listener color_listener = {
        [](void*, wp_color_manager_v1*, uint32_t) {},
        [](void*, wp_color_manager_v1*, uint32_t) {},
        [](void*, wp_color_manager_v1*, uint32_t) {},
        [](void*, wp_color_manager_v1*, uint32_t) {},
        [](void*, wp_color_manager_v1*) {}};
      wp_color_manager_v1_add_listener(result->color_manager_, &color_listener, nullptr);
    }
    if (globals.viewporter && globals.fractional) {
      result->viewport_ = wp_viewporter_get_viewport(globals.viewporter, result->surface_);
      result->fractional_ = wp_fractional_scale_manager_v1_get_fractional_scale(globals.fractional, result->surface_);
      wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(result->viewport_), nullptr);
      wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(result->fractional_), nullptr);
      static const wp_fractional_scale_v1_listener scale_listener = {
        [](void* p, wp_fractional_scale_v1*, uint32_t scale) {
          auto* self = static_cast<WaylandWindowSurface*>(p);
          if (scale && self->fractional_scale_ != scale) {
            self->fractional_scale_ = scale;
            self->ScheduleUpdate();
          }
        }};
      wp_fractional_scale_v1_add_listener(result->fractional_, &scale_listener, result.get());
    }
    auto* region = wl_compositor_create_region(globals.compositor);
    wl_surface_set_input_region(result->surface_, region);
    wl_region_destroy(region);
    result->subsurface_ = wl_subcompositor_get_subsurface(globals.subcompositor, result->surface_, parent);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(result->subsurface_), nullptr);
    wl_subsurface_set_desync(result->subsurface_);
    // The Vulkan image can completely cover GTK's surface. Compositors may
    // then stop sending GTK frame callbacks, leaving its committed geometry
    // and clipping at the old size even though our swapchain has been resized.
    // Let Vulkan presentation also wake GTK's frame clock so parent geometry
    // and subsurface positions are committed together with the new layout.
    result->parent_window_ = GDK_WINDOW(g_object_ref(gtk_widget_get_window(top)));
    gdk_wayland_window_add_frame_callback_surface(result->parent_window_, result->surface_);
    uint32_t width, height;
    result->GetSizeImpl(width, height);
    gtk_widget_queue_draw(top);
  }
  if (globals.color) wp_color_manager_v1_destroy(globals.color);
  if (globals.fractional) wp_fractional_scale_manager_v1_destroy(globals.fractional);
  if (globals.viewporter) wp_viewporter_destroy(globals.viewporter);
  if (globals.subcompositor) wl_subcompositor_destroy(globals.subcompositor);
  if (globals.compositor) wl_compositor_destroy(globals.compositor);
  wl_registry_destroy(registry);
  wl_event_queue_destroy(queue);
  return result->surface_ ? std::move(result) : nullptr;
}
WaylandWindowSurface::~WaylandWindowSurface() {
  if (update_source_) g_source_remove(update_source_);
  if (parent_window_) {
    gdk_wayland_window_remove_frame_callback_surface(parent_window_, surface_);
    g_object_unref(parent_window_);
  }
  if (color_output_) wp_color_management_output_v1_destroy(color_output_);
  if (color_manager_) wp_color_manager_v1_destroy(color_manager_);
  if (fractional_) wp_fractional_scale_v1_destroy(fractional_);
  if (viewport_) wp_viewport_destroy(viewport_);
  if (subsurface_) wl_subsurface_destroy(subsurface_);
  if (surface_) wl_surface_destroy(surface_);
}
void WaylandWindowSurface::ScheduleUpdate() {
  if (update_source_) return;
  update_source_ = g_idle_add([](void* p) -> gboolean {
    auto* self = static_cast<WaylandWindowSurface*>(p);
    self->update_source_ = 0;
    self->changed_();
    return G_SOURCE_REMOVE;
  }, this);
}
void WaylandWindowSurface::EnterOutput(wl_output* output) {
  if (output_ == output) return;
  output_ = output;
  if (color_output_) wp_color_management_output_v1_destroy(color_output_);
  color_output_ = nullptr;
  if (color_manager_) {
    color_output_ = wp_color_manager_v1_get_output(color_manager_, output);
    static const wp_color_management_output_v1_listener listener = {
      [](void* p, wp_color_management_output_v1*) { static_cast<WaylandWindowSurface*>(p)->ScheduleUpdate(); }};
    wp_color_management_output_v1_add_listener(color_output_, &listener, this);
  }
  ScheduleUpdate();
}
bool WaylandWindowSurface::GetSizeImpl(uint32_t& width, uint32_t& height) const {
  const int scale = gtk_widget_get_scale_factor(widget_);
  const uint32_t logical_width = std::max(0, gtk_widget_get_allocated_width(widget_));
  const uint32_t logical_height = std::max(0, gtk_widget_get_allocated_height(widget_));
  const uint32_t scale120 = viewport_ && fractional_scale_ ? fractional_scale_ : scale * 120;
  width = (logical_width * scale120 + 60) / 120;
  height = (logical_height * scale120 + 60) / 120;
  int x = 0, y = 0;
  gtk_widget_translate_coordinates(widget_, gtk_widget_get_toplevel(widget_), 0, 0, &x, &y);
  wl_subsurface_set_position(subsurface_, x, y);
  wl_surface_set_buffer_scale(surface_, viewport_ ? 1 : scale);
  if (viewport_ && logical_width && logical_height)
    wp_viewport_set_destination(viewport_, logical_width, logical_height);
  return width && height;
}
HdrDisplay WaylandWindowSurface::GetHdrDisplay() const {
  auto* display = gtk_widget_get_display(widget_);
  auto* monitor = gdk_display_get_monitor_at_window(display, gtk_widget_get_window(gtk_widget_get_toplevel(widget_)));
  return monitor ? ProbeHdrOutput(display_, gdk_wayland_monitor_get_wl_output(monitor)) : HdrDisplay{};
}

bool XcbWindowSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  xcb_get_geometry_reply_t* reply =
      xcb_get_geometry_reply(connection_, xcb_get_geometry(connection_, window_), nullptr);
  if (!reply) {
    return false;
  }
  width_out = reply->width;
  height_out = reply->height;
  std::free(reply);
  return true;
}

}  // namespace ui
}  // namespace rex
