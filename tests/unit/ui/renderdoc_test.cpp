#include <catch2/catch_test_macros.hpp>
#include <rex/ui/renderdoc_api.h>

#if REX_PLATFORM_LINUX
#include <dlfcn.h>

TEST_CASE("RenderDoc detection does not attach the debugger", "[renderdoc_detection]") {
  const char* name = rex::platform::lib_names::kRenderDoc;
  if (void* existing = dlopen(name, RTLD_NOW | RTLD_NOLOAD)) {
    dlclose(existing);
    SKIP("Run this regression without an injected RenderDoc library");
  }

  CHECK(rex::ui::RenderDocAPI::CreateIfConnected() == nullptr);
  void* after_detection = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
  CHECK(after_detection == nullptr);
  if (after_detection) {
    dlclose(after_detection);
  }
}
#endif
