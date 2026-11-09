#pragma once
/**
 * The lobby server (see lobby_protocol.h): rooms, virtual addresses, member
 * lists, broadcast fan-out and relay. Header-only, so the standalone
 * gears3-lobby and the game's built-in LAN server share it.
 *
 * Usage: open and bind a UDP socket, construct a Server on it and call Run()
 * on a thread until its `stop` returns true; Close() then tells every member the server
 * is going away so they look for another at once.
 */

#include <rex/system/xam/lobby_protocol.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

namespace rex::lobby {

#ifdef _WIN32
using ServerSocket = SOCKET;
#else
using ServerSocket = int;
#endif

class Server {
 public:
  using Log = std::function<void(const std::string&)>;
  Server(ServerSocket socket, size_t max_rooms, Log log = {})
      : socket_(socket), max_rooms_(max_rooms), log_(std::move(log)) {}

  // Serves until `stop` (checked every 200 ms) returns true; forever without.
  void Run(const std::function<bool()>& stop = {}) {
    std::vector<uint8_t> buffer(2048);
    auto last_sweep = Clock::now();
    while (!stop || !stop()) {
      fd_set read;
      FD_ZERO(&read);
      FD_SET(socket_, &read);
      timeval timeout{0, 200000};
      if (select(int(socket_) + 1, &read, nullptr, nullptr, &timeout) > 0) {
        sockaddr_in from{};
        socklen_t length = sizeof(from);
        const int size = recvfrom(socket_, reinterpret_cast<char*>(buffer.data()), int(buffer.size()),
                                  0, reinterpret_cast<sockaddr*>(&from), &length);
        if (size > 0)
          Handle({ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)}, buffer.data(), size_t(size));
      }
      if (Clock::now() - last_sweep > std::chrono::milliseconds(250)) {
        Sweep();
        last_sweep = Clock::now();
      }
    }
  }

  // Tells every member the server is closing (after Run has returned).
  void Close() {
    const auto bytes = Writer(Type::kClosing).bytes();
    for (const auto& [vip, member] : members_) Send(member.endpoint, bytes);
    members_.clear();
    rooms_.clear();
  }

 private:
  using Clock = std::chrono::steady_clock;
  // Keepalives come every second, so this is a game that is gone: its players
  // are told at once and their games drop it.
  static constexpr auto kMemberTimeout = std::chrono::seconds(4);

  struct Endpoint {
    uint32_t ip = 0;  // host order
    uint16_t port = 0;
    bool operator==(const Endpoint&) const = default;
  };
  struct ServerMember {
    Member info;
    Endpoint endpoint;
    std::string room;
    uint32_t title = 0;
    Clock::time_point last_seen;
  };
  struct Room {
    std::string password;
    std::vector<uint32_t> members;  // vips
  };

  static std::string Text(const Endpoint& e) {
    return std::to_string(e.ip >> 24) + "." + std::to_string((e.ip >> 16) & 255) + "." +
           std::to_string((e.ip >> 8) & 255) + "." + std::to_string(e.ip & 255) + ":" +
           std::to_string(e.port);
  }
  void Print(const std::string& line) {
    if (log_) log_(line);
  }

