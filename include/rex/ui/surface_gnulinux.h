#pragma once
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

#include <rex/ui/surface.h>

#include <xcb/xcb.h>
#include <rex/ui/hdr_display.h>
#include <memory>
#include <functional>
struct _GtkWidget;
struct _GdkWindow;
struct wl_surface;
struct wl_subsurface;
struct wp_viewport;
struct wp_fractional_scale_v1;
struct wp_color_manager_v1;
struct wp_color_management_output_v1;

namespace rex {
namespace ui {

class WaylandWindowSurface final : public Surface {
 public:
  static std::unique_ptr<WaylandWindowSurface> Create(_GtkWidget* widget, std::function<void()> changed);
  ~WaylandWindowSurface() override;
  TypeIndex GetType() const override { return kTypeIndex_WaylandWindow; }
  wl_display* display() const { return display_; }
  wl_surface* surface() const { return surface_; }
  HdrDisplay GetHdrDisplay() const;
 protected:
  bool GetSizeImpl(uint32_t& width, uint32_t& height) const override;
 private:
  void ScheduleUpdate();
  void EnterOutput(wl_output* output);
  std::function<void()> changed_;
  unsigned update_source_ = 0;
  uint32_t fractional_scale_ = 0;
  wp_viewport* viewport_ = nullptr;
  wp_fractional_scale_v1* fractional_ = nullptr;
  wp_color_manager_v1* color_manager_ = nullptr;
  wp_color_management_output_v1* color_output_ = nullptr;
  wl_output* output_ = nullptr;
  _GtkWidget* widget_ = nullptr;
  _GdkWindow* parent_window_ = nullptr;
  wl_display* display_ = nullptr;
  wl_surface* surface_ = nullptr;
  wl_subsurface* subsurface_ = nullptr;
};

class XcbWindowSurface final : public Surface {
 public:
  explicit XcbWindowSurface(xcb_connection_t* connection, xcb_window_t window)
      : connection_(connection), window_(window) {}
  TypeIndex GetType() const override { return kTypeIndex_XcbWindow; }
  xcb_connection_t* connection() const { return connection_; }
  xcb_window_t window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  xcb_connection_t* connection_;
  xcb_window_t window_;
};

}  // namespace ui
}  // namespace rex
