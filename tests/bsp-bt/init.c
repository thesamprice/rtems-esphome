/*
 * ESP32-C3 Bluetooth bring-up test.
 *
 * HARDWARE ONLY.  run-bsp-test.sh runs the other bsp-* tests under QEMU; this
 * one cannot go there.  The Espressif QEMU fork models neither the RTC power
 * domain forces nor the Bluetooth clock bits, so bsp_esp32_bt_enable() reads
 * back what it wrote as unchanged and returns RTEMS_IO_ERROR.  Build it
 * against a USB-Serial-JTAG BSP and flash it:
 *
 *   riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -O2 -Wall -Wextra \
 *     -isystem $LIB/include -B $LIB -qrtems -o bt.exe tests/bsp-bt/init.c
 *   riscv-rtems7-objcopy -O binary bt.exe bt.raw
 *   esptool --chip esp32c3 -p /dev/cu.usbmodem* write-flash 0 bt.raw
 *
 * hw-run.log beside this file is a recorded pass.
 *
 * Proves three things and says which of them it could not prove:
 *
 *  1. bsp_esp32_bt_enable() brings the Bluetooth block up from the powered
 *     down, isolated state.  The negative control is bsp_esp32_bt_disable(),
 *     which must put it back.  The down state is established by calling
 *     disable() first rather than assumed from the boot state: DIG_PWC and
 *     DIG_ISO live in the always-on RTC domain and survive a CPU reset, so
 *     after any run of this test the next boot starts with Bluetooth already
 *     up.  Only a power-on reset shows the ROM's own state, and the test says
 *     which reset it got.
 *  2. The seven Bluetooth interrupt sources are in the interrupt matrix table
 *     and route to the CPU channels the BSP assigns them.  The negative
 *     control is source 11, ETS_I2C_MASTER_SOURCE, which is deliberately not
 *     in the table and must still be refused.
 *  3. Handlers install on the Bluetooth sources through the ordinary RTEMS
 *     interrupt API.
 *
 * What it does not prove is that a Bluetooth interrupt fires: nothing on this
 * part can assert one without the link layer controller running, and
 * bsp_interrupt_raise() only reaches the four software sources.
 */

#include <bsp.h>
#include <bsp/bt.h>
#include <bsp/irq.h>

#include <rtems.h>
#include <rtems/bspIo.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define RTC_REG( off ) ( *( (volatile uint32_t *) ( RTC_CNTL_BASE + (off) ) ) )
#define RTC_CNTL_RESET_STATE   0x0038
#define RESET_CAUSE_PROCPU( v ) ( (v) & 0x3fu )
#define RESET_REASON_CHIP_POWER_ON 0x01u

#define INT_MATRIX_MAP( src ) \
  ( *( (volatile uint32_t *) ( INT_MATRIX_BASE + (src) * 4u ) ) )

#define BT_FORCE_PD_BIT   ( 1u << 11 )
#define BT_FORCE_PU_BIT   ( 1u << 12 )
#define BT_FORCE_ISO_BIT  ( 1u << 22 )
#define BT_FORCE_NOISO_BIT ( 1u << 23 )
#define BT_CLK_BITS       ( ( 1u << 11 ) | ( 1u << 16 ) | ( 1u << 17 ) )

/* Not in the interrupt matrix table: ETS_I2C_MASTER_SOURCE. */
#define UNMAPPED_SOURCE 11

static int failures;

static void check( const char *what, bool ok )
{
  printk( "%-52s %s\n", what, ok ? "ok" : "FAIL" );
  if ( !ok ) {
    ++failures;
  }
}

static volatile unsigned bt_handler_calls;

static void bt_handler( void *arg )
{
  (void) arg;
  ++bt_handler_calls;
}

