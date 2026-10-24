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
using Clock = std::chrono::steady_clock;
using namespace rex::lobby;
constexpr auto kHelloInterval = std::chrono::seconds(2);
constexpr auto kKeepAliveInterval = std::chrono::seconds(2);
constexpr auto kProbeInterval = std::chrono::seconds(2);
constexpr auto kDirectTimeout = std::chrono::seconds(10);
constexpr auto kServerTimeout = std::chrono::seconds(15);

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
}  // namespace

NetworkTunnel::NetworkTunnel(Config config, Deliver deliver)
    : config_(std::move(config)), deliver_(std::move(deliver)) {
  nonce_ = uint32_t(std::random_device{}());
  thread_ = std::thread([this] { Run(); });
}

NetworkTunnel::~NetworkTunnel() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

bool NetworkTunnel::Resolve() {
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
  server_ip_ = ntohl(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr);
  server_port_ = port ? port : kDefaultPort;
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
  const uint32_t own = vip();
  if (!own || size > kMaxPayload || socket_ < 0) return false;
  const auto bytes = Writer(Type::kData).U32(own).U64(token_.load()).U32(dst_vip).U16(src_port)
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
  SendRaw(server_ip_, server_port_, bytes);  // The server fans broadcasts out.
  return true;
}

std::vector<lobby::Member> NetworkTunnel::members() {
  std::lock_guard lock(mutex_);
  std::vector<lobby::Member> list;
  for (const auto& [vip, peer] : peers_) list.push_back(peer.info);
  return list;
}

void NetworkTunnel::UpdateMembers(std::vector<lobby::Member> members) {
  std::lock_guard lock(mutex_);
  std::unordered_map<uint32_t, Peer> next;
  for (auto& member : members) {
    if (member.vip == vip()) continue;
    auto& peer = next[member.vip];
    if (const auto it = peers_.find(member.vip); it != peers_.end() &&
        it->second.info.token == member.token)
      peer = it->second;
    else
      REXSYS_INFO("Lobby: {} joined the room", member.gamertag);
    peer.info = std::move(member);
  }
  for (const auto& [vip, peer] : peers_)
    if (!next.contains(vip)) REXSYS_INFO("Lobby: {} left the room", peer.info.gamertag);
  peers_ = std::move(next);
}

void NetworkTunnel::Handle(uint32_t ip, uint16_t port, const uint8_t* data, size_t size) {
  Reader reader(data, size);
  const auto type = reader.Header();
  if (!type) return;
  const bool from_server = ip == server_ip_ && port == server_port_;
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
      if (vip_.exchange(vip) != vip)
        REXSYS_INFO("Lobby: joined room '{}' as {} ({} other players)", config_.room, Ipv4(vip),
                    members.size() ? members.size() - 1 : 0);
      reject_reason_.clear();
      UpdateMembers(std::move(members));
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
      if (from_server && reader.Members(members)) UpdateMembers(std::move(members));
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
      if (!own || (dst_vip != own && dst_vip != kBroadcast) || src_vip == own) return;
      if (!from_server) {
        // Direct from a peer: must carry that peer's token.
        std::lock_guard lock(mutex_);
        const auto it = peers_.find(src_vip);
        if (it == peers_.end() || it->second.info.token != token) return;
        it->second.direct = true;
        it->second.last_direct = Clock::now();
      }
      const bool delivered = deliver_(src_vip, src_port, dst_port, reader.rest(), reader.remaining());
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

void NetworkTunnel::Run() {
  const SocketHandle handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in local{};
  local.sin_family = AF_INET;
  if (bind(handle, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    REXSYS_ERROR("Lobby: cannot open a UDP socket");
    CloseSocket(handle);
    return;
  }
  socket_ = intptr_t(handle);
  if (!Resolve()) REXSYS_ERROR("Lobby: cannot resolve server '{}'", config_.server);
  else REXSYS_INFO("Lobby: connecting to {}:{}", Ipv4(server_ip_), server_port_);
  auto last_hello = Clock::time_point{}, last_keepalive = Clock::time_point{},
       last_probe = Clock::time_point{};
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
    if (!server_ip_) {
      if (now - last_hello > std::chrono::seconds(10)) {
        last_hello = now;
        Resolve();
      }
      continue;
    }
    if (vip() && now - last_server_ > kServerTimeout) {
      REXSYS_WARN("Lobby: lost the server; reconnecting");
      vip_ = 0;
    }
    if (!vip()) {
      // Refused joins retry slowly; the reason was logged.
      const auto interval = reject_reason_.empty() ? kHelloInterval : std::chrono::seconds(10);
      if (now - last_hello > interval) {
        last_hello = now;
        SendRaw(server_ip_, server_port_,
                Writer(Type::kHello).U64(config_.xuid).Text(config_.gamertag, kNameBytes)
                    .U32(config_.title_id).Text(config_.room, kRoomBytes)
                    .Text(config_.password, kPasswordBytes).U32(nonce_).bytes());
      }
      continue;
    }
    if (now - last_keepalive > kKeepAliveInterval) {
      last_keepalive = now;
      SendRaw(server_ip_, server_port_, Writer(Type::kKeepAlive).U32(vip()).U64(token_.load()).bytes());
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
  }
  if (vip()) SendRaw(server_ip_, server_port_, Writer(Type::kBye).U32(vip()).U64(token_.load()).bytes());
  socket_ = -1;
  CloseSocket(handle);
}

}  // namespace rex::system::xam
