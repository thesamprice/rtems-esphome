#pragma once

// Prove that a pin configured from YAML is really driven.
//
// The pins here are declared the ordinary way -- an `output:` and a
// `binary_sensor:` -- so what runs is the whole path a real configuration
// takes: gpio.py's codegen, RTEMSGPIOPin::pin_mode() and digital_write(), the
// BSP driver, the registers.
//
// The check is made at the pad, through the BSP hook, rather than by asking
// the same object what it thinks it wrote.  A pin object that reported its own
// last value would pass this test with every register write removed; reading
// the pad is what makes it mean something.

#include "esphome/core/log.h"

#include <bsp/gpio.h>

namespace rtems_gpio_test {

static const char *const TAG = "gpio_test";

// Must match the pin numbers in gpio.yaml.
static constexpr uint32_t BANK = 0;
static constexpr uint32_t PIN_OUT = 4;
static constexpr uint32_t PIN_IN = 5;

static int failures = 0;

static void check(const char *what, bool ok) {
  ESP_LOGI(TAG, "%-44s %s", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

static bool pad_level(uint32_t pin) { return rtems_gpio_bsp_get_value(BANK, pin) != 0; }

/// Called from the boot lambda with the generated components, which are the
/// things under test: each holds an RTEMSGPIOPin that gpio.py built.
template<typename Output, typename Sensor> static void run(Output *out, Sensor *in) {
  ESP_LOGI(TAG, "--- ESPHome GPIO test ---");

  out->turn_on();
  out->loop();
  check("turn_on drives the pad high", pad_level(PIN_OUT));

  out->turn_off();
  out->loop();
  check("turn_off drives the pad low", !pad_level(PIN_OUT));

  // Twice, so a pad that was already at the right level does not pass by
  // standing still.
  out->turn_on();
  out->loop();
  const bool high = pad_level(PIN_OUT);
  out->turn_off();
  out->loop();
  const bool low = pad_level(PIN_OUT);
  check("the pad follows the output component", high && !low);

  // The input pin's mode says pullup, so with nothing driving it, it reads
  // high.  A pin_mode() that ignored the pull flags would read low.
  check("a pulled-up input reads high", pad_level(PIN_IN));

  // And driving one pin must not move the other.
  out->turn_on();
  out->loop();
  check("driving one pin leaves the other alone", pad_level(PIN_IN));
  out->turn_off();
  out->loop();

  // Now the interrupt path, end to end.  Nothing is wired to the input pin, so
  // its pull resistor is what decides its level -- and changing the resistor
  // moves the pad, which is a real edge.  That is the only stimulus available
  // to a board with nothing plugged into it, and it is enough: the binary
  // sensor is configured for interrupts, so its state changing means the
  // interrupt reached ESPHome's handler.
  in->loop();
  check("the pulled-up sensor reads on", in->state);

  rtems_gpio_bsp_set_resistor_mode(BANK, PIN_IN, PULL_DOWN);
  in->loop();
  check("switching to pull-down is seen", !in->state);

  rtems_gpio_bsp_set_resistor_mode(BANK, PIN_IN, PULL_UP);
  in->loop();
  check("and switching back is seen", in->state);

  ESP_LOGI(TAG, "%d failure(s)", failures);
  if (failures == 0) {
    ESP_LOGI(TAG, "CI-MARKER gpio ok");
  }
}

}  // namespace rtems_gpio_test
