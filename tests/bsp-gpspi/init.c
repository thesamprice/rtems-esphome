/*
 * Does the GPSPI2 model actually move bytes?
 *
 * No RTEMS SPI driver exists for this part yet (#84 step 2), so this drives
 * the controller's registers directly.  That is the point: it tests the QEMU
 * device rather than a driver stacked on it, so a failure here is the model's.
 *
 * The slave is an SSI flash attached from the command line:
 *
 *     -device is25lp016d,drive=...
 *
 * and the transfer is a JEDEC ID read -- send 0x9F, read three bytes back.
 * That is deliberately the shape that #86 got wrong: a full-duplex transfer
 * whose transmitted byte is larger than the number of bytes received.  With
 * the old bounds check the reply was discarded and the guest saw its own
 * command byte.
 */

#include <bsp.h>

#include <rtems.h>

#include <stdio.h>
#include <stdlib.h>

#define GPSPI2_BASE 0x60024000

#define GPSPI_CMD      0x00
#define GPSPI_ADDR     0x04
#define GPSPI_USER     0x10
#define GPSPI_USER1    0x14
#define GPSPI_USER2    0x18
#define GPSPI_MS_DLEN  0x1c
#define GPSPI_MISC     0x20
#define GPSPI_W0       0x98

#define CMD_USR            ( 1u << 24 )
#define USER_COMMAND       ( 1u << 31 )
#define USER_MISO          ( 1u << 28 )
#define USER_MOSI          ( 1u << 27 )
#define USER_DOUTDIN       ( 1u << 0 )
#define USER2_CMD_BITLEN_S 28
#define MISC_CS0_DIS       ( 1u << 0 )

#define REG( off ) ( *(volatile uint32_t *) ( GPSPI2_BASE + (off) ) )

static int failures;

static void check(const char *what, bool ok)
{
  printf("%-54s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

/* Send an 8-bit command, then clock in n bytes. */
static void spi_cmd_read(uint8_t cmd, uint8_t *out, int n)
{
  int i;

  /* CS0 takes part; the other two stay disabled. */
  REG( GPSPI_MISC ) = ~MISC_CS0_DIS & 0x7;

  REG( GPSPI_USER2 ) = ( 7u << USER2_CMD_BITLEN_S ) | cmd;
  REG( GPSPI_USER ) = USER_COMMAND | USER_MISO;
  REG( GPSPI_MS_DLEN ) = (uint32_t) ( n * 8 - 1 );
  REG( GPSPI_ADDR ) = 0;

  for ( i = 0; i < 4; ++i ) {
    REG( GPSPI_W0 + 4 * i ) = 0;
  }

  REG( GPSPI_CMD ) = CMD_USR;

  for ( i = 0; i < n; ++i ) {
    out[ i ] = (uint8_t) ( REG( GPSPI_W0 + 4 * ( i / 4 ) ) >> ( 8 * ( i % 4 ) ) );
  }
}

/*
 * The same JEDEC read done full duplex: the command goes out as the first data
 * byte rather than in the command phase, and bytes are clocked in at the same
 * time.
 *
 * This is the shape that matters for #86.  A read-only data phase does not
 * expose that defect at all -- tx_bytes is zero, so the byte being compared
 * stays zero and the bounds check accidentally reads true.  It takes a phase
 * that both sends and receives, with sent values larger than the number of
 * bytes received, before the wrong comparison changes the answer.  The first
 * version of this test used the read-only shape and passed with the defect
 * reintroduced, which is what prompted this one.
 */
static void spi_full_duplex(uint8_t *buf, int n)
{
  int i;

  REG( GPSPI_MISC ) = ~MISC_CS0_DIS & 0x7;

  REG( GPSPI_USER ) = USER_MOSI | USER_MISO | USER_DOUTDIN;
  REG( GPSPI_USER2 ) = 0;
  REG( GPSPI_MS_DLEN ) = (uint32_t) ( n * 8 - 1 );
  REG( GPSPI_ADDR ) = 0;

  for ( i = 0; i < 4; ++i ) {
    REG( GPSPI_W0 + 4 * i ) = 0;
  }
  for ( i = 0; i < n; ++i ) {
    volatile uint32_t *w = (volatile uint32_t *) ( GPSPI2_BASE + GPSPI_W0 + 4 * ( i / 4 ) );
    *w = ( *w & ~( 0xffu << ( 8 * ( i % 4 ) ) ) )
       | ( (uint32_t) buf[ i ] << ( 8 * ( i % 4 ) ) );
  }


  REG( GPSPI_CMD ) = CMD_USR;


  for ( i = 0; i < n; ++i ) {
    buf[ i ] = (uint8_t) ( REG( GPSPI_W0 + 4 * ( i / 4 ) ) >> ( 8 * ( i % 4 ) ) );
  }
}

static rtems_task Init(rtems_task_argument arg)
{
  uint8_t id[3];
  uint8_t fd[4];

  (void) arg;

  printf("\n*** ESP32C3 GPSPI2 TEST ***\n");

  spi_cmd_read( 0x9f, id, 3 );
  printf("JEDEC ID read back %02x %02x %02x\n", id[0], id[1], id[2]);

  /* is25lp016d: manufacturer 0x9d (ISSI), type 0x60, capacity 0x15. */
  check("manufacturer byte is ISSI (0x9d)", id[0] == 0x9d);
  check("it is not the command byte echoed back", id[0] != 0x9f);
  check("device type is 0x60", id[1] == 0x60);
  check("capacity byte is 0x15 (16 Mbit)", id[2] == 0x15);

  /* Full duplex: send 9f ff ff ff, expect the ID in the last three bytes. */
  fd[0] = 0x9f;
  fd[1] = 0x00;
  fd[2] = 0x00;
  fd[3] = 0x00;
  spi_full_duplex( fd, 4 );
  printf("full-duplex read back %02x %02x %02x %02x\n", fd[0], fd[1], fd[2], fd[3]);

  check("full duplex received the ID, not the sent bytes", fd[1] == 0x9d);
  check("  and the rest of it", fd[2] == 0x60 && fd[3] == 0x15);

  /*
   * The discriminator for #86, and the only check here that fails when the
   * defect is present.  Byte 0 is the slave's answer to the command byte,
   * which is zero; the buffer held 0x9f going in.  With the bounds check
   * comparing the data instead of the index, 0x9f is not less than the four
   * bytes being received, so the answer is never stored and the buffer still
   * reads back what was transmitted.  Bytes 1 to 3 are stored either way,
   * because the values sent there are 0x00, which is less than 4 -- so they
   * pass under the defect and prove nothing about it.
   */
  check("byte 0 holds the reply, not the byte that was sent", fd[0] == 0x00);

  printf("\n%d failure(s)\n", failures);
  if (failures == 0) {
    printf("CI-MARKER gpspi ok\n");
  }
  printf("*** END OF ESP32C3 GPSPI2 TEST ***\n");
  exit(0);
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_MICROSECONDS_PER_TICK 1000
#define CONFIGURE_FILESYSTEM_IMFS
#define CONFIGURE_MAXIMUM_TASKS 8
#define CONFIGURE_MAXIMUM_FILE_DESCRIPTORS 16
#define CONFIGURE_MAXIMUM_DRIVERS 8
#define CONFIGURE_INIT_TASK_STACK_SIZE (8 * 1024)
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
