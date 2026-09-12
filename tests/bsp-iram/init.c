/*
 * Does code placed in .fast_text execute from RAM?
 *
 * This is what #42 and the WiFi HAL both need and neither could have: a SPI
 * flash driver must not fetch instructions from the flash it is erasing, and
 * the Espressif WiFi libraries put about 40 KiB of code in IRAM for the same
 * class of reason.
 *
 * Before ESP32C_IRAM_REGION_SIZE existed, REGION_FAST_TEXT was aliased to
 * CODE_FLASH_MAPPED -- so .fast_text linked, and landed in flash.  The
 * section appeared to work and did nothing, which is the failure this checks
 * for: the address matters as much as the behaviour.
 *
 * The part reaches one SRAM through two windows, 0x3fc80000 on the data bus
 * and 0x40380000 on the instruction bus, so "in RAM" means "in the
 * instruction window" and is checkable against a constant.
 */

#include <bsp.h>
#include <bsp/linker-symbols.h>

#include <rtems.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define IRAM_BASE  0x40380000U
#define IRAM_LIMIT ( IRAM_BASE + 0x50000U )
#define FLASH_BASE 0x42000000U

static int failures;

static void check(const char *what, bool ok)
{
  printf("%-56s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

/*
 * Deliberately not inlinable and not constant-folded: the point is that this
 * body is fetched from RAM, so the compiler must actually emit and call it.
 */
BSP_FAST_TEXT_SECTION RTEMS_NO_INLINE int ram_resident_add(int a, int b)
{
  return a + b;
}

BSP_FAST_TEXT_SECTION RTEMS_NO_INLINE static uint32_t ram_resident_own_address(void)
{
  return (uint32_t) (uintptr_t) &ram_resident_own_address;
}

static rtems_task Init(rtems_task_argument arg)
{
  uintptr_t addr = (uintptr_t) &ram_resident_add;

  (void) arg;

  printf("\n*** ESP32C3 IRAM TEST ***\n");

  printf("ram_resident_add is at 0x%08" PRIxPTR "\n", addr);
  printf(".fast_text spans 0x%08" PRIxPTR " to 0x%08" PRIxPTR ", loaded from 0x%08" PRIxPTR "\n",
    (uintptr_t) bsp_section_fast_text_begin,
    (uintptr_t) bsp_section_fast_text_end,
    (uintptr_t) bsp_section_fast_text_load_begin);

  check("the function is in the instruction window, not flash",
        addr >= IRAM_BASE && addr < IRAM_LIMIT);
  check("and specifically not in mapped flash", addr < FLASH_BASE);

  /* If the boot copy did not run, these instructions are whatever the SRAM
   * held at reset and calling this either returns nonsense or faults. */
  check("calling it returns the right answer", ram_resident_add(40, 2) == 42);
  check("and again with different operands", ram_resident_add(-7, 7) == 0);

  /* Reading its own address from inside proves the executing copy is the one
   * in RAM rather than a flash-resident duplicate the linker kept. */
  check("the executing copy reports a RAM address",
        ram_resident_own_address() >= IRAM_BASE
          && ram_resident_own_address() < IRAM_LIMIT);

  check(".fast_text is not empty", bsp_section_fast_text_size > 0);
  check("its load address is in flash, so it was copied",
        (uintptr_t) bsp_section_fast_text_load_begin < 0x40000000U);

  printf("\n%d failure(s)\n", failures);
  if (failures == 0) {
    printf("CI-MARKER iram ok\n");
  }
  printf("*** END OF ESP32C3 IRAM TEST ***\n");
  exit(0);
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_MICROSECONDS_PER_TICK 1000
#define CONFIGURE_MAXIMUM_TASKS 4
#define CONFIGURE_MAXIMUM_FILE_DESCRIPTORS 8
#define CONFIGURE_MAXIMUM_DRIVERS 4
#define CONFIGURE_INIT_TASK_STACK_SIZE (8 * 1024)
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
