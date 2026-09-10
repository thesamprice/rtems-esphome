#pragma once

// network/ip_address.h on RTEMS, both address families.
//
// The header's POSIX arm is reused here rather than a new one written for
// RTEMS, on the grounds that it is <arpa/inet.h>, struct in_addr and
// inet_pton, with nothing host-specific about it. This is what checks that
// claim: a reused arm that compiles but parses wrongly would be worse than a
// new one.

#include "esphome/components/network/ip_address.h"
#include "esphome/components/network/util.h"
#include "esphome/core/log.h"

namespace rtems_ip_test {

static const char *const TAG = "ip_test";

static int failures = 0;

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
  check("it formats back to itself", v4.str() == "192.168.7.31");
  if (v4.str() != "192.168.7.31") {
    ESP_LOGI(TAG, "     got '%s'", v4.str().c_str());
  }

  // The octet constructor must agree with the parser, which is the property
  // that catches a byte-order mistake -- and a byte-order mistake is exactly
  // what reusing an arm written for another platform risks.
  IPAddress v4_octets(192, 168, 7, 31);
  check("the octet form equals the parsed form", v4_octets == v4);
  check("and formats the same", v4_octets.str() == v4.str());

  IPAddress v4_other("192.168.7.32");
  check("a different address is not equal", !(v4_other == v4));
  check("and != agrees with it", v4_other != v4);

  // --- IPv6 ---------------------------------------------------------------
  IPAddress v6("2001:db8::1");
  check("an IPv6 literal parses", v6.is_set());
  check("it is classified v6", v6.is_ip6() && !v6.is_ip4());
  check("it formats back to itself", v6.str() == "2001:db8::1");
  if (v6.str() != "2001:db8::1") {
    ESP_LOGI(TAG, "     got '%s'", v6.str().c_str());
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

  // Three-part shorthand is accepted, and that is correct: inet_aton() has
  // always read "a.b.c" as a.b plus a 16-bit c, so 192.168.7 is 192.168.0.7.
  // Asserted rather than left alone because it is surprising, and because it
  // is a property of the POSIX parser rather than of this port -- the same
  // string means something else on a platform whose arm uses lwip's
  // ipaddr_aton, which rejects it.
  IPAddress shorthand("192.168.7");
  check("three-part shorthand parses, as inet_aton defines it",
        shorthand.is_set());
  check("and means a.b.0.c", shorthand == IPAddress(192, 168, 0, 7));
  if (shorthand.is_set()) {
    ESP_LOGI(TAG, "     '192.168.7' parsed as '%s'", shorthand.str().c_str());
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
