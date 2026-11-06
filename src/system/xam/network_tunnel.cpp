#include <rex/system/xam/network_tunnel.h>

#include <random>

#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rex::system::xam {

namespace {
using namespace rex::lobby;
constexpr auto kHelloInterval = std::chrono::seconds(2);
constexpr auto kKeepAliveInterval = std::chrono::seconds(1);
constexpr auto kProbeInterval = std::chrono::seconds(2);
constexpr auto kDirectTimeout = std::chrono::seconds(10);
constexpr auto kServerTimeout = std::chrono::seconds(5);
// How long the configured server gets to answer before the LAN is searched,
// and how often it is tried again while there is no server.
constexpr auto kConfiguredWait = std::chrono::seconds(3);
constexpr auto kConfiguredRetry = std::chrono::seconds(10);
// A found server that does not admit us in this time is given up.
constexpr auto kJoinWait = std::chrono::seconds(4);
constexpr auto kDiscoverInterval = std::chrono::milliseconds(500);
// Offers collected after the first one before choosing.
constexpr auto kOfferWindow = std::chrono::milliseconds(150);
// The LAN search goes on this long after the title last tried to send.
constexpr auto kSearchTime = std::chrono::seconds(30);
// Broadcasts held for delivery on joining: the newest few, this recent.
constexpr size_t kMaxHeld = 8;
constexpr auto kHeldAge = std::chrono::seconds(5);

#if REX_PLATFORM_WIN32
using SocketHandle = SOCKET;
void CloseSocket(SocketHandle s) { closesocket(s); }
#else
using SocketHandle = int;
void CloseSocket(SocketHandle s) { close(s); }
#endif

std::string Ipv4(uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." +
         std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
}

uint64_t Pack(uint32_t ip, uint16_t port) { return uint64_t(ip) << 16 | port; }
uint32_t PackedIp(uint64_t packed) { return uint32_t(packed >> 16); }
uint16_t PackedPort(uint64_t packed) { return uint16_t(packed); }

// 10.69.x.y from the profile ID: the same on every server and every run.
uint32_t AddressFor(uint64_t xuid) {
  uint64_t h = xuid ? xuid : std::random_device{}();
  for (;;) {
    h += 0x9E3779B97F4A7C15ull;
    uint64_t z = h;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    const uint32_t vip = kVirtualNetwork | uint32_t(z & 0xFFFF);
    if ((vip & 0xFF) != 0 && (vip & 0xFF) != 0xFF) return vip;
  }
}
}  // namespace

NetworkTunnel::NetworkTunnel(Config config, Deliver deliver, PeerGone peer_gone)
    : config_(std::move(config)), deliver_(std::move(deliver)), peer_gone_(std::move(peer_gone)) {
  nonce_ = uint32_t(std::random_device{}());
  requested_vip_ = AddressFor(config_.xuid);
  vip_ = requested_vip_;
  thread_ = std::thread([this] { Run(); });
}

NetworkTunnel::~NetworkTunnel() { Stop(); }

void NetworkTunnel::Stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

bool NetworkTunnel::Resolve() {
  if (config_.server.empty()) return false;
  std::string host = config_.server;
  uint16_t port = kDefaultPort;
  if (const auto colon = host.rfind(':'); colon != std::string::npos) {
    port = uint16_t(std::strtoul(host.c_str() + colon + 1, nullptr, 10));
    host.resize(colon);
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) return false;
  configured_ip_ = ntohl(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr);
  configured_port_ = port ? port : kDefaultPort;
  freeaddrinfo(result);
  return true;
}

