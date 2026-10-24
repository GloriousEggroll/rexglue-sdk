#pragma once
/**
 * Host networking for Xbox System Link play.
 *
 * The Xbox network stack (XNet) gave every console a secure address (XNADDR)
 * and translated it to an IP the title used with sockets; sessions carried an
 * ID (XNKID) and key (XNKEY). Here XNet addresses carry the host's real IPv4
 * address, keys are recorded but traffic is plain UDP/TCP, and sessions are
 * kept locally like System Link sessions on the console.
 *
 * Ports: titles bind fixed low ports (1000 and the like), so every guest port
 * below 1024 maps to 10000 + port on every host OS, keeping Linux and Windows
 * peers compatible without privileges.
 *
 * Several instances on one machine: give each its own loopback address
 * (network_address = 127.0.0.2, 127.0.0.3, ...) and list the others in
 * network_broadcast_peers; broadcasts are then sent to those addresses.
 */

#include <array>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace rex::system::xam {

class NetworkTunnel;

// Guest XNet structures, in guest byte order.
struct XNetKid {
  std::array<uint8_t, 8> ab{};
  bool operator==(const XNetKid&) const = default;
};
struct XNetKey {
  std::array<uint8_t, 16> ab{};
};
// XNADDR: ina, inaOnline (network order), wPortOnline (big endian), abEnet[6],
// abOnline[20]; 36 bytes.
struct XNetAddress {
  std::array<uint8_t, 36> bytes{};
  uint32_t ip() const;  // Host order.
  bool operator==(const XNetAddress&) const = default;
};
// XSESSION_INFO: sessionID, hostAddress, keyExchangeKey; 60 bytes.
struct XSessionInfo {
  XNetKid id;
  XNetAddress host;
  XNetKey key;
};

class Network {
 public:
  static Network& Get();

  // Local IPv4 address in host order: the room's virtual address when playing
  // through a lobby server (zero until admitted), else network_address, or the
  // address of the interface with the default route, or loopback.
  uint32_t local_address();
  // False while joining a lobby room; titles then see a pending address.
  bool address_ready() { return local_address() != 0; }

  // Internet play through a lobby server (network_lobby_server).
  bool tunnel_enabled();
  // True if traffic for `ip` (host order) goes through the lobby tunnel.
  bool TunnelDestination(uint32_t ip);
  bool TunnelSend(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t* data,
                  size_t size);
  // A free guest port for a socket bound to port zero in tunnel mode.
  uint16_t AllocateTunnelPort();
  // Game sockets by bound guest port, for tunnel delivery.
  void RegisterSocket(uint16_t guest_port, uint32_t handle);
  void UnregisterSocket(uint16_t guest_port, uint32_t handle);
  // Loopback datagram that wakes a socket's select/recvfrom after a tunnel
  // packet was queued; recvfrom discards it.
  void Wake(uint64_t native_socket);
  bool IsWake(uint32_t ip, uint16_t port);
  // Bind sockets to the local address rather than any (loopback test setups).
  bool bind_local_address();
  // Unicast targets replacing broadcasts (host order); empty for a real LAN
  // broadcast.
  std::vector<uint32_t> broadcast_peers();

  static uint16_t HostPort(uint16_t guest_port);
  static uint16_t GuestPort(uint16_t host_port);

  XNetAddress local_xnaddr();
  void Random(uint8_t* out, size_t size);

  void RegisterKey(const XNetKid& id, const XNetKey& key);
  void UnregisterKey(const XNetKid& id);
  XNetKid CreateSystemLinkKid();
  void RememberPeer(const XNetAddress& address);
  std::optional<XNetAddress> FindPeer(uint32_t ip);

  // Sessions. Handles and object pointers are opaque values the title passes
  // back; the object pointer is a small guest allocation so it is unique.
  struct Session {
    uint32_t object = 0;
    uint32_t flags = 0, user_index = 0;
    uint32_t max_public = 0, max_private = 0;
    bool host = false;
    uint32_t state = 0;  // XSESSION_STATE: lobby, registration, in game, reporting, deleted.
    uint64_t nonce = 0;
    XSessionInfo info;
    struct Member {
      uint64_t xuid = 0;
      uint32_t user_index = 0xFE;  // XUSER_INDEX_NONE for remote members.
      bool private_slot = false;
    };
    std::vector<Member> members;
    uint32_t remote_members() const;
  };
  uint32_t CreateSessionHandle();
  uint32_t SessionObject(uint32_t handle);
  // Runs `fn` with the session under the lock; false if unknown.
  template <typename Fn>
  bool WithSession(uint32_t object, Fn&& fn) {
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(object);
    if (it == sessions_.end()) return false;
    fn(it->second);
    return true;
  }
  void DeleteSession(uint32_t object);
  // True while any session has a player on another machine, or game traffic
  // from another machine arrived in the last 15 seconds.
  bool RemotePlayersPresent();
  // Called for every datagram/stream read from `ip` (host order).
  void NoteTraffic(uint32_t ip);
  // The frame-rate limit to apply: `limit` (0 = none), lowered to
  // network_frame_rate_lock while remote players are present so every machine
  // simulates at the same rate.
  static double FrameRateLimit(double limit);

  // XUserSetContext values (game type, game mode) for session details.
  void SetContext(uint32_t id, uint32_t value);
  uint32_t Context(uint32_t id);

 private:
  Network() = default;
  std::mutex mutex_;
  std::optional<uint32_t> local_address_;
  std::unordered_map<uint64_t, XNetKey> keys_;
  std::unordered_map<uint32_t, XNetAddress> peers_;
  std::unordered_map<uint32_t, uint32_t> handles_;  // handle -> object
  std::unordered_map<uint32_t, Session> sessions_;  // object -> session
  std::unordered_map<uint32_t, uint32_t> contexts_;
  uint32_t next_handle_ = 0;
  std::atomic<int64_t> last_peer_traffic_{0};  // steady_clock ticks
  std::unique_ptr<NetworkTunnel> tunnel_;
  std::once_flag tunnel_once_;
  std::unordered_map<uint16_t, uint32_t> sockets_;  // guest port -> handle
  uint16_t next_tunnel_port_ = 49152;
  intptr_t wake_socket_ = -1;
  uint16_t wake_port_ = 0;
};

}  // namespace rex::system::xam
