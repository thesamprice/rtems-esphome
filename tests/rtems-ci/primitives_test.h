#pragma once
// Self-test for the RTEMS platform primitives, run from on_boot.
//
// These are the things components rely on and that nothing else in the lane
// exercises deliberately: the reference node proves the clock advances, but not
// that delay() waits long enough, that try_lock() fails when it should, or that
// a wake from another task reaches the main loop promptly.
//
// It runs in the target, on the real BSP, rather than as a host unit test,
// because every property here is a property of RTEMS on this board and a host
// test would prove nothing about it.
//
// Prints one line per check.  The harness asserts on "PRIMITIVES ok"; any
// failure prints "PRIMITIVES FAIL" first and the harness's failure signatures
// catch it.

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/wake.h"

#include <rtems.h>
#include <bsp/irq.h>

// The SYSTIMER comparator the clock driver does not use.  TARGET0 is the clock
// tick; TARGET1 and TARGET2 are free, so an interrupt can be raised here
// without disturbing timekeeping.  Offsets from the C3 TRM, confirmed against
// QEMU's model.
#define ST_BASE 0x60023000U
#define ST_REG(off) (*(volatile uint32_t *) (ST_BASE + (off)))
#define ST_CONF 0x00U
#define ST_TARGET1_HI 0x24U
#define ST_TARGET1_LO 0x28U
#define ST_TARGET1_CONF 0x38U
#define ST_UNIT0_OP 0x04U
#define ST_UNIT0_VALUE_LO 0x44U
#define ST_COMP1_LOAD 0x54U
#define ST_INT_ENA 0x64U
#define ST_INT_CLR 0x6cU
#define ST_TARGET1_WORK_EN (1U << 23)
#define ST_INT_TARGET1 (1U << 1)
#define SYSTIMER_TARGET1_IRQ 38  // from the BSP's c3/chip_definitions.h

