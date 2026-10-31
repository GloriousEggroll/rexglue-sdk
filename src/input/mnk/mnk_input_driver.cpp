/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <utility>
#include <chrono>
#include <string_view>
#include <vector>
#include <cmath>
#include <cstring>

#if REX_PLATFORM_WIN32
#include <rex/ui/window_win.h>
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(mnk_mode, true, "Input", "Enable keyboard/mouse controller emulation");
REXCVAR_DEFINE_BOOL(mnk_trace_input, false, "Input diagnostics", "Log bounded keyboard focus and controller polling diagnostics");
REXCVAR_DEFINE_INT32(mnk_user_index, 0, "Input", "Controller slot (0-3) for MnK").range(0, 3);
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 1.0, "Input", "Mouse sensitivity")
    .range(0.01, 10.0);
REXCVAR_DEFINE_STRING(mnk_mouse_mode, "look", "Input",
                      "look: the game's own mouse look where the game supports it; stick: the "
                      "mouse moves the right stick, for example for gyro mapped to the mouse");

// Each bind is a key or a comma-separated list of keys; WheelUp and WheelDown
// press briefly per wheel step. The defaults suit third-person shooters.
REXCVAR_DEFINE_STRING(keybind_a, "Space,Return", "Input/Keybinds/Controller", "A button");
REXCVAR_DEFINE_STRING(keybind_b, "F,Backspace", "Input/Keybinds/Controller", "B button");
REXCVAR_DEFINE_STRING(keybind_x, "E", "Input/Keybinds/Controller", "X button");
REXCVAR_DEFINE_STRING(keybind_y, "Q", "Input/Keybinds/Controller", "Y button");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "RMB", "Input/Keybinds/Controller", "Left trigger");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "LMB", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "Shift", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "R", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down");
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left");
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right");
REXCVAR_DEFINE_STRING(keybind_lstick_press, "C", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(keybind_rstick_up, "", "Input/Keybinds/Controller", "Right stick up");
REXCVAR_DEFINE_STRING(keybind_rstick_down, "", "Input/Keybinds/Controller", "Right stick down");
REXCVAR_DEFINE_STRING(keybind_rstick_left, "", "Input/Keybinds/Controller", "Right stick left");
REXCVAR_DEFINE_STRING(keybind_rstick_right, "", "Input/Keybinds/Controller", "Right stick right");
REXCVAR_DEFINE_STRING(keybind_rstick_press, "MMB,V", "Input/Keybinds/Controller",
                      "Right stick press");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "4,Up", "Input/Keybinds/Controller", "D-pad up");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "3,Down", "Input/Keybinds/Controller", "D-pad down");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "1,Left,WheelDown", "Input/Keybinds/Controller", "D-pad left");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "2,Right,WheelUp", "Input/Keybinds/Controller", "D-pad right");
REXCVAR_DEFINE_STRING(keybind_back, "Tab", "Input/Keybinds/Controller", "Back button");
REXCVAR_DEFINE_STRING(keybind_start, "Escape", "Input/Keybinds/Controller", "Start button");
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button");

