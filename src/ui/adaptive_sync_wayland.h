#pragma once

#include <string>

struct wl_display;
struct wl_output;

namespace rex::ui {
// UI thread only. Uses a private queue/object and does not replace GTK's
// wl_output listener or dispatch GTK events. Empty means unavailable.
std::string WaylandDisplayOutputName(wl_display* display, wl_output* output);
}  // namespace rex::ui
