#include <catch2/catch_test_macros.hpp>
#include <rex/system/xsocket.h>

#if REX_PLATFORM_LINUX
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>

using rex::X_STATUS;

TEST_CASE("Xbox socket controls configure Linux sockets", "[socket_translation]") {
  rex::system::XSocket socket(nullptr);
  REQUIRE(socket.Initialize(static_cast<rex::system::XSocket::AddressFamily>(2),
                            static_cast<rex::system::XSocket::Type>(2),
                            static_cast<rex::system::XSocket::Protocol>(17)) == X_STATUS_SUCCESS);
  const int fd = static_cast<int>(socket.native_handle());
  SECTION("broadcast uses the host option and guest byte order") {
    rex::be<uint32_t> enabled = 1;
    REQUIRE(socket.SetOption(0xffff, 0x20, &enabled, sizeof(enabled)) == X_STATUS_SUCCESS);
    int host_value = 0;
    socklen_t size = sizeof(host_value);
    REQUIRE(getsockopt(fd, SOL_SOCKET, SO_BROADCAST, &host_value, &size) == 0);
    CHECK(host_value == 1);
  }
  SECTION("receive buffer preserves its requested size") {
    rex::be<uint32_t> requested = 65536;
    REQUIRE(socket.SetOption(0xffff, 0x1002, &requested, sizeof(requested)) == X_STATUS_SUCCESS);
    int host_value = 0;
    socklen_t size = sizeof(host_value);
    REQUIRE(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &host_value, &size) == 0);
    CHECK(host_value >= 65536);
    rex::be<uint32_t> guest_value = 0;
    uint32_t guest_size = sizeof(guest_value);
    REQUIRE(socket.GetOption(0xffff, 0x1002, &guest_value, &guest_size) == X_STATUS_SUCCESS);
    CHECK(uint32_t(guest_value) == uint32_t(host_value));
    CHECK(guest_size == 4);
  }
  SECTION("nonblocking can be enabled and disabled") {
    rex::be<uint32_t> enabled = 1;
    REQUIRE(socket.IOControl(0x8004667e, reinterpret_cast<uint8_t*>(&enabled)) == X_STATUS_SUCCESS);
    CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
    enabled = 0;
    REQUIRE(socket.IOControl(0x8004667e, reinterpret_cast<uint8_t*>(&enabled)) == X_STATUS_SUCCESS);
    CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) == 0);
  }
  SECTION("pending datagram size is returned in guest byte order") {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    REQUIRE(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    REQUIRE(sendto(fd, "hello", 5, 0, reinterpret_cast<sockaddr*>(&address), size) == 5);
    rex::be<uint32_t> available = 0;
    REQUIRE(socket.IOControl(0x4004667f, reinterpret_cast<uint8_t*>(&available)) == X_STATUS_SUCCESS);
    CHECK(uint32_t(available) == 5);
  }
  SECTION("short option buffers are rejected without overwriting memory") {
    rex::be<uint32_t> value = 0x12345678;
    uint32_t size = 1;
    CHECK(socket.GetOption(0xffff, 0x1002, &value, &size) == X_STATUS_INVALID_PARAMETER);
    CHECK(uint32_t(value) == 0x12345678);
    CHECK(size == 1);
  }
  SECTION("low guest ports bind without elevated privileges") {
    rex::system::N_XSOCKADDR_IN address;
    address.sin_family = 2;
    address.sin_port = 1000;
    address.sin_addr = 0x7f000001;
    std::memset(address.x_sin_zero, 0, sizeof(address.x_sin_zero));
    REQUIRE(socket.Bind(&address, sizeof(address)) == X_STATUS_SUCCESS);
    sockaddr_in host{};
    socklen_t size = sizeof(host);
    REQUIRE(getsockname(fd, reinterpret_cast<sockaddr*>(&host), &size) == 0);
    CHECK(ntohs(host.sin_port) == 11000);
    CHECK(uint16_t(address.sin_port) == 1000);
    CHECK(socket.bound_port() == 1000);
  }
}
#endif