namespace rex::input::mnk {

using rex::ui::VirtualKey;

namespace {
std::atomic<MnkInputDriver*> instance{nullptr};
}

bool TakeMouseLook(uint32_t user_index, double& dx, double& dy) {
  auto* driver = instance.load(std::memory_order_acquire);
  return driver && driver->TakeMouseLook(user_index, dx, dy);
}

bool ShowKeyboardPrompts() {
  auto* driver = instance.load(std::memory_order_acquire);
  return driver && driver->IsEnabled() && LastInputWasKeyboardMouse();
}

std::string BoundKeyLabel(std::string_view bind) {
  auto* driver = instance.load(std::memory_order_acquire);
  return driver ? driver->BoundKeyLabel(bind) : std::string();
}

std::string MnkInputDriver::BoundKeyLabel(std::string_view bind) {
  std::lock_guard lock(state_mutex_);
  const auto it = binds_.find(bind);
  return it == binds_.end() ? std::string() : it->second.label;
}

bool MnkInputDriver::MouseLooks() const {
  return mouse_look_supported_ && !mouse_as_stick_;
}

namespace {
// Settings the driver keeps parsed copies of. They can change while the game
// runs; cvar storage itself is not synchronized with the polling threads.
constexpr const char* kBindNames[] = {
    "keybind_a", "keybind_b", "keybind_x", "keybind_y", "keybind_left_trigger",
    "keybind_right_trigger", "keybind_left_shoulder", "keybind_right_shoulder",
    "keybind_lstick_up", "keybind_lstick_down", "keybind_lstick_left", "keybind_lstick_right",
    "keybind_lstick_press", "keybind_rstick_up", "keybind_rstick_down", "keybind_rstick_left",
    "keybind_rstick_right", "keybind_rstick_press", "keybind_dpad_up", "keybind_dpad_down",
    "keybind_dpad_left", "keybind_dpad_right", "keybind_back", "keybind_start", "keybind_guide"};

// How a key name reads in a button prompt.
std::string PromptLabel(std::string_view key) {
  static constexpr std::pair<std::string_view, std::string_view> kLabels[] = {
      {"Return", "ENTER"},      {"Escape", "ESC"},          {"Control", "CTRL"},
      {"Mouse4", "MOUSE 4"},    {"Mouse5", "MOUSE 5"},      {"WheelUp", "WHEEL UP"},
      {"WheelDown", "WHEEL DOWN"}, {"PageUp", "PAGE UP"},   {"PageDown", "PAGE DOWN"},
      {"CapsLock", "CAPS LOCK"}, {"LBracket", "["},         {"RBracket", "]"},
      {"Backtick", "`"},        {"Minus", "-"},             {"Plus", "="},
      {"Comma", ","},           {"Period", "."},            {"Semicolon", ";"},
      {"Slash", "/"},           {"Backslash", "\\"},        {"Quote", "'"}};
  for (const auto& [name, label] : kLabels) {
    if (key == name) return std::string(label);
  }
  std::string label;
  if (key.starts_with("Numpad")) {
    label = "NUM ";
    key.remove_prefix(6);
  }
  for (const char c : key) label += char(std::toupper(static_cast<unsigned char>(c)));
  return label;
}
}  // namespace

void MnkInputDriver::ApplySetting(std::string_view name, std::string_view value) {
  if (name == "mnk_mouse_mode") {
    mouse_as_stick_ = value == "stick";
    return;
  }
  if (name == "mnk_sensitivity") {
    try {
      sensitivity_ = std::clamp(std::stod(std::string(value)), 0.01, 10.0);
    } catch (const std::exception&) {
    }
    return;
  }
  Bind bind;
  while (!value.empty()) {
    const size_t comma = value.find(',');
    std::string_view key = value.substr(0, comma);
    value = comma == std::string_view::npos ? std::string_view() : value.substr(comma + 1);
    while (!key.empty() && key.front() == ' ') key.remove_prefix(1);
    while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
    if (bind.label.empty() && (key == "WheelUp" || key == "WheelDown" ||
                               rex::ui::ParseVirtualKey(key) != VirtualKey::kNone))
      bind.label = PromptLabel(key);
    if (key == "WheelUp") {
      bind.wheel_up = true;
    } else if (key == "WheelDown") {
      bind.wheel_down = true;
    } else if (const auto vk = static_cast<uint16_t>(rex::ui::ParseVirtualKey(key)); vk && vk < 256) {
      bind.keys.push_back(vk);
    } else if (!key.empty()) {
      REXLOG_WARN("Unknown key {} in {}", key, name);
    }
  }
  binds_[std::string(name)] = std::move(bind);
}

bool MnkInputDriver::TakeMouseLook(uint32_t user_index, double& dx, double& dy) {
  std::lock_guard lock(state_mutex_);
  mouse_look_supported_ = true;
  if (!IsEnabled() || user_index != UserIndex() || !MouseLooks() || !is_active() || !has_focus_)
    return false;
  dx = mouse_dx_ * sensitivity_;
  dy = mouse_dy_ * sensitivity_;
  if (std::abs(mouse_dx_) + std::abs(mouse_dy_) > 2) NoteInputActivity(true);
  mouse_dx_ = 0;
  mouse_dy_ = 0;
  return dx != 0.0 || dy != 0.0;
}

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {
  std::vector<std::string_view> names(std::begin(kBindNames), std::end(kBindNames));
  names.push_back("mnk_mouse_mode");
  names.push_back("mnk_sensitivity");
  for (const auto name : names) {
    ApplySetting(name, rex::cvar::GetFlagByName(name));
    rex::cvar::RegisterChangeCallback(name, [this](std::string_view name, std::string_view value) {
      std::lock_guard lock(state_mutex_);
      ApplySetting(name, value);
    });
  }
  instance.store(this, std::memory_order_release);
}

MnkInputDriver::~MnkInputDriver() {
  MnkInputDriver* self = this;
  instance.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
  for (const char* name : kBindNames) rex::cvar::UnregisterChangeCallbacks(name);
  rex::cvar::UnregisterChangeCallbacks("mnk_mouse_mode");
  rex::cvar::UnregisterChangeCallbacks("mnk_sensitivity");
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  if (attached_window_) {
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

X_STATUS MnkInputDriver::Setup() {
  REXLOG_INFO("MnK input driver initialized");
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    attached_window_ = window;
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    mouse_captured_ = false;
    if (attached_window_->IsMouseCaptureRequested()) {
      attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      attached_window_->ReleaseMouse();
    }
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

uint32_t MnkInputDriver::UserIndex() const {
  return static_cast<uint32_t>(REXCVAR_GET(mnk_user_index));
}

bool MnkInputDriver::IsEnabled() const {
  return REXCVAR_GET(mnk_mode);
}

bool MnkInputDriver::IsBindPressed(std::string_view name) const {
  const auto it = binds_.find(name);
  if (it == binds_.end()) return false;
  const Bind& bind = it->second;
  for (const uint16_t vk : bind.keys) {
    if (key_down_[vk]) return true;
  }
  const auto now = std::chrono::steady_clock::now();
  return (bind.wheel_up && now < wheel_up_until_) || (bind.wheel_down && now < wheel_down_until_);
}

X_RESULT MnkInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (REXCVAR_GET(mnk_trace_input)) {
    static std::atomic_uint samples{0};
    if (samples.fetch_add(1) < 16)
      REXLOG_INFO("MnK input trace GetState: user={} configured={} enabled={} active={} focus={}",
                  user_index, UserIndex(), IsEnabled(), is_active(), has_focus_);
  }
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  UpdateMouseCapture();

  if (!is_active() || !has_focus_) {
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  std::lock_guard lock(state_mutex_);

  uint16_t buttons = 0;
  if (IsBindPressed("keybind_a"))
    buttons |= X_INPUT_GAMEPAD_A;
  if (IsBindPressed("keybind_b"))
    buttons |= X_INPUT_GAMEPAD_B;
  if (IsBindPressed("keybind_x"))
    buttons |= X_INPUT_GAMEPAD_X;
  if (IsBindPressed("keybind_y"))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (IsBindPressed("keybind_left_shoulder"))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (IsBindPressed("keybind_right_shoulder"))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (IsBindPressed("keybind_lstick_press"))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (IsBindPressed("keybind_rstick_press"))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (IsBindPressed("keybind_back"))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (IsBindPressed("keybind_start"))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed("keybind_guide"))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (IsBindPressed("keybind_dpad_up"))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (IsBindPressed("keybind_dpad_down"))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (IsBindPressed("keybind_dpad_left"))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (IsBindPressed("keybind_dpad_right"))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  uint8_t lt = IsBindPressed("keybind_left_trigger") ? 0xFF : 0;
  uint8_t rt = IsBindPressed("keybind_right_trigger") ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  if (IsBindPressed("keybind_lstick_left"))
    lx -= INT16_MAX;
  if (IsBindPressed("keybind_lstick_right"))
    lx += INT16_MAX;
  if (IsBindPressed("keybind_lstick_up"))
    ly += INT16_MAX;
  if (IsBindPressed("keybind_lstick_down"))
    ly -= INT16_MAX;

  // The right stick sets a turn rate, so mouse speed rather than distance per
  // poll drives it; this keeps the feel independent of the frame rate. Polls
  // closer together than a few milliseconds repeat the last rate.
  const auto now = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(now - last_mouse_sample_).count();
  if (MouseLooks()) {
    mouse_rate_x_ = mouse_rate_y_ = 0.0;
  } else if (elapsed >= 0.004) {
    const double rate_scale = elapsed < 0.1 ? sensitivity_ * kMouseRateScale / elapsed : 0.0;
    mouse_rate_x_ = mouse_dx_ * rate_scale;
    mouse_rate_y_ = -double(mouse_dy_) * rate_scale;
    mouse_dx_ = 0;
    mouse_dy_ = 0;
    last_mouse_sample_ = now;
  }
  // Accumulate in a wider type so mouse motion plus held keys cannot overflow
  // before the final controller-range clamp. Opposing keys cancel each other.
  int64_t rx = static_cast<int64_t>(mouse_rate_x_);
  int64_t ry = static_cast<int64_t>(mouse_rate_y_);

  if (IsBindPressed("keybind_rstick_left"))
    rx -= INT16_MAX;
  if (IsBindPressed("keybind_rstick_right"))
    rx += INT16_MAX;
  if (IsBindPressed("keybind_rstick_up"))
    ry += INT16_MAX;
  if (IsBindPressed("keybind_rstick_down"))
    ry -= INT16_MAX;

  auto clamp16 = [](int64_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, int64_t(INT16_MIN), int64_t(INT16_MAX)));
  };

  packet_number_++;
  if (buttons && REXCVAR_GET(mnk_trace_input)) {
    static std::atomic_uint samples{0};
    if (samples.fetch_add(1) < 64)
      REXLOG_INFO("MnK input trace buttons: user={} packet={} buttons=0x{:04X}", user_index, packet_number_, buttons);
  }

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  if (REXCVAR_GET(mnk_trace_input)) {
    static std::atomic_uint samples{0};
    if (samples.fetch_add(1) < 16)
      REXLOG_INFO("MnK input trace GetKeystroke: user={} configured={} enabled={} active={} focus={}",
                  user_index, UserIndex(), IsEnabled(), is_active(), has_focus_);
  }
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  ks.user_index = static_cast<uint8_t>(UserIndex());
  ks.hid_code = 0;
  keystroke_queue_.push(ks);
}

void MnkInputDriver::CenterCursor() {
  if (!attached_window_)
    return;
  int32_t cx = static_cast<int32_t>(attached_window_->GetActualLogicalWidth() / 2);
  int32_t cy = static_cast<int32_t>(attached_window_->GetActualLogicalHeight() / 2);
  prev_mouse_x_ = cx;
  prev_mouse_y_ = cy;
#if REX_PLATFORM_WIN32
  auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(attached_window_);
  if (win32_window && win32_window->hwnd()) {
    POINT pt = {static_cast<LONG>(cx), static_cast<LONG>(cy)};
    ClientToScreen(win32_window->hwnd(), &pt);
    SetCursorPos(pt.x, pt.y);
  }
#endif
}

void MnkInputDriver::UpdateMouseCapture() {
  if (!attached_window_)
    return;

  bool should_capture = IsEnabled() && has_focus_ && is_active();

  // The game polls from its own threads; windows are changed on the UI thread,
  // in the order requested.
  if (should_capture != mouse_captured_) {
    mouse_captured_ = should_capture;
    if (should_capture) {
      // Reset deltas to avoid a spike on capture start
      mouse_dx_ = 0;
      mouse_dy_ = 0;
    }
    attached_window_->app_context().CallInUIThread([this, should_capture] {
      if (!attached_window_) return;
      if (should_capture) {
        attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
        attached_window_->CaptureMouse();
      } else if (attached_window_->IsMouseCaptureRequested()) {
        attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
        attached_window_->ReleaseMouse();
      }
    });
  }

  // Re-center cursor each frame while captured to prevent edge clamping
  if (mouse_captured_) {
    CenterCursor();
  }
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  if (REXCVAR_GET(mnk_trace_input)) {
    static std::atomic_uint samples{0};
    if (samples.fetch_add(1) < 256)
      REXLOG_INFO("MnK input trace KeyDown: vk={} enabled={} active={} focus={}",
                  uint32_t(e.virtual_key()), IsEnabled(), is_active(), has_focus_);
  }
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, true);
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  if (REXCVAR_GET(mnk_trace_input))
    REXLOG_INFO("MnK input trace KeyUp: vk={} enabled={} active={} focus={}",
                uint32_t(e.virtual_key()), IsEnabled(), is_active(), has_focus_);
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, false);
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  int32_t x = e.x();
  int32_t y = e.y();
  if (!relative_motion_) {
    mouse_dx_ += x - prev_mouse_x_;
    mouse_dy_ += y - prev_mouse_y_;
  }
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
}

void MnkInputDriver::OnRelativeMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  relative_motion_ = true;
  mouse_dx_ += e.x();
  mouse_dy_ += e.y();
}

void MnkInputDriver::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_ || !e.scroll_y())
    return;
  std::lock_guard lock(state_mutex_);
  // Long enough for the game to see one press at any frame rate, short enough
  // that consecutive steps register separately.
  auto& until = e.scroll_y() > 0 ? wheel_up_until_ : wheel_down_until_;
  until = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = false;
  if (REXCVAR_GET(mnk_trace_input)) REXLOG_INFO("MnK input trace focus lost");
  std::memset(key_down_, 0, sizeof(key_down_));
  mouse_dx_ = 0;
  mouse_dy_ = 0;
  if (mouse_captured_ && attached_window_) {
    mouse_captured_ = false;
    if (attached_window_->IsMouseCaptureRequested()) {
      attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      attached_window_->ReleaseMouse();
    }
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = true;
  if (REXCVAR_GET(mnk_trace_input)) REXLOG_INFO("MnK input trace focus gained");
}

}  // namespace rex::input::mnk
