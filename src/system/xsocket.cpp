/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <cerrno>

#include <rex/kernel/xam/module.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
#include <rex/system/xam/network.h>

#include <rex/net/socket.h>
#include <rex/memory/utils.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <winsock2.h>

#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif

namespace rex::system {

namespace {
// Xbox uses Winsock option numbers and big-endian scalar values.
bool TranslateScalarOption(uint32_t level, uint32_t option, int& host_level,
                           int& host_option) {
  if (level == 0xffff) {
    host_level = SOL_SOCKET;
    switch (option) {
      case 0x0004: host_option = SO_REUSEADDR; return true;
      case 0x0008: host_option = SO_KEEPALIVE; return true;
      case 0x0010: host_option = SO_DONTROUTE; return true;
      case 0x0020: host_option = SO_BROADCAST; return true;
      case 0x1001: host_option = SO_SNDBUF; return true;
      case 0x1002: host_option = SO_RCVBUF; return true;
      case 0x1008: host_option = SO_TYPE; return true;
      default: return false;
    }
  }
  if (level == 6 && option == 1) {
    host_level = IPPROTO_TCP;
    host_option = TCP_NODELAY;
    return true;
  }
  // Structured options (linger/timeouts) need their own layout conversion.
  return false;
}
}  // namespace

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  if (bound_) xam::Network::Get().UnregisterSocket(bound_port_, handle());
  int ret = rex::net::socket_close(native_handle_);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  int host_level, host_option;
  if (!TranslateScalarOption(level, optname, host_level, host_option)) {
    return X_STATUS_NOT_SUPPORTED;
  }
  if (!optval_ptr || optlen != sizeof(uint32_t)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  int value = memory::load_and_swap<int32_t>(optval_ptr);
  int ret = setsockopt(native_handle_, host_level, host_option,
                      reinterpret_cast<const char*>(&value), sizeof(value));
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = value != 0;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::GetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                           uint32_t* optlen) {
  int host_level, host_option;
  if (!TranslateScalarOption(level, optname, host_level, host_option)) {
    return X_STATUS_NOT_SUPPORTED;
  }
  if (!optval_ptr || !optlen || *optlen < sizeof(uint32_t)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  int value = 0;
#if REX_PLATFORM_WIN32
  int size = sizeof(value);
#else
  socklen_t size = sizeof(value);
#endif
  if (getsockopt(native_handle_, host_level, host_option,
                 reinterpret_cast<char*>(&value), &size) < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
  memory::store_and_swap<int32_t>(optval_ptr, value);
  *optlen = sizeof(uint32_t);
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  if (!arg_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  uint32_t native_cmd;
  switch (cmd) {
    case 0x8004667e: native_cmd = FIONBIO; break;
    case 0x4004667f: native_cmd = FIONREAD; break;
    default: return X_STATUS_NOT_SUPPORTED;
  }
  uint32_t value = memory::load_and_swap<uint32_t>(arg_ptr);
  if (cmd == 0x8004667e) nonblocking_ = value != 0;
  int ret = rex::net::socket_ioctl(native_handle_, native_cmd,
                                 reinterpret_cast<uint8_t*>(&value));
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }
  // Queued tunnel packets are readable too; report the next one's size.
  if (cmd == 0x4004667f && HasQueuedPackets()) value = QueuedBytes();

  memory::store_and_swap<uint32_t>(arg_ptr, value);
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  sockaddr_in host_name{};
  if (name && name->address_family == AF_INET && name_len >= int(sizeof(sockaddr_in))) {
    // sa_data: port (network order), then the IPv4 address (network order).
    uint16_t port;
    std::memcpy(&port, name->sa_data, 2);
    host_name.sin_family = AF_INET;
    host_name.sin_port = htons(xam::Network::HostPort(ntohs(port)));
    std::memcpy(&host_name.sin_addr, name->sa_data + 2, 4);
    name = reinterpret_cast<N_XSOCKADDR*>(&host_name);
    name_len = sizeof(host_name);
  }
  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  if (!name || name_len != sizeof(N_XSOCKADDR_IN)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const uint16_t guest_port = name->sin_port;
  // Low guest ports map to 10000 + port on every OS (see xam/network.h); zero
  // still lets the OS choose.
  sockaddr_in host_name{};
  host_name.sin_family = AF_INET;
  host_name.sin_port = htons(xam::Network::HostPort(guest_port));
  host_name.sin_addr.s_addr = htonl(uint32_t(name->sin_addr));
  auto& network = xam::Network::Get();
  if (network.tunnel_enabled()) {
    // Game traffic arrives through the tunnel by guest port; the host socket
    // only needs to receive wake datagrams, so several instances can share a
    // machine.
    host_name.sin_port = 0;
    host_name.sin_addr.s_addr = htonl(0x7F000001);
  } else if (!host_name.sin_addr.s_addr && network.bind_local_address()) {
    host_name.sin_addr.s_addr = htonl(network.local_address());
  }
  int ret = bind(native_handle_, reinterpret_cast<sockaddr*>(&host_name), sizeof(host_name));
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = guest_port;
  if (!bound_port_ && network.tunnel_enabled()) bound_port_ = network.AllocateTunnelPort();
  if (!bound_port_) {
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    if (getsockname(native_handle_, reinterpret_cast<sockaddr*>(&local), &length) == 0)
      bound_port_ = xam::Network::GuestPort(ntohs(local.sin_port));
  }
  network.RegisterSocket(bound_port_, handle());

  return X_STATUS_SUCCESS;
}

bool XSocket::EnsureBound() {
  if (bound_) return true;
  N_XSOCKADDR_IN any{};
  any.sin_family = AF_INET;
  any.sin_port = 0;
  any.sin_addr = 0;
  return XSUCCEEDED(Bind(&any, sizeof(any)));
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == -1) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  auto byte_writer=memory()->BeginExternalHostWrite(buf,buf_len);
  if(!byte_writer){
#if REX_PLATFORM_WIN32
    WSASetLastError(WSAEFAULT);
#else
    errno=EFAULT;
#endif
    return -1;
  }
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::PopPacket(uint8_t* buf, uint32_t buf_len, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  if (incoming_packets_.empty()) return -1;
  packet* pkt = reinterpret_cast<packet*>(incoming_packets_.front());
  const int data_len = pkt->data_len;
  if (auto writer = memory()->BeginExternalHostWrite(buf, buf_len))
    std::memcpy(buf, pkt->data, std::min(uint32_t(data_len), buf_len));
  if (from) {
    from->sin_family = AF_INET;
    from->sin_addr = uint32_t(pkt->src_ip);
    from->sin_port = uint16_t(pkt->src_port);
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }
  if (from_len) *from_len = sizeof(N_XSOCKADDR_IN);
  incoming_packets_.pop();
  delete[] reinterpret_cast<uint8_t*>(pkt);
  return data_len;
}

bool XSocket::HasQueuedPackets() {
  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  return !incoming_packets_.empty();
}

uint32_t XSocket::QueuedBytes() {
  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  return incoming_packets_.empty() ? 0
                                   : reinterpret_cast<packet*>(incoming_packets_.front())->data_len;
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  // Lobby tunnel packets first; each is announced by a loopback wake datagram,
  // which is discarded here.
  for (;;) {
    if (const int queued = PopPacket(buf, buf_len, from, from_len); queued >= 0) return queued;
    sockaddr_in wake_from{};
    socklen_t wake_length = sizeof(wake_from);
    uint8_t probe;
    const int peeked = recvfrom(native_handle_, reinterpret_cast<char*>(&probe), 1, MSG_PEEK,
                                reinterpret_cast<sockaddr*>(&wake_from), &wake_length);
    if (peeked < 0 && !nonblocking_) {
      // Blocking socket with nothing to peek: fall through to the real read.
      break;
    }
    if (peeked >= 0 &&
        xam::Network::Get().IsWake(ntohl(wake_from.sin_addr.s_addr), ntohs(wake_from.sin_port))) {
      recvfrom(native_handle_, reinterpret_cast<char*>(&probe), 1, 0, nullptr, nullptr);
      if (const int queued = PopPacket(buf, buf_len, from, from_len); queued >= 0) return queued;
      if (nonblocking_) {
#if REX_PLATFORM_WIN32
        WSASetLastError(WSAEWOULDBLOCK);
#else
        errno = EWOULDBLOCK;
#endif
        return -1;
      }
      continue;
    }
    break;
  }

  auto byte_writer=memory()->BeginExternalHostWrite(buf,buf_len);
  if(!byte_writer){
#if REX_PLATFORM_WIN32
    WSASetLastError(WSAEFAULT);
#else
    errno=EFAULT;
#endif
    return -1;
  }
  sockaddr_in nfrom;
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nfrom, &nfromlen);
  if (ret > 0) xam::Network::Get().NoteTraffic(ntohl(nfrom.sin_addr.s_addr));
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);  // BE <- BE
    from->sin_port = xam::Network::GuestPort(ntohs(nfrom.sin_port));
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  // Virtual addresses and broadcasts go through the lobby tunnel.
  auto& network = xam::Network::Get();
  if (to && network.TunnelDestination(uint32_t(to->sin_addr))) {
    if (!EnsureBound()) return -1;
    network.TunnelSend(bound_port_, uint32_t(to->sin_addr), to->sin_port, buf, buf_len);
    return int(buf_len);
  }

  sockaddr_in nto{};
  if (to) {
    nto.sin_addr.s_addr = htonl(uint32_t(to->sin_addr));
    nto.sin_family = to->sin_family;
    nto.sin_port = htons(xam::Network::HostPort(to->sin_port));
    to_len = sizeof(nto);
  }
  // Broadcasts go to the configured peers instead when there are any.
  if (to && (uint32_t(to->sin_addr) == 0xFFFFFFFFu || broadcast_socket_)) {
    const auto peers = xam::Network::Get().broadcast_peers();
    if (!peers.empty() && uint32_t(to->sin_addr) == 0xFFFFFFFFu) {
      int ret = -1;
      for (const uint32_t peer : peers) {
        nto.sin_addr.s_addr = htonl(peer);
        ret = sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nto, to_len);
      }
      return ret < 0 ? ret : int(buf_len);
    }
  }

  return sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? (sockaddr*)&nto : nullptr, to_len);
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
