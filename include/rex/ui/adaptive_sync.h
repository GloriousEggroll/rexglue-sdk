#pragma once

#include <string>
#include <vector>

namespace rex::ui {

// Display-wide compositor state, not a swapchain/present-mode preference.
// Available means the compositor reported the state for this enabled output.
// This is not a hardware capability test or proof of variable scanout timing.
struct AdaptiveSyncDisplay {
  std::string name;
  std::string description;
  bool enabled = false;
  bool available = false;
  std::string reason;
};

// This bounded read-only query uses its own display connection. An empty list
// means no supported output-management connection could be established. It
// never creates, tests, applies, or restores a display configuration.
std::vector<AdaptiveSyncDisplay> ProbeAdaptiveSyncDisplays();

}  // namespace rex::ui
