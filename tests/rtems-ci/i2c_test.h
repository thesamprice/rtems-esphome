#pragma once

// Talk to a real I2C device through ESPHome's I2CBus interface.
//
// Everything here goes through write_readv(), which is the one method a bus
// has to implement and the one every i2c component reaches the wire with -- so
// what passes here is what a `bme280:` or an `ssd1306:` would get.
//
// The registers read are the TMP105's two alarm limits rather than its
// temperature. They reset to 75 C and 80 C: known, non-zero, and different
// from each other, so a bus stuck at zero fails and so does one returning the
// previous transfer's bytes. The temperature register is not usable for this,
// because QEMU's tmp105_reset() zeroes it after -device applies its
// properties, so a command-line temperature= never survives to be read.

#include "esphome/components/i2c/i2c_bus.h"
#include "esphome/core/log.h"

namespace rtems_i2c_test {

static const char *const TAG = "i2c_test";

static constexpr uint8_t TMP105 = 0x48;
static constexpr uint8_t REG_T_LOW = 0x02;
static constexpr uint8_t REG_T_HIGH = 0x03;
static constexpr uint8_t REG_CONFIG = 0x01;
static constexpr uint8_t NOBODY = 0x22;

static int failures = 0;

static void check(const char *what, bool ok) {
  ESP_LOGI(TAG, "%-44s %s", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

static void run(esphome::i2c::I2CBus *bus) {
  using namespace esphome::i2c;

  ESP_LOGI(TAG, "--- ESPHome I2C test ---");

  uint8_t reg = REG_T_LOW;
  uint8_t raw[2] = {0, 0};

  // write_readv with both halves is the repeated-start path: the register
  // pointer goes out, then the value comes back without the bus being
  // released in between.
  ErrorCode rc = bus->write_readv(TMP105, &reg, 1, raw, 2);
  check("read T_LOW", rc == ERROR_OK);
  check("T_LOW is 75 C, its reset value", raw[0] == 0x4b && raw[1] == 0x00);
  if (raw[0] != 0x4b || raw[1] != 0x00) {
    ESP_LOGI(TAG, "     expected 0x4b00, got 0x%02x%02x", raw[0], raw[1]);
  }

  reg = REG_T_HIGH;
  raw[0] = raw[1] = 0;
  rc = bus->write_readv(TMP105, &reg, 1, raw, 2);
  check("read T_HIGH", rc == ERROR_OK);
  check("T_HIGH is 80 C, so it differs from T_LOW", raw[0] == 0x50 && raw[1] == 0x00);

  // A write on its own, then read it back: proves bytes reach the device
  // rather than merely not erroring.
  const uint8_t set_config[2] = {REG_CONFIG, 0x60};
  rc = bus->write_readv(TMP105, set_config, 2, nullptr, 0);
  check("write the config register", rc == ERROR_OK);

  reg = REG_CONFIG;
  uint8_t back = 0;
  rc = bus->write_readv(TMP105, &reg, 1, &back, 1);
  check("read the config register back", rc == ERROR_OK);
  check("it holds what was written", back == 0x60);

  // Nothing answers at 0x22. A bus that reported success here would report
  // success whatever happened on the wire, which would make every check above
  // worthless -- so this is the one that keeps the others honest.
  reg = 0;
  back = 0;
  rc = bus->write_readv(NOBODY, &reg, 1, &back, 1);
  check("an address nothing answers on is NACKed", rc == ERROR_NOT_ACKNOWLEDGED);

  ESP_LOGI(TAG, "%d failure(s)", failures);
  if (failures == 0) {
    ESP_LOGI(TAG, "CI-MARKER i2c ok");
  }
}

}  // namespace rtems_i2c_test