static const struct {
  rtems_vector_number source;
  uint32_t            cpu_int;
  const char         *name;
} bt_sources[] = {
  { BT_MAC_INTR, 22, "BT_MAC" },
  { BT_BB_INTR, 22, "BT_BB" },
  { BT_BB_NMI_INTR, 22, "BT_BB_NMI" },
  { RWBT_INTR, 6, "RWBT" },
  { RWBLE_INTR, 6, "RWBLE" },
  { RWBT_NMI_INTR, 6, "RWBT_NMI" },
  { RWBLE_NMI_INTR, 6, "RWBLE_NMI" }
};

static void report_state( const char *when )
{
  bsp_esp32_bt_state s;

  bsp_esp32_bt_get_state( &s );
  printk(
    "  %-8s DIG_PWC=0x%08" PRIx32 " DIG_ISO=0x%08" PRIx32
    " CLK_EN=0x%08" PRIx32 "\n",
    when, s.dig_pwc, s.dig_iso, s.clk_en
  );
}

static void test_power( void )
{
  bsp_esp32_bt_state before;
  bsp_esp32_bt_state after;
  bsp_esp32_bt_state off;
  rtems_status_code  sc;
  uint32_t           cause;

  printk( "\n-- power, clocks and reset --\n" );

  cause = RESET_CAUSE_PROCPU( RTC_REG( RTC_CNTL_RESET_STATE ) );
  printk( "  reset cause 0x%02" PRIx32 "%s\n", cause,
          cause == RESET_REASON_CHIP_POWER_ON ? " (power on)"
                                              : " (not a power-on reset)" );

  bsp_esp32_bt_get_state( &before );
  report_state( "boot" );

  /*
   * The clocks are in SYSCON, which a CPU reset does clear, so this holds
   * however the part was reset.
   */
  check(
    "the Bluetooth clocks are off at boot",
    ( before.clk_en & BT_CLK_BITS ) == 0
  );

  if ( cause == RESET_REASON_CHIP_POWER_ON ) {
    check( "the ROM leaves Bluetooth forced down",
           ( before.dig_pwc & BT_FORCE_PD_BIT ) != 0 );
    check( "the ROM leaves Bluetooth isolated",
           ( before.dig_iso & BT_FORCE_ISO_BIT ) != 0 );
    check( "bsp_esp32_bt_is_enabled() is false at boot",
           !bsp_esp32_bt_is_enabled() );
  } else {
    printk( "  RTC_CNTL keeps DIG_PWC and DIG_ISO across a CPU reset, so the\n"
            "  boot state here is whatever the last run left.  Skipping the\n"
            "  three checks that only a power-on reset can answer.\n" );
  }

  /* Establish the down state rather than assume it. */
  (void) bsp_esp32_bt_disable();
  check( "from a known down state, is_enabled() is false",
         !bsp_esp32_bt_is_enabled() );

  sc = bsp_esp32_bt_enable();
  bsp_esp32_bt_get_state( &after );
  report_state( "enabled" );

  check( "bsp_esp32_bt_enable() returns RTEMS_SUCCESSFUL",
         sc == RTEMS_SUCCESSFUL );
  check( "the power-down force is cleared",
         ( after.dig_pwc & BT_FORCE_PD_BIT ) == 0 );
  check( "the power-up force is set",
         ( after.dig_pwc & BT_FORCE_PU_BIT ) != 0 );
  check( "the isolation force is cleared",
         ( after.dig_iso & BT_FORCE_ISO_BIT ) == 0 );
  check( "the no-isolation force is set",
         ( after.dig_iso & BT_FORCE_NOISO_BIT ) != 0 );
  check( "the baseband and link controller clocks are on",
         ( after.clk_en & BT_CLK_BITS ) == BT_CLK_BITS );
  check( "bsp_esp32_bt_is_enabled() is true", bsp_esp32_bt_is_enabled() );

  /*
   * The negative control for all of the above.  If enable() had done nothing
   * and the block had merely been up all along, this would not change anything
   * back.
   */
  sc = bsp_esp32_bt_disable();
  bsp_esp32_bt_get_state( &off );
  report_state( "disabled" );

  check( "bsp_esp32_bt_disable() returns RTEMS_SUCCESSFUL",
         sc == RTEMS_SUCCESSFUL );
  check( "NEGATIVE CONTROL: disable puts it back down",
         ( off.dig_pwc & BT_FORCE_PD_BIT ) != 0 &&
         ( off.dig_iso & BT_FORCE_ISO_BIT ) != 0 &&
         ( off.clk_en & BT_CLK_BITS ) == 0 );
  check( "NEGATIVE CONTROL: bsp_esp32_bt_is_enabled() is false again",
         !bsp_esp32_bt_is_enabled() );

  /* Leave it on for the rest of the run. */
  sc = bsp_esp32_bt_enable();
  check( "re-enable succeeds, so enable() is idempotent",
         sc == RTEMS_SUCCESSFUL );
}

