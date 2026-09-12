/*
 * A transaction on the BSP's I2C driver, with no ESPHome above it.
 *
 * tests/bsp-pin proves the bus registers and that the pads are claimed.  It
 * does not prove a byte moves.  This reads the TMP105's T_LOW register, whose
 * reset value is 75 C -- a value that is not zero, so a driver returning
 * nothing is distinguishable from one that works.
 */

#include <bsp.h>
#include <bsp/i2c.h>

#include <dev/i2c/i2c.h>

#include <rtems.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BUS "/dev/i2c-0"
#define TMP105_ADDR 0x48
#define TMP105_T_LOW 0x02

static int failures;

static void check(const char *what, bool ok)
{
  printf("%-50s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

static rtems_task Init(rtems_task_argument arg)
{
  rtems_status_code sc;
  int fd;
  uint8_t reg = TMP105_T_LOW;
  uint8_t buf[2] = { 0, 0 };
  i2c_msg msgs[2];
  struct i2c_rdwr_ioctl_data payload;

  (void) arg;

  printf("\n*** ESP32C3 BSP I2C TEST ***\n");

  sc = esp32c3_i2c_register( BUS );
  check("the bus registers", sc == RTEMS_SUCCESSFUL);

  fd = open( BUS, O_RDWR );
  check("the device opens", fd >= 0);

  if (fd < 0) {
    goto done;
  }

  /* ESPHome sets the clock before transferring; the BSP default is the same
   * value, so if this changes the outcome the set_clock path is the fault. */
  check("setting the clock to 100 kHz succeeds",
        ioctl( fd, I2C_BUS_SET_CLOCK, (unsigned long) 100000 ) == 0);

  memset( msgs, 0, sizeof( msgs ) );
  msgs[0].addr = TMP105_ADDR;
  msgs[0].flags = 0;
  msgs[0].len = 1;
  msgs[0].buf = &reg;
  msgs[1].addr = TMP105_ADDR;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len = 2;
  msgs[1].buf = buf;

  payload.msgs = msgs;
  payload.nmsgs = 2;

  check("the transfer completes", ioctl( fd, I2C_RDWR, &payload ) == 0);
  printf("T_LOW read back 0x%02x%02x\n", buf[0], buf[1]);
  check("T_LOW is 0x4b00, its reset value", buf[0] == 0x4b && buf[1] == 0x00);

  close( fd );

done:
  printf("\n%d failure(s)\n", failures);
  if (failures == 0) {
    printf("CI-MARKER bspi2c ok\n");
  }
  printf("*** END OF ESP32C3 BSP I2C TEST ***\n");
  exit(0);
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_MICROSECONDS_PER_TICK 1000
#define CONFIGURE_FILESYSTEM_IMFS
#define CONFIGURE_MAXIMUM_TASKS 8
#define CONFIGURE_MAXIMUM_SEMAPHORES 8
#define CONFIGURE_MAXIMUM_FILE_DESCRIPTORS 16
#define CONFIGURE_MAXIMUM_DRIVERS 8
#define CONFIGURE_INIT_TASK_STACK_SIZE (8 * 1024)
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
