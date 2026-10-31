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

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <optional>
#include <chrono>
#include <fstream>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#elif REX_PLATFORM_GNU_LINUX
#include <pthread.h>
#endif

REXCVAR_DEFINE_STRING(hid_mappings_file, "gamecontrollerdb.txt", "Input",
                      "Path to SDL gamecontroller mappings file");

// Per-player controller settings (player slot N = 1-4): gyro aiming for
// controllers with motion sensors (off by default; otherwise Steam Input's
// defaults: always on, turning with yaw), button mapping, sticks.
#define REX_CONTROLLER_SETTINGS(N)                                                              \
  REXCVAR_DEFINE_STRING(controller##N##_gyro_mode, "off", "Input",                              \
                        "Player " #N " gyro aiming. off; mouse: turns the camera like a mouse; "  \
                        "joystick: moves the right stick")                                      \
      .allowed({"off", "mouse", "joystick"});                                                   \
  REXCVAR_DEFINE_STRING(controller##N##_gyro_turn_axis, "yaw", "Input",                         \
                        "Player " #N " gyro turning. yaw: turn the controller left and right; "   \
                        "roll: tilt it; player: either, relative to how it is held")            \
      .allowed({"yaw", "roll", "player"});                                                      \
  REXCVAR_DEFINE_STRING(controller##N##_gyro_activation, "always", "Input",                     \
                        "Player " #N " gyro: always; aiming: while the left trigger is held; "    \
                        "button: while a button mapped to gyro is held")                         \
      .allowed({"always", "aiming", "button"});                                                 \
  REXCVAR_DEFINE_BOOL(controller##N##_gyro_invert_y, false, "Input",                            \
                      "Player " #N " gyro: invert up and down");                                \
  REXCVAR_DEFINE_DOUBLE(controller##N##_gyro_sensitivity, 1.0, "Input",                         \
                        "Player " #N " gyro: degrees the camera turns per degree the controller turns") \
      .range(0.1, 10.0);                                                                        \
  REXCVAR_DEFINE_DOUBLE(controller##N##_gyro_stick_deadzone, 0.3, "Input",                      \
                        "Player " #N " gyro as joystick: stick deflection where the game's dead zone ends") \
      .range(0.0, 0.9);                                                                         \
  REXCVAR_DEFINE_STRING(controller##N##_buttons, "", "Input",                                   \
                        "Player " #N " button mapping, source=target pairs separated by commas "  \
                        "(sources a b x y back guide start ls rs lb rb up down left right misc1 " \
                        "rpaddle1 lpaddle1 rpaddle2 lpaddle2 touchpad misc2-misc6 lt rt; targets "\
                        "the same Xbox buttons, lt, rt, none or gyro); unlisted inputs keep their defaults"); \
  REXCVAR_DEFINE_BOOL(controller##N##_swap_sticks, false, "Input", "Player " #N ": swap the sticks"); \
  REXCVAR_DEFINE_BOOL(controller##N##_invert_look_y, false, "Input",                            \
                      "Player " #N ": invert the right stick up and down");                     \
  REXCVAR_DEFINE_DOUBLE(controller##N##_left_deadzone, 0.0, "Input",                            \
                        "Player " #N ": left stick dead zone, added to the game's own")          \
      .range(0.0, 0.9);                                                                         \
  REXCVAR_DEFINE_DOUBLE(controller##N##_right_deadzone, 0.0, "Input",                           \
                        "Player " #N ": right stick dead zone, added to the game's own")         \
      .range(0.0, 0.9);
REX_CONTROLLER_SETTINGS(1)
REX_CONTROLLER_SETTINGS(2)
REX_CONTROLLER_SETTINGS(3)
REX_CONTROLLER_SETTINGS(4)
#undef REX_CONTROLLER_SETTINGS

namespace rex::input::sdl {

namespace {
std::atomic<SDLInputDriver*> instance{nullptr};

// Player 1 memory (SetControllerMemoryPath). Accessed under the driver's
// controllers mutex once controllers connect.
std::filesystem::path controller_memory_path;
std::string primary_controller;
bool primary_controller_loaded = false;
// Startup enumeration window: a pad placed in slot 0 before the remembered
// player 1 arrived, and before any button press, may still move aside.
constexpr auto kStartupReorderWindow = std::chrono::seconds(3);
std::optional<std::chrono::steady_clock::time_point> first_controller_time;

std::string ControllerIdentity(SDL_Gamepad* controller) {
  if (const char* serial = SDL_GetGamepadSerial(controller); serial && *serial)
    return fmt::format("{:04x}:{:04x}:{}", SDL_GetGamepadVendor(controller),
                       SDL_GetGamepadProduct(controller), serial);
  char guid[64] = {};
  SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(controller)), guid, sizeof(guid));
  return guid;
}

void LoadPrimaryController() {
  if (primary_controller_loaded) return;
  primary_controller_loaded = true;
  if (controller_memory_path.empty()) return;
  std::ifstream file(controller_memory_path);
  std::getline(file, primary_controller);
}

void SavePrimaryController(const std::string& identity) {
  if (identity == primary_controller) return;
  primary_controller = identity;
  REXLOG_INFO("SDL: player 1 is now controller {}", identity);
  if (controller_memory_path.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(controller_memory_path.parent_path(), error);
  std::ofstream(controller_memory_path, std::ios::trunc) << identity << '\n';
}

constexpr const char* kPlayerSettings[] = {"gyro_mode",        "gyro_turn_axis",     "gyro_activation",
                                           "gyro_invert_y",    "gyro_sensitivity",   "gyro_stick_deadzone",
                                           "buttons",          "swap_sticks",        "invert_look_y",
                                           "left_deadzone",    "right_deadzone"};

std::vector<std::string> PlayerSettingNames() {
  std::vector<std::string> names;
  for (int player = 1; player <= HID_SDL_USER_COUNT; ++player)
    for (const char* setting : kPlayerSettings) names.push_back(fmt::format("controller{}_{}", player, setting));
  return names;
}

// Mapping source names, in SDL_GamepadButton order, then the triggers.
constexpr const char* kSourceNames[] = {
    "a",        "b",        "x",        "y",        "back",     "guide",    "start",
    "ls",       "rs",       "lb",       "rb",       "up",       "down",     "left",
    "right",    "misc1",    "rpaddle1", "lpaddle1", "rpaddle2", "lpaddle2", "touchpad",
    "misc2",    "misc3",    "misc4",    "misc5",    "misc6",    "lt",       "rt"};
static_assert(std::size(kSourceNames) == SDLInputDriver::kMappingSources);

// Target names and their XInput button bit (see X_INPUT_GAMEPAD_BUTTON).
constexpr std::pair<const char*, int8_t> kTargets[] = {
    {"up", 0},   {"down", 1},  {"left", 2},  {"right", 3},  {"start", 4}, {"back", 5},
    {"ls", 6},   {"rs", 7},    {"lb", 8},    {"rb", 9},     {"guide", 10}, {"a", 12},
    {"b", 13},   {"x", 14},    {"y", 15},
    {"lt", SDLInputDriver::kTargetLeftTrigger}, {"rt", SDLInputDriver::kTargetRightTrigger},
    {"none", SDLInputDriver::kTargetNone}, {"gyro", SDLInputDriver::kTargetGyro}};

// What each source does without a mapping: the controller's own button, the
// Misc button and touchpad as Guide, Elite/Edge paddles as Y B X A.
std::array<int8_t, SDLInputDriver::kMappingSources> DefaultMap() {
  return {12, 13, 14, 15, 5, 10, 4, 6, 7, 8, 9, 0, 1, 2, 3, 10, 15, 13, 14, 12, 10,
          SDLInputDriver::kTargetNone, SDLInputDriver::kTargetNone, SDLInputDriver::kTargetNone,
          SDLInputDriver::kTargetNone, SDLInputDriver::kTargetNone,
          SDLInputDriver::kTargetLeftTrigger, SDLInputDriver::kTargetRightTrigger};
}

std::array<int8_t, SDLInputDriver::kMappingSources> ParseMap(std::string_view text) {
  auto map = DefaultMap();
  while (!text.empty()) {
    const auto comma = text.find(',');
    auto entry = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    const auto trim = [](std::string_view value) {
      while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
      while (!value.empty() && value.back() == ' ') value.remove_suffix(1);
      return value;
    };
    const auto equals = entry.find('=');
    if (equals == std::string_view::npos) continue;
    const auto source = trim(entry.substr(0, equals)), target = trim(entry.substr(equals + 1));
    const auto source_it = std::find(std::begin(kSourceNames), std::end(kSourceNames), source);
    const auto target_it = std::find_if(std::begin(kTargets), std::end(kTargets),
                                        [&](const auto& t) { return t.first == target; });
    if (source_it == std::end(kSourceNames) || target_it == std::end(kTargets)) {
      REXLOG_WARN("Controller mapping: ignoring '{}'", entry);
      continue;
    }
    map[source_it - std::begin(kSourceNames)] = target_it->second;
  }
  return map;
}
}  // namespace

void SetControllerMemoryPath(std::filesystem::path path) {
  controller_memory_path = std::move(path);
  primary_controller_loaded = false;
}

bool TakeGyroLook(uint32_t user_index, double& dx, double& dy) {
  auto* driver = instance.load(std::memory_order_acquire);
  return driver && driver->TakeGyroLook(user_index, dx, dy);
}

SDLInputDriver::SDLInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      sdl_events_initialized_(false),
      SDL_Gamepad_initialized_(false),
      sdl_events_unflushed_(0),
      sdl_pumpevents_queued_(false),
      controllers_(),
      controllers_mutex_(),
      keystroke_states_() {
  for (auto& player : players_) player.map = DefaultMap();
  for (const auto& name : PlayerSettingNames()) {
    ApplySetting(name, rex::cvar::GetFlagByName(name));
    rex::cvar::RegisterChangeCallback(name, [this](std::string_view name, std::string_view value) {
      std::lock_guard lock(controllers_mutex_);
      ApplySetting(name, value);
    });
  }
  instance.store(this, std::memory_order_release);
}

SDLInputDriver::~SDLInputDriver() {
  SDLInputDriver* self = this;
  instance.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
  for (const auto& name : PlayerSettingNames()) rex::cvar::UnregisterChangeCallbacks(name);
}

void SDLInputDriver::ApplySetting(std::string_view name, std::string_view value) {
  // controllerN_<setting>
  constexpr std::string_view kPrefix = "controller";
  if (!name.starts_with(kPrefix) || name.size() < kPrefix.size() + 3) return;
  const size_t slot = size_t(name[kPrefix.size()] - '1');
  if (slot >= players_.size() || name[kPrefix.size() + 1] != '_') return;
  const auto setting = name.substr(kPrefix.size() + 2);
  auto& player = players_[slot];
  const auto number = [&](double fallback, double low, double high) {
    try {
      return std::clamp(std::stod(std::string(value)), low, high);
    } catch (const std::exception&) {
      return fallback;
    }
  };
  const bool flag = value == "true" || value == "1";
  if (setting == "gyro_mode") {
    player.gyro_mode = value == "off"        ? GyroMode::kOff
                       : value == "joystick" ? GyroMode::kJoystick
                                             : GyroMode::kMouse;
  } else if (setting == "gyro_turn_axis") {
    player.gyro_axis = value == "roll" ? GyroAxis::kRoll
                       : value == "player" ? GyroAxis::kPlayer
                                           : GyroAxis::kYaw;
  } else if (setting == "gyro_activation") {
    player.gyro_activation = value == "aiming"   ? GyroActivation::kAiming
                             : value == "button" ? GyroActivation::kButton
                                                 : GyroActivation::kAlways;
  } else if (setting == "gyro_invert_y") {
    player.gyro_invert_y = flag;
  } else if (setting == "gyro_sensitivity") {
    player.gyro_sensitivity = number(player.gyro_sensitivity, 0.1, 10.0);
  } else if (setting == "gyro_stick_deadzone") {
    player.gyro_stick_deadzone = number(player.gyro_stick_deadzone, 0.0, 0.9);
  } else if (setting == "buttons") {
    player.map = ParseMap(value);
  } else if (setting == "swap_sticks") {
    player.swap_sticks = flag;
  } else if (setting == "invert_look_y") {
    player.invert_look_y = flag;
  } else if (setting == "left_deadzone") {
    player.left_deadzone = number(player.left_deadzone, 0.0, 0.9);
  } else if (setting == "right_deadzone") {
    player.right_deadzone = number(player.right_deadzone, 0.0, 0.9);
  }
  auto& controller = controllers_[slot];
  controller.gyro.look_x = controller.gyro.look_y = 0.0;
  controller.gyro.rate_x = controller.gyro.rate_y = 0.0;
  if (controller.sdl) Remap(controller, slot);
}

bool SDLInputDriver::TakeGyroLook(uint32_t user_index, double& dx, double& dy) {
  if (!attached_window_ || !sdl_events_initialized_ || !SDL_Gamepad_initialized_) return false;
  auto guard = DrainAndLock();
  gyro_look_supported_ = true;
  auto* controller = GetControllerState(user_index);
  if (!controller) return false;
  const auto& player = players_[user_index];
  auto& gyro = controller->gyro;
  dx = gyro.look_x * player.gyro_sensitivity;
  dy = gyro.look_y * player.gyro_sensitivity;
  gyro.look_x = gyro.look_y = 0.0;
  if (player.gyro_mode != GyroMode::kMouse || !is_active()) return false;
  return dx != 0.0 || dy != 0.0;
}

bool SDLInputDriver::GyroEnabled(const ControllerState& controller, size_t slot) const {
  switch (players_[slot].gyro_activation) {
    case GyroActivation::kAiming:
      return controller.state.gamepad.left_trigger > HID_SDL_TRIGG_THRES;
    case GyroActivation::kButton:
      return controller.gyro_held;
    default:
      return true;
  }
}

void SDLInputDriver::ApplyGyroStick(ControllerState& controller, size_t slot, X_INPUT_GAMEPAD& pad) {
  const auto& player = players_[slot];
  if (player.gyro_mode == GyroMode::kOff || (player.gyro_mode == GyroMode::kMouse && gyro_look_supported_))
    return;
  const double x = controller.gyro.rate_x * player.gyro_sensitivity;
  const double y = controller.gyro.rate_y * player.gyro_sensitivity;
  const double speed = std::hypot(x, y);
  if (speed < 0.05) return;
  // Full deflection at this turn rate, in degrees per second. Small rates start
  // where the game's dead zone ends so that slow motion still turns.
  constexpr double kFullSpeed = 360.0;
  const double deflection = player.gyro_stick_deadzone +
                            (1.0 - player.gyro_stick_deadzone) * std::min(1.0, speed / kFullSpeed);
  const auto add = [](int16_t axis, double value) {
    return int16_t(std::clamp(double(axis) + value * 32767.0, -32768.0, 32767.0));
  };
  pad.thumb_rx = add(pad.thumb_rx, x / speed * deflection);
  pad.thumb_ry = add(pad.thumb_ry, -y / speed * deflection);
}

X_STATUS SDLInputDriver::Setup() {
  if (!TestSDLVersion()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

void SDLInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window && !attached_window_) {
    attached_window_ = window;
    window->AddListener(this);
#if REX_PLATFORM_WIN32 || REX_PLATFORM_GNU_LINUX
    input_thread_ = std::thread([this] { InputThreadMain(); });
    std::unique_lock lock(pump_mutex_);
    pump_condition_.wait(lock, [this] { return input_thread_ready_; });
#else
    window->app_context().CallInUIThreadSynchronous([this]() { InitializeSdl(); });
#endif
  }
}

void SDLInputDriver::InitializeSdl() {
  // Initialize SDL events subsystem
  if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
    REXLOG_ERROR("SDL: Failed to init events subsystem: {}", SDL_GetError());
    return;
  }
  sdl_events_initialized_ = true;
  pending_events_.reserve(64);

  // With an event watch we will always get notified, even if the event queue
  // is full, which can happen if another subsystem does not clear its events.
  SDL_AddEventWatch(
      [](void* userdata, SDL_Event* event) -> bool {
        if (!userdata || !event) {
          assert_always();
          return false;
        }

        const auto type = event->type;
        if (type < SDL_EVENT_JOYSTICK_AXIS_MOTION || type >= SDL_EVENT_FINGER_DOWN) {
          return false;
        }

        // If another part of rex uses another SDL subsystem that generates
        // events, this may seem like a bad idea. They will however not
        // subscribe to controller events so we get away with that.
        const auto driver = static_cast<SDLInputDriver*>(userdata);
        driver->HandleEvent(*event);

        return false;
      },
      this);

  // Initialize game controller subsystem
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    REXLOG_ERROR("SDL: Failed to init gamecontroller subsystem: {}", SDL_GetError());
    return;
  }
  SDL_Gamepad_initialized_ = true;

  // Load custom controller mappings if available
  if (!REXCVAR_GET(hid_mappings_file).empty()) {
    std::filesystem::path mappings_path(REXCVAR_GET(hid_mappings_file));
    if (!std::filesystem::exists(mappings_path)) {
      REXLOG_WARN("SDL GameControllerDB: file '{}' does not exist.",
                  REXCVAR_GET(hid_mappings_file));
    } else {
      auto mappings_result =
          SDL_AddGamepadMappingsFromFile(REXCVAR_GET(hid_mappings_file).c_str());
      if (mappings_result < 0) {
        REXLOG_ERROR("SDL GameControllerDB: error loading file '{}': {}.",
                     REXCVAR_GET(hid_mappings_file), mappings_result);
      } else {
        REXLOG_INFO("SDL GameControllerDB: loaded {} mappings.", mappings_result);
      }
    }
  }
  REXLOG_INFO("SDL input driver initialized successfully");
}

void SDLInputDriver::InputThreadMain() {
#if REX_PLATFORM_WIN32
  SetThreadDescription(GetCurrentThread(), L"SDL Input");
#elif REX_PLATFORM_GNU_LINUX
  pthread_setname_np(pthread_self(), "SDL Input");
#endif
  InitializeSdl();
  {
    std::lock_guard lock(pump_mutex_);
    input_thread_ready_ = true;
  }
  pump_condition_.notify_all();
  std::unique_lock lock(pump_mutex_);
  while (!input_thread_stop_) {
    // Pump on request from GetState, and regularly for hotplug.
    pump_condition_.wait_for(lock, std::chrono::milliseconds(8),
                             [this] { return pump_requested_ || input_thread_stop_; });
    if (input_thread_stop_) break;
    pump_requested_ = false;
    lock.unlock();
    if (sdl_events_initialized_) SDL_PumpEvents();
    sdl_pumpevents_queued_ = false;
    lock.lock();
  }
  lock.unlock();
  ShutdownSdl();
}

void SDLInputDriver::ShutdownSdl() {
  for (size_t i = 0; i < controllers_.size(); i++) {
    if (controllers_.at(i).sdl) {
      SDL_CloseGamepad(controllers_.at(i).sdl);
      controllers_.at(i) = {};
    }
  }
  if (SDL_Gamepad_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_Gamepad_initialized_ = false;
  }
  if (sdl_events_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    sdl_events_initialized_ = false;
  }
}

void SDLInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    attached_window_->RemoveListener(this);
#if REX_PLATFORM_WIN32 || REX_PLATFORM_GNU_LINUX
    {
      std::lock_guard lock(pump_mutex_);
      input_thread_stop_ = true;
    }
    pump_condition_.notify_all();
    if (input_thread_.joinable()) input_thread_.join();
#else
    if (sdl_pumpevents_queued_) {
      attached_window_->app_context().CallInUIThreadSynchronous(
          [this]() { attached_window_->app_context().ExecutePendingFunctionsFromUIThread(); });
    }
    ShutdownSdl();
#endif
    attached_window_ = nullptr;
  }
}

void SDLInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {}

void SDLInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {}

X_RESULT SDLInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT || !out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Unfortunately drivers can't present all information immediately (e.g.
  // battery information) so this needs to be refreshed every time.
  UpdateXCapabilities(*controller);

  std::memcpy(out_caps, &controller->caps, sizeof(*out_caps));

  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetHostState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (!attached_window_ || !sdl_events_initialized_ || !SDL_Gamepad_initialized_ ||
      user_index >= HID_SDL_USER_COUNT || !out_state) return X_ERROR_DEVICE_NOT_CONNECTED;
  QueueControllerUpdate();
  auto guard = DrainAndLock();
  auto* controller = GetControllerState(user_index);
  if (!controller) return X_ERROR_DEVICE_NOT_CONNECTED;
  *out_state = controller->state;
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Make sure packet_number is only incremented by 1, even if there have been
  // multiple updates between GetState calls. Also track `is_active` to
  // increment the packet number if it changed.
  if ((is_active != controller->is_active) || (is_active && controller->state_changed)) {
    controller->state.packet_number++;
    controller->is_active = is_active;
    controller->state_changed = false;
  }
  std::memcpy(out_state, &controller->state, sizeof(*out_state));
  if (is_active) ApplyGyroStick(*controller, user_index, out_state->gamepad);
  if (!REXCVAR_GET(guide_button)) out_state->gamepad.buttons = uint16_t(out_state->gamepad.buttons) & ~X_INPUT_GAMEPAD_GUIDE;
  if (!is_active) {
    // Simulate an "untouched" controller. When we become active again the
    // pressed buttons aren't lost and will be visible again.
    std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

#if SDL_VERSION_ATLEAST(2, 0, 9)
  if (SDL_RumbleGamepad(controller->sdl, vibration->left_motor_speed, vibration->right_motor_speed,
                        0)) {
    return X_ERROR_FUNCTION_FAILED;
  } else {
    return X_ERROR_SUCCESS;
  }
#else
  return X_ERROR_SUCCESS;
#endif
}

X_RESULT SDLInputDriver::GetKeystroke(uint32_t users, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  // TODO(JoelLinn): Figure out the flags
  // https://github.com/evilC/UCR/blob/0489929e2a8e39caa3484c67f3993d3fba39e46f/Libraries/XInput.ahk#L85-L98
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  bool user_any = users == 0xFF;
  if (users >= HID_SDL_USER_COUNT && !user_any) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  // The order of this list is also the order in which events are send if
  // multiple buttons change at once.
  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
      // 00 - True buttons from xinput button field
      rex::ui::VirtualKey::kXInputPadDpadUp,
      rex::ui::VirtualKey::kXInputPadDpadDown,
      rex::ui::VirtualKey::kXInputPadDpadLeft,
      rex::ui::VirtualKey::kXInputPadDpadRight,
      rex::ui::VirtualKey::kXInputPadStart,
      rex::ui::VirtualKey::kXInputPadBack,
      rex::ui::VirtualKey::kXInputPadLThumbPress,
      rex::ui::VirtualKey::kXInputPadRThumbPress,
      rex::ui::VirtualKey::kXInputPadLShoulder,
      rex::ui::VirtualKey::kXInputPadRShoulder,
      rex::ui::VirtualKey::kNone, /* Guide has no VK */
      rex::ui::VirtualKey::kNone, /* Unknown */
      rex::ui::VirtualKey::kXInputPadA,
      rex::ui::VirtualKey::kXInputPadB,
      rex::ui::VirtualKey::kXInputPadX,
      rex::ui::VirtualKey::kXInputPadY,
      // 16 - Fake buttons generated from analog inputs
      rex::ui::VirtualKey::kXInputPadLTrigger,
      rex::ui::VirtualKey::kXInputPadRTrigger,
      // 18
      rex::ui::VirtualKey::kXInputPadLThumbUp,
      rex::ui::VirtualKey::kXInputPadLThumbDown,
      rex::ui::VirtualKey::kXInputPadLThumbRight,
      rex::ui::VirtualKey::kXInputPadLThumbLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
      // 26
      rex::ui::VirtualKey::kXInputPadRThumbUp,
      rex::ui::VirtualKey::kXInputPadRThumbDown,
      rex::ui::VirtualKey::kXInputPadRThumbRight,
      rex::ui::VirtualKey::kXInputPadRThumbLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
  };

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  for (uint32_t user_index = (user_any ? 0 : users);
       user_index < (user_any ? HID_SDL_USER_COUNT : users + 1); user_index++) {
    auto controller = GetControllerState(user_index);
    if (!controller) {
      if (user_any) {
        continue;
      } else {
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
    }

    // If input is not active (e.g. due to a dialog overlay), force buttons to
    // "unpressed". The algorithm will automatically send UP events when
    // `is_active()` goes low and DOWN events when it goes high again.
    const uint64_t curr_butts =
        is_active
            ? (controller->state.gamepad.buttons | AnalogToKeyfield(controller->state.gamepad))
            : uint64_t(0);
    KeystrokeState& last = keystroke_states_.at(user_index);

    // Handle repeating
    auto guest_now = rex::chrono::Clock::QueryGuestUptimeMillis();
    static_assert(HID_SDL_REPEAT_DELAY >= HID_SDL_REPEAT_RATE);
    if (last.repeat_state == RepeatState::Waiting &&
        (last.repeat_time + HID_SDL_REPEAT_DELAY < guest_now)) {
      last.repeat_state = RepeatState::Repeating;
    }
    if (last.repeat_state == RepeatState::Repeating &&
        (last.repeat_time + HID_SDL_REPEAT_RATE < guest_now)) {
      last.repeat_time = guest_now;
      rex::ui::VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
      assert_true(vk != rex::ui::VirtualKey::kNone);
      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = user_index;
      out_keystroke->hid_code = 0;
      out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
      return X_ERROR_SUCCESS;
    }

    auto butts_changed = curr_butts ^ last.buttons;
    if (!butts_changed) {
      continue;
    }

    // First try to clear buttons with up events. This is to match xinput
    // behaviour when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
    // up before THUMB_LEFT is down.
    for (auto [clear_pass, i] = std::tuple{true, 0}; i < 2; clear_pass = false, i++) {
      for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
        auto fbutton = uint64_t(1) << i;
        if (!(butts_changed & fbutton)) {
          continue;
        }
        rex::ui::VirtualKey vk = kVkLookup.at(i);
        if (vk == rex::ui::VirtualKey::kNone) {
          continue;
        }

        out_keystroke->virtual_key = uint16_t(vk);
        out_keystroke->unicode = 0;
        out_keystroke->user_index = user_index;
        out_keystroke->hid_code = 0;

        bool is_pressed = curr_butts & fbutton;
        if (clear_pass && !is_pressed) {
          // up
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
          last.buttons &= ~fbutton;
          last.repeat_state = RepeatState::Idle;
          return X_ERROR_SUCCESS;
        }
        if (!clear_pass && is_pressed) {
          // down
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
          last.buttons |= fbutton;
          last.repeat_state = RepeatState::Waiting;
          last.repeat_butt_idx = i;
          last.repeat_time = guest_now;
          return X_ERROR_SUCCESS;
        }
      }
    }
  }
  return X_ERROR_EMPTY;
}

void SDLInputDriver::HandleEvent(const SDL_Event& event) {
  // This callback will likely run on the thread that posts the event, which
  // may be a dedicated thread SDL has created for the joystick subsystem.

  // Event queue should never be (this) full
  assert(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) < 0xFFFF);

  // The queue could grow up to 3.5MB since it is never polled.
  if (++sdl_events_unflushed_ > 64) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    sdl_events_unflushed_ = 0;
  }

  // Buffer only - no controllers_mutex_ acquisition here.
  // This breaks the lock ordering inversion between controllers_mutex_ and
  // SDL's internal joystick lock that caused deadlocks.
  std::lock_guard<std::mutex> guard(event_queue_mutex_);
  // Motion sensors report up to 1000 times a second; while the game is not
  // polling, drop samples instead of growing the queue.
  if (event.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE && pending_events_.size() >= 4096) return;
  pending_events_.push_back(event);
}