static void test_interrupts( void )
{
  rtems_status_code sc;
  uint32_t          map;
  size_t            i;

  printk( "\n-- interrupt matrix --\n" );

  for ( i = 0; i < RTEMS_ARRAY_SIZE( bt_sources ); ++i ) {
    char label[ 64 ];

    sc = rtems_interrupt_handler_install(
      bt_sources[ i ].source,
      bt_sources[ i ].name,
      RTEMS_INTERRUPT_SHARED,
      bt_handler,
      NULL
    );
    snprintf( label, sizeof( label ), "install a handler on %s (source %u)",
              bt_sources[ i ].name,
              (unsigned) bt_sources[ i ].source );
    check( label, sc == RTEMS_SUCCESSFUL );

    map = INT_MATRIX_MAP( bt_sources[ i ].source );
    snprintf( label, sizeof( label ), "  %s routes to CPU channel %u",
              bt_sources[ i ].name, (unsigned) bt_sources[ i ].cpu_int );
    printk( "%-52s %s  (reads %" PRIu32 ")\n", label,
            map == bt_sources[ i ].cpu_int ? "ok" : "FAIL", map );
    if ( map != bt_sources[ i ].cpu_int ) {
      ++failures;
    }
  }

  /*
   * The negative control.  Source 11 is a real peripheral source on this part
   * and a valid vector, but it is not in the BSP's mapping table, so enabling
   * it has to fail and its matrix entry has to stay clear.  If this passed,
   * the seven results above would prove nothing about the table.
   */
  sc = rtems_interrupt_vector_enable( UNMAPPED_SOURCE );
  check( "NEGATIVE CONTROL: an unmapped source is refused",
         sc != RTEMS_SUCCESSFUL );
  check( "NEGATIVE CONTROL: and its matrix entry stays clear",
         INT_MATRIX_MAP( UNMAPPED_SOURCE ) == 0 );

  printk( "\n  no Bluetooth interrupt can be raised without the link layer\n" );
  printk( "  controller, so the handler was called %u times, as expected\n",
          bt_handler_calls );
}

static rtems_task Init( rtems_task_argument arg )
{
  (void) arg;

  printk( "\n*** ESP32-C3 BLUETOOTH BRING-UP TEST ***\n" );

  test_power();
  test_interrupts();

  /*
   * Put the block back down before the run ends, so the next boot starts from
   * the state a power-on reset would give even though it is a CPU reset.
   */
  (void) bsp_esp32_bt_disable();

  printk( "\n%d failure(s)\n", failures );
  if ( failures == 0 ) {
    printk( "CI-MARKER bt bring-up ok\n" );
  }
  printk( "*** END OF ESP32-C3 BLUETOOTH BRING-UP TEST ***\n" );

  exit( 0 );
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_MAXIMUM_TASKS 4
#define CONFIGURE_UNLIMITED_OBJECTS
#define CONFIGURE_UNIFIED_WORK_AREAS
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_STACK_SIZE ( 8 * 1024 )
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
