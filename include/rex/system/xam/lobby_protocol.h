#pragma once
/**
 * Wire protocol between game instances and the lobby server
 * (squarehole-lobby), over UDP. All integers are little endian.
 *
 * Clients join a room on the server and receive a virtual IPv4 address
 * (10.69.0.0/16) that stands for them on the room's virtual LAN, plus a
 * random token. Game traffic travels as DATA packets: through the server, or
 * directly between players once a PROBE/PROBE_ACK exchange shows their public
 * endpoints reach each other. A direct DATA packet carries the sender's token,
 * which the receiver checks against the server's member list; the server sets
 * the token to zero on packets it relays.
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::lobby {

inline constexpr uint32_t kMagic = 0x36335153;  // "SQ36"
inline constexpr uint8_t kVersion = 1;
inline constexpr uint16_t kDefaultPort = 36360;
inline constexpr uint32_t kVirtualNetwork = 0x0A450000;  // 10.69.0.0/16
inline constexpr uint32_t kVirtualMask = 0xFFFF0000;
inline constexpr uint32_t kBroadcast = 0xFFFFFFFF;
inline constexpr size_t kMaxPayload = 1400;
inline constexpr size_t kMaxMembers = 16;
inline constexpr size_t kNameBytes = 16, kRoomBytes = 32, kPasswordBytes = 32, kReasonBytes = 64;

enum class Type : uint8_t {
  kHello = 1,      // C->S: xuid, gamertag, title, room, password, nonce
  kWelcome = 2,    // S->C: nonce, vip, token, members
  kReject = 3,     // S->C: nonce, reason
  kKeepAlive = 4,  // C->S: vip, token; S->C echo with token zero
  kMembers = 5,    // S->C: members
  kData = 6,       // any: src vip, token, dst vip, src port, dst port, payload
  kProbe = 7,      // C->C: src vip, token, nonce
  kProbeAck = 8,   // C->C: src vip, token, nonce
  kBye = 9,        // C->S: vip, token
};

inline bool IsVirtual(uint32_t ip) { return (ip & kVirtualMask) == kVirtualNetwork; }

struct Member {
  uint32_t vip = 0;
  uint64_t xuid = 0;
  std::string gamertag;
  uint32_t public_ip = 0;  // host order
  uint16_t public_port = 0;
  uint64_t token = 0;
};

// Minimal little-endian writer/reader.
class Writer {
 public:
  explicit Writer(Type type) {
    U32(kMagic);
    U8(kVersion);
    U8(uint8_t(type));
  }
  Writer& U8(uint8_t v) { bytes_.push_back(v); return *this; }
  Writer& U16(uint16_t v) { for (int i = 0; i < 2; ++i) bytes_.push_back(uint8_t(v >> (8 * i))); return *this; }
  Writer& U32(uint32_t v) { for (int i = 0; i < 4; ++i) bytes_.push_back(uint8_t(v >> (8 * i))); return *this; }
  Writer& U64(uint64_t v) { for (int i = 0; i < 8; ++i) bytes_.push_back(uint8_t(v >> (8 * i))); return *this; }
  Writer& Text(std::string_view text, size_t size) {
    for (size_t i = 0; i < size; ++i) bytes_.push_back(i < text.size() ? uint8_t(text[i]) : 0);
    return *this;
  }
  Writer& Bytes(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    bytes_.insert(bytes_.end(), p, p + size);
    return *this;
  }
  Writer& Members(const std::vector<Member>& members) {
    U32(uint32_t(members.size()));
    for (const auto& m : members)
      U32(m.vip).U64(m.xuid).Text(m.gamertag, kNameBytes).U32(m.public_ip).U16(m.public_port).U64(m.token);
    return *this;
  }
  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
  // Validates the header; returns the type.
  std::optional<Type> Header() {
    uint32_t magic;
    uint8_t version, type;
    if (!U32(magic) || magic != kMagic || !U8(version) || version != kVersion || !U8(type))
      return std::nullopt;
    return Type(type);
  }
  bool U8(uint8_t& v) { if (end_ - p_ < 1) return false; v = *p_++; return true; }
  bool U16(uint16_t& v) { return Little(v); }
  bool U32(uint32_t& v) { return Little(v); }
  bool U64(uint64_t& v) { return Little(v); }
  bool Text(std::string& text, size_t size) {
    if (size_t(end_ - p_) < size) return false;
    const auto* nul = static_cast<const uint8_t*>(std::memchr(p_, 0, size));
    text.assign(reinterpret_cast<const char*>(p_), nul ? size_t(nul - p_) : size);
    p_ += size;
    return true;
  }
  bool Members(std::vector<Member>& members) {
    uint32_t count;
    if (!U32(count) || count > kMaxMembers) return false;
    members.resize(count);
    for (auto& m : members)
      if (!U32(m.vip) || !U64(m.xuid) || !Text(m.gamertag, kNameBytes) || !U32(m.public_ip) ||
          !U16(m.public_port) || !U64(m.token))
        return false;
    return true;
  }
  const uint8_t* rest() const { return p_; }
  size_t remaining() const { return size_t(end_ - p_); }

 private:
  template <typename T>
  bool Little(T& v) {
    if (size_t(end_ - p_) < sizeof(T)) return false;
    v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) v |= T(p_[i]) << (8 * i);
    p_ += sizeof(T);
    return true;
  }
  const uint8_t* p_;
  const uint8_t* end_;
};

}  // namespace rex::lobby
