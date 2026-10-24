#pragma once
/**
 * Client side of internet System Link: joins a room on a lobby server and
 * carries the title's LAN traffic as DATA packets (see lobby_protocol.h).
 *
 * The title sees an ordinary LAN: its address is the room's virtual address,
 * broadcasts reach everyone in the room, and packets for it are queued on the
 * socket bound to the destination port. Traffic goes directly to a peer once
 * a probe shows its public endpoint answers, and through the server otherwise.
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
    std::string server;  // host[:port]
    std::string room, password, gamertag;
    uint64_t xuid = 0;
    uint32_t title_id = 0;
  };
  // Receives a packet for local guest port `dst_port`; returns false if no
  // socket is bound there.
  using Deliver = std::function<bool(uint32_t src_vip, uint16_t src_port, uint16_t dst_port,
                                     const uint8_t* data, size_t size)>;

  NetworkTunnel(Config config, Deliver deliver);
  ~NetworkTunnel();

  // Virtual address once the server has admitted us; zero before.
  uint32_t vip() const { return vip_.load(std::memory_order_acquire); }
  bool Send(uint16_t src_port, uint32_t dst_vip, uint16_t dst_port, const uint8_t* data,
            size_t size);
  std::vector<lobby::Member> members();

 private:
  void Run();
  void Handle(uint32_t ip, uint16_t port, const uint8_t* data, size_t size);
  void SendRaw(uint32_t ip, uint16_t port, const std::vector<uint8_t>& bytes);
  void UpdateMembers(std::vector<lobby::Member> members);
  bool Resolve();

  struct Peer {
    lobby::Member info;
    bool direct = false;
    std::chrono::steady_clock::time_point last_direct{};
  };

  Config config_;
  Deliver deliver_;
  intptr_t socket_ = -1;
  uint32_t server_ip_ = 0;
  uint16_t server_port_ = lobby::kDefaultPort;
  std::atomic<uint32_t> vip_{0};
  std::atomic<uint64_t> token_{0};
  uint32_t nonce_ = 0;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::unordered_map<uint32_t, Peer> peers_;  // vip -> peer
  std::chrono::steady_clock::time_point last_server_{};
  std::string reject_reason_;
  std::thread thread_;
};

}  // namespace rex::system::xam
