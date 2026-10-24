#include <rex/system/xam/network.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/network_tunnel.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xsocket.h>

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_STRING(network_address, "", "Network",
                      "IPv4 address other players reach you at. Empty detects the address of "
                      "the network interface with the default route");
REXCVAR_DEFINE_STRING(network_lobby_server, "", "Network",
                      "Lobby server for internet play (host or host:port). Empty plays on the "
                      "local network only");
REXCVAR_DEFINE_STRING(network_lobby_room, "public", "Network",
                      "Room on the lobby server; only players in the same room see each other");
REXCVAR_DEFINE_STRING(network_lobby_password, "", "Network",
                      "Room password; the first player in a room sets it");
REXCVAR_DECLARE(bool, network_enabled);
REXCVAR_DEFINE_INT32(network_frame_rate_lock, 60, "Network",
                     "Frame-rate limit while playing with other machines; 0 keeps the normal limit")
    .range(0, 1000);
REXCVAR_DEFINE_STRING(network_broadcast_peers, "", "Network",
                      "Comma-separated IPv4 addresses that receive System Link broadcasts "
                      "instead of the LAN broadcast (several instances on one machine)");

namespace rex::system::xam {

namespace {
std::optional<uint32_t> ParseIpv4(const std::string& text) {
  in_addr address{};
  if (inet_pton(AF_INET, text.c_str(), &address) != 1) return std::nullopt;
  return ntohl(address.s_addr);
}

// Address of the interface the OS would route to the internet through; a UDP
// connect sends no packets.
std::optional<uint32_t> DetectAddress() {
  const auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if REX_PLATFORM_WIN32
  if (handle == INVALID_SOCKET) return std::nullopt;
#else
  if (handle < 0) return std::nullopt;
#endif
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(9);
  target.sin_addr.s_addr = htonl(0xC0000201);  // 192.0.2.1, documentation range
  std::optional<uint32_t> result;
  if (::connect(handle, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&local), &length) == 0 &&
        local.sin_addr.s_addr != 0)
      result = ntohl(local.sin_addr.s_addr);
  }
#if REX_PLATFORM_WIN32
  ::closesocket(handle);
#else
  ::close(handle);
#endif
  return result;
}

uint64_t KidKey(const XNetKid& id) {
  uint64_t value;
  std::memcpy(&value, id.ab.data(), sizeof(value));
  return value;
}

std::string Ipv4(uint32_t address) {
  return std::to_string(address >> 24) + "." + std::to_string((address >> 16) & 255) + "." +
         std::to_string((address >> 8) & 255) + "." + std::to_string(address & 255);
}
}  // namespace

uint32_t XNetAddress::ip() const {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
         bytes[3];
}

Network& Network::Get() {
  static Network* network = new Network;
  return *network;
}

bool Network::tunnel_enabled() {
  if (!REXCVAR_GET(network_enabled) || REXCVAR_GET(network_lobby_server).empty()) return false;
  std::call_once(tunnel_once_, [this] {
    NetworkTunnel::Config config;
    config.server = REXCVAR_GET(network_lobby_server);
    config.room = REXCVAR_GET(network_lobby_room);
    config.password = REXCVAR_GET(network_lobby_password);
    if (auto* state = REX_KERNEL_STATE()) {
      config.title_id = state->title_id();
      if (state->user_profile()) {
        config.xuid = state->user_profile()->xuid();
        config.gamertag = state->user_profile()->name();
      }
    }
    tunnel_ = std::make_unique<NetworkTunnel>(
        config, [this](uint32_t src_vip, uint16_t src_port, uint16_t dst_port, const uint8_t* data,
                       size_t size) {
          uint32_t handle = 0;
          {
            std::lock_guard lock(mutex_);
            const auto it = sockets_.find(dst_port);
            if (it == sockets_.end()) return false;
            handle = it->second;
          }
          auto socket = REX_KERNEL_STATE()->object_table()->LookupObject<XSocket>(handle);
          if (!socket) return false;
          NoteTraffic(src_vip);
          socket->QueuePacket(src_vip, src_port, data, size);
          Wake(socket->native_handle());
          return true;
        });
  });
  return true;
}

bool Network::TunnelDestination(uint32_t ip) {
  return tunnel_enabled() && (lobby::IsVirtual(ip) || ip == lobby::kBroadcast);
}

bool Network::TunnelSend(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
                         const uint8_t* data, size_t size) {
  return tunnel_enabled() && tunnel_->Send(src_port, dst_ip, dst_port, data, size);
}

