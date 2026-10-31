/**
 * @file        rex/ui/window_sdl.h
 * @brief       Window on SDL3, presenting through a Metal layer (macOS).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <rex/ui/menu_item.h>
#include <rex/ui/surface.h>
#include <rex/ui/window.h>

struct SDL_Window;
union SDL_Event;

namespace rex {
namespace ui {

// A CAMetalLayer backing an SDL window, for VK_EXT_metal_surface.
class MetalLayerSurface final : public Surface {
 public:
  MetalLayerSurface(SDL_Window* window, void* metal_view, void* layer)
      : window_(window), metal_view_(metal_view), layer_(layer) {}
  ~MetalLayerSurface() override;

  TypeIndex GetType() const override { return kTypeIndex_MetalLayer; }
  void* layer() const { return layer_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  SDL_Window* window_;
  void* metal_view_;
  void* layer_;
};

class SDLWindow : public Window {
  using super = Window;

 public:
  SDLWindow(WindowedAppContext& app_context, const std::string_view title,
            uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~SDLWindow() override;

  double GetMonitorRefreshRateHz() const override;
  std::string GetDisplayOutputName() const override;
  void* GetNativeWindowHandle() const override { return window_; }

  SDL_Window* window() const { return window_; }

  // Dispatches an SDL event that belongs to this window, or to all windows.
  static void DispatchEvent(const SDL_Event& event);

 protected:
  uint32_t GetLatestDpiImpl() const override;

  bool OpenImpl() override;
  void RequestCloseImpl() override;

  void ApplyNewFullscreen() override;
  void ApplyNewLogicalSize() override;
  void ApplyNewTitle() override;
  // While captured the pointer is hidden and locked in place, and its motion
  // is reported through OnRelativeMouseMove.
  void ApplyNewMouseCapture() override;
  void ApplyNewMouseRelease() override;
  void ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) override;
  void FocusImpl() override;

  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  void HandleEvent(const SDL_Event& event);
  void HandleSizeUpdate(WindowDestructionReceiver& destruction_receiver);
  void HandleKey(const SDL_Event& event, WindowDestructionReceiver& destruction_receiver);
  void HandleMouse(const SDL_Event& event, WindowDestructionReceiver& destruction_receiver);
  void Close();

  SDL_Window* window_ = nullptr;
  uint32_t window_id_ = 0;
  std::atomic<bool> paint_pending_{false};
  double motion_remainder_x_ = 0.0;
  double motion_remainder_y_ = 0.0;
};

// Menus are not shown; the app uses its own in-game settings.
class SDLMenuItem : public MenuItem {
 public:
  SDLMenuItem(Type type, const std::string& text, const std::string& hotkey,
              std::function<void()> callback)
      : MenuItem(type, text, hotkey, std::move(callback)) {}
};

}  // namespace ui
}  // namespace rex
