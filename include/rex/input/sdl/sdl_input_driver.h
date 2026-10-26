/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/input/input_driver.h>

#include <SDL3/SDL.h>

#define HID_SDL_USER_COUNT 4
#define HID_SDL_THUMB_THRES 0x4E00
#define HID_SDL_TRIGG_THRES 0x1F
#define HID_SDL_REPEAT_DELAY 400
#define HID_SDL_REPEAT_RATE 100

namespace rex::input::sdl {

// Camera rotation from a controller gyro set to aim as a mouse (gyro_mode
// "mouse"), in degrees since the last call, already scaled by
// gyro_sensitivity; positive x turns right, positive y looks down. A game hook
// passes it to the game's own mouse look. A first call marks that as
// supported; until then the gyro moves the right stick instead. Returns false
// when there is no motion to apply.
bool TakeGyroLook(uint32_t user_index, double& dx, double& dy);

class SDLInputDriver final : public InputDriver, public rex::ui::WindowListener {
 public:
  explicit SDLInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~SDLInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT GetHostState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  void OnWindowAvailable(rex::ui::Window* window) override;

  bool TakeGyroLook(uint32_t user_index, double& dx, double& dy);

 private:
  // Motion sensor state of one controller. Rates are in degrees per second
  // with SDL's axes: x right, y up, z toward the player.
  struct GyroState {
    bool present;
    bool calibrated;
    float bias[3];
    float gravity[3];
    float last_accel[3];
    float still_time;
    uint64_t last_timestamp;
    // Turn (positive right) and look (positive down): degrees since the last
    // mouse read, and the smoothed rate for the stick.
    double look_x, look_y;
    double rate_x, rate_y;
  };

  struct ControllerState {
    SDL_Gamepad* sdl;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed;
    bool is_active;
    GyroState gyro;
  };

  enum class RepeatState {
    Idle,       // no buttons pressed or repeating has ended
    Waiting,    // a button is held and the delay is awaited
    Repeating,  // actively repeating at a rate
  };
  struct KeystrokeState {
    uint64_t buttons;
    RepeatState repeat_state;
    // the button number that was pressed last:
    uint8_t repeat_butt_idx;
    // the last time (ms) a down (and/or repeat) event for that button was send:
    uint32_t repeat_time;
  };

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

  void HandleEvent(const SDL_Event& event);
  std::unique_lock<std::mutex> DrainAndLock();
  void ProcessEventLocked(const SDL_Event& event);
  void OnControllerDeviceAddedLocked(const SDL_Event& event);
  void OnControllerDeviceRemovedLocked(const SDL_Event& event);
  void OnControllerDeviceAxisMotionLocked(const SDL_Event& event);
  void OnControllerDeviceButtonChangedLocked(const SDL_Event& event);
  void OnControllerDeviceSensorLocked(const SDL_Event& event);
  void ApplySetting(std::string_view name, std::string_view value);
  bool GyroAiming(const ControllerState& controller) const;
  void ApplyGyroStick(ControllerState& controller, X_INPUT_GAMEPAD& pad);

  inline uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const;
  std::optional<size_t> GetControllerIndexFromInstanceID(SDL_JoystickID instance_id);
  ControllerState* GetControllerState(uint32_t user_index);
  bool TestSDLVersion() const;
  void UpdateXCapabilities(ControllerState& state);
  void QueueControllerUpdate();

  rex::ui::Window* attached_window_ = nullptr;
  bool sdl_events_initialized_;
  bool SDL_Gamepad_initialized_;
  std::atomic<int> sdl_events_unflushed_;
  std::atomic<bool> sdl_pumpevents_queued_;
  std::array<ControllerState, HID_SDL_USER_COUNT> controllers_;
  std::mutex controllers_mutex_;
  std::mutex event_queue_mutex_;
  std::vector<SDL_Event> pending_events_;
  std::array<KeystrokeState, HID_SDL_USER_COUNT> keystroke_states_;

  // Parsed gyro settings, guarded by controllers_mutex_.
  enum class GyroMode { kOff, kMouse, kJoystick };
  enum class GyroAxis { kYaw, kRoll, kPlayer };
  GyroMode gyro_mode_ = GyroMode::kOff;
  GyroAxis gyro_axis_ = GyroAxis::kYaw;
  bool gyro_while_aiming_ = false;
  bool gyro_invert_y_ = false;
  double gyro_sensitivity_ = 1.0;
  double gyro_stick_deadzone_ = 0.3;
  bool gyro_look_supported_ = false;
};

}  // namespace rex::input::sdl