namespace rtems_primitives {

static const char *const TAG = "primtest";

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static int g_failures = 0;

inline void check(bool ok, const char *what) {
  if (ok) {
    ESP_LOGI(TAG, "  ok   %s", what);
  } else {
    ++g_failures;
    ESP_LOGE(TAG, "  FAIL %s", what);
  }
}

// --- shared state for the helper task ---------------------------------------
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static esphome::Mutex *g_mutex = nullptr;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_helper_holds = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_helper_release = false;

extern "C" inline rtems_task primitives_helper(rtems_task_argument arg) {
  (void) arg;
  g_mutex->lock();
  g_helper_holds = true;
  // Hold until the main task has had its look, then let go.
  while (!g_helper_release) {
    rtems_task_wake_after(1);
  }
  g_mutex->unlock();
  g_helper_holds = false;
  rtems_task_delete(RTEMS_SELF);
}

inline void test_time() {
  ESP_LOGI(TAG, "time:");

  // Monotonic: millis_64() must never go backwards across many reads. This is
  // the property the scheduler depends on and the one a truncating or
  // rollover-tracking clock gets wrong.
  uint64_t prev = esphome::millis_64();
  bool monotonic = true;
  for (int i = 0; i < 2000; ++i) {
    const uint64_t now = esphome::millis_64();
    if (now < prev) {
      monotonic = false;
      break;
    }
    prev = now;
  }
  check(monotonic, "millis_64 is monotonic over 2000 reads");

  // millis() must agree with the low bits of millis_64(); they are two views of
  // one clock and a disagreement means one of them is derived wrongly.
  const uint64_t wide = esphome::millis_64();
  const uint32_t narrow = esphome::millis();
  check(narrow - static_cast<uint32_t>(wide) < 50, "millis agrees with millis_64");

  // delay() must wait at least as long as asked. Waiting slightly longer is
  // fine and expected -- it rounds up to a whole tick -- but waiting less would
  // turn every rate limit built on it into a busy loop.
  const uint64_t before = esphome::millis_64();
  esphome::delay(50);
  const uint64_t elapsed = esphome::millis_64() - before;
  check(elapsed >= 50, "delay(50) waits at least 50ms");
  check(elapsed < 200, "delay(50) does not overshoot wildly");
  ESP_LOGI(TAG, "       delay(50) measured %ums", static_cast<unsigned>(elapsed));

  // micros() must advance across a busy wait far shorter than a tick, which is
  // the resolution claim the counter makes.
  const uint32_t us_before = esphome::micros();
  esphome::delayMicroseconds(1000);
  const uint32_t us_elapsed = esphome::micros() - us_before;
  check(us_elapsed >= 900 && us_elapsed < 20000, "delayMicroseconds(1000) is about 1ms");
  ESP_LOGI(TAG, "       delayMicroseconds(1000) measured %uus", static_cast<unsigned>(us_elapsed));
}

inline void test_mutex() {
  ESP_LOGI(TAG, "mutex:");

  esphome::Mutex m;
  g_mutex = &m;

  // Uncontended: try_lock must succeed on a free mutex.
  check(m.try_lock(), "try_lock succeeds when free");
  m.unlock();

  // Contended: this is the property NASA OSAL cannot express, and the reason
  // docs/architecture.md says not to use it for Mutex. It needs a second task,
  // because a mutex held by the asking task is a different question.
  rtems_id helper = RTEMS_INVALID_ID;
  const rtems_status_code sc =
      rtems_task_create(rtems_build_name('P', 'R', 'M', 'T'), 100, RTEMS_MINIMUM_STACK_SIZE * 4,
                        RTEMS_DEFAULT_MODES, RTEMS_DEFAULT_ATTRIBUTES, &helper);
  check(sc == RTEMS_SUCCESSFUL, "helper task created");
  if (sc != RTEMS_SUCCESSFUL) {
    return;
  }
  g_helper_release = false;
  rtems_task_start(helper, primitives_helper, 0);

  // Wait for the helper to take it.
  for (int i = 0; i < 500 && !g_helper_holds; ++i) {
    esphome::delay(1);
  }
  check(g_helper_holds, "helper acquired the mutex");
  check(!m.try_lock(), "try_lock fails when another task holds it");

  g_helper_release = true;
  for (int i = 0; i < 500 && g_helper_holds; ++i) {
    esphome::delay(1);
  }
  check(!g_helper_holds, "helper released the mutex");
  check(m.try_lock(), "try_lock succeeds again after release");

  // Recursion.  RTEMS lets the owner re-take a binary semaphore under priority
  // inheritance; the FreeRTOS mutex ESP32 and LibreTiny use does not, so this
  // is a real platform divergence rather than a contract.
  //
  // The point of the check is not that it succeeds -- it always did -- but that
  // USE_RTEMS_MUTEX_RECURSION_CHECK notices and says so.  A diagnostic printed
  // where nobody reads it is the failure mode worth ruling out, so the harness
  // greps the console for the message rather than this asserting on it: see
  // "CI-MARKER recursion reported" in the lane.  m is held here, from the
  // try_lock above.
  ESP_LOGI(TAG, "  re-taking from the owning task, expect a report below:");
  m.lock();
  m.unlock();
  m.unlock();
}

inline void test_isr_context() {
  ESP_LOGI(TAG, "isr context:");
  check(!esphome::in_isr_context(), "in_isr_context is false in task context");
}

// --- ISR-safe wake ----------------------------------------------------------
// The reasoning behind wake_loop_isrsafe() -- that rtems_semaphore_release() is
// callable from an ISR and RTEMS defers dispatch to the outermost interrupt
// exit -- was never executed until this test. Nothing else in the lane raises
// an interrupt that calls it.

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_isr_ran = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_isr_saw_isr_context = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile uint64_t g_isr_fired_ns = 0;

inline void systimer_target1_isr(void *arg) {
  (void) arg;
  ST_REG(ST_INT_CLR) = ST_INT_TARGET1;
  g_isr_saw_isr_context = esphome::in_isr_context();
  g_isr_fired_ns = rtems_clock_get_uptime_nanoseconds();
  g_isr_ran = true;
  // The call under test.
  esphome::wake_loop_isrsafe();
}

inline void test_isr_wake() {
  ESP_LOGI(TAG, "isr wake:");

  const rtems_status_code sc = rtems_interrupt_handler_install(
      SYSTIMER_TARGET1_IRQ, "prim-t1", RTEMS_INTERRUPT_UNIQUE, systimer_target1_isr, nullptr);
  check(sc == RTEMS_SUCCESSFUL, "installed a handler on SYSTIMER TARGET1");
  if (sc != RTEMS_SUCCESSFUL) {
    return;
  }

  g_isr_ran = false;
  g_isr_saw_isr_context = false;

  // One-shot, 50 ms out: read the counter, add 50 ms of 16 MHz ticks, arm.
  ST_REG(ST_UNIT0_OP) = (1U << 30);
  while ((ST_REG(ST_UNIT0_OP) & (1U << 29)) == 0) {
  }
  const uint32_t now_ticks = ST_REG(ST_UNIT0_VALUE_LO);
  ST_REG(ST_TARGET1_CONF) = 0;  // one-shot: PERIOD_MODE clear, unit0 selected
  ST_REG(ST_TARGET1_HI) = 0;
  ST_REG(ST_TARGET1_LO) = now_ticks + (16000000U / 20U);
  ST_REG(ST_COMP1_LOAD) = 1U;
  ST_REG(ST_CONF) |= ST_TARGET1_WORK_EN;
  ST_REG(ST_INT_ENA) |= ST_INT_TARGET1;

  // Block the way Application::loop() does. If the ISR's wake works this
  // returns early; if it does not, the timeout expires and the elapsed time
  // gives it away.
  const uint64_t before_ns = rtems_clock_get_uptime_nanoseconds();
  esphome::internal::wakeable_delay(5000);
  // Immediately: anything between the wait returning and this read -- a check(),
  // a log line -- is measured as latency and is not.
  const uint64_t resumed_ns = rtems_clock_get_uptime_nanoseconds();

  check(g_isr_ran, "the interrupt fired");
  check(g_isr_saw_isr_context, "in_isr_context is TRUE inside the handler");
  check(resumed_ns - before_ns < 4000000000ULL, "wakeable_delay returned before its 5s timeout");
  if (g_isr_ran) {
    // Nanoseconds, not microseconds: the wake path is a semaphore release and a
    // task dispatch, which is well under a microsecond, and integer division
    // would report every one of them as zero.
    ESP_LOGI(TAG, "       isr-to-loop latency %uns", static_cast<unsigned>(resumed_ns - g_isr_fired_ns));
  }

  ST_REG(ST_INT_ENA) &= ~ST_INT_TARGET1;
  ST_REG(ST_CONF) &= ~ST_TARGET1_WORK_EN;
  rtems_interrupt_handler_remove(SYSTIMER_TARGET1_IRQ, systimer_target1_isr, nullptr);
}

// --- wake latency under repeated signalling -------------------------------
// One measurement under one condition says nothing about the tail, and the tail
// is what a latency-sensitive event loop cares about. This signals from a
// background task at randomised intervals and reports the distribution.

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile uint64_t g_signal_ns = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_stress_stop = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_signal_pending = false;

extern "C" inline rtems_task wake_stress_helper(rtems_task_argument arg) {
  (void) arg;
  // xorshift rather than rand(): no allocation, no lock, and reproducible from
  // a fixed seed, which matters for a test whose output is a distribution.
  uint32_t state = 0x9e3779b9U;
  while (!g_stress_stop) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    // 1..20 ms, so the main loop is sometimes already waiting and sometimes
    // not. Both are worth covering: a signal arriving while the loop is awake
    // must not be swallowed, which is why the semaphore counts.
    rtems_task_wake_after((state % 20U) + 1U);
    if (g_stress_stop) {
      break;
    }
    g_signal_ns = rtems_clock_get_uptime_nanoseconds();
    g_signal_pending = true;
    esphome::wake_loop_threadsafe();
  }
  rtems_task_delete(RTEMS_SELF);
}

