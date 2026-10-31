/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/ui/window_listener.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace rex::input::mnk {

// Mouse motion since the last call, in counts scaled by mnk_sensitivity, for a
// game hook that feeds the game's own mouse look. A first call marks mouse look
// as supported; afterwards the mouse no longer moves the right stick unless
// mnk_mouse_mode is "stick". Returns false when there is no motion to apply.
bool TakeMouseLook(uint32_t user_index, double& dx, double& dy);

// Whether keyboard and mouse is enabled and was used more recently than a
// controller, so button prompts should name keys.
bool ShowKeyboardPrompts();

// How the first key bound to a keybind_* setting reads in a prompt, such as
// "SPACE" or "LMB"; empty when nothing is bound.
std::string BoundKeyLabel(std::string_view bind);

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;
  bool TakeMouseLook(uint32_t user_index, double& dx, double& dy);
  std::string BoundKeyLabel(std::string_view bind);
  bool IsKeyboardMouse() const override { return true; }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnRelativeMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

 private:
  friend bool ShowKeyboardPrompts();
  uint32_t UserIndex() const;
  bool IsEnabled() const;
  void CenterCursor();
  void UpdateMouseCapture();
  // On the UI thread: makes the window's capture match mouse_captured_.
  void ApplyMouseCapture();
  // Whether the game window is the one the user is typing into.
  bool WindowFocused() const;
  void SetKeyState(uint16_t vk, bool down);
  bool IsBindPressed(std::string_view name) const;
  void ApplySetting(std::string_view name, std::string_view value);
  bool MouseLooks() const;

  // Stick deflection per mouse count per second at sensitivity 1.
  static constexpr double kMouseRateScale = 24.0;
  void EnqueueKeystroke(uint16_t vk_pad, bool down);

  rex::ui::Window* attached_window_ = nullptr;

  std::mutex state_mutex_;
  bool key_down_[256] = {};

  // Mouse delta tracking
  int32_t mouse_dx_ = 0;
  int32_t mouse_dy_ = 0;
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  bool mouse_captured_ = false;
  // The window reports captured motion as deltas instead of moving the cursor.
  bool relative_motion_ = false;
  bool mouse_look_supported_ = false;

  // Parsed settings, guarded by state_mutex_.
  struct Bind {
    std::vector<uint16_t> keys;
    std::string label;
    bool wheel_up = false;
    bool wheel_down = false;
  };
  std::map<std::string, Bind, std::less<>> binds_;
  bool mouse_as_stick_ = false;
  double sensitivity_ = 1.0;
  std::chrono::steady_clock::time_point last_mouse_sample_{};
  double mouse_rate_x_ = 0.0;
  double mouse_rate_y_ = 0.0;
  std::chrono::steady_clock::time_point wheel_up_until_{};
  std::chrono::steady_clock::time_point wheel_down_until_{};
  // Taken from the window when attached, then kept by focus events: a game
  // started in the background never gets the event that would clear it.
  bool has_focus_ = false;

  // Keystroke queue
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;

  // Packet number incremented on state change
  uint32_t packet_number_ = 0;
};

}  // namespace rex::input::mnk
