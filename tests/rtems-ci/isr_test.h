#pragma once
#include "esphome/core/log.h"
#include "esphome/components/output/binary_output.h"
#include "esphome/components/rtems/gpio.h"

namespace rtems_isr_test {

static const char *const TAG = "isr_test";

/// Drives the wired pad so the other one takes an edge.  If the ISR path is
/// sound this returns and the marker prints; if calling the GPIO API from
/// interrupt context is a fault, the run ends before the next line.
inline void run(esphome::output::BinaryOutput *drive) {
  ESP_LOGI(TAG, "--- ISR edge test ---");

  ESP_LOGI(TAG, "about to drive an edge into the interrupt pin");
  drive->turn_on();
  ESP_LOGI(TAG, "survived the rising edge; isr count %u", (unsigned) esphome::rtems::gpio_isr_count);
  drive->turn_off();
  ESP_LOGI(TAG, "survived the falling edge; isr count %u", (unsigned) esphome::rtems::gpio_isr_count);

  ESP_LOGI(TAG, "CI-MARKER isr ok");
}

}  // namespace rtems_isr_test