inline void test_wake_stress() {
  ESP_LOGI(TAG, "wake latency:");

  rtems_id helper = RTEMS_INVALID_ID;
  const rtems_status_code sc =
      rtems_task_create(rtems_build_name('W', 'A', 'K', 'E'), 100, RTEMS_MINIMUM_STACK_SIZE * 4,
                        RTEMS_DEFAULT_MODES, RTEMS_DEFAULT_ATTRIBUTES, &helper);
  check(sc == RTEMS_SUCCESSFUL, "stress helper task created");
  if (sc != RTEMS_SUCCESSFUL) {
    return;
  }

  g_stress_stop = false;
  g_signal_pending = false;
  rtems_task_start(helper, wake_stress_helper, 0);

  static constexpr int SAMPLES = 200;
  uint64_t min_ns = ~0ULL;
  uint64_t max_ns = 0;
  uint64_t total_ns = 0;
  int counted = 0;
  int missed = 0;

  for (int i = 0; i < SAMPLES; ++i) {
    // Wait the way Application::loop() does. The timeout is far longer than the
    // helper's interval, so returning on it rather than on a wake is itself a
    // failure worth counting.
    esphome::internal::wakeable_delay(500);
    // Read before anything else, for the same reason as above.
    const uint64_t resumed_ns = rtems_clock_get_uptime_nanoseconds();
    if (!g_signal_pending) {
      ++missed;
      continue;
    }
    const uint64_t latency = resumed_ns - g_signal_ns;
    g_signal_pending = false;
    // Discard the first few: the helper's first interval overlaps this loop
    // starting up, so they measure setup rather than wake latency.
    if (i < 5) {
      continue;
    }
    min_ns = latency < min_ns ? latency : min_ns;
    max_ns = latency > max_ns ? latency : max_ns;
    total_ns += latency;
    ++counted;
  }

  g_stress_stop = true;
  esphome::delay(100);

  check(counted > 100, "most waits were ended by a wake, not by the timeout");
  if (counted > 0) {
    const uint64_t mean = total_ns / counted;
    ESP_LOGI(TAG, "       %d samples: min %uns  mean %uns  max %uns (%d timed out)", counted,
             static_cast<unsigned>(min_ns), static_cast<unsigned>(mean), static_cast<unsigned>(max_ns), missed);
    // A wake that takes longer than a clock tick has lost its point: the loop
    // would have run on the tick anyway. 10ms is the default tick period here.
    check(max_ns < 10000000ULL, "worst-case wake latency is under one clock tick");
  }
}

