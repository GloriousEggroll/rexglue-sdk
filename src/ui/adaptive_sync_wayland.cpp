#include <rex/ui/adaptive_sync.h>
#include "adaptive_sync_wayland.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <poll.h>
#include <wayland-client.h>
#include "wayland/wlr-output-management-unstable-v1-client.h"
#include "wayland/xdg-output-unstable-v1-client.h"

namespace rex::ui {
namespace {
constexpr int kTimeoutMs = 500;

// Dispatch only our queue, including when the display belongs to GTK. Respect
// prepare_read/cancel_read pairing and socket backpressure. No nested GTK loop.
template <typename Ready>
bool PumpUntil(wl_display* display, wl_event_queue* queue, Ready ready,
               int timeout_ms = kTimeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (wl_display_dispatch_queue_pending(display, queue) < 0) return false;
    if (ready()) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    if (wl_display_prepare_read_queue(display, queue) != 0) continue;
    const int flushed = wl_display_flush(display);
    if (flushed < 0 && errno != EAGAIN) {
      wl_display_cancel_read(display);
      return false;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    pollfd fd{wl_display_get_fd(display),
              short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0};
    const int polled = poll(&fd, 1, int(std::max<int64_t>(1, remaining)));
    if (polled <= 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      wl_display_cancel_read(display);
      if (polled < 0 && errno == EINTR) continue;
      return false;
    }
    if (fd.revents & POLLIN) {
      if (wl_display_read_events(display) < 0) return false;
    } else {
      wl_display_cancel_read(display);
    }
  }
}

bool Sync(wl_display* display, wl_event_queue* queue, int timeout_ms = kTimeoutMs) {
  auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
  if (!wrapper) return false;
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
  auto* callback = wl_display_sync(wrapper);
  wl_proxy_wrapper_destroy(wrapper);
  if (!callback) return false;
  bool done = false;
  static const wl_callback_listener listener = {
      [](void* p, wl_callback*, uint32_t) { *static_cast<bool*>(p) = true; }};
  wl_callback_add_listener(callback, &listener, &done);
  const bool ok = PumpUntil(display, queue, [&] { return done; }, timeout_ms);
  wl_callback_destroy(callback);
  return ok;
}

template <typename T> void DestroyProxy(T*& object) {
  if (object) wl_proxy_destroy(reinterpret_cast<wl_proxy*>(object));
  object = nullptr;
}

struct Mode {
  zwlr_output_mode_v1* object = nullptr;
  int32_t width = 0, height = 0, refresh = 0;
  bool alive = true;
  bool* dirty = nullptr;
  void Touch() { *dirty = true; }
};
struct Head {
  zwlr_output_head_v1* object = nullptr;
  std::string name, description;
  bool alive = true, enabled = false, enabled_known = false;
  bool adaptive = false, adaptive_known = false;
  int32_t x = 0, y = 0, transform = WL_OUTPUT_TRANSFORM_NORMAL;
  wl_fixed_t scale = wl_fixed_from_int(1);
  Mode* current = nullptr;
  bool position_known = false, transform_known = false, scale_known = false;
  bool* dirty = nullptr;
  void Touch() { *dirty = true; }
  std::vector<std::unique_ptr<Mode>> modes;
};

const zwlr_output_mode_v1_listener mode_listener = {
    .size = [](void* p, zwlr_output_mode_v1*, int32_t w, int32_t h) {
      auto& m = *static_cast<Mode*>(p); m.Touch(); m.width = w; m.height = h;
    },
    .refresh = [](void* p, zwlr_output_mode_v1*, int32_t refresh) {
      auto& m = *static_cast<Mode*>(p); m.Touch(); m.refresh = refresh;
    },
    .preferred = [](void*, zwlr_output_mode_v1*) {},
    .finished = [](void* p, zwlr_output_mode_v1*) { auto& m = *static_cast<Mode*>(p); m.Touch(); m.alive = false; },
};
const zwlr_output_head_v1_listener head_listener = {
    .name = [](void* p, zwlr_output_head_v1*, const char* s) { auto& h = *static_cast<Head*>(p); h.Touch(); h.name = s; },
    .description = [](void* p, zwlr_output_head_v1*, const char* s) { auto& h = *static_cast<Head*>(p); h.Touch(); h.description = s; },
    .physical_size = [](void*, zwlr_output_head_v1*, int32_t, int32_t) {},
    .mode = [](void* p, zwlr_output_head_v1*, zwlr_output_mode_v1* object) {
      auto& h = *static_cast<Head*>(p); h.Touch();
      auto mode = std::make_unique<Mode>(); mode->object = object; mode->dirty = h.dirty;
      zwlr_output_mode_v1_add_listener(object, &mode_listener, mode.get());
      h.modes.push_back(std::move(mode));
    },
    .enabled = [](void* p, zwlr_output_head_v1*, int32_t enabled) {
      auto& h = *static_cast<Head*>(p); h.Touch(); h.enabled = enabled != 0; h.enabled_known = true;
    },
    .current_mode = [](void* p, zwlr_output_head_v1*, zwlr_output_mode_v1* mode) {
      auto& h = *static_cast<Head*>(p); h.Touch(); h.current = nullptr;
      for (auto& m : h.modes) if (m->object == mode) h.current = m.get();
    },
    .position = [](void* p, zwlr_output_head_v1*, int32_t x, int32_t y) {
      auto& h = *static_cast<Head*>(p); h.Touch(); h.x = x; h.y = y; h.position_known = true;
    },
    .transform = [](void* p, zwlr_output_head_v1*, int32_t value) { auto& h = *static_cast<Head*>(p); h.Touch(); h.transform = value; h.transform_known = true; },
    .scale = [](void* p, zwlr_output_head_v1*, wl_fixed_t value) { auto& h = *static_cast<Head*>(p); h.Touch(); h.scale = value; h.scale_known = true; },
    .finished = [](void* p, zwlr_output_head_v1*) { auto& h = *static_cast<Head*>(p); h.Touch(); h.alive = false; },
    .make = [](void*, zwlr_output_head_v1*, const char*) {},
    .model = [](void*, zwlr_output_head_v1*, const char*) {},
    .serial_number = [](void*, zwlr_output_head_v1*, const char*) {},
    .adaptive_sync = [](void* p, zwlr_output_head_v1*, uint32_t value) {
      auto& h = *static_cast<Head*>(p);
      h.Touch();
      h.adaptive_known = value <= ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED;
      h.adaptive = value == ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED;
    },
};

struct Session {
  wl_display* display = nullptr;
  wl_event_queue* queue = nullptr;
  wl_registry* registry = nullptr;
  zwlr_output_manager_v1* manager = nullptr;
  uint32_t manager_id = 0, version = 0, serial = 0;
  uint64_t done_count = 0;
  bool finished = false;
  bool dirty = false;
  std::vector<std::unique_ptr<Head>> heads;
  ~Session() {
    // The owned connection is immediately disconnected; local proxy teardown
    // leaves server-side destruction to disconnect, including finished heads.
    for (auto& h : heads) {
      for (auto& m : h->modes) DestroyProxy(m->object);
      DestroyProxy(h->object);
    }
    DestroyProxy(manager);
    if (registry) wl_registry_destroy(registry);
    if (queue) wl_event_queue_destroy(queue);
    if (display) wl_display_disconnect(display);
  }
  bool Open() {
    display = wl_display_connect(nullptr);
    if (!display) return false;
    queue = wl_display_create_queue(display);
    if (!queue) return false;
    auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
    if (!wrapper) return false;
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
    registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    if (!registry) return false;
    static const zwlr_output_manager_v1_listener manager_listener = {
        .head = [](void* p, zwlr_output_manager_v1*, zwlr_output_head_v1* object) {
          auto& s = *static_cast<Session*>(p); s.dirty = true;
          auto head = std::make_unique<Head>(); head->object = object; head->dirty = &s.dirty;
          zwlr_output_head_v1_add_listener(object, &head_listener, head.get());
          s.heads.push_back(std::move(head));
        },
        .done = [](void* p, zwlr_output_manager_v1*, uint32_t serial) {
          auto& s = *static_cast<Session*>(p); s.serial = serial; ++s.done_count; s.dirty = false;
        },
        .finished = [](void* p, zwlr_output_manager_v1*) { static_cast<Session*>(p)->finished = true; },
    };
    static const wl_registry_listener listener = {
        .global = [](void* p, wl_registry* registry, uint32_t name, const char* type, uint32_t version) {
          auto& s = *static_cast<Session*>(p);
          if (std::strcmp(type, "zwlr_output_manager_v1") || s.manager) return;
          s.version = std::min(version, 4u); s.manager_id = name;
          s.manager = static_cast<zwlr_output_manager_v1*>(
              wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, s.version));
          if (s.manager) zwlr_output_manager_v1_add_listener(s.manager, &manager_listener, p);
        },
        .global_remove = [](void* p, wl_registry*, uint32_t name) {
          auto& s = *static_cast<Session*>(p); if (name == s.manager_id) s.finished = true;
        },
    };
    wl_registry_add_listener(registry, &listener, this);
    return Sync(display, queue) && manager &&
           PumpUntil(display, queue, [&] { return (done_count && !dirty) || finished; }) && !finished;
  }
  Head* Find(const std::string& name) const {
    Head* found = nullptr;
    for (const auto& h : heads) if (h->alive && h->name == name) {
      if (found) return nullptr;  // Never choose an ambiguous head.
      found = h.get();
    }
    return found;
  }
  AdaptiveSyncDisplay Describe(const Head& h) const {
    AdaptiveSyncDisplay d{h.name, h.description, h.adaptive, false, {}};
    if (finished || !h.alive) d.reason = "Display is no longer available";
    else if (version < 4) d.reason = "Compositor lacks output-management version 4";
    else if (!h.enabled_known || !h.enabled) d.reason = "Display is disabled";
    else if (!h.adaptive_known) d.reason = "Compositor did not report the display VRR state";
    else if (h.name.empty() || Find(h.name) != &h) d.reason = "Display name is unavailable or ambiguous";
    else {
      d.available = true;
      d.reason = "Read-only compositor VRR state";
    }
    return d;
  }
};

}  // namespace

