#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <rex/audio/conversion.h>
#include <rex/audio/sdl/sdl_audio_driver.h>

namespace {
class TestAudioDriver : public rex::audio::sdl::SDLAudioDriver {
 public:
  TestAudioDriver() : SDLAudioDriver(nullptr, nullptr) {}
  ~TestAudioDriver() override { Shutdown(); }
  int channels() const { return sdl_device_channels_; }
};
}  // namespace

TEST_CASE("SDL audio uses the preferred output layout before opening", "[sdl_audio]") {
  int channels = 2;
  SECTION("Stereo output uses the normalized downmix") {}
  SECTION("Surround output keeps all six channels") {
    channels = 6;
  }
  REQUIRE(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"));
  const bool initialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
  INFO(SDL_GetError());
  REQUIRE(initialized);
  SDL_AudioDeviceID surround_device = 0;
  if (channels == 6) {
    // The dummy device starts in stereo; opening it models an active 5.1 device.
    SDL_AudioSpec surround{SDL_AUDIO_F32LE, 6, 48000};
    surround_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &surround);
    REQUIRE(surround_device != 0);
  }
  SDL_AudioSpec preferred{};
  REQUIRE(SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &preferred, nullptr));
  REQUIRE(preferred.channels == channels);
  {
    TestAudioDriver driver;
    REQUIRE(driver.Initialize());
    CHECK(driver.channels() == channels);
  }
  if (surround_device) {
    SDL_CloseAudioDevice(surround_device);
  }
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
}

TEST_CASE("Stereo downmix preserves headroom for correlated surround channels", "[sdl_audio]") {
  // All surround channels at full scale must not sum beyond full scale.
  // Check both signs and nonconstant samples across SIMD group boundaries.
  constexpr size_t samples = 256;
  std::array<float, samples * 6> input{};
  std::array<float, samples * 2> output{};
  for (size_t channel = 0; channel < 6; ++channel) {
    for (size_t sample = 0; sample < samples; ++sample) {
      const float value = (int(sample % 9) - 4) * 0.25f;
      input[channel * samples + sample] = rex::byte_swap(value);
    }
  }
  rex::audio::conversion::sequential_6_BE_to_interleaved_2_LE(
      output.data(), input.data(), samples);
  for (size_t sample = 0; sample < samples; ++sample) {
    const float expected = (int(sample % 9) - 4) * 0.25f;
    CHECK(std::abs(output[sample * 2] - expected) < 0.000001f);
    CHECK(std::abs(output[sample * 2 + 1] - expected) < 0.000001f);
    CHECK(std::abs(output[sample * 2]) <= 1.0f);
    CHECK(std::abs(output[sample * 2 + 1]) <= 1.0f);
  }
}