std::unique_lock<std::mutex> SDLInputDriver::DrainAndLock() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> guard(event_queue_mutex_);
    events.swap(pending_events_);
  }
  std::unique_lock<std::mutex> guard(controllers_mutex_);
  for (const auto& event : events) {
    ProcessEventLocked(event);
  }
  return guard;
}

void SDLInputDriver::ProcessEventLocked(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      OnControllerDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnControllerDeviceRemovedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnControllerDeviceAxisMotionLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnControllerDeviceButtonChangedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
      OnControllerDeviceSensorLocked(event);
      break;
    default:
      break;
  }
}

void SDLInputDriver::OnControllerDeviceAddedLocked(const SDL_Event& event) {
  // Open the controller.
  const auto controller = SDL_OpenGamepad(event.cdevice.which);
  if (!controller) {
    assert_always();
    return;
  }
  REXLOG_INFO(
      "SDL OnControllerDeviceAdded: \"{}\", "
      "JoystickType({}), "
      "GameControllerType({}), "
      "VendorID(0x{:04X}), "
      "ProductID(0x{:04X})",
      SDL_GetGamepadName(controller),
      static_cast<int>(SDL_GetJoystickType(SDL_GetGamepadJoystick(controller))),
      static_cast<int>(SDL_GetGamepadType(controller)), SDL_GetGamepadVendor(controller),
      SDL_GetGamepadProduct(controller));
  // Take the first free slot rather than the player index the device reports:
  // a reconnected pad can report a stale index (Wine reports player 2 after a
  // reconnect), which would leave user 0, the one games and host shortcuts
  // read, without a controller. The remembered player 1 reclaims slot 0 from a
  // pad that was merely listed before it at startup.
  LoadPrimaryController();
  const auto now = std::chrono::steady_clock::now();
  if (!first_controller_time) first_controller_time = now;
  const std::string identity = ControllerIdentity(controller);
  int user_id = -1;
  auto& first = controllers_.at(0);
  if (!primary_controller.empty() && identity == primary_controller && first.sdl && !first.had_input &&
      now - *first_controller_time < kStartupReorderWindow) {
    for (size_t i = 1; i < controllers_.size(); i++) {
      if (controllers_.at(i).sdl) continue;
      controllers_.at(i) = first;
      keystroke_states_.at(i) = {};
      Remap(controllers_.at(i), i);
      SDL_SetGamepadPlayerIndex(controllers_.at(i).sdl, static_cast<int>(i));
      first = {};
      keystroke_states_.at(0) = {};
      REXLOG_INFO("SDL OnControllerDeviceAdded: moved \"{}\" to index {} for the remembered player 1.",
                  SDL_GetGamepadName(controllers_.at(i).sdl), i);
      break;
    }
  }
  for (size_t i = 0; i < controllers_.size(); i++) {
    if (!controllers_.at(i).sdl) {
      user_id = static_cast<int>(i);
      SDL_SetGamepadPlayerIndex(controller, user_id);
      break;
    }
  }

  if (user_id >= 0) {
    auto& state = controllers_.at(user_id);
    state = {controller, {}};
    state.identity = identity;
    Remap(state, size_t(user_id));
    // XInput seems to start with packet_number = 1 .
    state.state_changed = true;
    UpdateXCapabilities(state);
    state.gyro.gravity[1] = 1.0f;
    state.gyro.present = SDL_GamepadHasSensor(controller, SDL_SENSOR_GYRO) &&
                         SDL_SetGamepadSensorEnabled(controller, SDL_SENSOR_GYRO, true);
    if (state.gyro.present && SDL_GamepadHasSensor(controller, SDL_SENSOR_ACCEL))
      SDL_SetGamepadSensorEnabled(controller, SDL_SENSOR_ACCEL, true);

    REXLOG_INFO("SDL OnControllerDeviceAdded: Added at index {}{}.", user_id,
                state.gyro.present ? ", with gyro" : "");
  } else {
    // No more controllers needed, close it.
    SDL_CloseGamepad(controller);
    REXLOG_WARN("SDL OnControllerDeviceAdded: Ignored. No free slots.");
  }
}