std::vector<AdaptiveSyncDisplay> ProbeAdaptiveSyncDisplays() {
  Session session;
  if (!session.Open()) return {};
  std::vector<AdaptiveSyncDisplay> result;
  for (const auto& h : session.heads) if (h->alive) result.push_back(session.Describe(*h));
  return result;
}

std::string WaylandDisplayOutputName(wl_display* display, wl_output* output) {
  if (!display || !output) return {};
  auto* queue = wl_display_create_queue(display);
  if (!queue) return {};
  auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
  if (!wrapper) { wl_event_queue_destroy(queue); return {}; }
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
  auto* registry = wl_display_get_registry(wrapper);
  wl_proxy_wrapper_destroy(wrapper);
  if (!registry) { wl_event_queue_destroy(queue); return {}; }
  struct Probe { zxdg_output_manager_v1* manager = nullptr; std::string name; bool done = false; } probe;
  static const wl_registry_listener registry_listener = {
      .global = [](void* p, wl_registry* registry, uint32_t name, const char* type, uint32_t version) {
        auto& probe = *static_cast<Probe*>(p);
        if (std::strcmp(type, "zxdg_output_manager_v1") || version < 2 || probe.manager) return;
        // v2 has a private done event; v3 instead uses GTK-owned wl_output.done.
        probe.manager = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 2));
      },
      .global_remove = [](void*, wl_registry*, uint32_t) {},
  };
  wl_registry_add_listener(registry, &registry_listener, &probe);
  zxdg_output_v1* xdg_output = nullptr;
  bool ok = Sync(display, queue, 250) && probe.manager;
  if (ok) {
    xdg_output = zxdg_output_manager_v1_get_xdg_output(probe.manager, output);
    if (xdg_output) {
      static const zxdg_output_v1_listener listener = {
          .logical_position = [](void*, zxdg_output_v1*, int32_t, int32_t) {},
          .logical_size = [](void*, zxdg_output_v1*, int32_t, int32_t) {},
          .done = [](void* p, zxdg_output_v1*) { static_cast<Probe*>(p)->done = true; },
          .name = [](void* p, zxdg_output_v1*, const char* name) { static_cast<Probe*>(p)->name = name; },
          .description = [](void*, zxdg_output_v1*, const char*) {},
      };
      zxdg_output_v1_add_listener(xdg_output, &listener, &probe);
      ok = PumpUntil(display, queue, [&] { return probe.done; }, 250);
    } else ok = false;
  }
  if (xdg_output) zxdg_output_v1_destroy(xdg_output);
  if (probe.manager) zxdg_output_manager_v1_destroy(probe.manager);
  wl_registry_destroy(registry);
  wl_event_queue_destroy(queue);
  return ok ? probe.name : std::string{};
}
}  // namespace rex::ui
