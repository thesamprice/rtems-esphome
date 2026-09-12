#pragma once

// Reads a real SPI device through ESPHome's own SPI stack.
//
// The bus is /dev/spi-0, which the RTEMS platform registers on the BSP's
// GPSPI2 controller.  The pin numbers in the YAML are not what routes the
// bus -- which pads the controller reaches is fixed when the BSP is built --
// but ESPHome's schema requires them, so they name the pads the BSP driver
// actually claims.
//
// QEMU supplies the slave:
//
//     -device is25lp016d,bus=gpspi2,drive=<file>
//
// and the exchange is a JEDEC ID read.  Chip select is the controller's, not
// a GPIO: see the note in spi_rtems.cpp about why this delegate does not
// drive cs_pin.

#include "esphome/components/spi/spi.h"
#include "esphome/core/log.h"

namespace rtems_spi_test {

static const char *const TAG = "spi_test";

static int failures = 0;

inline void check(const char *what, bool ok) {
  ESP_LOGI(TAG, "  %-4s %s", ok ? "ok" : "FAIL", what);
  if (!ok) {
    failures++;
  }
}

inline void run(esphome::spi::SPIComponent *bus) {
  ESP_LOGI(TAG, "--- ESPHome SPI test ---");

  // A component that failed setup still exists, and asking it for a delegate
  // walks a null bus.  Say so and stop, rather than turning a diagnosable
  // setup failure into a fatal exception that buries it.
  check("the bus set up without failing", !bus->is_failed());
  if (bus->is_failed()) {
    ESP_LOGI(TAG, "%d failure(s)", failures);
    return;
  }

  // cs_pin is a NullPin rather than a real pad: chip select belongs to the
  // controller on this platform, and driving a second pad from the delegate
  // would select nothing.  See spi_rtems.cpp.
  static esphome::spi::NullPin null_cs;
  auto *delegate = bus->register_device(nullptr, esphome::spi::MODE0, esphome::spi::BIT_ORDER_MSB_FIRST,
                                        1000000, &null_cs, false, false);
  check("a delegate was handed out", delegate != nullptr);

  if (delegate == nullptr) {
    ESP_LOGI(TAG, "%d failure(s)", failures);
    return;
  }

  uint8_t tx[4] = {0x9f, 0x00, 0x00, 0x00};
  uint8_t rx[4] = {0xee, 0xee, 0xee, 0xee};

  delegate->begin_transaction();
  delegate->transfer(tx, rx, sizeof(tx));
  delegate->end_transaction();

  ESP_LOGI(TAG, "  read back %02x %02x %02x %02x", rx[0], rx[1], rx[2], rx[3]);

  check("the buffer was written at all", rx[0] != 0xee);
  check("byte 0 is the reply, not the command that was sent", rx[0] == 0x00);
  check("manufacturer is ISSI (0x9d)", rx[1] == 0x9d);
  check("device type is 0x60", rx[2] == 0x60);
  check("capacity is 0x15", rx[3] == 0x15);

  ESP_LOGI(TAG, "%d failure(s)", failures);
  if (failures == 0) {
    ESP_LOGI(TAG, "CI-MARKER spi ok");
  }
}

}  // namespace rtems_spi_test