void NetworkTunnel::SendRaw(uint32_t ip, uint16_t port, const std::vector<uint8_t>& bytes) {
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(ip);
  to.sin_port = htons(port);
  sendto(SocketHandle(socket_), reinterpret_cast<const char*>(bytes.data()), int(bytes.size()), 0,
         reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

bool NetworkTunnel::Send(uint16_t src_port, uint32_t dst_vip, uint16_t dst_port,
                         const uint8_t* data, size_t size) {
  const uint64_t target = target_.load(std::memory_order_acquire);
  if (size > kMaxPayload || socket_ < 0) return false;
  if (!joined() || !target) {
    const auto now = Clock::now();
    last_wanted_.store(now.time_since_epoch().count(), std::memory_order_relaxed);
    if (dst_vip == kBroadcast) {
      std::lock_guard lock(mutex_);
      if (held_.size() >= kMaxHeld) held_.erase(held_.begin());
      held_.push_back({src_port, dst_port, std::vector<uint8_t>(data, data + size), now});
    }
    return false;
  }
  const auto bytes = Writer(Type::kData).U32(vip()).U64(token_.load()).U32(dst_vip).U16(src_port)
                         .U16(dst_port).Bytes(data, size).bytes();
  REXSYS_DEBUG("Lobby: send {} bytes :{} -> {}:{}", size, src_port, Ipv4(dst_vip), dst_port);
  if (dst_vip != kBroadcast) {
    std::lock_guard lock(mutex_);
    const auto it = peers_.find(dst_vip);
    if (it != peers_.end() && it->second.direct &&
        Clock::now() - it->second.last_direct < kDirectTimeout) {
      SendRaw(it->second.info.public_ip, it->second.info.public_port, bytes);
      return true;
    }
  }
  SendRaw(PackedIp(target), PackedPort(target), bytes);  // The server fans broadcasts out.
  return true;
}

std::vector<lobby::Member> NetworkTunnel::members() {
  std::lock_guard lock(mutex_);
  std::vector<lobby::Member> list;
  for (const auto& [vip, peer] : peers_) list.push_back(peer.info);
  return list;
}

bool NetworkTunnel::Wanted(Clock::time_point now) const {
  const int64_t last = last_wanted_.load(std::memory_order_relaxed);
  return last && now - Clock::time_point(Clock::duration(last)) < kSearchTime;
}

void NetworkTunnel::FlushHeld() {
  std::vector<Held> held;
  {
    std::lock_guard lock(mutex_);
    held.swap(held_);
  }
  const auto now = Clock::now();
  for (const auto& packet : held)
    if (now - packet.when < kHeldAge)
      Send(packet.src_port, kBroadcast, packet.dst_port, packet.data.data(), packet.data.size());
}

void NetworkTunnel::UseLocalServer(uint32_t ip, uint16_t port) {
  std::lock_guard lock(mutex_);
  local_request_ = Pack(ip, port);
}

bool NetworkTunnel::FromTarget(uint32_t ip, uint16_t port) const {
  const uint64_t target = target_.load(std::memory_order_acquire);
  return target && PackedIp(target) == ip && PackedPort(target) == port;
}

void NetworkTunnel::SetTarget(uint32_t ip, uint16_t port, Source source) {
  target_.store(ip ? Pack(ip, port) : 0, std::memory_order_release);
  source_ = source;
  target_since_ = Clock::now();
  last_hello_ = {};
  reject_reason_.clear();
  if (source == Source::kNone) {
    best_offer_ = 0;
    best_players_ = -1;
    last_discover_ = {};
  }
}

void NetworkTunnel::Leave() {
  const uint64_t target = target_.load();
  if (joined() && target)
    SendRaw(PackedIp(target), PackedPort(target), Writer(Type::kBye).U32(vip()).U64(token_.load()).bytes());
  joined_ = false;
  std::lock_guard lock(mutex_);
  for (const auto& [peer_vip, peer] : peers_)
    REXSYS_INFO("Lobby: {} left the room", peer.info.gamertag);
  peers_.clear();
}

void NetworkTunnel::ForgetPeers() {
  joined_ = false;
  std::lock_guard lock(mutex_);
  for (const auto& [peer_vip, peer] : peers_) {
    REXSYS_INFO("Lobby: {} left the room", peer.info.gamertag);
    peer_gone_(peer_vip);
  }
  peers_.clear();
}

void NetworkTunnel::SendHello() {
  const uint64_t target = target_.load();
  SendRaw(PackedIp(target), PackedPort(target),
          Writer(Type::kHello).U64(config_.xuid).Text(config_.gamertag, kNameBytes)
              .U32(config_.title_id).Text(config_.room, kRoomBytes)
              .Text(config_.password, kPasswordBytes).U32(nonce_).U32(requested_vip_).bytes());
}

void NetworkTunnel::Discover() {
  discover_nonce_ = nonce_ ^ uint32_t(std::random_device{}());
  const auto bytes = Writer(Type::kDiscover).U32(discover_nonce_).U32(config_.title_id).bytes();
  SendRaw(0xFFFFFFFF, kDefaultPort, bytes);
  for (const uint32_t target : config_.discovery_targets) SendRaw(target, kDefaultPort, bytes);
}

void NetworkTunnel::UpdateMembers(std::vector<lobby::Member> members) {
  std::lock_guard lock(mutex_);
  std::unordered_map<uint32_t, Peer> next;
  for (auto& member : members) {
    if (member.vip == vip()) continue;
    auto& peer = next[member.vip];
    const auto it = peers_.find(member.vip);
    if (it != peers_.end() && it->second.info.token == member.token) {
      peer = it->second;
    } else {
      // A new token on a known address: the old game is gone, this is a new one.
      if (it != peers_.end()) peer_gone_(member.vip);
      REXSYS_INFO("Lobby: {} joined the room", member.gamertag);
    }
    peer.info = std::move(member);
  }
  for (const auto& [vip, peer] : peers_) {
    if (next.contains(vip)) continue;
    REXSYS_INFO("Lobby: {} left the room", peer.info.gamertag);
    peer_gone_(vip);
  }
  peers_ = std::move(next);
}

void NetworkTunnel::Handle(uint32_t ip, uint16_t port, const uint8_t* data, size_t size) {
  Reader reader(data, size);
  const auto type = reader.Header();
  if (!type) return;
  const bool from_server = FromTarget(ip, port);
  if (from_server) last_server_ = Clock::now();
  switch (*type) {
    case Type::kWelcome: {
      uint32_t nonce, vip;
      uint64_t token;
      std::vector<Member> members;
      if (!from_server || !reader.U32(nonce) || nonce != nonce_ || !reader.U32(vip) ||
          !reader.U64(token) || !reader.Members(members) || !IsVirtual(vip))
        return;
      token_ = token;
      if (vip != requested_vip_ && vip_.load() != vip)
        REXSYS_WARN("Lobby: the server gave address {} instead of {}; players who already saw "
                    "the old one may not reach this game until it restarts",
                    Ipv4(vip), Ipv4(requested_vip_));
      vip_ = vip;
      if (!joined_.exchange(true)) {
        static constexpr const char* kSources[] = {"", "the configured server", "a server on the LAN",
                                                   "this game's own server"};
        REXSYS_INFO("Lobby: joined room '{}' on {} ({}:{}) as {} ({} other players)", config_.room,
                    kSources[int(source_.load())], Ipv4(ip), port, Ipv4(vip),
                    members.size() ? members.size() - 1 : 0);
      }
      reject_reason_.clear();
      if (source_.load() == Source::kConfigured) configured_silent_ = false;
      UpdateMembers(std::move(members));
      FlushHeld();
      break;
    }
    case Type::kReject: {
      uint32_t nonce;
      std::string reason;
      if (!from_server || !reader.U32(nonce) || nonce != nonce_ || !reader.Text(reason, kReasonBytes))
        return;
      if (reason != reject_reason_) REXSYS_ERROR("Lobby: the server refused to join: {}", reason);
      reject_reason_ = reason;
      break;
    }
    case Type::kMembers: {
      std::vector<Member> members;
      if (from_server && joined() && reader.Members(members)) UpdateMembers(std::move(members));
      break;
    }
    case Type::kClosing: {
      if (!from_server) return;
      REXSYS_INFO("Lobby: the server closed");
      ForgetPeers();
      // Our own server closing is handled by UseLocalServer(0, 0).
      if (source_.load() != Source::kLocal) SetTarget(0, 0, Source::kNone);
      break;
    }
    case Type::kNotMember: {
      if (!from_server || !joined()) return;
      // The server timed us out (we stalled, or the network did): rejoin now.
      REXSYS_WARN("Lobby: the server dropped this game; joining again");
      ForgetPeers();
      target_since_ = Clock::now();
      last_hello_ = {};
      break;
    }
    case Type::kOffer: {
      uint32_t nonce, players;
      const Source source = source_.load();
      if ((source != Source::kNone && !(source == Source::kConfigured && configured_silent_)) ||
          !reader.U32(nonce) || nonce != discover_nonce_ || !reader.U32(players))
        return;
      if (best_players_ < 0) first_offer_ = Clock::now();
      if (int64_t(players) > best_players_) {
        best_players_ = players;
        best_offer_ = Pack(ip, port);
      }
      break;
    }
    case Type::kData: {
      uint32_t src_vip, dst_vip;
      uint64_t token;
      uint16_t src_port, dst_port;
      if (!reader.U32(src_vip) || !reader.U64(token) || !reader.U32(dst_vip) ||
          !reader.U16(src_port) || !reader.U16(dst_port))
        return;
      const uint32_t own = vip();
      if (!joined() || (dst_vip != own && dst_vip != kBroadcast) || src_vip == own) return;
      if (!from_server) {
        // Direct from a peer: must carry that peer's token.
        std::lock_guard lock(mutex_);
        const auto it = peers_.find(src_vip);
        if (it == peers_.end() || it->second.info.token != token) return;
        it->second.direct = true;
        it->second.last_direct = Clock::now();
      }
      const bool delivered = deliver_(src_vip, src_port, dst_port, dst_vip == kBroadcast, reader.rest(),
                                      reader.remaining());
      REXSYS_DEBUG("Lobby: {} {} bytes {}:{} -> :{}{}", from_server ? "relayed" : "direct",
                    reader.remaining(), Ipv4(src_vip), src_port, dst_port,
                    delivered ? "" : " (no socket on that port)");
      break;
    }
    case Type::kProbe:
    case Type::kProbeAck: {
      uint32_t src_vip, nonce;
      uint64_t token;
      if (from_server || !reader.U32(src_vip) || !reader.U64(token) || !reader.U32(nonce)) return;
      std::lock_guard lock(mutex_);
      const auto it = peers_.find(src_vip);
      if (it == peers_.end() || it->second.info.token != token) return;
      if (*type == Type::kProbe) {
        SendRaw(ip, port, Writer(Type::kProbeAck).U32(vip()).U64(token_.load()).U32(nonce).bytes());
      } else {
        if (!it->second.direct)
          REXSYS_INFO("Lobby: direct connection to {} established", it->second.info.gamertag);
        it->second.direct = true;
        it->second.last_direct = Clock::now();
      }
      break;
    }
    default: break;
  }
}

void NetworkTunnel::Search(Clock::time_point now) {
  if (best_players_ >= 0) {
    if (now - first_offer_ <= kOfferWindow) return;
    const uint64_t offer = best_offer_;
    REXSYS_INFO("Lobby: found a server on the LAN at {}:{} ({} players)", Ipv4(PackedIp(offer)),
                PackedPort(offer), best_players_);
    SetTarget(PackedIp(offer), PackedPort(offer), Source::kDiscovered);
    best_offer_ = 0;
    best_players_ = -1;
  } else if (now - last_discover_ > kDiscoverInterval) {
    last_discover_ = now;
    Discover();
  }
}

void NetworkTunnel::Run() {
  const SocketHandle handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in local{};
  local.sin_family = AF_INET;
  if (bind(handle, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    REXSYS_ERROR("Lobby: cannot open a UDP socket");
    CloseSocket(handle);
    return;
  }
  const int enable = 1;
  setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enable), sizeof(enable));
  socket_ = intptr_t(handle);
  const bool configured = !config_.server.empty();
  if (configured) {
    if (!Resolve()) REXSYS_ERROR("Lobby: cannot resolve server '{}'", config_.server);
    else REXSYS_INFO("Lobby: trying the configured server {}:{}", Ipv4(configured_ip_), configured_port_);
  }
  if (config_.lan)
    REXSYS_INFO("Lobby: address {}; {}", Ipv4(requested_vip_),
                configured ? "without the configured server, a server on the LAN is used"
                           : "a server on the LAN is used when playing with others");
  auto last_keepalive = Clock::time_point{}, last_probe = Clock::time_point{},
       last_resolve = Clock::time_point{};
  std::vector<uint8_t> buffer(2048);
  while (!stop_) {
    fd_set read;
    FD_ZERO(&read);
    FD_SET(handle, &read);
    timeval timeout{0, 100000};
    if (select(int(handle) + 1, &read, nullptr, nullptr, &timeout) > 0) {
      sockaddr_in from{};
      socklen_t length = sizeof(from);
      const int size = recvfrom(handle, reinterpret_cast<char*>(buffer.data()), int(buffer.size()),
                                0, reinterpret_cast<sockaddr*>(&from), &length);
      if (size > 0)
        Handle(ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), buffer.data(), size_t(size));
    }
    const auto now = Clock::now();

    // The game's own server starting or stopping.
    std::optional<uint64_t> request;
    {
      std::lock_guard lock(mutex_);
      request.swap(local_request_);
    }
    if (request) {
      Leave();
      if (*request) {
        REXSYS_INFO("Lobby: hosting; using this game's server");
        SetTarget(PackedIp(*request), PackedPort(*request), Source::kLocal);
      } else {
        SetTarget(0, 0, Source::kNone);
      }
    }

    const Source source = source_.load();
    if (joined()) {
      if (now - last_server_ > kServerTimeout) {
        REXSYS_WARN("Lobby: lost the server");
        ForgetPeers();
        if (source != Source::kLocal) SetTarget(0, 0, Source::kNone);
        continue;
      }
      if (now - last_keepalive > kKeepAliveInterval) {
        last_keepalive = now;
        const uint64_t target = target_.load();
        SendRaw(PackedIp(target), PackedPort(target),
                Writer(Type::kKeepAlive).U32(vip()).U64(token_.load()).bytes());
      }
      if (now - last_probe > kProbeInterval) {
        last_probe = now;
        // Probes open NAT mappings both ways and keep direct paths alive.
        std::lock_guard lock(mutex_);
        for (auto& [peer_vip, peer] : peers_) {
          if (peer.direct && now - peer.last_direct > kDirectTimeout) {
            REXSYS_INFO("Lobby: direct connection to {} lost; using the server", peer.info.gamertag);
            peer.direct = false;
          }
          SendRaw(peer.info.public_ip, peer.info.public_port,
                  Writer(Type::kProbe).U32(vip()).U64(token_.load()).U32(nonce_).bytes());
        }
      }
      continue;
    }

    // Not in a room: pick a server and say hello.
    if (configured && !configured_ip_ && now - last_resolve > std::chrono::seconds(10)) {
      last_resolve = now;
      Resolve();
    }
    switch (source) {
      case Source::kLocal:
        if (now - last_hello_ > std::chrono::seconds(1)) {
          last_hello_ = now;
          SendHello();
        }
        break;
      case Source::kConfigured: {
        // Refused joins retry slowly; the reason was logged.
        const auto interval = reject_reason_.empty() ? kHelloInterval : std::chrono::seconds(10);
        if (config_.lan && now - target_since_ > kConfiguredWait) {
          if (!configured_silent_)
            REXSYS_INFO("Lobby: the configured server does not answer; looking on the LAN");
          configured_silent_ = true;
          SetTarget(0, 0, Source::kNone);
        } else {
          if (now - last_hello_ > interval) {
            last_hello_ = now;
            SendHello();
          }
          // Asking a server that was silent last time again; keep looking too.
          if (config_.lan && configured_silent_ && Wanted(now)) Search(now);
        }
        break;
      }
      case Source::kDiscovered:
        if (now - target_since_ > kJoinWait) {
          SetTarget(0, 0, Source::kNone);
        } else if (now - last_hello_ > std::chrono::seconds(1)) {
          last_hello_ = now;
          SendHello();
        }
        break;
      case Source::kNone:
        if (configured_ip_ && (!config_.lan || last_configured_try_ == Clock::time_point{} ||
                               now - last_configured_try_ > kConfiguredRetry)) {
          last_configured_try_ = now;
          SetTarget(configured_ip_, configured_port_, Source::kConfigured);
          break;
        }
        if (config_.lan && Wanted(now)) Search(now);
        break;
    }
  }
  Leave();
  socket_ = -1;
  CloseSocket(handle);
}

}  // namespace rex::system::xam
