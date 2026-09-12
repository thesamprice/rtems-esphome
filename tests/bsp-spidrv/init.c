/*
 * The RTEMS SPI driver, through the framework rather than the registers.
 *
 * tests/bsp-gpspi drives GPSPI2's registers directly, so a failure there is
 * the QEMU model's.  This one goes through <dev/spi/spi.h> -- open the bus
 * device, ioctl a message list, read the reply -- so a failure here is the
 * driver's or the framework's, and the two lanes bracket the layer between
 * them.
 *
 * The slave is an is25lp016d attached with -device, and the exchange is a
 * JEDEC ID read: 0x9f out, three bytes back.
 */

#include <bsp.h>
#include <bsp/spi.h>
#include <bsp/pin.h>

#include <dev/spi/spi.h>

#include <rtems.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUS_PATH "/dev/spi-0"

static int failures;

static void check(const char *what, bool ok)
{
  printf("%-54s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

static rtems_task Init(rtems_task_argument arg)
{
  rtems_status_code sc;
  int fd;
  int rv;
  uint8_t tx[4] = { 0x9f, 0x00, 0x00, 0x00 };
  uint8_t rx[4] = { 0xee, 0xee, 0xee, 0xee };
  struct spi_ioc_transfer msg;

  (void) arg;

  printf("\n*** ESP32C3 SPI DRIVER TEST ***\n");

  sc = esp32c3_spi_register( BUS_PATH );
  check("the bus registers", sc == RTEMS_SUCCESSFUL);

  if (sc != RTEMS_SUCCESSFUL) {
    goto done;
  }

  /* The pads are the driver's, and it says which. */
  check("it claimed the clock pad", bsp_pin_owner(0) != NULL);
  check("and the chip select", bsp_pin_owner(4) != NULL);

  /* Registering twice must fail on the pads rather than double-configure
   * them, which is the whole point of the claim. */
  sc = esp32c3_spi_register( "/dev/spi-1" );
  check("a second bus is refused, not silently allowed",
        sc == RTEMS_RESOURCE_IN_USE);

  fd = open( BUS_PATH, O_RDWR );
  check("the bus device opens", fd >= 0);

  if (fd < 0) {
    goto done;
  }

  memset( &msg, 0, sizeof( msg ) );
  msg.tx_buf = tx;
  msg.rx_buf = rx;
  msg.len = sizeof( tx );
  msg.bits_per_word = 8;
  msg.mode = 0;

  rv = ioctl( fd, SPI_IOC_MESSAGE( 1 ), &msg );
  check("the transfer completes", rv == 0);

  printf("read back %02x %02x %02x %02x\n", rx[0], rx[1], rx[2], rx[3]);

  check("the buffer was written at all", rx[0] != 0xee);
  check("byte 0 is the reply to the command, not the command",
        rx[0] == 0x00);
  check("manufacturer is ISSI (0x9d)", rx[1] == 0x9d);
  check("device type is 0x60", rx[2] == 0x60);
  check("capacity is 0x15", rx[3] == 0x15);

  close( fd );

done:
  printf("\n%d failure(s)\n", failures);
  if (failures == 0) {
    printf("CI-MARKER spidrv ok\n");
  }
  printf("*** END OF ESP32C3 SPI DRIVER TEST ***\n");
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
