/*
 * What does open drain built from select_input()/select_output() cost?
 *
 * #54 option 2 emulates open drain with the two calls <bsp/gpio.h> already
 * has.  Whether that is usable depends entirely on the per-write cost, and
 * guessing is what the issue says to avoid.
 *
 * Measured against the push-pull path in the same run, on the same pin, so
 * the comparison does not depend on the absolute clock being right -- which
 * under QEMU it is not (#77).
 */

#include <bsp.h>
#include <bsp/gpio.h>

#include <rtems.h>
#include <rtems/counter.h>

#include <stdio.h>
#include <stdlib.h>

#define PIN   4
#define BANK  0
#define N     2000

/* 1-Wire's tightest constraint: after driving the line low to start a read
 * slot, the master must release it and sample within 15 us. */
#define ONEWIRE_RELEASE_BUDGET_NS 15000

static rtems_task Init(rtems_task_argument arg)
{
  rtems_counter_ticks t0, t1;
  uint64_t push_ns, od_ns, od_release_ns;
  rtems_counter_ticks push_ticks, od_ticks, rel_ticks;
  int i;

  (void) arg;

  printf("\n*** ESP32C3 OPEN DRAIN COST ***\n");

  rtems_gpio_initialize();
  rtems_gpio_request_pin( PIN, DIGITAL_OUTPUT, false, false, NULL );
  rtems_gpio_resistor_mode( PIN, PULL_UP );

  /* Warm: the first call through either path pays for whatever it caches. */
  for ( i = 0; i < 16; ++i ) {
    rtems_gpio_bsp_set( BANK, PIN );
    rtems_gpio_bsp_clear( BANK, PIN );
    rtems_gpio_bsp_select_input( BANK, PIN, NULL );
    rtems_gpio_bsp_select_output( BANK, PIN, NULL );
  }

  /* Push-pull: one register store per write. */
  t0 = rtems_counter_read();
  for ( i = 0; i < N; ++i ) {
    rtems_gpio_bsp_set( BANK, PIN );
    rtems_gpio_bsp_clear( BANK, PIN );
  }
  t1 = rtems_counter_read();
  push_ticks = t1 - t0;
  push_ns = rtems_counter_ticks_to_nanoseconds( t1 - t0 ) / ( 2 * N );

  /* Open drain emulated: release, then drive. */
  t0 = rtems_counter_read();
  for ( i = 0; i < N; ++i ) {
    rtems_gpio_bsp_select_input( BANK, PIN, NULL );
    rtems_gpio_bsp_select_output( BANK, PIN, NULL );
    rtems_gpio_bsp_clear( BANK, PIN );
  }
  t1 = rtems_counter_read();
  od_ticks = t1 - t0;
  od_ns = rtems_counter_ticks_to_nanoseconds( t1 - t0 ) / ( 2 * N );

  /* The release alone, which is the half 1-Wire has a deadline for. */
  t0 = rtems_counter_read();
  for ( i = 0; i < N; ++i ) {
    rtems_gpio_bsp_select_input( BANK, PIN, NULL );
  }
  t1 = rtems_counter_read();
  rel_ticks = t1 - t0;
  od_release_ns = rtems_counter_ticks_to_nanoseconds( t1 - t0 ) / N;

  printf( "\ncounter                %8llu Hz (the systimer, not the CPU clock)\n",
    (unsigned long long) rtems_counter_nanoseconds_to_ticks( 1000000000UL ) );
  printf( "raw ticks over %d iterations: push %llu  od %llu  release %llu\n",
    N, (unsigned long long) push_ticks, (unsigned long long) od_ticks,
    (unsigned long long) rel_ticks );

  /*
   * The ratio is the number worth quoting.  Both halves are measured with
   * the same counter in the same run, so whatever the clock is really doing
   * cancels out of it.
   */
  printf( "\nopen-drain release costs %llu.%02llux a push-pull write\n",
    (unsigned long long) ( ( rel_ticks * 2 ) / ( push_ticks ) ),
    (unsigned long long) ( ( ( rel_ticks * 200 ) / ( push_ticks ) ) % 100 ) );

  /*
   * The absolute figures are not silicon figures and must not be read as
   * such.  Two independent reasons: the counter is the systimer and #77
   * records guest time running several times fast under this QEMU, and
   * -icount shift=0 charges one cycle per instruction with no memory or
   * peripheral-bus stall -- which is exactly where writing IO_MUX costs on
   * real hardware.  So these are a floor, and a loose one.
   */
  printf( "\nunder emulation only, and optimistic:\n" );
  printf( "  push-pull write      %8llu ns\n", (unsigned long long) push_ns );
  printf( "  open-drain write     %8llu ns\n", (unsigned long long) od_ns );
  printf( "  of which, release    %8llu ns\n", (unsigned long long) od_release_ns );
  printf( "  1-Wire release budget %7d ns\n", ONEWIRE_RELEASE_BUDGET_NS );
  printf( "\nheadroom against that budget, at this floor: %llux\n",
    (unsigned long long) ( ONEWIRE_RELEASE_BUDGET_NS / ( od_release_ns ? od_release_ns : 1 ) ) );
  printf( "QEMU cannot answer whether 1-Wire works; it can only say this is\n" );
  printf( "not obviously disqualifying.  Confirm on hardware (#53).\n" );

  printf( "\nCI-MARKER od measured\n" );
  printf( "*** END OF ESP32C3 OPEN DRAIN COST ***\n" );
  exit( 0 );
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
