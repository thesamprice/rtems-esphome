#pragma once

// Asynchronous DNS on RTEMS, and proof that it does not stall the main loop.
//
// This is the question #14 is really about.  It was written expecting RTEMS to
// use libbsd and a blocking getaddrinfo(), which would have needed a resolver
// facade with two backends.  #50 chose lwip instead, so RTEMS reaches the same
// asynchronous dns_gethostbyname_addrtype() that ESP-IDF does, and what is left
// to establish is whether it behaves.
//
// The loop cadence is measured across every case, including the one that ends
// in a timeout, because "does not block" is the claim that matters and it is
// the one an implementation can quietly fail.

#include "esphome/components/network/ip_address.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <lwip/dns.h>
#include <lwip/ip_addr.h>

namespace rtems_dns_test {

static const char *const TAG = "dns_test";

static int failures = 0;

static void check(const char *what, bool ok) {
  ESP_LOGI(TAG, "%-46s %s", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

static volatile bool resolved = false;
static volatile bool failed = false;
static ip_addr_t result;

static void dns_cb(const char *name, const ip_addr_t *addr, void *arg) {
  (void) name;
  (void) arg;
  if (addr == nullptr) {
    failed = true;
    return;
  }
  result = *addr;
  resolved = true;
}

/// Spin the "main loop" while a lookup is outstanding, and report the worst gap
/// between iterations.  A blocking resolver shows up here as one enormous gap.
static uint32_t pump(uint32_t budget_ms) {
  const uint32_t start = esphome::millis();
  uint32_t last = start;
  uint32_t worst = 0;

  while (esphome::millis() - start < budget_ms) {
    if (resolved || failed) {
      break;
    }
    esphome::delay(5);
    const uint32_t now = esphome::millis();
    const uint32_t gap = now - last;
    if (gap > worst) {
      worst = gap;
    }
    last = now;
  }
  return worst;
}

static void one(const char *what, const char *host, bool expect_success, uint32_t budget_ms) {
  resolved = false;
  failed = false;

  ip_addr_t addr;
  err_t err;
  {
    esphome::LwIPLock lock;
    err = dns_gethostbyname_addrtype(host, &addr, dns_cb, nullptr, LWIP_DNS_ADDRTYPE_IPV4);
  }

  uint32_t worst = 0;
  bool ok;
  if (err == ERR_OK) {
    // Answered from cache or a literal, without leaving the call.
    result = addr;
    ok = expect_success;
    ESP_LOGI(TAG, "     %s: immediate", what);
  } else if (err == ERR_INPROGRESS) {
    worst = pump(budget_ms);
    ok = expect_success ? resolved : failed;
    ESP_LOGI(TAG, "     %s: %s after pumping, worst loop gap %ums", what,
             resolved ? "resolved" : (failed ? "failed" : "timed out"), worst);
  } else {
    ok = !expect_success;
    ESP_LOGI(TAG, "     %s: refused immediately, err=%d", what, err);
  }

  check(what, ok);
  if (resolved) {
    char buf[esphome::network::IP_ADDRESS_BUFFER_SIZE];
    ESP_LOGI(TAG, "     -> %s", esphome::network::IPAddress(&result).str_to(buf));
  }
  // 5 ms is the pump's own step; anything near the budget means it blocked.
  check("  the loop kept running throughout", worst < 100);
}

static void run() {
  ESP_LOGI(TAG, "--- async DNS test ---");

  // A literal must not go to the network at all.
  one("a numeric address resolves", "10.0.2.2", true, 5000);

  // QEMU's user-mode networking answers DNS at 10.0.2.3, which the DHCP-less
  // static configuration has to be told about.
  {
    // IP_ADDR4 sets the union's type tag itself when LWIP_IPV6 is on, which it
    // is here, so it takes the ip_addr_t rather than the ip4 half of it.
    ip_addr_t dnsserver;
    IP_ADDR4(&dnsserver, 10, 0, 2, 3);
    esphome::LwIPLock lock;
    dns_setserver(0, &dnsserver);
  }

  one("a real hostname resolves", "localhost.", true, 10000);

  // The case that matters most: a name that cannot resolve must fail without
  // taking the loop with it.
  one("a name that does not exist fails", "no-such-host.invalid", false, 15000);

  ESP_LOGI(TAG, "%d failure(s)", failures);
  if (failures == 0) {
    ESP_LOGI(TAG, "CI-MARKER dns ok");
  }
}

}  // namespace rtems_dns_test
