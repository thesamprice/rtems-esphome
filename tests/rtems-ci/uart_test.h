#pragma once

// Send and receive over a real serial port through ESPHome's UARTComponent.
//
// Everything here goes through write_array(), available() and read_array(),
// which is what every uart component reaches the wire with -- so what passes
// here is what a `modbus:` or a `sml:` would get.
//
// The far end echoes in upper case. Upper case rather than verbatim because a
// UART looping TX back to RX inside the chip is a real fault, and a verbatim
// echo would pass while it was happening.

#include "esphome/components/uart/uart.h"
#include "esphome/core/log.h"

namespace rtems_uart_test {

static const char *const TAG = "uart_test";

static int failures = 0;

static void check(const char *what, bool ok) {
  ESP_LOGI(TAG, "%-44s %s", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

// available() is not a promise that the bytes have arrived yet, only that they
// have so far; this waits for the far end to answer.
static bool wait_for(esphome::uart::UARTComponent *uart, size_t want, unsigned tries) {
  for (unsigned i = 0; i < tries; ++i) {
    if (uart->available() >= want) {
      return true;
    }
    esphome::delay(10);
  }
  return false;
}

static void run(esphome::uart::UARTComponent *uart) {
  ESP_LOGI(TAG, "--- ESPHome UART test ---");

  check("nothing is waiting before we send", uart->available() == 0);

  static const char msg[] = "esphome";
  constexpr size_t len = sizeof(msg) - 1;

  uart->write_array(reinterpret_cast<const uint8_t *>(msg), len);
  uart->flush();

  check("the reply arrives", wait_for(uart, len, 500));

  uint8_t reply[len + 1] = {};
  check("read it", uart->read_array(reply, len));
  check("it is what the far end sent, not what we sent",
        memcmp(reply, "ESPHOME", len) == 0);
  if (memcmp(reply, "ESPHOME", len) != 0) {
    ESP_LOGI(TAG, "     got '%s'", reinterpret_cast<const char *>(reply));
  }

  check("and nothing is left over", uart->available() == 0);

  // peek_byte must not consume, which is the whole of its contract and the
  // easiest thing to get wrong.
  uart->write_array(reinterpret_cast<const uint8_t *>("ab"), 2);
  uart->flush();
  check("two more arrive", wait_for(uart, 2, 500));

  uint8_t peeked = 0;
  check("peek", uart->peek_byte(&peeked));
  check("peek returns the first byte", peeked == 'A');
  check("peek does not consume it", uart->available() == 2);

  uint8_t both[2] = {};
  check("read both", uart->read_array(both, 2));
  check("the peeked byte is still first", both[0] == 'A' && both[1] == 'B');
  check("now nothing is left", uart->available() == 0);

  ESP_LOGI(TAG, "%d failure(s)", failures);
  if (failures == 0) {
    ESP_LOGI(TAG, "CI-MARKER uart ok");
  }
}

}  // namespace rtems_uart_test