uint16_t Network::AllocateTunnelPort() {
  std::lock_guard lock(mutex_);
  for (int i = 0; i < 16384; ++i) {
    const uint16_t port = next_tunnel_port_;
    next_tunnel_port_ = next_tunnel_port_ == 65535 ? 49152 : next_tunnel_port_ + 1;
    if (!sockets_.contains(port)) return port;
  }
  return 0;
}

void Network::RegisterSocket(uint16_t guest_port, uint32_t handle) {
  if (!guest_port) return;
  std::lock_guard lock(mutex_);
  sockets_[guest_port] = handle;
}

void Network::UnregisterSocket(uint16_t guest_port, uint32_t handle) {
  std::lock_guard lock(mutex_);
  if (const auto it = sockets_.find(guest_port); it != sockets_.end() && it->second == handle)
    sockets_.erase(it);
}

void Network::Wake(uint64_t native_socket) {
  sockaddr_in target{};
  socklen_t length = sizeof(target);
#if REX_PLATFORM_WIN32
  using Handle = SOCKET;
#else
  using Handle = int;
#endif
  if (getsockname(Handle(native_socket), reinterpret_cast<sockaddr*>(&target), &length) != 0) return;
  if (!target.sin_addr.s_addr) target.sin_addr.s_addr = htonl(0x7F000001);
  std::lock_guard lock(mutex_);
  if (wake_socket_ < 0) {
    const Handle handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(0x7F000001);
    socklen_t local_length = sizeof(local);
    if (::bind(handle, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
        ::getsockname(handle, reinterpret_cast<sockaddr*>(&local), &local_length) != 0)
      return;
    wake_socket_ = intptr_t(handle);
    wake_port_ = ntohs(local.sin_port);
  }
  const char byte = 'W';
  ::sendto(Handle(wake_socket_), &byte, 1, 0, reinterpret_cast<sockaddr*>(&target), sizeof(target));
}

bool Network::IsWake(uint32_t ip, uint16_t port) {
  std::lock_guard lock(mutex_);
  return wake_socket_ >= 0 && ip == 0x7F000001 && port == wake_port_;
}

uint32_t Network::local_address() {
  if (tunnel_enabled()) return tunnel_->vip();
  std::lock_guard lock(mutex_);
  if (!local_address_) {
    const std::string configured = REXCVAR_GET(network_address);
    if (!configured.empty()) {
      local_address_ = ParseIpv4(configured);
      if (!local_address_) REXSYS_WARN("Network: invalid network_address '{}'", configured);
    }
    if (!local_address_) local_address_ = DetectAddress();
    if (!local_address_) local_address_ = 0x7F000001;
    REXSYS_INFO("Network: local address {}", Ipv4(*local_address_));
  }
  return *local_address_;
}

bool Network::bind_local_address() {
  // A socket bound to a unicast address does not receive LAN broadcasts, so
  // only loopback test setups bind to their address. Virtual addresses are not
  // host interfaces.
  if (tunnel_enabled()) return false;
  return (local_address() >> 24) == 127;
}

std::vector<uint32_t> Network::broadcast_peers() {
  std::vector<uint32_t> peers;
  std::stringstream list(REXCVAR_GET(network_broadcast_peers));
  std::string item;
  while (std::getline(list, item, ',')) {
    item.erase(0, item.find_first_not_of(' '));
    item.erase(item.find_last_not_of(' ') + 1);
    if (item.empty()) continue;
    if (const auto address = ParseIpv4(item)) peers.push_back(*address);
    else REXSYS_WARN("Network: invalid broadcast peer '{}'", item);
  }
  return peers;
}

uint16_t Network::HostPort(uint16_t guest_port) {
  return guest_port && guest_port < 1024 ? uint16_t(guest_port + 10000) : guest_port;
}

uint16_t Network::GuestPort(uint16_t host_port) {
  return host_port >= 10001 && host_port < 11024 ? uint16_t(host_port - 10000) : host_port;
}

XNetAddress Network::local_xnaddr() {
  XNetAddress address;
  const uint32_t ip = local_address();
  for (int i = 0; i < 4; ++i) {
    address.bytes[i] = uint8_t(ip >> (24 - 8 * i));      // ina
    address.bytes[4 + i] = uint8_t(ip >> (24 - 8 * i));  // inaOnline
  }
  // abEnet: a locally administered MAC derived from the profile XUID and the
  // address, so it is unique per player (some titles seed IDs from it).
  uint64_t xuid = 0;
  if (auto* state = REX_KERNEL_STATE(); state && state->user_profile())
    xuid = state->user_profile()->xuid();
  const uint64_t mac = xuid ^ (uint64_t(ip) << 16);
  address.bytes[10] = 0x02;
  for (int i = 1; i < 6; ++i) address.bytes[10 + i] = uint8_t(mac >> (8 * (5 - i)));
  // abOnline: the XUID, for diagnostics.
  for (int i = 0; i < 8; ++i) address.bytes[16 + i] = uint8_t(xuid >> (56 - 8 * i));
  return address;
}

void Network::Random(uint8_t* out, size_t size) {
  static thread_local std::mt19937_64 generator{std::random_device{}()};
  for (size_t i = 0; i < size; ++i) out[i] = uint8_t(generator());
}

void Network::RegisterKey(const XNetKid& id, const XNetKey& key) {
  std::lock_guard lock(mutex_);
  keys_[KidKey(id)] = key;
}

void Network::UnregisterKey(const XNetKid& id) {
  std::lock_guard lock(mutex_);
  keys_.erase(KidKey(id));
}

XNetKid Network::CreateSystemLinkKid() {
  XNetKid id;
  Random(id.ab.data(), id.ab.size());
  id.ab[0] &= 0x0F;  // XNET_XNKID_SYSTEM_LINK
  return id;
}

void Network::RememberPeer(const XNetAddress& address) {
  std::lock_guard lock(mutex_);
  peers_[address.ip()] = address;
}

std::optional<XNetAddress> Network::FindPeer(uint32_t ip) {
  if (ip == local_address()) return local_xnaddr();
  std::lock_guard lock(mutex_);
  const auto it = peers_.find(ip);
  if (it == peers_.end()) return std::nullopt;
  return it->second;
}

uint32_t Network::Session::remote_members() const {
  uint32_t count = 0;
  for (const auto& member : members) count += member.user_index == 0xFE;
  return count;
}

uint32_t Network::CreateSessionHandle() {
  // Opaque to the title; never a kernel object.
  const uint32_t object = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(16);
  std::lock_guard lock(mutex_);
  const uint32_t handle = 0xF5E50000u | (++next_handle_ & 0xFFFF);
  handles_[handle] = object;
  sessions_[object].object = object;
  return handle;
}

uint32_t Network::SessionObject(uint32_t handle) {
  std::lock_guard lock(mutex_);
  const auto it = handles_.find(handle);
  return it == handles_.end() ? 0 : it->second;
}

void Network::DeleteSession(uint32_t object) {
  std::lock_guard lock(mutex_);
  if (auto it = sessions_.find(object); it != sessions_.end()) {
    keys_.erase(KidKey(it->second.info.id));
    it->second = Session{};
    it->second.object = object;
    it->second.state = 5;  // XSESSION_STATE_DELETED
  }
}

void Network::NoteTraffic(uint32_t ip) {
  if (!ip || ip == local_address()) return;
  last_peer_traffic_.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                           std::memory_order_relaxed);
}

