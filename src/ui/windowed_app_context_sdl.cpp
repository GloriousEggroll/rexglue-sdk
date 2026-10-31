/**
 * @file        ui/windowed_app_context_sdl.cpp
 * @brief       UI loop on SDL3, used on macOS.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/window_sdl.h>
#include <rex/ui/windowed_app_context_sdl.h>

#include <SDL3/SDL.h>

namespace rex {
namespace ui {

namespace {
uint32_t RegisteredEventTypes() {
  static const uint32_t first = SDL_RegisterEvents(2);
  return first;
}
}  // namespace

uint32_t SDLWindowedAppContext::PendingFunctionsEventType() { return RegisteredEventTypes(); }

uint32_t SDLWindowedAppContext::PaintEventType() { return RegisteredEventTypes() + 1; }

SDLWindowedAppContext::SDLWindowedAppContext() { RegisteredEventTypes(); }

SDLWindowedAppContext::~SDLWindowedAppContext() = default;

void SDLWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  // Coalesced: one queued event runs every function pending at that time.
  if (pending_functions_notified_.exchange(true, std::memory_order_acq_rel)) return;
  SDL_Event event{};
  event.type = PendingFunctionsEventType();
  SDL_PushEvent(&event);
}

void SDLWindowedAppContext::PlatformQuitFromUIThread() {
  quit_ = true;
  SDL_Event event{};
  event.type = PendingFunctionsEventType();
  SDL_PushEvent(&event);
}

void SDLWindowedAppContext::RunMainSDLLoop() {
  while (!quit_ && !HasQuitFromUIThread()) {
    SDL_Event event;
    if (!SDL_WaitEvent(&event)) continue;
    if (event.type == PendingFunctionsEventType()) {
      pending_functions_notified_.store(false, std::memory_order_release);
      ExecutePendingFunctionsFromUIThread();
      continue;
    }
    SDLWindow::DispatchEvent(event);
  }
  QuitFromUIThread();
}

}  // namespace ui
}  // namespace rex
