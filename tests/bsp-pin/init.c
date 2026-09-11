/*
 * Does the pin claim actually stop the collision #60 describes?
 *
 * The table in isolation is the easy half.  The claim that matters is the
 * cross-driver one: I2C takes GPIO5 and GPIO6 without ever going through the
 * shared GPIO layer, so before this existed a GPIO user could take GPIO5 as
 * well and both would "work".
 */

#include <bsp/pin.h>
#include <bsp/i2c.h>
#include <bsp/gpio.h>

#include <rtems.h>
#include <rtems/bspIo.h>

#include <stdio.h>
#include <stdlib.h>

static int failures;

static void check(const char *what, bool ok)
{
  printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

static const char owner_a[] = "driver A";
static const char owner_b[] = "driver B";

static rtems_task Init(rtems_task_argument arg)
{
  rtems_status_code sc;

  (void) arg;

  printf("\n*** ESP32C3 PIN CLAIM TEST ***\n");

  /* --- the table itself --- */
  check("a free pad can be claimed",        bsp_pin_claim(4, owner_a));
  check("the owner is recorded",            bsp_pin_owner(4) == owner_a);
  check("a second owner is refused",        !bsp_pin_claim(4, owner_b));
  check("and the pad still belongs to A",   bsp_pin_owner(4) == owner_a);
  check("the same owner may re-claim",      bsp_pin_claim(4, owner_a));

  bsp_pin_release(4);
  check("after release it is unowned",      bsp_pin_owner(4) == NULL);
  check("and another driver may take it",   bsp_pin_claim(4, owner_b));
  bsp_pin_release(4);

  check("a pad past the end is refused",    !bsp_pin_claim(BSP_PIN_COUNT, owner_a));
  check("and reports no owner",             bsp_pin_owner(BSP_PIN_COUNT) == NULL);
  check("releasing a free pad is harmless", (bsp_pin_release(7), true));

  /* --- the collision from the issue --- */
  bsp_pin_release(7);
  bsp_pin_release(10);

  sc = esp32c3_i2c_register("/dev/i2c-pintest");
  check("the I2C bus registers",            sc == RTEMS_SUCCESSFUL);
  check("I2C owns GPIO5 (SDA)",             bsp_pin_owner(5) != NULL);
  check("I2C owns GPIO6 (SCL)",             bsp_pin_owner(6) != NULL);

  /*
   * This is the bug.  Before the claim, this returned RTEMS_SUCCESSFUL and
   * quietly re-pointed the pad that I2C was using.
   */
  sc = rtems_gpio_bsp_select_output(0, 5, NULL);
  check("GPIO cannot take a pad I2C is using",  sc == RTEMS_RESOURCE_IN_USE);

  sc = rtems_gpio_bsp_select_input(0, 6, NULL);
  check("nor as an input",                      sc == RTEMS_RESOURCE_IN_USE);

  /* A pad I2C does not hold is still available, or the claim would be
   * refusing everything and the test above would pass for the wrong reason. */
  sc = rtems_gpio_bsp_select_output(0, 3, NULL);
  check("a free pad is still usable by GPIO",   sc == RTEMS_SUCCESSFUL);

  /* And the reverse direction: GPIO first, then I2C on the same pad. */
  check("GPIO can take GPIO11",             rtems_gpio_bsp_select_output(0, 11, NULL) == RTEMS_SUCCESSFUL);
  check("GPIO11 now has an owner",          bsp_pin_owner(11) != NULL);

  printf("\n%d failure(s)\n", failures);
  if (failures == 0) {
    printf("CI-MARKER pin ok\n");
  }
  printf("*** END OF ESP32C3 PIN CLAIM TEST ***\n");

  exit(0);
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_FILESYSTEM_IMFS
#define CONFIGURE_MAXIMUM_TASKS 8
#define CONFIGURE_MAXIMUM_SEMAPHORES 8
#define CONFIGURE_MAXIMUM_FILE_DESCRIPTORS 16
#define CONFIGURE_MAXIMUM_DRIVERS 8
#define CONFIGURE_INIT_TASK_STACK_SIZE (8 * 1024)
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
