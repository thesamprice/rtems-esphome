/*
 * How much does a sleep overshoot, and is the overshoot the clock tick?
 *
 * #49 records that RTEMS examines watchdog deadlines once per tick, so a sleep
 * ends at the first tick at or after its deadline, and cites delay(50)
 * returning in 50..60 ms on this BSP.  That was measured against a 10 ms tick.
 * The clock has since been configured to 1 kHz, so the number wants
 * re-measuring before anyone decides a one-shot clock driver is worth writing.
 *
 * Sleeps are requested at offsets that deliberately straddle a tick boundary,
 * because a request that is already a whole number of ticks would understate
 * the quantisation -- it has nothing to round up.
 */

#include <rtems.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int failures;

static void check(const char *what, bool ok)
{
  printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

static uint64_t now_ns(void)
{
  return rtems_clock_get_uptime_nanoseconds();
}

/* Sleep for exactly this many nanoseconds and report what it cost. */
static uint64_t measure_sleep(uint64_t want_ns)
{
  struct timespec req;
  uint64_t before;
  uint64_t after;

  req.tv_sec = (time_t) ( want_ns / 1000000000ULL );
  req.tv_nsec = (long) ( want_ns % 1000000000ULL );

  before = now_ns();
  clock_nanosleep( CLOCK_MONOTONIC, 0, &req, NULL );
  after = now_ns();

  return after - before;
}

static rtems_task Init(rtems_task_argument arg)
{
  /* Offsets chosen to land mid-tick at 1 kHz: .1, .3, .5, .7, .9 of a tick. */
  static const uint64_t requests[] = {
    1100000ULL, 5300000ULL, 10500000ULL, 20700000ULL, 50900000ULL
  };
  uint64_t tick_ns;
  uint64_t worst = 0;
  size_t i;

  (void) arg;

  printf("\n*** ESP32C3 SLEEP QUANTISATION ***\n");

  tick_ns = (uint64_t) rtems_configuration_get_microseconds_per_tick() * 1000ULL;
  printf("clock tick is %llu ns (%llu Hz)\n\n",
    (unsigned long long) tick_ns,
    (unsigned long long) ( 1000000000ULL / tick_ns ));

  /* Warm: the first sleep of a run pays for whatever it faults in. */
  (void) measure_sleep( 2000000ULL );

  for ( i = 0; i < RTEMS_ARRAY_SIZE( requests ); ++i ) {
    uint64_t got = measure_sleep( requests[ i ] );
    uint64_t over = got > requests[ i ] ? got - requests[ i ] : 0;

    printf("  asked %8llu ns, slept %8llu ns, over by %7llu ns\n",
      (unsigned long long) requests[ i ],
      (unsigned long long) got,
      (unsigned long long) over);

    /* The contract that matters: never short.  A sleep that returns early is
     * a correctness bug; one that returns late is quantisation. */
    if ( got < requests[ i ] ) {
      printf("       returned EARLY, which is a defect not a rounding\n");
      ++failures;
    }

    if ( over > worst ) {
      worst = over;
    }
  }

  printf("\nworst overshoot %llu ns against a %llu ns tick\n",
    (unsigned long long) worst, (unsigned long long) tick_ns);

  check("no sleep returned early", failures == 0);
  check("worst overshoot is within one tick", worst <= tick_ns);

  printf("\n%d failure(s)\n", failures);
  if (failures == 0) {
    printf("CI-MARKER tick ok\n");
  }
  printf("*** END OF ESP32C3 SLEEP QUANTISATION ***\n");
  exit(0);
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_MICROSECONDS_PER_TICK 1000
#define CONFIGURE_FILESYSTEM_IMFS
#define CONFIGURE_MAXIMUM_TASKS 8
#define CONFIGURE_MAXIMUM_POSIX_THREADS 4
#define CONFIGURE_MAXIMUM_FILE_DESCRIPTORS 16
#define CONFIGURE_MAXIMUM_DRIVERS 8
#define CONFIGURE_INIT_TASK_STACK_SIZE (8 * 1024)
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
