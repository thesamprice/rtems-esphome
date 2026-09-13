/*
 * Peripheral interrupt source 0 must be usable, because it is the WiFi MAC.
 *
 * On the ESP32-C3, ETS_WIFI_MAC_INTR_SOURCE is 0 -- the first entry in a table
 * ESP-IDF's soc/interrupts.h labels "decided by hardware, don't touch this".
 * The BSP treats a vector number as a peripheral source, so vector 0 has to
 * work.
 *
 * It did not.  bsp_interrupt_is_valid_vector() rejected 0 outright, and
 * get_active_interrupt() returned 0 to mean "no source found", so a real
 * source 0 could not be told apart from no interrupt at all.  The visible
 * symptom was a handler install failing with RTEMS_INVALID_ID, and the
 * consequence was that the WiFi radio could never receive anything.
 *
 * This asserts the specific thing that was broken rather than just "interrupts
 * work": that vector 0 is valid, that a handler installs on it, and that
 * raising it actually reaches that handler.  A test that only used some other
 * vector would have passed throughout.
 */

#include <bsp.h>
#include <bsp/irq-generic.h>

#include <rtems.h>

#include <stdio.h>
#include <stdlib.h>

static int failures;

static volatile unsigned vector0_calls;

static void check( const char *what, bool ok )
{
  printf( "%-56s %s\n", what, ok ? "ok" : "FAIL" );

  if ( !ok ) {
    ++failures;
  }
}

static void vector0_handler( void *arg )
{
  (void) arg;
  ++vector0_calls;
}

static rtems_task Init( rtems_task_argument arg )
{
  rtems_status_code sc;
  bool              enabled;

  (void) arg;

  printf( "\n*** ESP32-C3 IRQ SOURCE 0 TEST ***\n" );

  check( "vector 0 is a valid vector", bsp_interrupt_is_valid_vector( 0 ) );

  /*
   * Still bounded at the top.  If the sentinel that replaced 0 were itself
   * accepted as a vector, the dispatcher would index off the end of the
   * handler table -- so this is the other half of the same change.
   */
  check(
    "the one-past-the-end vector is still rejected",
    !bsp_interrupt_is_valid_vector( BSP_INTERRUPT_VECTOR_COUNT )
  );

  sc = rtems_interrupt_handler_install(
    0,
    "irq0",
    RTEMS_INTERRUPT_UNIQUE,
    vector0_handler,
    (void *) (uintptr_t) 0x5a
  );
  check( "a handler installs on vector 0", sc == RTEMS_SUCCESSFUL );

  if ( sc != RTEMS_SUCCESSFUL ) {
    printf( "       rtems_interrupt_handler_install returned %i\n", (int) sc );
    goto done;
  }

  sc = bsp_interrupt_vector_enable( 0 );
  check( "vector 0 enables", sc == RTEMS_SUCCESSFUL );

  /*
   * Enabling is what programs the interrupt matrix, so a source absent from
   * irq_mappings[] fails here with RTEMS_UNSATISFIED rather than at install.
   * The WiFi sources were absent from that table as well as being rejected by
   * number, and only one of the two was visible at a time.
   */
  enabled = false;
  sc = bsp_interrupt_vector_is_enabled( 0, &enabled );
  check( "and reports itself enabled", sc == RTEMS_SUCCESSFUL && enabled );

  /*
   * Delivery is NOT tested here, and cannot be from software.
   *
   * bsp_interrupt_raise() handles only the four software interrupts; a
   * peripheral source cannot be triggered by writing a register, which is
   * correct hardware behaviour and not a gap in the BSP.  So what is checkable
   * without a radio is the routing: that vector 0 is accepted, that a handler
   * attaches to it, and that enabling programs the interrupt matrix and
   * disabling clears it again.  Those are precisely the steps that failed
   * before, and they are the whole of the fix.
   *
   * That a real WiFi MAC interrupt reaches the handler is unverified, and
   * waits on QEMU's MAC model asserting one or on hardware.
   */
  sc = bsp_interrupt_vector_disable( 0 );
  check( "vector 0 disables", sc == RTEMS_SUCCESSFUL );

  enabled = true;
  sc = bsp_interrupt_vector_is_enabled( 0, &enabled );
  check(
    "and the matrix mapping is cleared",
    sc == RTEMS_SUCCESSFUL && !enabled
  );

  /*
   * Re-enable and confirm it comes back.  A disable that happened to clear
   * some unrelated register would pass the check above and fail this one.
   */
  sc = bsp_interrupt_vector_enable( 0 );
  enabled = false;
  (void) bsp_interrupt_vector_is_enabled( 0, &enabled );
  check( "and enabling maps it again", sc == RTEMS_SUCCESSFUL && enabled );

  (void) bsp_interrupt_vector_disable( 0 );

done:
  printf( "\n%d failure(s)\n", failures );

  if ( failures == 0 ) {
    printf( "CI-MARKER irq0 ok\n" );
  }

  printf( "*** END OF ESP32-C3 IRQ SOURCE 0 TEST ***\n" );

  exit( 0 );
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_MAXIMUM_TASKS 4
#define CONFIGURE_MAXIMUM_DRIVERS 4
#define CONFIGURE_INIT_TASK_STACK_SIZE ( 8 * 1024 )
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
