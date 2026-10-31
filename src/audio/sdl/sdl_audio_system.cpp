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

#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace rex::audio::sdl {

std::unique_ptr<AudioSystem> SDLAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<SDLAudioSystem>(function_dispatcher);
}

SDLAudioSystem::SDLAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

SDLAudioSystem::~SDLAudioSystem() {}

void SDLAudioSystem::Initialize() {
  AudioSystem::Initialize();
}

namespace {
// Stands in when no output device can be opened, for example without an
// audio session: frames are consumed silently at the device rate, so the game
// keeps its audio pacing instead of facing a missing renderer.
class SilentAudioDriver final : public AudioDriver {
 public:
  SilentAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore), thread_([this] { Run(); }) {}
  ~SilentAudioDriver() override {
    stop_.store(true, std::memory_order_release);
    thread_.join();
  }

  void SubmitFrame(uint32_t) override { queued_.fetch_add(1, std::memory_order_acq_rel); }

 private:
  void Run() {
    // 256 samples per channel at 48 kHz, as the SDL driver plays them.
    constexpr auto kFrame = std::chrono::microseconds(256 * 1000000 / 48000);
    auto next = std::chrono::steady_clock::now();
    while (!stop_.load(std::memory_order_acquire)) {
      next += kFrame;
      std::this_thread::sleep_until(next);
      uint32_t queued = queued_.load(std::memory_order_acquire);
      while (queued && !queued_.compare_exchange_weak(queued, queued - 1, std::memory_order_acq_rel)) {
      }
      if (queued) semaphore_->Release(1, nullptr);
    }
  }

  rex::thread::Semaphore* semaphore_;
  std::atomic<uint32_t> queued_{0};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};
}  // namespace

X_STATUS SDLAudioSystem::CreateDriver([[maybe_unused]] size_t index,
                                      rex::thread::Semaphore* semaphore, AudioDriver** out_driver) {
  assert_not_null(out_driver);
  auto driver = new SDLAudioDriver(memory_, semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    delete driver;
    REXAPU_WARN("No audio output device; the game runs without sound");
    *out_driver = new SilentAudioDriver(memory_, semaphore);
    return X_STATUS_SUCCESS;
  }

  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void SDLAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  if (auto sdldriver = dynamic_cast<SDLAudioDriver*>(driver)) {
    sdldriver->Shutdown();
  }
  delete driver;
}

}  // namespace rex::audio::sdl
