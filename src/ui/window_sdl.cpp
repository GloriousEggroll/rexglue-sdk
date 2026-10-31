/**
 * @file        ui/window_sdl.cpp
 * @brief       Window on SDL3, presenting through a Metal layer (macOS).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/logging.h>
#include <rex/ui/flags.h>
#include <rex/ui/presenter.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window_sdl.h>
#include <rex/ui/windowed_app_context_sdl.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

namespace {

uint32_t ResolveWindowWidth(uint32_t requested_width) {
  if (REXCVAR_GET(window_width) > 0) {
    return uint32_t(REXCVAR_GET(window_width));
  }
  if (!rex::cvar::HasNonDefaultValue("window_width")) {
    if (rex::cvar::HasNonDefaultValue("video_mode_width") && REXCVAR_GET(video_mode_width) > 0) {
      return uint32_t(std::clamp(REXCVAR_GET(video_mode_width), 1, 8192));
    }
    int32_t preset_width = 0;
    int32_t preset_height = 0;
    if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                       preset_height)) {
      return uint32_t(std::clamp(preset_width, 1, 8192));
    }
  }
  return requested_width;
}

uint32_t ResolveWindowHeight(uint32_t requested_height) {
  if (REXCVAR_GET(window_height) > 0) {
    return uint32_t(REXCVAR_GET(window_height));
  }
  if (!rex::cvar::HasNonDefaultValue("window_height")) {
    if (rex::cvar::HasNonDefaultValue("video_mode_height") && REXCVAR_GET(video_mode_height) > 0) {
      return uint32_t(std::clamp(REXCVAR_GET(video_mode_height), 1, 8192));
    }
    int32_t preset_width = 0;
    int32_t preset_height = 0;
    if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                       preset_height)) {
      return uint32_t(std::clamp(preset_height, 1, 8192));
    }
  }
  return requested_height;
}

using rex::ui::VirtualKey;

// Layout-dependent key, as GTK's unmodified keyval: the key labelled A is A.
VirtualKey TranslateSDLKey(SDL_Keycode key) {
  if (key >= 'a' && key <= 'z') return VirtualKey(uint16_t(VirtualKey::kA) + (key - 'a'));
  if (key >= '0' && key <= '9') return VirtualKey(uint16_t(VirtualKey::k0) + (key - '0'));
  if (key >= SDLK_F1 && key <= SDLK_F12) return VirtualKey(uint16_t(VirtualKey::kF1) + (key - SDLK_F1));
  if (key >= SDLK_F13 && key <= SDLK_F24) {
    return VirtualKey(uint16_t(VirtualKey::kF13) + (key - SDLK_F13));
  }
  if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
    return VirtualKey(uint16_t(VirtualKey::kNumpad1) + (key - SDLK_KP_1));
  }
  switch (key) {
    case SDLK_ESCAPE: return VirtualKey::kEscape;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return VirtualKey::kReturn;
    case SDLK_SPACE: return VirtualKey::kSpace;
    case SDLK_TAB: return VirtualKey::kTab;
    case SDLK_BACKSPACE: return VirtualKey::kBack;
    case SDLK_DELETE: return VirtualKey::kDelete;
    case SDLK_INSERT: return VirtualKey::kInsert;
    case SDLK_HOME: return VirtualKey::kHome;
    case SDLK_END: return VirtualKey::kEnd;
    case SDLK_PAGEUP: return VirtualKey::kPrior;
    case SDLK_PAGEDOWN: return VirtualKey::kNext;
    case SDLK_LEFT: return VirtualKey::kLeft;
    case SDLK_RIGHT: return VirtualKey::kRight;
    case SDLK_UP: return VirtualKey::kUp;
    case SDLK_DOWN: return VirtualKey::kDown;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT: return VirtualKey::kShift;
    case SDLK_LCTRL:
    case SDLK_RCTRL: return VirtualKey::kControl;
    case SDLK_LALT:
    case SDLK_RALT: return VirtualKey::kMenu;
    case SDLK_LGUI: return VirtualKey::kLWin;
    case SDLK_RGUI: return VirtualKey::kRWin;
    case SDLK_APPLICATION: return VirtualKey::kApps;
    case SDLK_CAPSLOCK: return VirtualKey::kCapital;
    case SDLK_NUMLOCKCLEAR: return VirtualKey::kNumLock;
    case SDLK_SCROLLLOCK: return VirtualKey::kScroll;
    case SDLK_PRINTSCREEN: return VirtualKey::kSnapshot;
    case SDLK_PAUSE: return VirtualKey::kPause;
    case SDLK_KP_0: return VirtualKey::kNumpad0;
    case SDLK_KP_PLUS: return VirtualKey::kAdd;
    case SDLK_KP_MINUS: return VirtualKey::kSubtract;
    case SDLK_KP_MULTIPLY: return VirtualKey::kMultiply;
    case SDLK_KP_DIVIDE: return VirtualKey::kDivide;
    case SDLK_KP_PERIOD: return VirtualKey::kDecimal;
    case SDLK_GRAVE: return VirtualKey::kOem3;
    case SDLK_MINUS: return VirtualKey::kOemMinus;
    case SDLK_EQUALS: return VirtualKey::kOemPlus;
    case SDLK_COMMA: return VirtualKey::kOemComma;
    case SDLK_PERIOD: return VirtualKey::kOemPeriod;
    case SDLK_SEMICOLON: return VirtualKey::kOem1;
    case SDLK_SLASH: return VirtualKey::kOem2;
    case SDLK_BACKSLASH: return VirtualKey::kOem5;
    case SDLK_LEFTBRACKET: return VirtualKey::kOem4;
    case SDLK_RIGHTBRACKET: return VirtualKey::kOem6;
    case SDLK_APOSTROPHE: return VirtualKey::kOem7;
    default: return VirtualKey::kNone;
  }
}

std::map<SDL_WindowID, rex::ui::SDLWindow*>& OpenWindows() {
  static std::map<SDL_WindowID, rex::ui::SDLWindow*> windows;
  return windows;
}

}  // namespace

namespace rex {
namespace ui {

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title, uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  desired_logical_width = ResolveWindowWidth(desired_logical_width);
  desired_logical_height = ResolveWindowHeight(desired_logical_height);
  return std::make_unique<SDLWindow>(app_context, title, desired_logical_width,
                                     desired_logical_height);
}

std::unique_ptr<ui::MenuItem> MenuItem::Create(Type type, const std::string& text,
                                               const std::string& hotkey,
                                               std::function<void()> callback) {
  return std::make_unique<SDLMenuItem>(type, text, hotkey, std::move(callback));
}

MetalLayerSurface::~MetalLayerSurface() {
  if (metal_view_) SDL_Metal_DestroyView(SDL_MetalView(metal_view_));
}

bool MetalLayerSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  int width = 0, height = 0;
  if (!SDL_GetWindowSizeInPixels(window_, &width, &height) || width <= 0 || height <= 0) {
    return false;
  }
  width_out = uint32_t(width);
  height_out = uint32_t(height);
  return true;
}

SDLWindow::SDLWindow(WindowedAppContext& app_context, const std::string_view title,
                     uint32_t desired_logical_width, uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {}

SDLWindow::~SDLWindow() {
  EnterDestructor();
  Close();
}

void SDLWindow::Close() {
  if (!window_) return;
  OpenWindows().erase(window_id_);
  SDL_Window* window = window_;
  window_ = nullptr;
  window_id_ = 0;
  SDL_DestroyWindow(window);
}

double SDLWindow::GetMonitorRefreshRateHz() const {
  if (!window_) return 60.0;
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
  return mode && mode->refresh_rate > 0.0f ? double(mode->refresh_rate) : 60.0;
}

std::string SDLWindow::GetDisplayOutputName() const {
  if (!window_) return {};
  const char* name = SDL_GetDisplayName(SDL_GetDisplayForWindow(window_));
  return name ? name : "";
}

uint32_t SDLWindow::GetLatestDpiImpl() const {
  // Logical sizes are in points; physical sizes are pixels.
  const float density = window_ ? SDL_GetWindowPixelDensity(window_) : 0.0f;
  return density > 0.0f ? uint32_t(std::lround(GetMediumDpi() * density)) : GetMediumDpi();
}

bool SDLWindow::OpenImpl() {
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL;
  if (IsFullscreen()) flags |= SDL_WINDOW_FULLSCREEN;
  window_ = SDL_CreateWindow(GetTitle().c_str(), int(GetDesiredLogicalWidth()),
                             int(GetDesiredLogicalHeight()), flags);
  if (!window_) {
    REXLOG_ERROR("SDLWindow: SDL_CreateWindow failed: {}", SDL_GetError());
    return false;
  }
  window_id_ = SDL_GetWindowID(window_);
  OpenWindows()[window_id_] = this;

  if (const int32_t monitor = REXCVAR_GET(monitor); monitor > 0) {
    int count = 0;
    if (SDL_DisplayID* displays = SDL_GetDisplays(&count)) {
      if (monitor <= count) {
        const SDL_DisplayID display = displays[monitor - 1];
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(display));
      }
      SDL_free(displays);
    }
  }
  if (IsMouseCaptureRequested()) ApplyNewMouseCapture();
  ApplyNewCursorVisibility(CursorVisibility::kVisible);

  WindowDestructionReceiver destruction_receiver(this);
  HandleSizeUpdate(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyedOrClosed()) return true;
  if (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) {
    OnFocusUpdate(true, destruction_receiver);
  }
  return true;
}

void SDLWindow::RequestCloseImpl() {
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) return;
  Close();
  OnAfterClose();
}

void SDLWindow::ApplyNewFullscreen() {
  if (!window_) return;
  SDL_SetWindowFullscreen(window_, IsFullscreen());
  if (!IsFullscreen()) ApplyNewLogicalSize();
}

void SDLWindow::ApplyNewLogicalSize() {
  if (!window_ || IsFullscreen()) return;
  SDL_SetWindowSize(window_, int(GetDesiredLogicalWidth()), int(GetDesiredLogicalHeight()));
  REXLOG_INFO("Windowed client size requested: {}x{} logical pixels", GetDesiredLogicalWidth(),
              GetDesiredLogicalHeight());
}

void SDLWindow::ApplyNewTitle() {
  if (window_) SDL_SetWindowTitle(window_, GetTitle().c_str());
}

void SDLWindow::ApplyNewMouseCapture() {
  if (!window_) return;
  motion_remainder_x_ = motion_remainder_y_ = 0.0;
  SDL_SetWindowRelativeMouseMode(window_, true);
}

void SDLWindow::ApplyNewMouseRelease() {
  if (window_) SDL_SetWindowRelativeMouseMode(window_, false);
}

void SDLWindow::ApplyNewCursorVisibility(CursorVisibility) {
  if (GetCursorVisibility() == CursorVisibility::kVisible) {
    SDL_ShowCursor();
  } else {
    SDL_HideCursor();
  }
}

void SDLWindow::FocusImpl() {
  if (window_) SDL_RaiseWindow(window_);
}

std::unique_ptr<Surface> SDLWindow::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!(allowed_types & Surface::kTypeFlag_MetalLayer) || !window_) return nullptr;
  SDL_MetalView view = SDL_Metal_CreateView(window_);
  if (!view) {
    REXLOG_ERROR("SDLWindow: SDL_Metal_CreateView failed: {}", SDL_GetError());
    return nullptr;
  }
  REXLOG_INFO("Presentation backend: Metal layer");
  return std::make_unique<MetalLayerSurface>(window_, view, SDL_Metal_GetLayer(view));
}

void SDLWindow::RequestPaintImpl() {
  // May be called from other threads; the UI loop paints once per request.
  if (paint_pending_.exchange(true, std::memory_order_acq_rel)) return;
  SDL_Event event{};
  event.type = SDLWindowedAppContext::PaintEventType();
  event.user.windowID = window_id_;
  SDL_PushEvent(&event);
}

void SDLWindow::HandleSizeUpdate(WindowDestructionReceiver& destruction_receiver) {
  if (!window_) return;
  int width = 0, height = 0;
  SDL_GetWindowSizeInPixels(window_, &width, &height);
  OnActualSizeUpdate(uint32_t(std::max(width, 0)), uint32_t(std::max(height, 0)),
                     destruction_receiver);
  if (destruction_receiver.IsWindowDestroyedOrClosed()) return;
  if (!IsFullscreen()) {
    int logical_width = 0, logical_height = 0;
    SDL_GetWindowSize(window_, &logical_width, &logical_height);
    if (logical_width > 0 && logical_height > 0) {
      OnDesiredLogicalSizeUpdate(uint32_t(logical_width), uint32_t(logical_height));
    }
  }
  if (presenter() && HasSurface()) presenter()->OnSurfaceResizeFromUIThread();
}

void SDLWindow::DispatchEvent(const SDL_Event& event) {
  const SDL_WindowID id = SDL_GetWindowFromEvent(&event)
                              ? SDL_GetWindowID(SDL_GetWindowFromEvent(&event))
                              : (event.type == SDLWindowedAppContext::PaintEventType()
                                     ? event.user.windowID
                                     : 0);
  if (id) {
    const auto it = OpenWindows().find(id);
    if (it != OpenWindows().end()) it->second->HandleEvent(event);
    return;
  }
  if (event.type == SDL_EVENT_QUIT) {
    // Cmd+Q and the Dock's Quit close every window.
    std::vector<SDLWindow*> windows;
    for (const auto& [window_id, window] : OpenWindows()) windows.push_back(window);
    for (SDLWindow* window : windows) {
      if (OpenWindows().count(window->window_id_)) window->RequestClose();
    }
  }
}

void SDLWindow::HandleEvent(const SDL_Event& event) {
  WindowDestructionReceiver destruction_receiver(this);
  if (event.type == SDLWindowedAppContext::PaintEventType()) {
    paint_pending_.store(false, std::memory_order_release);
    if (HasSurface()) OnPaint();
    return;
  }
  switch (event.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      HandleSizeUpdate(destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
      OnDesiredFullscreenUpdate(event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN);
      HandleSizeUpdate(destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_EXPOSED:
      if (HasSurface()) OnPaint();
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      OnFocusUpdate(event.type == SDL_EVENT_WINDOW_FOCUS_GAINED, destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      RequestCloseImpl();
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      HandleKey(event, destruction_receiver);
      break;
    case SDL_EVENT_TEXT_INPUT: {
      // One character event per code point.
      const char* text = event.text.text;
      while (text && *text && !destruction_receiver.IsWindowDestroyedOrClosed()) {
        const Uint32 code_point = SDL_StepUTF8(&text, nullptr);
        if (!code_point || code_point > 0xFFFF) continue;
        const SDL_Keymod mod = SDL_GetModState();
        KeyEvent e(this, VirtualKey(code_point), 1, false, mod & SDL_KMOD_SHIFT,
                   mod & SDL_KMOD_CTRL, mod & SDL_KMOD_ALT, mod & SDL_KMOD_GUI);
        OnKeyChar(e, destruction_receiver);
      }
    } break;
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
      HandleMouse(event, destruction_receiver);
      break;
    default:
      break;
  }
}

void SDLWindow::HandleKey(const SDL_Event& event,
                          WindowDestructionReceiver& destruction_receiver) {
  const SDL_KeyboardEvent& key = event.key;
  const bool release = event.type == SDL_EVENT_KEY_UP;
  KeyEvent e(this, TranslateSDLKey(key.key), 1, release || key.repeat, key.mod & SDL_KMOD_SHIFT,
             key.mod & SDL_KMOD_CTRL, key.mod & SDL_KMOD_ALT, key.mod & SDL_KMOD_GUI);
  if (release) {
    OnKeyUp(e, destruction_receiver);
  } else {
    OnKeyDown(e, destruction_receiver);
  }
}

void SDLWindow::HandleMouse(const SDL_Event& event,
                            WindowDestructionReceiver& destruction_receiver) {
  // SDL reports points; the window's coordinates are physical pixels.
  const float density = SDL_GetWindowPixelDensity(window_);
  if (event.type == SDL_EVENT_MOUSE_MOTION) {
    if (IsMouseCaptureRequested()) {
      double dx = event.motion.xrel + motion_remainder_x_;
      double dy = event.motion.yrel + motion_remainder_y_;
      const int32_t x = int32_t(dx), y = int32_t(dy);
      motion_remainder_x_ = dx - x;
      motion_remainder_y_ = dy - y;
      if (!x && !y) return;
      MouseEvent relative(this, MouseEvent::Button::kNone, x, y);
      OnRelativeMouseMove(relative, destruction_receiver);
      return;
    }
    MouseEvent e(this, MouseEvent::Button::kNone, int32_t(event.motion.x * density),
                 int32_t(event.motion.y * density));
    OnMouseMove(e, destruction_receiver);
    return;
  }
  if (event.type == SDL_EVENT_MOUSE_WHEEL) {
    MouseEvent e(this, MouseEvent::Button::kNone, int32_t(event.wheel.mouse_x * density),
                 int32_t(event.wheel.mouse_y * density),
                 int32_t(event.wheel.x * MouseEvent::kScrollPerDetent),
                 int32_t(event.wheel.y * MouseEvent::kScrollPerDetent));
    OnMouseWheel(e, destruction_receiver);
    return;
  }
  MouseEvent::Button button = MouseEvent::Button::kNone;
  switch (event.button.button) {
    case SDL_BUTTON_LEFT: button = MouseEvent::Button::kLeft; break;
    case SDL_BUTTON_MIDDLE: button = MouseEvent::Button::kMiddle; break;
    case SDL_BUTTON_RIGHT: button = MouseEvent::Button::kRight; break;
    case SDL_BUTTON_X1: button = MouseEvent::Button::kX1; break;
    case SDL_BUTTON_X2: button = MouseEvent::Button::kX2; break;
    default: break;
  }
  MouseEvent e(this, button, int32_t(event.button.x * density), int32_t(event.button.y * density));
  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    OnMouseDown(e, destruction_receiver);
  } else {
    OnMouseUp(e, destruction_receiver);
  }
}

}  // namespace ui
}  // namespace rex