void SDLInputDriver::OnControllerDeviceRemovedLocked(const SDL_Event& event) {
  // Find the disconnected gamecontroller and close it.
  auto idx = GetControllerIndexFromInstanceID(event.cdevice.which);
  if (idx) {
    SDL_CloseGamepad(controllers_.at(*idx).sdl);
    controllers_.at(*idx) = {};
    keystroke_states_.at(*idx) = {};
    REXLOG_INFO("SDL OnControllerDeviceRemoved: Removed at player index {}.", *idx);
  } else {
    // Can happen in case all slots where full previously.
    REXLOG_WARN("SDL OnControllerDeviceRemoved: Ignored. Unused device.");
  }
}

void SDLInputDriver::OnControllerDeviceAxisMotionLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gaxis.which);
  assert(idx);
  auto& controller = controllers_.at(*idx);
  switch (event.gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      controller.raw_lx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFTY:
      controller.raw_ly = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      controller.raw_rx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      controller.raw_ry = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      controller.raw_lt = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      controller.raw_rt = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    default:
      assert_always();
      break;
  }
  Remap(controller, *idx);
}

// The XInput pad the game sees, from the physical input and the player's
// mapping: buttons and triggers to any button or trigger (or none, or the gyro
// activation button), stick swap, right stick inversion and dead zones.
void SDLInputDriver::Remap(ControllerState& controller, size_t slot) {
  const auto& player = players_[slot];
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  bool gyro_held = false;
  for (size_t source = 0; source < kMappingSources; ++source) {
    const bool trigger = source >= kTriggerSourceLeft;
    const uint8_t analog = source == kTriggerSourceLeft    ? controller.raw_lt
                           : source == kTriggerSourceRight ? controller.raw_rt
                           : (controller.raw_buttons >> source) & 1 ? 255 : 0;
    const bool pressed = trigger ? analog > HID_SDL_TRIGG_THRES : analog != 0;
    const int8_t target = player.map[source];
    if (target == kTargetLeftTrigger) {
      left_trigger = std::max(left_trigger, analog);
    } else if (target == kTargetRightTrigger) {
      right_trigger = std::max(right_trigger, analog);
    } else if (target == kTargetGyro) {
      gyro_held |= pressed;
    } else if (target >= 0 && pressed) {
      // Inputs other than the Guide button reach Guide only when allowed.
      if (target == 10 && source != SDL_GAMEPAD_BUTTON_GUIDE && !REXCVAR_GET(guide_button)) continue;
      buttons |= uint16_t(1u << target);
    }
  }
  int16_t lx = controller.raw_lx, ly = controller.raw_ly, rx = controller.raw_rx, ry = controller.raw_ry;
  if (player.swap_sticks) {
    std::swap(lx, rx);
    std::swap(ly, ry);
  }
  if (player.invert_look_y) ry = int16_t(std::clamp(-int32_t(ry), -32768, 32767));
  const auto dead_zone = [](int16_t& x, int16_t& y, double zone) {
    if (zone <= 0.0) return;
    const double magnitude = std::hypot(double(x), double(y)) / 32767.0;
    if (magnitude <= zone) {
      x = y = 0;
      return;
    }
    const double scale = std::min(1.0, (magnitude - zone) / (1.0 - zone)) / magnitude;
    x = int16_t(std::clamp(double(x) * scale, -32768.0, 32767.0));
    y = int16_t(std::clamp(double(y) * scale, -32768.0, 32767.0));
  };
  dead_zone(lx, ly, player.left_deadzone);
  dead_zone(rx, ry, player.right_deadzone);
  auto& pad = controller.state.gamepad;
  pad.buttons = buttons;
  pad.left_trigger = left_trigger;
  pad.right_trigger = right_trigger;
  pad.thumb_lx = lx;
  pad.thumb_ly = ly;
  pad.thumb_rx = rx;
  pad.thumb_ry = ry;
  controller.gyro_held = gyro_held;
  controller.state_changed = true;
}

