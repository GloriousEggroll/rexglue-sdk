/**
 * @file        ui/windowed_app_main_sdl.cpp
 * @brief       Entry point for windowed apps on SDL3 (macOS).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <mach-o/dyld.h>
#include <unistd.h>

namespace {
// The Vulkan loader and MoltenVK are opened by bare name, which dyld looks up
// only in DYLD_FALLBACK_LIBRARY_PATH: put the libraries a package carries
// (../lib, then the executable's folder) and Homebrew's on it, then restart
// once so dyld sees the variable. A package's MoltenVK driver description is
// used unless the player chose Vulkan drivers.
void EnsureLibrarySearchPath(char** argv) {
  constexpr const char* kVariable = "DYLD_FALLBACK_LIBRARY_PATH";
  constexpr const char* kMarker = "SQUAREHOLE_LIBRARY_PATH_SET";
  if (std::getenv(kMarker)) return;
  char executable[4096];
  uint32_t size = sizeof(executable);
  if (_NSGetExecutablePath(executable, &size) != 0) return;
  std::string folder = executable;
  folder.erase(folder.find_last_of('/'));
  const std::string lib = folder + "/../lib";
  std::string value = lib + ":" + folder + ":/opt/homebrew/lib:/usr/local/lib:/usr/lib";
  if (const char* current = std::getenv(kVariable); current && *current) {
    value += std::string(":") + current;
  }
  setenv(kVariable, value.c_str(), 1);
  const std::string driver = lib + "/vulkan/icd.d/MoltenVK_icd.json";
  if (!std::getenv("VK_DRIVER_FILES") && !std::getenv("VK_ICD_FILENAMES") &&
      access(driver.c_str(), R_OK) == 0) {
    setenv("VK_DRIVER_FILES", driver.c_str(), 1);
  }
  setenv(kMarker, "1", 1);
  execv(executable, argv);
  // If restarting fails, continue; libraries next to the system ones still load.
}
}  // namespace

extern "C" int main(int argc, char** argv) {
  EnsureLibrarySearchPath(argv);
  SDL_SetMainReady();
  // Gamepads are handled by the input driver; keep their events off the
  // window loop until it initializes them.
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "Failed to initialize SDL video: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  int result;
  {
    rex::ui::SDLWindowedAppContext app_context;

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    // Match remaining positional args to app's expected options
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    if (app->OnInitialize()) {
      app_context.RunMainSDLLoop();
      result = EXIT_SUCCESS;
    } else {
      result = EXIT_FAILURE;
    }

    app->InvokeOnDestroy();
  }

  // Logging may still be needed in the destructors.
  rex::ShutdownLogging();
  SDL_Quit();

  return result;
}