  void Send(const Endpoint& to, const std::vector<uint8_t>& bytes) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(to.ip);
    address.sin_port = htons(to.port);
    sendto(socket_, reinterpret_cast<const char*>(bytes.data()), int(bytes.size()), 0,
           reinterpret_cast<sockaddr*>(&address), sizeof(address));
  }

  std::vector<Member> RoomMembers(const std::string& room) {
    std::vector<Member> list;
    for (const uint32_t vip : rooms_[room].members) list.push_back(members_[vip].info);
    return list;
  }

  void BroadcastMembers(const std::string& room) {
    const auto list = RoomMembers(room);
    const auto bytes = Writer(Type::kMembers).Members(list).bytes();
    for (const uint32_t vip : rooms_[room].members) Send(members_[vip].endpoint, bytes);
  }

  // The authenticated member a packet comes from, if any.
  ServerMember* Sender(const Endpoint& from, uint32_t vip, uint64_t token) {
    const auto it = members_.find(vip);
    if (it == members_.end() || it->second.info.token != token || !(it->second.endpoint == from))
      return nullptr;
    it->second.last_seen = Clock::now();
    return &it->second;
  }

  void Handle(const Endpoint& from, const uint8_t* data, size_t size) {
    Reader reader(data, size);
    const auto type = reader.Header();
    if (!type) return;
    switch (*type) {
      case Type::kHello: Hello(from, reader); break;
      case Type::kDiscover: {
        // A game on the LAN looking for a server: say how many players of
        // its title are here, so it can prefer one with a party.
        uint32_t nonce, title;
        if (!reader.U32(nonce) || !reader.U32(title)) return;
        uint32_t players = 0;
        for (const auto& [vip, member] : members_) players += member.title == title;
        Send(from, Writer(Type::kOffer).U32(nonce).U32(players).bytes());
        break;
      }
      case Type::kKeepAlive: {
        uint32_t vip;
        uint64_t token;
        // Echoed so clients know the server is still there.
        if (!reader.U32(vip) || !reader.U64(token)) break;
        if (Sender(from, vip, token)) Send(from, Writer(Type::kKeepAlive).U32(vip).U64(0).bytes());
        else Send(from, Writer(Type::kNotMember).bytes());  // Timed out: it says hello again.
        break;
      }
      case Type::kData: {
        uint32_t src_vip, dst_vip;
        uint64_t token;
        uint16_t src_port, dst_port;
        if (!reader.U32(src_vip) || !reader.U64(token) || !reader.U32(dst_vip) ||
            !reader.U16(src_port) || !reader.U16(dst_port) || reader.remaining() > kMaxPayload)
          return;
        auto* sender = Sender(from, src_vip, token);
        if (!sender) return;
        // Relayed: token zero marks it as vouched for by the server.
        const auto bytes = Writer(Type::kData)
                               .U32(src_vip).U64(0).U32(dst_vip).U16(src_port).U16(dst_port)
                               .Bytes(reader.rest(), reader.remaining()).bytes();
        for (const uint32_t vip : rooms_[sender->room].members) {
          if (vip == src_vip || (dst_vip != kBroadcast && vip != dst_vip)) continue;
          Send(members_[vip].endpoint, bytes);
        }
        break;
      }
      case Type::kBye: {
        uint32_t vip;
        uint64_t token;
        if (reader.U32(vip) && reader.U64(token) && Sender(from, vip, token)) Remove(vip, "left");
        break;
      }
      default: break;
    }
  }

  void Hello(const Endpoint& from, Reader& reader) {
    uint64_t xuid;
    uint32_t title, nonce;
    std::string gamertag, room, password;
    if (!reader.U64(xuid) || !reader.Text(gamertag, kNameBytes) || !reader.U32(title) ||
        !reader.Text(room, kRoomBytes) || !reader.Text(password, kPasswordBytes) || !reader.U32(nonce))
      return;
    // Newer clients ask for the address they already gave the game.
    uint32_t requested = 0;
    if (!reader.U32(requested)) requested = 0;
    auto reject = [&](const char* reason) {
      Send(from, Writer(Type::kReject).U32(nonce).Text(reason, kReasonBytes).bytes());
    };
    if (room.empty()) room = "public";
    room += "#" + std::to_string(title);  // Rooms never mix games.
    // A repeated HELLO (lost WELCOME) from the same endpoint gets the same seat.
    for (auto& [vip, member] : members_) {
      if (member.endpoint == from && member.info.xuid == xuid && member.room == room) {
        member.last_seen = Clock::now();
        Welcome(from, nonce, member);
        return;
      }
    }
    if (!rooms_.contains(room) && rooms_.size() >= max_rooms_) return reject("The server is full.");
    auto& target = rooms_[room];
    if (target.members.empty()) target.password = password;
    else if (target.password != password) return reject("Wrong room password.");
    if (target.members.size() >= kMaxMembers) return reject("The room is full.");
    // The same profile again (a restarted game whose old seat has not timed
    // out yet) takes over that seat.
    for (const uint32_t vip : std::vector<uint32_t>(target.members))
      if (members_[vip].info.xuid == xuid) Remove(vip, "rejoined");
    const uint32_t vip = Available(requested) ? requested : AllocateVip();
    if (!vip) return reject("The server is full.");
    auto& seat = rooms_[room];
    ServerMember member;
    member.info.vip = vip;
    member.info.xuid = xuid;
    member.info.gamertag = gamertag.substr(0, kNameBytes - 1);
    member.info.public_ip = from.ip;
    member.info.public_port = from.port;
    member.info.token = random_();
    member.endpoint = from;
    member.room = room;
    member.title = title;
    member.last_seen = Clock::now();
    members_[vip] = member;
    seat.password = seat.members.empty() ? password : seat.password;
    seat.members.push_back(vip);
    Print("join  " + member.info.gamertag + " " + Text(from) + " room " + room + " as 10.69." +
          std::to_string((vip >> 8) & 255) + "." + std::to_string(vip & 255));
    Welcome(from, nonce, members_[vip]);
    BroadcastMembers(room);
  }

  void Welcome(const Endpoint& to, uint32_t nonce, const ServerMember& member) {
    Send(to, Writer(Type::kWelcome).U32(nonce).U32(member.info.vip).U64(member.info.token)
                 .Members(RoomMembers(member.room)).bytes());
  }

  bool Available(uint32_t vip) const {
    return IsVirtual(vip) && (vip & 0xFF) != 0 && (vip & 0xFF) != 0xFF && !members_.contains(vip);
  }

  uint32_t AllocateVip() {
    for (uint32_t i = 0; i < 0xFFFE; ++i) {
      next_host_ = next_host_ % 0xFFFE + 1;  // 10.69.0.1 .. 10.69.255.254
      if (const uint32_t vip = kVirtualNetwork | next_host_; Available(vip)) return vip;
    }
    return 0;
  }

  void Remove(uint32_t vip, const char* why) {
    const auto it = members_.find(vip);
    if (it == members_.end()) return;
    const std::string room = it->second.room;
    Print("leave " + it->second.info.gamertag + " " + Text(it->second.endpoint) + " (" + why + ")");
    members_.erase(it);
    auto& list = rooms_[room].members;
    list.erase(std::remove(list.begin(), list.end(), vip), list.end());
    if (list.empty()) rooms_.erase(room);
    else BroadcastMembers(room);
  }

  void Sweep() {
    std::vector<uint32_t> expired;
    for (const auto& [vip, member] : members_)
      if (Clock::now() - member.last_seen > kMemberTimeout) expired.push_back(vip);
    for (const uint32_t vip : expired) Remove(vip, "timed out");
  }

  ServerSocket socket_;
  size_t max_rooms_;
  Log log_;
  std::unordered_map<uint32_t, ServerMember> members_;
  std::map<std::string, Room> rooms_;
  uint32_t next_host_ = 0;
  std::mt19937_64 random_{std::random_device{}()};
};

}  // namespace rex::lobby
