// Platform-independent part of hdr_display.h: the presenter's HDR capability
// flag. Display probing itself is Wayland-specific (hdr_display.cpp).
#include <rex/ui/hdr_display.h>
#include <atomic>

namespace rex::ui {
namespace {
std::atomic<bool> native_hdr_available{false};
}
bool IsNativeHdrAvailable() { return native_hdr_available.load(std::memory_order_relaxed); }
void SetNativeHdrAvailable(bool available) { native_hdr_available.store(available, std::memory_order_relaxed); }
}