void SDLInputDriver::OnControllerDeviceButtonChangedLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  assert(idx);
  auto& controller = controllers_.at(*idx);
  // Player 1 is remembered once it is actually used, so a pad that was only
  // listed first at startup never replaces the remembered one.
  if (!controller.had_input && *idx == 0 && event.gbutton.down) SavePrimaryController(controller.identity);
  controller.had_input = true;
  if (event.gbutton.button >= kTriggerSourceLeft) {
    // A newer SDL Version may have added new buttons.
    REXLOG_INFO("SDL HID: Unknown button was pressed: {}.", event.gbutton.button);
    return;
  }
  const uint32_t bit = 1u << event.gbutton.button;
  controller.raw_buttons = event.gbutton.down ? controller.raw_buttons | bit : controller.raw_buttons & ~bit;
  Remap(controller, *idx);
}

void SDLInputDriver::OnControllerDeviceSensorLocked(const SDL_Event& event) {
  const auto idx = GetControllerIndexFromInstanceID(event.gsensor.which);
  if (!idx) return;
  auto& controller = controllers_.at(*idx);
  const auto& player = players_[*idx];
  auto& gyro = controller.gyro;
  const float* data = event.gsensor.data;
  if (event.gsensor.sensor == SDL_SENSOR_ACCEL) {
    // Gravity points away from the floor; it orients the player turn axis and
    // tells a resting controller apart from a moving one.
    float change = 0.0f;
    for (int i = 0; i < 3; ++i) change += std::abs(data[i] - gyro.last_accel[i]);
    std::copy(data, data + 3, gyro.last_accel);
    if (change > 0.5f) gyro.still_time = 0.0f;
    const float length = std::sqrt(data[0] * data[0] + data[1] * data[1] + data[2] * data[2]);
    if (length > 1.0f) {
      for (int i = 0; i < 3; ++i) gyro.gravity[i] += (data[i] / length - gyro.gravity[i]) * 0.05f;
    }
    return;
  }
  if (event.gsensor.sensor != SDL_SENSOR_GYRO) return;
  const uint64_t timestamp =
      event.gsensor.sensor_timestamp ? event.gsensor.sensor_timestamp : event.gsensor.timestamp;
  const double dt = gyro.last_timestamp && timestamp > gyro.last_timestamp
                        ? std::min(double(timestamp - gyro.last_timestamp) * 1e-9, 0.05)
                        : 0.0;
  gyro.last_timestamp = timestamp;

  constexpr float kDegrees = 57.2957795f;
  float rate[3];
  for (int i = 0; i < 3; ++i) rate[i] = data[i] * kDegrees - gyro.bias[i];

  // Continuous calibration, like Steam Input: while the controller rests, what
  // the gyro still reads is its bias. Until the first rest, larger readings
  // count as rest so that a large bias is found.
  const float rest = gyro.calibrated ? 1.5f : 5.0f;
  const bool still =
      std::abs(rate[0]) < rest && std::abs(rate[1]) < rest && std::abs(rate[2]) < rest;
  gyro.still_time = still ? gyro.still_time + float(dt) : 0.0f;
  if (gyro.still_time > 0.5f) {
    const float k = std::min(1.0f, float(dt) * (gyro.calibrated ? 0.5f : 4.0f));
    for (int i = 0; i < 3; ++i) {
      gyro.bias[i] += rate[i] * k;
      rate[i] -= rate[i] * k;
    }
    if (gyro.still_time > 1.0f) gyro.calibrated = true;
  }

  // Below half a degree per second the reading is mostly noise; scale it down
  // smoothly rather than cutting it off.
  const float magnitude = std::sqrt(rate[0] * rate[0] + rate[1] * rate[1] + rate[2] * rate[2]);
  constexpr float kTighten = 0.5f;
  if (magnitude < kTighten) {
    for (float& r : rate) r *= magnitude / kTighten;
  }

  float turn = rate[1];
  if (player.gyro_axis == GyroAxis::kRoll) {
    turn = rate[2];
  } else if (player.gyro_axis == GyroAxis::kPlayer) {
    // Rotation about the vertical axis, whichever way the controller is held,
    // allowing for some lean.
    const float world = rate[1] * gyro.gravity[1] + rate[2] * gyro.gravity[2];
    turn = std::copysign(std::min(std::abs(world) * 1.41f, std::hypot(rate[1], rate[2])), world);
  }
  const double turn_right = -turn;
  const double look_down = player.gyro_invert_y ? rate[0] : -rate[0];

  const bool enabled = player.gyro_mode != GyroMode::kOff && GyroEnabled(controller, *idx);
  const double k = std::min(1.0, dt / 0.02);
  gyro.rate_x += ((enabled ? turn_right : 0.0) - gyro.rate_x) * k;
  gyro.rate_y += ((enabled ? look_down : 0.0) - gyro.rate_y) * k;
  if (!enabled) return;
  if (player.gyro_mode == GyroMode::kMouse && gyro_look_supported_) {
    gyro.look_x += turn_right * dt;
    gyro.look_y += look_down * dt;
  } else {
    controller.state_changed = true;
  }
}

