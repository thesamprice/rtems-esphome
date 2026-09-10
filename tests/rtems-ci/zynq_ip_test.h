#pragma once

// network/ip_address.h on RTEMS, both address families.
//
// RTEMS uses the *lwIP* arm of that header, not the POSIX one, even though its
// sockets are POSIX. The POSIX arm defines its own ip_addr_t out of struct
// in_addr, which collides outright with lwIP's the moment anything includes
// both -- and something does here, because the stack underneath is lwIP and
// components reach it for DNS.
//
// So this checks the lwIP arm on RTEMS: a reused arm that compiles but parses
// wrongly would be worse than a new one.

#include "esphome/components/network/ip_address.h"
#include "esphome/components/network/util.h"
#include "esphome/core/log.h"

#include <cstring>

namespace rtems_ip_test {

static const char *const TAG = "ip_test";

static int failures = 0;

/// The lwIP arm formats through str_to(); there is no str().
static const char *fmt(const esphome::network::IPAddress &a, char *buf) { return a.str_to(buf); }

static void check(const char *what, bool ok) {
  ESP_LOGI(TAG, "%-46s %s", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

static void run() {
  using esphome::network::IPAddress;

  ESP_LOGI(TAG, "--- ip_address test ---");

  // --- IPv4: parse, format, classify -------------------------------------
  IPAddress v4("192.168.7.31");
  check("an IPv4 literal parses", v4.is_set());
  check("it is classified v4", v4.is_ip4() && !v4.is_ip6());
  char b1[esphome::network::IP_ADDRESS_BUFFER_SIZE];
  check("it formats back to itself", strcmp(fmt(v4, b1), "192.168.7.31") == 0);
  if (strcmp(fmt(v4, b1), "192.168.7.31") != 0) {
    ESP_LOGI(TAG, "     got '%s'", fmt(v4, b1));
  }

  // The octet constructor must agree with the parser, which is the property
  // that catches a byte-order mistake -- and a byte-order mistake is exactly
  // what reusing an arm written for another platform risks.
  IPAddress v4_octets(192, 168, 7, 31);
  check("the octet form equals the parsed form", v4_octets == v4);
  {
    char a[esphome::network::IP_ADDRESS_BUFFER_SIZE], b[esphome::network::IP_ADDRESS_BUFFER_SIZE];
    check("and formats the same", strcmp(fmt(v4_octets, a), fmt(v4, b)) == 0);
  }

  IPAddress v4_other("192.168.7.32");
  check("a different address is not equal", !(v4_other == v4));
  check("and != agrees with it", v4_other != v4);

  // --- IPv6 ---------------------------------------------------------------
  IPAddress v6("2001:db8::1");
  check("an IPv6 literal parses", v6.is_set());
  check("it is classified v6", v6.is_ip6() && !v6.is_ip4());
  {
    char b[esphome::network::IP_ADDRESS_BUFFER_SIZE];
    check("it formats back to itself", strcmp(fmt(v6, b), "2001:db8::1") == 0);
    if (strcmp(fmt(v6, b), "2001:db8::1") != 0) {
      ESP_LOGI(TAG, "     got '%s'", fmt(v6, b));
    }
  }

  IPAddress v6_same("2001:0db8:0000:0000:0000:0000:0000:0001");
  check("a longhand v6 form parses to the same address", v6_same == v6);

  IPAddress v6_other("2001:db8::2");
  check("a different v6 address is not equal", !(v6_other == v6));

  // A v4 and a v6 address must never compare equal, whatever their bytes.
  check("v4 and v6 are never equal", !(v4 == v6));

  // --- rubbish in -------------------------------------------------------
  // A parser that accepts anything would pass every check above, so this is
  // the one that keeps them honest.
  check("a non-address does not parse", !IPAddress("not-an-address").is_set());
  check("an out-of-range octet does not parse", !IPAddress("192.168.7.999").is_set());

  // Three-part shorthand is ACCEPTED here too, as 192.168.0.7 -- lwIP's
  // ip4addr_aton is a full inet_aton clone, "a.b.c" meaning a.b plus a 16-bit
  // c. I expected lwIP to be the strict one and it is not, so both arms agree
  // and #66's premise was wrong.
  //
  // Asserted rather than assumed, twice over now: it is surprising, and the
  // consequence is that a mistyped address in a configuration becomes a
  // plausible wrong one on every platform rather than an error on some.
  IPAddress shorthand("192.168.7");
  check("three-part shorthand parses here too", shorthand.is_set());
  check("and means a.b.0.c", shorthand == IPAddress(192, 168, 0, 7));
  if (shorthand.is_set()) {
    char b[esphome::network::IP_ADDRESS_BUFFER_SIZE];
    ESP_LOGI(TAG, "     '192.168.7' parsed as '%s'", fmt(shorthand, b));
  }

  // --- the platform's own view of the link --------------------------------
  // No interface has been brought up in this configuration, so the honest
  // answer is false. A platform that assumed a link would say true here.
  check("with no interface up, the network reports disconnected",
        !esphome::network::is_connected());

  ESP_LOGI(TAG, "%d failure(s)", failures);
  if (failures == 0) {
    ESP_LOGI(TAG, "CI-MARKER ip ok");
  }
}

}  // namespace rtems_ip_test
