#pragma once

// Accept a TCP connection through esphome::socket, on RTEMS.
//
// esphome::socket is what api, web_server, mqtt and the rest reach the network
// with, so this is the layer that decides whether any of them can work here.
// tests/zynq-lwip/ proves the stack underneath; this proves the seam.

#include "esphome/components/socket/socket.h"
#include "esphome/core/log.h"

#include <netstart.h>
#include <lwip/netif.h>

#include <cstring>

namespace rtems_socket_test {

static const char *const TAG = "socket_test";

static constexpr uint16_t PORT = 5555;

static int failures = 0;
static struct netif net_interface;
static unsigned char mac_address[6] = {0x02, 0x52, 0x54, 0x00, 0x12, 0x34};

static void check(const char *what, bool ok) {
  ESP_LOGI(TAG, "%-44s %s", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

static void run() {
  ESP_LOGI(TAG, "--- ESPHome socket test ---");

  // The interface is brought up here rather than by a network component,
  // because there is not one yet -- that is rtems-esphome#13 and #14. Nothing
  // below this line knows that.
  ip_addr_t ipaddr, netmask, gateway;
  IP_ADDR4(&ipaddr, 10, 0, 2, 15);
  IP_ADDR4(&netmask, 255, 255, 255, 0);
  IP_ADDR4(&gateway, 10, 0, 2, 2);
  check("the interface comes up",
        start_networking(&net_interface, &ipaddr, &netmask, &gateway, mac_address) == 0);

  auto sock = esphome::socket::socket_ip(SOCK_STREAM, 0);
  check("socket_ip()", sock != nullptr);
  if (sock == nullptr) {
    goto done;
  }

  {
    int one = 1;
    sock->setsockopt(SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_storage server;
    socklen_t len = esphome::socket::set_sockaddr_any(
        reinterpret_cast<struct sockaddr *>(&server), sizeof(server), PORT);
    check("set_sockaddr_any", len > 0);
    check("bind()", sock->bind(reinterpret_cast<struct sockaddr *>(&server), len) == 0);
    check("listen()", sock->listen(1) == 0);

    ESP_LOGI(TAG, "listening on %u, waiting for the host", PORT);

    // Blocking accept. A stack that came up but cannot be reached times the
    // run out, which is the correct outcome rather than a pass.
    auto conn = sock->accept(nullptr, nullptr);
    check("a connection arrived from outside the guest", conn != nullptr);

    if (conn != nullptr) {
      char buf[64] = {};
      ssize_t n = conn->read(buf, sizeof(buf) - 1);
      check("bytes arrived over it", n > 0);
      check("they are what the host sent", n > 0 && strncmp(buf, "hello", 5) == 0);
      ESP_LOGI(TAG, "     received '%s'", buf);

      // Reply, so the path is shown to work in both directions -- a receive
      // alone would pass with a transmit path that never worked.
      n = conn->write("RTEMS", 5);
      check("a reply could be sent", n == 5);
      conn->close();
    }
    sock->close();
  }

done:
  ESP_LOGI(TAG, "%d failure(s)", failures);
  if (failures == 0) {
    ESP_LOGI(TAG, "CI-MARKER socket ok");
  }
}

}  // namespace rtems_socket_test
