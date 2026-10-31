/**
 * @file        rex/ui/windowed_app_context_sdl.h
 * @brief       UI loop on SDL3, used on macOS.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <atomic>

#include <rex/ui/windowed_app_context.h>

union SDL_Event;

namespace rex {
namespace ui {

class SDLWindowedAppContext final : public WindowedAppContext {
 public:
  SDLWindowedAppContext();
  ~SDLWindowedAppContext();

  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

  // Runs until the app quits. The calling thread must be the main thread,
  // where SDL handles video and window events.
  void RunMainSDLLoop();

  // User event types: pending UI-thread functions, and paints requested from
  // other threads.
  static uint32_t PendingFunctionsEventType();
  static uint32_t PaintEventType();

 private:
  std::atomic<bool> pending_functions_notified_{false};
  bool quit_ = false;
};

}  // namespace ui
}  // namespace rex