std::optional<size_t> SDLInputDriver::GetControllerIndexFromInstanceID(SDL_JoystickID instance_id) {
  // Loop through our controllers and try to match the given ID.
  for (size_t i = 0; i < controllers_.size(); i++) {
    auto controller = controllers_.at(i).sdl;
    if (!controller) {
      continue;
    }
    auto joystick = SDL_GetGamepadJoystick(controller);
    assert(joystick);
    auto joy_instance_id = SDL_GetJoystickID(joystick);
    assert(joy_instance_id >= 0);
    if (joy_instance_id == instance_id) {
      return i;
    }
  }
  return std::nullopt;
}

SDLInputDriver::ControllerState* SDLInputDriver::GetControllerState(uint32_t user_index) {
  if (user_index >= controllers_.size()) {
    return nullptr;
  }
  auto controller = &controllers_.at(user_index);
  if (!controller->sdl) {
    return nullptr;
  }
  return controller;
}

bool SDLInputDriver::TestSDLVersion() const {
  REXLOG_INFO("SDL: Using version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
              SDL_MICRO_VERSION);
  return true;
}

void SDLInputDriver::UpdateXCapabilities(ControllerState& state) {
  assert(state.sdl);
  uint16_t cap_flags = 0x0;

  // The RAWINPUT driver combines and enhances input from different APIs. For
  // details, see `SDL_rawinputjoystick.c`. This correlation however has latency
  // which might confuse games calling `GetCapabilities()` (The power level is
  // only available after the controller has been "touched"). Generally that
  // should not be a problem, when in doubt disable the RAWINPUT driver via hint
  // (env var).

  if (SDL_GetJoystickConnectionState(SDL_GetGamepadJoystick(state.sdl)) ==
      SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  }

  // Check if all navigational buttons are present
  static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
      SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (auto it = nav_buttons.begin(); it < nav_buttons.end(); it++) {
    if (!SDL_GamepadHasButton(state.sdl, *it)) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
      break;
    }
  }

  auto& c = state.caps;
  c.type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  c.sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  c.flags = cap_flags;
  c.gamepad.buttons = 0xF3FF | (REXCVAR_GET(guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0x0);
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
}

