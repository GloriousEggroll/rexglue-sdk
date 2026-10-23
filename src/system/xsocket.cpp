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
// #include <rex/system/xnet.h>

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
  int ret = rex::net::socket_ioctl(native_handle_, native_cmd,
                                 reinterpret_cast<uint8_t*>(&value));
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }

  memory::store_and_swap<uint32_t>(arg_ptr, value);
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
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
  auto host_name = *name;
  const uint16_t guest_port = name->sin_port;
#if !REX_PLATFORM_WIN32
  // Match the local Xenia build's low-port mapping. Single-player titles can
  // create local sessions on ports such as 1000, which require privileges on
  // Linux. Preserve zero so the OS can still choose an ephemeral port.
  if (guest_port != 0 && guest_port < 1024) {
    host_name.sin_port = guest_port + 10000;
  }
#endif
  int ret = bind(native_handle_, reinterpret_cast<sockaddr*>(&host_name), name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = guest_port;

  return X_STATUS_SUCCESS;
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

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

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
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);  // BE <- BE
    from->sin_port = nfrom.sin_port;
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

  sockaddr_in nto;
  if (to) {
    nto.sin_addr.s_addr = to->sin_addr;
    nto.sin_family = to->sin_family;
    nto.sin_port = to->sin_port;
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
