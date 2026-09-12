#pragma once

// #54's exit criterion: two pins configured open drain, where one holding low
// wins over the other releasing, through ESPHome's GPIOPin with no
// chip-specific code above the BSP.
//
// GPIO0 and GPIO11 are tied together by QEMU:
//
//     -global driver=esp32c3.gpio,property=wire,value=0x801
//
// which is what a board does with a track. Two pads of one controller are not
// connected otherwise, and open drain means nothing with a single driver --
// the property under test is what happens when two disagree.
//
// Nothing here names a register. The pins are configured through
// gpio::Flags and driven through digital_write(), so a pass says the
// platform's open-drain support works at the interface a component uses.

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace rtems_od_test {

static const char *const TAG = "od_test";

static int failures = 0;

inline void check(const char *what, bool ok) {
  ESP_LOGI(TAG, "  %-4s %s", ok ? "ok" : "FAIL", what);
  if (!ok) {
    failures++;
  }
}

inline void run(esphome::GPIOPin *a, esphome::GPIOPin *b, esphome::GPIOPin *c) {
  ESP_LOGI(TAG, "--- open drain on a shared net ---");

  a->setup();
  b->setup();

  // Both released: the pull-up owns the net.
  a->digital_write(true);
  b->digital_write(true);
  check("both released, the net is high", a->digital_read() && b->digital_read());

  // One low. The other is still releasing, and must lose.
  a->digital_write(false);
  check("A pulls low, A reads low", !a->digital_read());
  check("A pulls low, B reads low too -- low wins", !b->digital_read());

  // The other releases as well; still low, because A holds it.
  b->digital_write(true);
  check("B releasing does not lift the net while A holds it", !a->digital_read());

  // A releases. Nothing drives, so the pull-up takes it back.
  a->digital_write(true);
  check("both released again, the net returns high",
        a->digital_read() && b->digital_read());

  // And the mirror case, so a pass cannot come from A being special.
  b->digital_write(false);
  check("B pulls low, A reads low", !a->digital_read());
  b->digital_write(true);
  check("and releases", a->digital_read());

  /*
   * The check that separates open drain from push-pull, and without it every
   * check above passes either way -- they would only be testing the wire.
   *
   * C is open drain with no pull resistor and nothing else on its net.
   * Writing 1 releases it, so nothing drives it at all and it reads low.  A
   * push-pull output writing 1 drives high and would read high.  So this is
   * the one check that fails if the pad is not really open drain.
   */
  c->setup();
  c->digital_write(false);
  check("C, open drain and unpulled, reads low when driven low", !c->digital_read());
  c->digital_write(true);
  check("and still low when released, because nothing drives it",
        !c->digital_read());

  ESP_LOGI(TAG, "%d failure(s)", failures);
  if (failures == 0) {
    ESP_LOGI(TAG, "CI-MARKER opendrain ok");
  }
}

}  // namespace rtems_od_test