void SDLInputDriver::QueueControllerUpdate() {
  // Pump SDL events to ensure controller state is up to date.
  bool is_queued = false;
  sdl_pumpevents_queued_.compare_exchange_strong(is_queued, true);
  if (!is_queued) {
#if REX_PLATFORM_WIN32 || REX_PLATFORM_GNU_LINUX
    {
      std::lock_guard lock(pump_mutex_);
      pump_requested_ = true;
    }
    pump_condition_.notify_one();
#else
    attached_window_->app_context().CallInUIThread([this]() {
      SDL_PumpEvents();
      sdl_pumpevents_queued_ = false;
    });
#endif
  }
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
inline uint64_t SDLInputDriver::AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > HID_SDL_TRIGG_THRES) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > HID_SDL_TRIGG_THRES) << 17;

  auto thumb_x = static_cast<int16_t>(gamepad.thumb_lx);
  auto thumb_y = static_cast<int16_t>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > HID_SDL_THUMB_THRES;
    uint64_t d = thumb_y < ~HID_SDL_THUMB_THRES;
    uint64_t r = thumb_x > HID_SDL_THUMB_THRES;
    uint64_t l = thumb_x < ~HID_SDL_THUMB_THRES;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<int16_t>(gamepad.thumb_rx);
    thumb_y = static_cast<int16_t>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace rex::input::sdl