// --- priority inheritance ---------------------------------------------------
// Mutex is built RTEMS_BINARY_SEMAPHORE | RTEMS_PRIORITY | RTEMS_INHERIT_PRIORITY
// specifically to stop priority inversion, and those flags have never been
// exercised -- everything so far contends with one task and no timing pressure.
//
// The classic three-task inversion: a low-priority task holds the mutex, a
// medium-priority task spins and would starve it, and a high-priority task
// waits for the mutex.  Without inheritance the high task is blocked for as
// long as the medium task runs.  With it, the holder is boosted above the
// spinner, finishes, and releases.
//
// RTEMS priorities: lower number is higher priority.
#define PI_PRIO_HIGH 50
#define PI_PRIO_MEDIUM 100
#define PI_PRIO_LOW 150

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static esphome::Mutex *g_pi_mutex = nullptr;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_pi_low_holds = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_pi_medium_spinning = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_pi_high_done = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile uint64_t g_pi_block_ns = 0;

/// Busy wait without yielding.  A delay() here would hand the CPU over and
/// destroy the scenario: the point is to hold the processor.
inline void busy_wait_ms(uint32_t ms) {
  const uint64_t end = rtems_clock_get_uptime_nanoseconds() + (uint64_t) ms * 1000000ULL;
  while (rtems_clock_get_uptime_nanoseconds() < end) {
  }
}

extern "C" inline rtems_task pi_low_task(rtems_task_argument arg) {
  (void) arg;
  g_pi_mutex->lock();
  g_pi_low_holds = true;
  // Hold it across work that needs the CPU, for long enough that the other two
  // tasks reliably reach their part of the scenario first -- they poll on a
  // tick, so anything near the tick period races. Without inheritance the
  // medium task preempts here and this never finishes until the spinner stops.
  busy_wait_ms(150);
  g_pi_mutex->unlock();
  g_pi_low_holds = false;
  rtems_task_delete(RTEMS_SELF);
}