bool Network::RemotePlayersPresent() {
  const auto last = std::chrono::steady_clock::time_point(
      std::chrono::steady_clock::duration(last_peer_traffic_.load(std::memory_order_relaxed)));
  // Level loads pause traffic for several seconds; keep the lock through them.
  if (std::chrono::steady_clock::now() - last < std::chrono::seconds(15)) return true;
  std::lock_guard lock(mutex_);
  for (const auto& [object, session] : sessions_)
    if (session.state != 5 && session.remote_members()) return true;
  return false;
}

double Network::FrameRateLimit(double limit) {
  const double lock = REXCVAR_GET(network_frame_rate_lock);
  const bool locked = lock > 0 && Get().RemotePlayersPresent();
  static std::atomic<bool> was_locked{false};
  if (was_locked.exchange(locked) != locked)
    REXSYS_INFO("Network: {} the {:g} FPS multiplayer lock", locked ? "applying" : "releasing", lock);
  if (!locked) return limit;
  return limit > 0 ? std::min(limit, lock) : lock;
}

void Network::SetContext(uint32_t id, uint32_t value) {
  std::lock_guard lock(mutex_);
  contexts_[id] = value;
}

uint32_t Network::Context(uint32_t id) {
  std::lock_guard lock(mutex_);
  const auto it = contexts_.find(id);
  return it == contexts_.end() ? 0 : it->second;
}

}  // namespace rex::system::xam
