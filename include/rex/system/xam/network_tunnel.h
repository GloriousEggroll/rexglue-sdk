#pragma once
/**
 * Client side of lobby-server System Link: joins a room on a lobby server and
 * carries the title's LAN traffic as DATA packets (see lobby_protocol.h).
 *
 * The title sees an ordinary LAN: its address is a virtual address derived
 * from the profile (the server grants it unless it is taken), broadcasts
 * reach everyone in the room, and packets for it are queued on the socket
 * bound to the destination port. Traffic goes directly to a peer once a probe
 * shows its public endpoint answers, and through the server otherwise.
 *
 * Which server: the configured one if it answers; otherwise (`lan`) one found
 * on the LAN by DISCOVER broadcasts, preferring one with players of this
 * title; or the game's own server while it hosts a party (UseLocalServer).
 * The LAN is only searched while the title is trying to reach someone, so a
 * solo game never joins a room; broadcasts sent meanwhile go out on joining.
 * That must take well under the title's LAN query time (3 s in Gears 3), which
 * is why retrying a silent configured server does not pause the search.
 * The address stays the same across servers, since the title keeps the one
 * it read at startup.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/system/xam/lobby_protocol.h>

namespace rex::system::xam {

class NetworkTunnel {
 public:
  struct Config {
    std::string server;  // host[:port]; may be empty with `lan`
    bool lan = false;    // look for a server on the LAN when `server` does not answer
    std::vector<uint32_t> discovery_targets;  // extra DISCOVER destinations (host order)
    std::string room, password, gamertag;
    uint64_t xuid = 0;
    uint32_t title_id = 0;
  };
  // Receives a packet for local guest port `dst_port`; returns false if no
  // socket is bound there.
  using Deliver = std::function<bool(uint32_t src_vip, uint16_t src_port, uint16_t dst_port,
                                     bool broadcast, const uint8_t* data, size_t size)>;
  // A player at this address is gone: its game closed, or it came back as a
  // new game on the same address.
  using PeerGone = std::function<void(uint32_t vip)>;

  NetworkTunnel(Config config, Deliver deliver, PeerGone peer_gone);
  ~NetworkTunnel();

  // This player's virtual address: requested at once, then whatever the
  // server granted.
  uint32_t vip() const { return vip_.load(std::memory_order_acquire); }
  // In a room on some server.
  bool joined() const { return joined_.load(std::memory_order_acquire); }
  // In a room on the configured server.
  bool joined_configured() const { return joined() && source_.load() == Source::kConfigured; }
  bool Send(uint16_t src_port, uint32_t dst_vip, uint16_t dst_port, const uint8_t* data,
            size_t size);
  std::vector<lobby::Member> members();
  // Join this machine's own server (ip, port in host order), or leave it
  // (zero) and look for a server again.
  void UseLocalServer(uint32_t ip, uint16_t port);
  // Leaves the room and stops the tunnel; Send then fails.
  void Stop();

 private:
  enum class Source : uint8_t { kNone, kConfigured, kDiscovered, kLocal };
  using Clock = std::chrono::steady_clock;

  void Run();
  void Handle(uint32_t ip, uint16_t port, const uint8_t* data, size_t size);
  void SendRaw(uint32_t ip, uint16_t port, const std::vector<uint8_t>& bytes);
  void UpdateMembers(std::vector<lobby::Member> members);
  bool Resolve();
  void SetTarget(uint32_t ip, uint16_t port, Source source);
  void Leave();
  void ForgetPeers();  // The room is gone without us choosing to leave it.
  void Search(Clock::time_point now);
  void SendHello();
  void Discover();
  bool FromTarget(uint32_t ip, uint16_t port) const;

  struct Peer {
    lobby::Member info;
    bool direct = false;
    Clock::time_point last_direct{};
  };

  Config config_;
  Deliver deliver_;
  PeerGone peer_gone_;
  intptr_t socket_ = -1;
  uint32_t configured_ip_ = 0;
  uint16_t configured_port_ = lobby::kDefaultPort;
  // The server in use: ip << 16 | port (host order); zero while looking.
  std::atomic<uint64_t> target_{0};
  std::atomic<Source> source_{Source::kNone};
  uint32_t requested_vip_ = 0;
  std::atomic<uint32_t> vip_{0};
  std::atomic<bool> joined_{false};
  std::atomic<uint64_t> token_{0};
  uint32_t nonce_ = 0;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::unordered_map<uint32_t, Peer> peers_;  // vip -> peer
  std::optional<uint64_t> local_request_;     // UseLocalServer, applied by Run
  Clock::time_point last_server_{}, target_since_{}, last_hello_{}, last_configured_try_{};
  // Discovery: offers to this round's DISCOVER, best first.
  uint32_t discover_nonce_ = 0;
  Clock::time_point last_discover_{}, first_offer_{};
  uint64_t best_offer_ = 0;
  int64_t best_players_ = -1;
  std::string reject_reason_;
  bool configured_silent_ = false;  // Its silence was logged.
  // Sends without a room, and the broadcasts among them to deliver on joining.
  struct Held {
    uint16_t src_port = 0, dst_port = 0;
    std::vector<uint8_t> data;
    Clock::time_point when;
  };
  std::vector<Held> held_;
  std::atomic<int64_t> last_wanted_{0};  // steady_clock ticks; zero: never
  bool Wanted(Clock::time_point now) const;
  void FlushHeld();
  std::thread thread_;
};

}  // namespace rex::system::xam