extern "C" inline rtems_task pi_medium_task(rtems_task_argument arg) {
  (void) arg;
  while (!g_pi_low_holds) {
    rtems_task_wake_after(1);
  }
  g_pi_medium_spinning = true;
  busy_wait_ms(600);
  g_pi_medium_spinning = false;
  rtems_task_delete(RTEMS_SELF);
}

extern "C" inline rtems_task pi_high_task(rtems_task_argument arg) {
  (void) arg;
  // Both conditions: the scenario only exists while the low task still holds
  // the mutex AND the medium task is spinning. Waiting on the spinner alone
  // let this run after the holder had already released, which measured
  // nothing and passed.
  while (!(g_pi_medium_spinning && g_pi_low_holds)) {
    rtems_task_wake_after(1);
  }
  const uint64_t before = rtems_clock_get_uptime_nanoseconds();
  g_pi_mutex->lock();
  g_pi_block_ns = rtems_clock_get_uptime_nanoseconds() - before;
  g_pi_mutex->unlock();
  g_pi_high_done = true;
  rtems_task_delete(RTEMS_SELF);
}

inline void test_priority_inheritance() {
  ESP_LOGI(TAG, "priority inheritance:");

  esphome::Mutex m;
  g_pi_mutex = &m;
  g_pi_low_holds = false;
  g_pi_medium_spinning = false;
  g_pi_high_done = false;
  g_pi_block_ns = 0;

  struct {
    const char *name;
    rtems_task_priority prio;
    rtems_task_entry entry;
  } tasks[] = {
      {"PILO", PI_PRIO_LOW, pi_low_task},
      {"PIME", PI_PRIO_MEDIUM, pi_medium_task},
      {"PIHI", PI_PRIO_HIGH, pi_high_task},
  };

  bool created = true;
  for (auto &t : tasks) {
    rtems_id id = RTEMS_INVALID_ID;
    if (rtems_task_create(rtems_build_name(t.name[0], t.name[1], t.name[2], t.name[3]), t.prio,
                          RTEMS_MINIMUM_STACK_SIZE * 4, RTEMS_DEFAULT_MODES, RTEMS_DEFAULT_ATTRIBUTES,
                          &id) != RTEMS_SUCCESSFUL) {
      created = false;
      break;
    }
    rtems_task_start(id, t.entry, 0);
  }
  check(created, "three tasks at three priorities created");
  if (!created) {
    return;
  }

  for (int i = 0; i < 1000 && !g_pi_high_done; ++i) {
    esphome::delay(1);
  }
  check(g_pi_high_done, "the high-priority task acquired the mutex");
  if (!g_pi_high_done) {
    return;
  }

  const uint32_t block_ms = static_cast<uint32_t>(g_pi_block_ns / 1000000ULL);
  ESP_LOGI(TAG, "       high-priority task blocked for %ums", static_cast<unsigned>(block_ms));
  // The holder needs 150ms of CPU; the spinner holds the processor for 600ms.
  // Blocking for something near 150 means the holder was boosted past the
  // spinner and finished -- inheritance working. Blocking for ~600 would mean
  // it was not, and the inversion is real. The threshold sits between the two
  // rather than near either, so a result close to it is a reason to look
  // rather than a pass.
  check(block_ms > 0, "the high-priority task actually blocked");
  check(block_ms < 400, "blocked for the holder's work, not the spinner's");
}

inline void run() {
  ESP_LOGI(TAG, "PRIMITIVES start");
  g_failures = 0;
  test_time();
  test_mutex();
  test_isr_context();
  test_isr_wake();
  test_wake_stress();
  test_priority_inheritance();
  if (g_failures == 0) {
    ESP_LOGI(TAG, "PRIMITIVES ok");
  } else {
    ESP_LOGE(TAG, "PRIMITIVES FAIL (%d)", g_failures);
  }
}

}  // namespace rtems_primitives
