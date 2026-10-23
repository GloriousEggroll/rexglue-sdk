#pragma once
#include <string>
#include <vector>

struct wl_display;
struct wl_output;
struct wl_event_queue;

namespace rex::ui {
struct HdrDisplay {
  std::string name;
  std::string description;
  bool hdr = false;
  float peak_nits = 0;
};
// Live compositor output descriptions, not EDID or saved desktop preferences.
// Missing protocols, connection failures and timeouts report unavailable.
std::vector<HdrDisplay> ProbeHdrDisplays();
HdrDisplay ProbeHdrOutput(wl_display* display, wl_output* output);
// Bounded round trip on an isolated queue; does not dispatch GTK events.
bool WaylandRoundtrip(wl_display* display, struct wl_event_queue* queue, int timeout_ms = 500);
// The presenter publishes the actual selected surface's capability to the
// in-game settings overlay, including Vulkan HDR10 format support.
bool IsNativeHdrAvailable();
void SetNativeHdrAvailable(bool available);
}
